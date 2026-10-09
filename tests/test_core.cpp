// Проверка ядра плеера (PlayerCore) без звука и без окна.
//
//     tests\test_core.exe
//
// Создаёт временный WAV на 2 секунды и гоняет по нему play/pause/seek/stop.
// Звук идёт в «пустое» устройство вывода (Device::Null) — слышно ничего не будет,
// и звуковая карта не нужна. Фреймворк не нужен: обычная программа с assert'ами.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "../src/common.h"
#include "../src/media_probe.h"
#include "../src/player_core.h"
#include "../src/tags.h"

namespace {

int g_failed = 0;

void check(const std::string& name, bool condition, const std::string& extra = std::string()) {
    std::printf("  %s  %s%s\n", condition ? "OK  " : "FAIL", name.c_str(),
                extra.empty() ? "" : (" — " + extra).c_str());
    if (!condition) {
        ++g_failed;
    }
}

void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

// Синусоида заданной длительности — обычный моно WAV 16 бит.
bool write_wav(const std::filesystem::path& path, double seconds, double freq = 440.0,
               int rate = 44100) {
    const double pi = 3.14159265358979323846;
    const int frames = static_cast<int>(seconds * rate);
    std::vector<short> samples(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        samples[static_cast<size_t>(i)] =
            static_cast<short>(12000.0 * std::sin(2.0 * pi * freq * i / rate));
    }
    const unsigned int data_size = static_cast<unsigned int>(samples.size() * sizeof(short));

    FILE* file = _wfopen(path.wstring().c_str(), L"wb");
    if (file == nullptr) {
        return false;
    }
    unsigned char header[44] = {0};
    const unsigned int byte_rate = static_cast<unsigned int>(rate * 2);
    std::memcpy(header, "RIFF", 4);
    const unsigned int riff_size = 36 + data_size;
    std::memcpy(header + 4, &riff_size, 4);
    std::memcpy(header + 8, "WAVEfmt ", 8);
    const unsigned int fmt_size = 16;
    std::memcpy(header + 16, &fmt_size, 4);
    const unsigned short audio_format = 1;
    const unsigned short channels = 1;
    const unsigned short bits = 16;
    const unsigned int sample_rate = static_cast<unsigned int>(rate);
    const unsigned short block_align = 2;
    std::memcpy(header + 20, &audio_format, 2);
    std::memcpy(header + 22, &channels, 2);
    std::memcpy(header + 24, &sample_rate, 4);
    std::memcpy(header + 28, &byte_rate, 4);
    std::memcpy(header + 32, &block_align, 2);
    std::memcpy(header + 34, &bits, 2);
    std::memcpy(header + 36, "data", 4);
    std::memcpy(header + 40, &data_size, 4);
    std::fwrite(header, 1, sizeof(header), file);
    std::fwrite(samples.data(), sizeof(short), samples.size(), file);
    std::fclose(file);
    return true;
}

bool write_bytes(const std::filesystem::path& path, const std::vector<unsigned char>& data) {
    FILE* file = _wfopen(path.wstring().c_str(), L"wb");
    if (file == nullptr) {
        return false;
    }
    if (!data.empty()) {
        std::fwrite(data.data(), 1, data.size(), file);
    }
    std::fclose(file);
    return true;
}

std::filesystem::path project_dir() {
    // Тесты собираются в tests\, а test-media лежит рядом с src\.
    std::filesystem::path dir = std::filesystem::path(__FILE__).parent_path().parent_path();
    return dir;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // чтобы падение было видно сразу
    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() /
        ("audioplayer-test-" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::create_directories(tmp, ec);

    const std::filesystem::path wav = tmp / "tone.wav";
    write_wav(wav, 2.0);
    const std::string wav_path = app::path_to_utf8(wav);

    std::printf("PlayerCore: состояние после инициализации (звук — в «пустое» устройство)\n");
    PlayerCore core(app::kDefaultVolume, PlayerCore::Device::Null);
    check("нет трека", !core.has_track());
    check("позиция 0", core.position() == 0.0);

    std::printf("Загрузка файла\n");
    core.open(wav_path);
    check("трек загружен", core.has_track());
    check("длительность ~2 c", std::abs(core.duration() - 2.0) < 0.05,
          std::to_string(core.duration()));
    check("перемотка доступна", core.seekable());
    check("состояние stopped", core.state() == PlayerCore::State::Stopped);

    std::printf("format_time()\n");
    check("0 -> 0:00", app::format_time(0) == "0:00", app::format_time(0));
    check("65 -> 1:05", app::format_time(65) == "1:05", app::format_time(65));
    check("3725 -> 1:02:05", app::format_time(3725) == "1:02:05", app::format_time(3725));
    check("отрицательное -> --:--", app::format_time(-1) == "--:--", app::format_time(-1));

    std::printf("Теги (tags::read)\n");
    const tags::Info plain = tags::read(wav_path);
    check("без тегов — имя файла", plain.title == "tone" && plain.artist.empty(),
          plain.title + "/" + plain.artist);
    const std::filesystem::path media_dir = project_dir() / "test-media";
    const std::filesystem::path media =
        std::filesystem::is_directory(media_dir, ec) ? media_dir : std::filesystem::path("test-media");
    for (const char* name : {"01-test-tone-440hz-10s.mp3", "02-test-sweep-80-12000hz-60s.mp3",
                             "03-ode-to-joy-synth-40s.mp3"}) {
        const std::filesystem::path file = media / name;
        if (!std::filesystem::is_regular_file(file, ec)) {
            continue;
        }
        const tags::Info info = tags::read(app::path_to_utf8(file));
        check(std::string("теги ") + name, !info.title.empty(),
              info.title + " / " + info.artist);
    }

    std::printf("Запасная оценка длительности MP3\n");
    struct Expect {
        const char* name;
        double seconds;
    } expectations[] = {{"01-test-tone-440hz-10s.mp3", 10.0},
                        {"02-test-sweep-80-12000hz-60s.mp3", 60.0},
                        {"03-ode-to-joy-synth-40s.mp3", 39.4}};
    for (const Expect& item : expectations) {
        const std::filesystem::path file = media / item.name;
        if (!std::filesystem::is_regular_file(file, ec)) {
            continue;
        }
        const double estimate = media::estimate_mp3_duration(app::path_to_utf8(file));
        const double tolerance = std::max(1.0, item.seconds * 0.1);
        char extra[64];
        std::snprintf(extra, sizeof(extra), "%.1f при %.1f", estimate, item.seconds);
        check(std::string("оценка ") + std::string(item.name).substr(0, 12) + "…",
              std::abs(estimate - item.seconds) < tolerance, extra);
    }

    std::printf("Воспроизведение\n");
    core.play();
    check("состояние playing", core.state() == PlayerCore::State::Playing);
    sleep_ms(400);
    double pos = core.position();
    check("позиция растёт", pos > 0.2, std::to_string(pos));

    std::printf("Пауза\n");
    core.pause();
    check("состояние paused", core.state() == PlayerCore::State::Paused);
    const double frozen = core.position();
    sleep_ms(300);
    check("позиция не меняется", std::abs(core.position() - frozen) < 0.02,
          std::to_string(frozen) + " -> " + std::to_string(core.position()));

    std::printf("Снятие с паузы (toggle)\n");
    core.toggle();
    check("снова playing", core.state() == PlayerCore::State::Playing);
    sleep_ms(200);
    check("позиция продолжила расти", core.position() > frozen + 0.1,
          std::to_string(frozen) + " -> " + std::to_string(core.position()));

    std::printf("Перемотка во время игры\n");
    core.seek(1.5);
    check("позиция ~1.5", std::abs(core.position() - 1.5) < 0.15, std::to_string(core.position()));
    core.seek(99);
    check("не выходит за длительность", core.position() <= core.duration() + 0.01,
          std::to_string(core.position()));
    core.seek(-5);
    check("не уходит в минус", core.position() >= 0.0, std::to_string(core.position()));

    std::printf("Перемотка на паузе\n");
    core.pause();
    core.seek(0.8);
    check("остались на паузе", core.state() == PlayerCore::State::Paused);
    check("позиция 0.8", std::abs(core.position() - 0.8) < 0.15, std::to_string(core.position()));

    std::printf("Стоп\n");
    core.stop();
    check("состояние stopped", core.state() == PlayerCore::State::Stopped);
    check("позиция 0", core.position() == 0.0);

    std::printf("Содержимое не совпадает с расширением (WAV внутри .mp3)\n");
    const std::filesystem::path disguised = tmp / "disguised.mp3";
    std::filesystem::copy_file(wav, disguised, std::filesystem::copy_options::overwrite_existing, ec);
    const std::string disguised_path = app::path_to_utf8(disguised);
    core.open(disguised_path);
    check("открылся вопреки расширению", core.has_track() && core.path() == disguised_path);
    check("длительность найдена", std::abs(core.duration() - 2.0) < 0.05,
          std::to_string(core.duration()));
    core.play();
    sleep_ms(300);
    check("играет", core.state() == PlayerCore::State::Playing && core.position() > 0.1,
          std::to_string(core.position()));
    core.seek(1.0);
    check("перемотка работает", std::abs(core.position() - 1.0) < 0.2,
          std::to_string(core.position()));
    core.stop();

    std::printf("Понятные ошибки вместо «Out of memory»\n");
    const std::filesystem::path garbage = tmp / "garbage.mp3";
    write_bytes(garbage, std::vector<unsigned char>(300, 0x01));
    try {
        core.open(app::path_to_utf8(garbage));
        check("мусор отклонён", false);
    } catch (const AudioLoadError& error) {
        check("мусор -> AudioLoadError", true, error.what());
    }

    const std::filesystem::path fake_m4a = tmp / "fake_m4a.mp3";
    std::vector<unsigned char> m4a;
    m4a.reserve(76);
    const unsigned char mp4_header[] = {0x00, 0x00, 0x00, 0x18, 'f', 't', 'y', 'p', 'M', '4', 'A', ' '};
    m4a.insert(m4a.end(), std::begin(mp4_header), std::end(mp4_header));
    m4a.insert(m4a.end(), 64, 0x00);
    write_bytes(fake_m4a, m4a);
    try {
        core.open(app::path_to_utf8(fake_m4a));
        check("m4a отклонён", false);
    } catch (const AudioLoadError& error) {
        check("m4a внутри .mp3 -> подсказка про M4A/AAC",
              std::string(error.what()).find("M4A/AAC") != std::string::npos, error.what());
    }

    const std::filesystem::path empty = tmp / "empty.mp3";
    write_bytes(empty, {});
    try {
        core.open(app::path_to_utf8(empty));
        check("пустой отклонён", false);
    } catch (const AudioLoadError& error) {
        check("пустой файл -> AudioLoadError", true, error.what());
    }
    check("плеер жив после ошибок", core.has_track() && core.path() == disguised_path,
          core.path());

    std::printf("Громкость\n");
    core.set_volume(0.42);
    check("значение применилось", std::abs(core.volume() - 0.42) < 1e-9, std::to_string(core.volume()));
    core.change_volume(5);
    check("зажато до 1.0", core.volume() == 1.0, std::to_string(core.volume()));
    core.change_volume(-50);
    check("зажато до 0.0", core.volume() == 0.0, std::to_string(core.volume()));

    std::printf("Конец трека определяется сам (poll)\n");
    core.open(wav_path);
    core.set_volume(0.8);
    core.play();
    bool ended = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < deadline) {
        if (core.poll()) {
            ended = true;
            break;
        }
        sleep_ms(50);
    }
    check("poll() вернул True", ended);
    check("состояние stopped", core.state() == PlayerCore::State::Stopped);

    core.close();
    std::filesystem::remove_all(tmp, ec);

    if (g_failed != 0) {
        std::printf("\nПРОВАЛ: %d проверок не прошло\n", g_failed);
        return 1;
    }
    std::printf("\nВсе проверки пройдены\n");
    return 0;
}
