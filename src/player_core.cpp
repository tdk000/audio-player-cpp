#include "player_core.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <system_error>

#include "media_probe.h"

namespace {

double clamp01(double value) {
    return std::max(0.0, std::min(1.0, value));
}

// Человеческое объяснение вместо «Out of memory» от декодера: смотрим, что
// на самом деле лежит в файле, и подсказываем, что с ним делать.
std::string describe_load_failure(const std::string& path, ma_result result) {
    const media::Format format = media::sniff_format(path);
    switch (format) {
        case media::Format::M4a:
        case media::Format::Aac:
        case media::Format::Wma:
            return std::string("Внутри этого файла на самом деле ") + media::format_name(format) +
                   ", а не MP3.\nЭтот плеер такие форматы не воспроизводит.\n"
                   "Конвертируйте файл в MP3, например:\n"
                   "    ffmpeg -i \"файл\" -b:a 192k \"файл.mp3\"";
        case media::Format::Wav:
        case media::Format::Flac:
        case media::Format::Ogg:
        case media::Format::Mp3:
            return std::string("Файл повреждён: декодер не смог прочитать аудиоданные.\n(") +
                   ma_result_description(result) + ")";
        case media::Format::Unknown:
            break;
    }
    return std::string("Это не аудиофайл или он повреждён (пустой, обрезанный).\n(") +
           ma_result_description(result) + ")";
}

}  // namespace

double PlayerCore::now() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

PlayerCore::PlayerCore(double volume, Device device) : volume_(clamp01(volume)) {
    ma_engine_config config = ma_engine_config_init();
    if (device == Device::Null) {
        ma_backend backends[] = {ma_backend_null};
        const ma_result result = ma_context_init(backends, 1, nullptr, &context_);
        if (result != MA_SUCCESS) {
            throw std::runtime_error(std::string("miniaudio (null backend): ") +
                                     ma_result_description(result));
        }
        context_ready_ = true;
        config.pContext = &context_;
    }

    const ma_result result = ma_engine_init(&config, &engine_);
    if (result != MA_SUCCESS) {
        if (context_ready_) {
            ma_context_uninit(&context_);
            context_ready_ = false;
        }
        throw std::runtime_error(std::string("miniaudio: ") + ma_result_description(result));
    }
    engine_ready_ = true;
    create_eq_nodes();
    stamp();
}

// Цепочка фильтров создаётся один раз на весь движок: при смене настроек
// достаточно пересчитать коэффициенты (apply_eq), а не пересобирать граф.
void PlayerCore::create_eq_nodes() {
    if (!engine_ready_ || eq_nodes_ready_) {
        return;
    }
    ma_node_graph* graph = ma_engine_get_node_graph(&engine_);
    const ma_uint32 channels = ma_engine_get_channels(&engine_);
    const ma_uint32 sample_rate = ma_engine_get_sample_rate(&engine_);

    ma_loshelf_node_config low = ma_loshelf_node_config_init(channels, sample_rate, 0.0, 0.9, 200.0);
    if (ma_loshelf_node_init(graph, &low, nullptr, &eq_low_) != MA_SUCCESS) {
        return;
    }
    ma_peak_node_config mid = ma_peak_node_config_init(channels, sample_rate, 0.0, 0.9, 1000.0);
    if (ma_peak_node_init(graph, &mid, nullptr, &eq_mid_) != MA_SUCCESS) {
        ma_loshelf_node_uninit(&eq_low_, nullptr);
        return;
    }
    ma_hishelf_node_config high =
        ma_hishelf_node_config_init(channels, sample_rate, 0.0, 0.9, 4000.0);
    if (ma_hishelf_node_init(graph, &high, nullptr, &eq_high_) != MA_SUCCESS) {
        ma_peak_node_uninit(&eq_mid_, nullptr);
        ma_loshelf_node_uninit(&eq_low_, nullptr);
        return;
    }

    // НЧ -> СЧ -> ВЧ -> выход движка. Вход подключается позже, когда появится звук.
    ma_node_attach_output_bus(reinterpret_cast<ma_node*>(&eq_low_), 0,
                              reinterpret_cast<ma_node*>(&eq_mid_), 0);
    ma_node_attach_output_bus(reinterpret_cast<ma_node*>(&eq_mid_), 0,
                              reinterpret_cast<ma_node*>(&eq_high_), 0);
    ma_node_attach_output_bus(reinterpret_cast<ma_node*>(&eq_high_), 0,
                              ma_engine_get_endpoint(&engine_), 0);
    eq_nodes_ready_ = true;
}

void PlayerCore::destroy_eq_nodes() {
    if (!eq_nodes_ready_) {
        return;
    }
    // Отключаем звук от цепочки, иначе он останется ссылаться на удаляемые узлы.
    if (sound_ != nullptr) {
        ma_node_attach_output_bus(reinterpret_cast<ma_node*>(sound_), 0,
                                  ma_engine_get_endpoint(&engine_), 0);
    }
    ma_hishelf_node_uninit(&eq_high_, nullptr);
    ma_peak_node_uninit(&eq_mid_, nullptr);
    ma_loshelf_node_uninit(&eq_low_, nullptr);
    eq_nodes_ready_ = false;
}

// Подключить текущий звук ко входу цепочки. Вызывается после каждой загрузки:
// ma_sound создаётся заново на каждый файл и по умолчанию идёт прямо на выход.
void PlayerCore::wire_eq_input() {
    if (!eq_nodes_ready_ || sound_ == nullptr) {
        return;
    }
    ma_node_attach_output_bus(reinterpret_cast<ma_node*>(sound_), 0,
                              reinterpret_cast<ma_node*>(&eq_low_), 0);
}

void PlayerCore::apply_eq() {
    if (!eq_nodes_ready_) {
        return;
    }
    const ma_uint32 channels = ma_engine_get_channels(&engine_);
    const ma_uint32 sample_rate = ma_engine_get_sample_rate(&engine_);
    const double low = eq_enabled_ ? eq_gain_[0] : 0.0;
    const double mid = eq_enabled_ ? eq_gain_[1] : 0.0;
    const double high = eq_enabled_ ? eq_gain_[2] : 0.0;

    ma_loshelf_node_config low_config =
        ma_loshelf_node_config_init(channels, sample_rate, low, 0.9, 200.0);
    ma_loshelf_node_reinit(&low_config.loshelf, &eq_low_);
    ma_peak_node_config mid_config =
        ma_peak_node_config_init(channels, sample_rate, mid, 0.9, 1000.0);
    ma_peak_node_reinit(&mid_config.peak, &eq_mid_);
    ma_hishelf_node_config high_config =
        ma_hishelf_node_config_init(channels, sample_rate, high, 0.9, 4000.0);
    ma_hishelf_node_reinit(&high_config.hishelf, &eq_high_);
}

void PlayerCore::set_eq_enabled(bool on) {
    eq_enabled_ = on;
    apply_eq();
}

void PlayerCore::toggle_eq() {
    set_eq_enabled(!eq_enabled_);
}

void PlayerCore::set_eq_gain(Band band, double gain_db) {
    const double clamped = std::max(-kMaxGainDb, std::min(kMaxGainDb, gain_db));
    eq_gain_[static_cast<int>(band)] = clamped;
    apply_eq();
}

void PlayerCore::change_eq_gain(Band band, double delta_db) {
    set_eq_gain(band, eq_gain(band) + delta_db);
}

std::string PlayerCore::eq_label() const {
    auto signed_db = [](double value) {
        const int rounded = static_cast<int>(std::lround(value * 10.0));
        const double scaled = rounded / 10.0;
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%+.1f", scaled);
        return std::string(buffer);
    };
    if (!eq_nodes_ready_) {
        return "Эквалайзер недоступен";
    }
    if (!eq_enabled_) {
        return "Эквалайзер: выкл";
    }
    return "Эквалайзер: НЧ " + signed_db(eq_gain_[0]) + " · СЧ " + signed_db(eq_gain_[1]) +
           " · ВЧ " + signed_db(eq_gain_[2]) + " дБ";
}

// Старт всегда с плавного входа: убирает щелчок в начале трека.
void PlayerCore::fade_in() {
    if (sound_ == nullptr) {
        return;
    }
    ma_sound_reset_stop_time_and_fade(sound_);
    ma_sound_set_fade_in_milliseconds(sound_, 0.0f, 1.0f, static_cast<ma_uint64>(kFadeMs));
}

// Затухание планируется в звуковом потоке: интерфейс не ждёт окончания.
void PlayerCore::fade_out() {
    if (sound_ == nullptr) {
        return;
    }
    ma_sound_stop_with_fade_in_milliseconds(sound_, static_cast<ma_uint64>(kFadeMs));
}

PlayerCore::~PlayerCore() {
    close();
}

void PlayerCore::release_sound() {
    if (sound_ == nullptr) {
        return;
    }
    ma_sound_stop(sound_);
    ma_sound_uninit(sound_);
    delete sound_;
    sound_ = nullptr;
}

void PlayerCore::unload() {
    release_sound();
    path_.clear();
    duration_ = 0.0;
    state_ = State::Stopped;
    anchor_pos_ = 0.0;
}

void PlayerCore::open(const std::string& utf8_path) {
    std::error_code ec;
    const std::filesystem::path given = app::path_from_utf8(utf8_path);
    if (!std::filesystem::is_regular_file(given, ec)) {
        throw AudioLoadError("Файл не найден или это не файл:\n" + utf8_path);
    }
    const std::filesystem::path absolute = std::filesystem::absolute(given, ec);
    const std::string path = ec ? utf8_path : app::path_to_utf8(absolute);

    // Загружаем в отдельный звук: если файл битый, текущий трек остаётся целым.
    auto* fresh = new ma_sound{};
    const ma_result result = ma_sound_init_from_file_w(
        &engine_, app::utf8_to_wide(path).c_str(), MA_SOUND_FLAG_STREAM, nullptr, nullptr, fresh);
    if (result != MA_SUCCESS) {
        delete fresh;
        throw AudioLoadError(describe_load_failure(path, result));
    }

    release_sound();
    sound_ = fresh;
    ma_sound_set_volume(sound_, static_cast<float>(volume_));
    apply_eq();                                  // коэффициенты могли измениться без звука
    wire_eq_input();                             // звук идёт через эквалайзер

    path_ = path;
    duration_ = media::probe_duration(path);
    state_ = State::Stopped;
    anchor_pos_ = 0.0;
    stamp();
}

double PlayerCore::position() const {
    double pos = anchor_pos_;
    if (state_ == State::Playing) {
        pos += now() - anchor_time_;
    }
    if (duration_ > 0.0) {
        pos = std::min(pos, duration_);
    }
    return std::max(0.0, pos);
}

double PlayerCore::clamp_position(double seconds) const {
    double pos = std::max(0.0, seconds);
    if (duration_ > 0.0) {
        pos = std::min(pos, duration_);
    }
    return pos;
}

void PlayerCore::play(std::optional<double> start) {
    if (sound_ == nullptr || !has_track()) {
        return;
    }
    if (state_ == State::Paused && !start.has_value()) {
        fade_in();
        ma_sound_start(sound_);                  // продолжение с курсора
        stamp();
        state_ = State::Playing;
        return;
    }

    double pos = start.has_value() ? clamp_position(*start) : anchor_pos_;
    if (duration_ > 0.0 && pos >= duration_ - 0.05) {
        pos = 0.0;                               // дошли до конца — начинаем сначала
    }
    // Порядок важен: ma_sound_seek_to_second() только откладывает перемотку,
    // её применяет звуковой поток. ma_sound_start() при этом сам сбрасывает
    // флаг «трек доигран» и уходит в начало, после чего применится наша цель.
    ma_sound_seek_to_second(sound_, static_cast<float>(pos));
    fade_in();
    ma_sound_start(sound_);
    anchor_pos_ = pos;
    stamp();
    state_ = State::Playing;
}

void PlayerCore::pause() {
    if (state_ != State::Playing || sound_ == nullptr) {
        return;
    }
    anchor_pos_ = position();
    ma_sound_stop(sound_);
    stamp();
    state_ = State::Paused;
}

PlayerCore::State PlayerCore::toggle() {
    if (state_ == State::Playing) {
        pause();
    } else {
        play();
    }
    return state_;
}

void PlayerCore::stop() {
    if (sound_ == nullptr || !has_track()) {
        return;
    }
    if (state_ == State::Playing) {
        // Плавное затухание вместо обрыва. Остановку завершает звуковой поток,
        // поэтому позицию не сбрасываем в звуке — только в своём состоянии:
        // следующий play() перемотает на начало сам.
        fade_out();
    } else {
        ma_sound_stop(sound_);
        ma_sound_seek_to_second(sound_, 0.0f);
    }
    state_ = State::Stopped;
    anchor_pos_ = 0.0;
    stamp();
}

void PlayerCore::seek(double seconds) {
    if (!seekable()) {
        return;
    }
    const double pos = clamp_position(seconds);
    if (state_ == State::Stopped) {
        anchor_pos_ = pos;                       // стартуем с этой точки по play()
        stamp();
        return;
    }
    // На паузе звук остаётся на паузе: перемотка применится до возобновления.
    if (sound_ != nullptr) {
        ma_sound_seek_to_second(sound_, static_cast<float>(pos));
    }
    anchor_pos_ = pos;
    stamp();
}

void PlayerCore::seek_by(double delta) {
    seek(position() + delta);
}

bool PlayerCore::poll() {
    if (state_ != State::Playing) {
        return false;
    }
    if (sound_ == nullptr || !ma_sound_at_end(sound_)) {
        return false;
    }
    if (now() - anchor_time_ < 0.5) {
        return false;                            // только что стартовали — не конец
    }
    anchor_pos_ = duration_ > 0.0 ? duration_ : anchor_pos_;
    stamp();
    state_ = State::Stopped;
    return true;
}

void PlayerCore::set_volume(double value) {
    volume_ = clamp01(value);
    if (sound_ != nullptr) {
        ma_sound_set_volume(sound_, static_cast<float>(volume_));
    }
}

double PlayerCore::change_volume(double delta) {
    set_volume(volume_ + delta);
    return volume_;
}

void PlayerCore::close() {
    unload();
    if (engine_ready_) {
        destroy_eq_nodes();          // узлы принадлежат графу движка — снимаем до его остановки
        ma_engine_uninit(&engine_);
        engine_ready_ = false;
    }
    if (context_ready_) {
        ma_context_uninit(&context_);
        context_ready_ = false;
    }
}
