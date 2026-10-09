#include "player_core.h"

#include <algorithm>
#include <chrono>
#include <cmath>
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
    stamp();
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
    ma_sound_stop(sound_);
    ma_sound_seek_to_second(sound_, 0.0f);
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
        ma_engine_uninit(&engine_);
        engine_ready_ = false;
    }
    if (context_ready_) {
        ma_context_uninit(&context_);
        context_ready_ = false;
    }
}
