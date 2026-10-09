// Определение формата по содержимому и длительности трека.
//
// В Python-версии этим занимался mutagen; здесь длительность спрашиваем
// у miniaudio (декодер по фактическому содержимому), а для OGG, где
// stb_vorbis не отдаёт длину, считаем её по granule-позиции последней страницы.
#pragma once

#include <string>

namespace media {

enum class Format { Unknown, Wav, Flac, Ogg, Mp3, Aac, M4a, Wma };

// Фактический формат по магическим байтам — расширению не верим.
Format sniff_format(const std::string& utf8_path);

// Человекочитаемое имя формата: "MP3", "M4A/AAC" и т.п.
const char* format_name(Format format);

// Грубая длительность MP3 без декодера: размер файла * 8 / битрейт.
double estimate_mp3_duration(const std::string& utf8_path);

// Точная длительность в секундах; 0.0 — определить не удалось.
double probe_duration(const std::string& utf8_path);

}  // namespace media
