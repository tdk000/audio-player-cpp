// Общие константы и мелкие утилиты.
//
// Строки внутри программы — UTF-8 (std::string), вызовы Win32 API — UTF-16
// (std::wstring): конвертация только на границе с системой.
#pragma once

#include <filesystem>
#include <string>

namespace app {

inline constexpr double kSeekStep = 5.0;      // секунд, шаг перемотки стрелками
inline constexpr double kVolumeStep = 0.05;   // 5 % шага громкости
inline constexpr double kEqStepDb = 1.5;      // шаг полосы эквалайзера, дБ
inline constexpr int kTickMs = 200;           // период обновления интерфейса, мс
inline constexpr double kDefaultVolume = 0.8;
inline constexpr const char* kVersion = "2.1.0";

// Пути, которые принимает плеер.
inline constexpr const char* kAudioExtensions[] = {".mp3", ".wav", ".ogg", ".oga", ".flac"};

std::wstring utf8_to_wide(const std::string& text);
std::string wide_to_utf8(const std::wstring& text);

std::string path_to_utf8(const std::filesystem::path& path);
std::filesystem::path path_from_utf8(const std::string& text);

// 125.4 -> "2:05"; 3725 -> "1:02:05"; отрицательное/NaN -> "--:--".
std::string format_time(double seconds);

}  // namespace app
