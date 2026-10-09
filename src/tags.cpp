// Реализация чтения метаданных без внешних библиотек (аналог mutagen).
//
// Файл открывается в бинарном режиме по UTF-8 пути (через app::utf8_to_wide),
// читаются только нужные куски: начало (ID3v2 / fLaC / RIFF / поиск OGG)
// и последние 128 байт (ID3v1).
#include "tags.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common.h"

namespace tags {
namespace {

constexpr size_t kHeadBytes = 64 * 1024;   // сколько читаем с начала файла
constexpr size_t kId3v1Size = 128;         // фиксированный размер ID3v1

// ---------------------------------------------------------------------------
// Файл: открытие по UTF-8 пути, чтение кусков, определение размера.
// ---------------------------------------------------------------------------
class File {
public:
    explicit File(const std::string& utf8_path) {
        std::wstring wide = app::utf8_to_wide(utf8_path);
        fp_ = _wfopen(wide.c_str(), L"rb");
    }

    ~File() {
        if (fp_) std::fclose(fp_);
    }

    File(const File&) = delete;
    File& operator=(const File&) = delete;

    bool ok() const { return fp_ != nullptr; }

    // Полный размер файла в байтах или 0 при неудаче.
    long long size() {
        if (!fp_) return 0;
        if (_fseeki64(fp_, 0, SEEK_END) != 0) return 0;
        long long end = _ftelli64(fp_);
        if (end < 0) return 0;
        return end;
    }

    void seek(long long offset) {
        if (!fp_) return;
        _fseeki64(fp_, offset, SEEK_SET);
    }

    // Читает до `count` байт; возвращает сколько реально прочитано.
    size_t read(void* buffer, size_t count) {
        if (!fp_ || count == 0) return 0;
        return std::fread(buffer, 1, count, fp_);
    }

    // Читает `count` байт с позиции `offset`; короткий результат допустим.
    size_t read_at(long long offset, void* buffer, size_t count) {
        seek(offset);
        return read(buffer, count);
    }

private:
    std::FILE* fp_ = nullptr;
};

bool starts_with(const std::string& data, size_t pos, const char* needle, size_t len) {
    if (pos > data.size() || data.size() - pos < len) return false;
    return std::memcmp(data.data() + pos, needle, len) == 0;
}

bool starts_with(const std::string& data, size_t pos, const char* needle) {
    return starts_with(data, pos, needle, std::strlen(needle));
}

uint32_t read_be24(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) |
           static_cast<uint32_t>(p[2]);
}

uint32_t read_be32(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

// Synchsafe: по 7 значащих бит в байте (ID3v2.3/2.4 размеры).
uint32_t read_synchsafe32(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0] & 0x7F) << 21) |
           (static_cast<uint32_t>(p[1] & 0x7F) << 14) |
           (static_cast<uint32_t>(p[2] & 0x7F) << 7) |
           static_cast<uint32_t>(p[3] & 0x7F);
}

uint32_t read_le32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

// Оборвать строку по первому NUL и убрать пробелы по краям.
std::string trim_nul_and_spaces(const std::string& in) {
    size_t end = in.find('\0');
    std::string text = (end == std::string::npos) ? in : in.substr(0, end);
    size_t begin = 0;
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r' ||
            text[begin] == '\n')) {
        ++begin;
    }
    size_t last = text.size();
    while (last > begin &&
           (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r' ||
            text[last - 1] == '\n')) {
        --last;
    }
    return text.substr(begin, last - begin);
}

// Первое непустое значение из списка, разделённого NUL (многозначные теги v2.4).
std::string first_nonempty_value(const std::string& text) {
    size_t start = 0;
    while (start <= text.size()) {
        size_t sep = text.find('\0', start);
        std::string part =
            (sep == std::string::npos) ? text.substr(start) : text.substr(start, sep - start);
        std::string trimmed = trim_nul_and_spaces(part);
        if (!trimmed.empty()) return trimmed;
        if (sep == std::string::npos) break;
        start = sep + 1;
    }
    return std::string();
}

// ---------------------------------------------------------------------------
// ID3v2
// ---------------------------------------------------------------------------
struct Id3v2Tags {
    std::string title;
    std::string artist;
};

// Текст кадра ID3v2: [1 байт кодировки][данные].
std::string decode_id3_text(const std::string& body) {
    if (body.empty()) return std::string();
    const unsigned char encoding = static_cast<unsigned char>(body[0]);
    const std::string payload = body.substr(1);

    switch (encoding) {
        case 0:  // ISO-8859-1
            return trim_nul_and_spaces(latin1_to_utf8(payload));
        case 1: {  // UTF-16 с BOM
            if (payload.size() >= 2) {
                const unsigned char b0 = static_cast<unsigned char>(payload[0]);
                const unsigned char b1 = static_cast<unsigned char>(payload[1]);
                if (b0 == 0xFF && b1 == 0xFE) {
                    return trim_nul_and_spaces(utf16_to_utf8(payload.substr(2), false));
                }
                if (b0 == 0xFE && b1 == 0xFF) {
                    return trim_nul_and_spaces(utf16_to_utf8(payload.substr(2), true));
                }
            }
            // BOM отсутствует — считаем little-endian (как большинство тегов).
            return trim_nul_and_spaces(utf16_to_utf8(payload, false));
        }
        case 2:  // UTF-16BE без BOM
            return trim_nul_and_spaces(utf16_to_utf8(payload, true));
        case 3:  // UTF-8
            return trim_nul_and_spaces(payload);
        default:
            return trim_nul_and_spaces(latin1_to_utf8(payload));
    }
}

// Разбирает буфер, начинающийся с заголовка "ID3" (10 байт). Возвращает true,
// если тег найден; в out попадают найденные поля.
bool parse_id3v2(const std::string& data, Id3v2Tags& out) {
    if (data.size() < 10) return false;
    if (std::memcmp(data.data(), "ID3", 3) != 0) return false;

    const unsigned char major = static_cast<unsigned char>(data[3]);
    if (major < 2 || major > 4) return false;

    const unsigned char flags = static_cast<unsigned char>(data[5]);
    const unsigned char* size_bytes = reinterpret_cast<const unsigned char*>(data.data() + 6);

    // В v2.2 размер — обычные 3 байта; в 2.3/2.4 — synchsafe.
    uint32_t tag_size = (major == 2) ? read_be24(size_bytes) : read_synchsafe32(size_bytes);

    size_t pos = 10;

    // Расширенный заголовок (2.3/2.4): флаг 0x40.
    if ((major == 3 || major == 4) && (flags & 0x40)) {
        const size_t ext_len = (major == 4) ? 4 : 6;
        if (data.size() >= pos + ext_len) {
            const unsigned char* ext =
                reinterpret_cast<const unsigned char*>(data.data() + pos);
            uint32_t ext_size = (major == 4) ? read_synchsafe32(ext) : read_be32(ext);
            pos += ext_len + ext_size;
        } else {
            return false;
        }
    }

    // Границы тега внутри прочитанного буфера.
    size_t limit = pos + tag_size;
    if (limit > data.size()) limit = data.size();

    // Footer (флаг 0x10) входит в tag_size, поэтому limit уже учитывает его.

    bool title_done = false;
    bool artist_done = false;

    while (pos < limit) {
        if (major == 2) {
            if (pos + 6 > limit) break;
            const std::string id = data.substr(pos, 3);
            if (id[0] == '\0') break;  // padding
            const uint32_t frame_size = read_be24(reinterpret_cast<const unsigned char*>(
                data.data() + pos + 3));
            const size_t header = 6;
            size_t body_start = pos + header;
            size_t body_end = body_start + frame_size;
            if (body_end > limit) body_end = limit;

            if (id == "TT2" && !title_done) {
                out.title = first_nonempty_value(
                    decode_id3_text(data.substr(body_start, body_end - body_start)));
                title_done = true;
            } else if (id == "TP1" && !artist_done) {
                out.artist = first_nonempty_value(
                    decode_id3_text(data.substr(body_start, body_end - body_start)));
                artist_done = true;
            }
            if (frame_size == 0) break;
            pos = body_end;
        } else {
            if (pos + 10 > limit) break;
            const std::string id = data.substr(pos, 4);
            if (id[0] == '\0') break;  // padding
            const unsigned char* sz =
                reinterpret_cast<const unsigned char*>(data.data() + pos + 4);
            const uint32_t frame_size = (major == 4) ? read_synchsafe32(sz) : read_be32(sz);
            const size_t body_start = pos + 10;
            size_t body_end = body_start + frame_size;
            if (body_end > limit) body_end = limit;

            if (id == "TIT2" && !title_done) {
                out.title = first_nonempty_value(
                    decode_id3_text(data.substr(body_start, body_end - body_start)));
                title_done = true;
            } else if (id == "TPE1" && !artist_done) {
                out.artist = first_nonempty_value(
                    decode_id3_text(data.substr(body_start, body_end - body_start)));
                artist_done = true;
            }
            if (frame_size == 0) break;
            pos = body_end;
        }

        if (title_done && artist_done) break;
    }

    return title_done || artist_done;
}

// ---------------------------------------------------------------------------
// Vorbis Comment
// ---------------------------------------------------------------------------
void apply_vorbis_comment(const std::string& data, size_t pos, Id3v2Tags& out) {
    if (pos > data.size() || data.size() - pos < 4) return;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data.data() + pos);
    uint32_t vendor_len = read_le32(p);
    pos += 4;
    if (vendor_len > data.size() || data.size() - pos < vendor_len) return;
    pos += vendor_len;
    if (data.size() - pos < 4) return;

    p = reinterpret_cast<const unsigned char*>(data.data() + pos);
    uint32_t count = read_le32(p);
    pos += 4;

    for (uint32_t i = 0; i < count; ++i) {
        if (data.size() - pos < 4) return;
        p = reinterpret_cast<const unsigned char*>(data.data() + pos);
        uint32_t entry_len = read_le32(p);
        pos += 4;
        if (entry_len > data.size() || data.size() - pos < entry_len) return;

        std::string entry = data.substr(pos, entry_len);
        pos += entry_len;

        const size_t eq = entry.find('=');
        if (eq == std::string::npos) continue;
        std::string key = entry.substr(0, eq);
        for (char& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        std::string value = trim_nul_and_spaces(entry.substr(eq + 1));

        if (key == "TITLE" && out.title.empty()) {
            out.title = value;
        } else if (key == "ARTIST" && out.artist.empty()) {
            out.artist = value;
        }
    }
}

bool parse_flac(const std::string& data, Id3v2Tags& out) {
    if (!starts_with(data, 0, "fLaC")) return false;
    size_t pos = 4;
    while (pos + 4 <= data.size()) {
        const unsigned char header = static_cast<unsigned char>(data[pos]);
        const bool last = (header & 0x80) != 0;
        const unsigned char type = header & 0x7F;
        const uint32_t length =
            read_be24(reinterpret_cast<const unsigned char*>(data.data() + pos + 1));
        pos += 4;
        if (length > data.size() || data.size() - pos < length) return false;
        if (type == 4) {
            apply_vorbis_comment(data, pos, out);
            return true;
        }
        if (type == 127) break;  // невалидный тип
        pos += length;
        if (last) break;
    }
    return false;
}

bool parse_ogg(const std::string& data, Id3v2Tags& out) {
    static const char kSig[] = "\x03vorbis";
    const size_t sig_len = sizeof(kSig) - 1;  // 7
    if (data.size() < sig_len) return false;
    size_t found = std::string::npos;
    for (size_t i = 0; i + sig_len <= data.size(); ++i) {
        if (std::memcmp(data.data() + i, kSig, sig_len) == 0) {
            found = i;
            break;
        }
    }
    if (found == std::string::npos) return false;
    apply_vorbis_comment(data, found + sig_len, out);
    return true;
}

// ---------------------------------------------------------------------------
// WAV (RIFF)
// ---------------------------------------------------------------------------
void apply_riff_info(const std::string& data, size_t pos, size_t size, Id3v2Tags& out) {
    if (size < 4) return;
    const size_t end = (pos + size <= data.size()) ? pos + size : data.size();
    size_t cur = pos + 4;  // пропускаем тип "INFO"
    while (cur + 8 <= end) {
        const std::string id = data.substr(cur, 4);
        const uint32_t chunk_size =
            read_le32(reinterpret_cast<const unsigned char*>(data.data() + cur + 4));
        const size_t body = cur + 8;
        if (chunk_size > data.size() || body + chunk_size > data.size()) break;

        if (id == "INAM" && out.title.empty()) {
            out.title = trim_nul_and_spaces(
                latin1_to_utf8(data.substr(body, chunk_size)));
        } else if (id == "IART" && out.artist.empty()) {
            out.artist = trim_nul_and_spaces(
                latin1_to_utf8(data.substr(body, chunk_size)));
        }
        size_t advance = 8 + chunk_size + (chunk_size & 1u);  // выравнивание
        if (advance == 0) break;
        cur += advance;
    }
}

bool parse_riff(const std::string& data, Id3v2Tags& out) {
    if (data.size() < 12) return false;
    if (!starts_with(data, 0, "RIFF") || !starts_with(data, 8, "WAVE")) return false;

    size_t pos = 12;
    while (pos + 8 <= data.size()) {
        const std::string id = data.substr(pos, 4);
        const uint32_t chunk_size =
            read_le32(reinterpret_cast<const unsigned char*>(data.data() + pos + 4));
        const size_t body = pos + 8;
        if (chunk_size > data.size() || body + chunk_size > data.size()) {
            // Чанк выходит за пределы прочитанного куска: для "id3 " пробуем
            // разобрать хотя бы то, что есть.
            if (id == "id3 ") {
                Id3v2Tags sub;
                if (parse_id3v2(data.substr(body), sub)) {
                    if (out.title.empty()) out.title = sub.title;
                    if (out.artist.empty()) out.artist = sub.artist;
                }
            }
            break;
        }

        if (id == "LIST" && chunk_size >= 4 && data.compare(body, 4, "INFO") == 0) {
            apply_riff_info(data, body, chunk_size, out);
        } else if (id == "id3 ") {
            Id3v2Tags sub;
            if (parse_id3v2(data.substr(body, chunk_size), sub)) {
                if (out.title.empty() && !sub.title.empty()) out.title = sub.title;
                if (out.artist.empty() && !sub.artist.empty()) out.artist = sub.artist;
            }
        }

        size_t advance = 8 + chunk_size + (chunk_size & 1u);
        if (advance == 0) break;
        pos += advance;
    }
    return true;
}

// ---------------------------------------------------------------------------
// ID3v1
// ---------------------------------------------------------------------------
void parse_id3v1(const std::string& data, Id3v2Tags& out) {
    if (data.size() < kId3v1Size) return;
    const std::string tag = data.substr(data.size() - kId3v1Size);
    if (std::memcmp(tag.data(), "TAG", 3) != 0) return;
    if (out.title.empty()) {
        out.title = trim_nul_and_spaces(latin1_to_utf8(tag.substr(3, 30)));
    }
    if (out.artist.empty()) {
        out.artist = trim_nul_and_spaces(latin1_to_utf8(tag.substr(33, 30)));
    }
}

std::string basename_without_extension(const std::string& utf8_path) {
    std::string name = utf8_path;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
    return name;
}

}  // namespace

std::string latin1_to_utf8(const std::string& in) {
    std::string out;
    out.reserve(in.size() * 2);
    for (unsigned char c : in) {
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

std::string utf16_to_utf8(const std::string& in, bool big_endian) {
    std::string out;
    out.reserve(in.size() * 2);

    auto unit_at = [&](size_t i) -> uint32_t {
        const unsigned char b0 = static_cast<unsigned char>(in[i]);
        const unsigned char b1 = static_cast<unsigned char>(in[i + 1]);
        return big_endian ? ((static_cast<uint32_t>(b0) << 8) | b1)
                          : ((static_cast<uint32_t>(b1) << 8) | b0);
    };

    auto append_codepoint = [&](uint32_t cp) {
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    };

    size_t i = 0;
    while (i + 1 < in.size()) {
        uint32_t unit = unit_at(i);
        i += 2;
        if (unit == 0) break;  // конец строки
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < in.size()) {
            const uint32_t low = unit_at(i);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                i += 2;
                const uint32_t cp = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                append_codepoint(cp);
                continue;
            }
        }
        if (unit >= 0xD800 && unit <= 0xDFFF) continue;  // одиночный суррогат
        append_codepoint(unit);
    }
    return out;
}

Info read(const std::string& utf8_path) {
    Info info;
    const std::string fallback_title = basename_without_extension(utf8_path);
    info.title = fallback_title;

    try {
        File file(utf8_path);
        if (!file.ok()) {
            return info;
        }

        // Читаем начало файла (до kHeadBytes) — этого достаточно для ID3v2,
        // FLAC, WAV и поиска сигнатуры OGG.
        std::vector<char> head_buffer(kHeadBytes);
        const size_t head_len = file.read(head_buffer.data(), head_buffer.size());
        const std::string head(head_buffer.data(), head_len);

        Id3v2Tags parsed;

        // ID3v2 (наибольший приоритет). Тег начинается с "ID3".
        if (starts_with(head, 0, "ID3")) {
            parse_id3v2(head, parsed);
        }

        if (parsed.title.empty() || parsed.artist.empty()) {
            Id3v2Tags vorbis;
            if (starts_with(head, 0, "fLaC")) {
                parse_flac(head, vorbis);
            } else if (starts_with(head, 0, "OggS")) {
                parse_ogg(head, vorbis);
            }
            if (parsed.title.empty() && !vorbis.title.empty()) parsed.title = vorbis.title;
            if (parsed.artist.empty() && !vorbis.artist.empty()) parsed.artist = vorbis.artist;
        }

        if (parsed.title.empty() || parsed.artist.empty()) {
            Id3v2Tags riff;
            if (starts_with(head, 0, "RIFF")) {
                parse_riff(head, riff);
            }
            if (parsed.title.empty() && !riff.title.empty()) parsed.title = riff.title;
            if (parsed.artist.empty() && !riff.artist.empty()) parsed.artist = riff.artist;
        }

        // ID3v1 — самый низкий приоритет, читаем последние 128 байт.
        if (parsed.title.empty() || parsed.artist.empty()) {
            const long long file_size = file.size();
            if (file_size >= static_cast<long long>(kId3v1Size)) {
                std::vector<char> tail_buffer(kId3v1Size);
                const size_t got =
                    file.read_at(file_size - static_cast<long long>(kId3v1Size),
                                 tail_buffer.data(), tail_buffer.size());
                parse_id3v1(std::string(tail_buffer.data(), got), parsed);
            }
        }

        if (!parsed.title.empty()) info.title = parsed.title;
        info.artist = parsed.artist;
    } catch (...) {
        // Никогда не бросаем: при любой ошибке — имя файла и пустой исполнитель.
        info.title = fallback_title;
        info.artist.clear();
    }

    return info;
}

}  // namespace tags
