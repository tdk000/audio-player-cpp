// Чтение метаданных из аудиофайлов без внешних библиотек.
//
// Расширению файла не верим: формат определяется по фактическому содержимому.
// Поддерживаются ID3v2.2/2.3/2.4, ID3v1, Vorbis Comment (FLAC, OGG),
// RIFF LIST/INFO и чанк "id3 " (WAV).
#pragma once

#include <string>

namespace tags {

struct Info {
    std::string title;    // UTF-8; если тега нет — имя файла без расширения
    std::string artist;   // UTF-8; пустая строка, если тега нет
};

// Читает метаданные по ФАКТИЧЕСКОМУ содержимому файла (расширению не верим).
// Поддерживает: ID3v2.2/2.3/2.4 (TT2/TIT2, TP1/TPE1), ID3v1, Vorbis Comment
// (FLAC, OGG), RIFF LIST/INFO и чанк "id3 " (WAV).
// Никогда не бросает исключений: при любой ошибке возвращает {имя файла без расширения, ""}.
// utf8_path — путь в UTF-8 (может содержать кириллицу).
Info read(const std::string& utf8_path);

// Декодирование текста в UTF-8 — нужно тестам.
std::string latin1_to_utf8(const std::string& in);
std::string utf16_to_utf8(const std::string& in, bool big_endian);

}  // namespace tags
