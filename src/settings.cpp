#include "settings.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "common.h"

namespace settings {
namespace {

constexpr int kVersion = 1;
constexpr double kMaxGainDb = 12.0;
constexpr const wchar_t* kFolder = L"AudioPlayerCpp";
constexpr const wchar_t* kFileName = L"settings.ini";

std::string trim(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return std::string();
    }
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

double clamp(double value, double low, double high) {
    if (!(value == value)) {                 // NaN из битого файла
        return low;
    }
    return std::max(low, std::min(high, value));
}

// Числа разбираем мягко: мусор в строке не должен ломать остальные настройки.
double to_double(const std::string& text, double fallback) {
    const std::string value = trim(text);
    if (value.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || (end != nullptr && *end != '\0')) {
        return fallback;
    }
    return parsed;
}

int to_int(const std::string& text, int fallback) {
    const std::string value = trim(text);
    if (value.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || (end != nullptr && *end != '\0')) {
        return fallback;
    }
    return static_cast<int>(parsed);
}

std::string number(double value, int digits) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.*f", digits, value);
    return std::string(buffer);
}

}  // namespace

std::string default_path() {
    wchar_t appdata[MAX_PATH] = {0};
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return std::string();               // настроек не будет, плеер просто не помнит
    }
    std::filesystem::path folder(appdata);
    folder /= kFolder;
    folder /= kFileName;
    return app::path_to_utf8(folder);
}

std::string serialise(const State& state) {
    std::string text;
    text += "# Аудиоплеер: состояние между запусками. Файл можно удалить —\n";
    text += "# плеер просто начнёт с настроек по умолчанию.\n";
    text += "version=" + std::to_string(kVersion) + "\n";
    text += "volume=" + number(clamp(state.volume, 0.0, 1.0), 2) + "\n";
    text += std::string("shuffle=") + (state.shuffle ? "1" : "0") + "\n";
    text += "repeat=" + std::to_string(std::max(0, std::min(2, state.repeat))) + "\n";
    text += std::string("eq=") + (state.eq_enabled ? "1" : "0") + "\n";
    text += "eq_low=" + number(clamp(state.eq_gain[0], -kMaxGainDb, kMaxGainDb), 1) + "\n";
    text += "eq_mid=" + number(clamp(state.eq_gain[1], -kMaxGainDb, kMaxGainDb), 1) + "\n";
    text += "eq_high=" + number(clamp(state.eq_gain[2], -kMaxGainDb, kMaxGainDb), 1) + "\n";
    if (!state.track.empty()) {
        text += "current=" + state.track + "\n";
        text += "position=" + number(std::max(0.0, state.position), 2) + "\n";
    }
    text += "index=" + std::to_string(state.playlist_index) + "\n";
    for (const std::string& track : state.playlist) {
        if (!track.empty()) {
            text += "track=" + track + "\n";
        }
    }
    return text;
}

bool parse(const std::string& text, State& state) {
    State parsed;                            // разбираем в стороне: битый файл не портит текущее
    parsed.playlist.clear();
    bool any_known = false;

    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string line = trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const size_t separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        const std::string key = trim(line.substr(0, separator));
        const std::string value = trim(line.substr(separator + 1));
        if (key.empty()) {
            continue;
        }

        if (key == "version") {
            any_known = true;
        } else if (key == "volume") {
            parsed.volume = clamp(to_double(value, parsed.volume), 0.0, 1.0);
            any_known = true;
        } else if (key == "shuffle") {
            parsed.shuffle = to_int(value, 0) != 0;
            any_known = true;
        } else if (key == "repeat") {
            parsed.repeat = std::max(0, std::min(2, to_int(value, 0)));
            any_known = true;
        } else if (key == "eq") {
            parsed.eq_enabled = to_int(value, 0) != 0;
            any_known = true;
        } else if (key == "eq_low") {
            parsed.eq_gain[0] = clamp(to_double(value, 0.0), -kMaxGainDb, kMaxGainDb);
            any_known = true;
        } else if (key == "eq_mid") {
            parsed.eq_gain[1] = clamp(to_double(value, 0.0), -kMaxGainDb, kMaxGainDb);
            any_known = true;
        } else if (key == "eq_high") {
            parsed.eq_gain[2] = clamp(to_double(value, 0.0), -kMaxGainDb, kMaxGainDb);
            any_known = true;
        } else if (key == "current") {
            parsed.track = value;
            any_known = true;
        } else if (key == "position") {
            parsed.position = std::max(0.0, to_double(value, 0.0));
            any_known = true;
        } else if (key == "index") {
            parsed.playlist_index = to_int(value, -1);
            any_known = true;
        } else if (key == "track") {
            if (!value.empty()) {
                parsed.playlist.push_back(value);
            }
            any_known = true;
        }
    }

    if (!any_known) {
        return false;
    }
    state = parsed;
    return true;
}

bool load(const std::string& utf8_file, State& state) {
    if (utf8_file.empty()) {
        return false;
    }
    std::ifstream input(app::path_from_utf8(utf8_file), std::ios::binary);
    if (!input) {
        return false;                        // первого запуска файла ещё нет
    }
    const std::string text((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    input.close();
    return parse(text, state);
}

bool save(const std::string& utf8_file, const State& state) {
    if (utf8_file.empty()) {
        return false;
    }
    const std::filesystem::path path = app::path_from_utf8(utf8_file);
    std::error_code ec;
    const std::filesystem::path folder = path.parent_path();
    if (!folder.empty()) {
        std::filesystem::create_directories(folder, ec);   // ec: папка может уже быть
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output << serialise(state);
    output.flush();
    return output.good();
}

}  // namespace settings
