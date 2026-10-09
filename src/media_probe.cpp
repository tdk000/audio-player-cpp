#include "media_probe.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "common.h"
#include "miniaudio.h"

namespace media {
namespace {

// Открытие файла по UTF-8 пути: _wfopen не зависит от текущей кодовой страницы,
// поэтому кириллица в путях работает.
FILE* open_utf8(const std::string& utf8_path) {
    const std::wstring wide = app::utf8_to_wide(utf8_path);
    if (wide.empty()) {
        return nullptr;
    }
    return _wfopen(wide.c_str(), L"rb");
}

size_t read_bytes(FILE* file, void* buffer, size_t count) {
    return std::fread(buffer, 1, count, file);
}

// Длительность OGG/Vorbis: granule последней страницы / частоту дискретизации.
// ma_decoder_get_length_in_pcm_frames() для Vorbis всегда возвращает 0
// (ограничение stb_vorbis в push-режиме), поэтому считаем сами.
double ogg_duration(FILE* file) {
    std::vector<unsigned char> head(64 * 1024);
    const size_t head_size = read_bytes(file, head.data(), head.size());
    head.resize(head_size);

    const unsigned char identification[] = {0x01, 'v', 'o', 'r', 'b', 'i', 's'};
    const auto signature = std::search(head.begin(), head.end(),
                                       std::begin(identification), std::end(identification));
    if (signature == head.end() || static_cast<size_t>(head.end() - signature) < 7 + 12) {
        return 0.0;
    }
    // после сигнатуры: version(4) + channels(1) + sample_rate(4, LE)
    const unsigned char* rate_ptr = &*(signature + 7) + 5;
    const unsigned int rate = static_cast<unsigned int>(rate_ptr[0]) |
                              (static_cast<unsigned int>(rate_ptr[1]) << 8) |
                              (static_cast<unsigned int>(rate_ptr[2]) << 16) |
                              (static_cast<unsigned int>(rate_ptr[3]) << 24);
    if (rate == 0) {
        return 0.0;
    }

    if (_fseeki64(file, 0, SEEK_END) != 0) {
        return 0.0;
    }
    const long long size = _ftelli64(file);
    const long long tail_size = std::min<long long>(size, 64 * 1024);
    if (tail_size <= 0 || _fseeki64(file, size - tail_size, SEEK_SET) != 0) {
        return 0.0;
    }
    std::vector<unsigned char> tail(static_cast<size_t>(tail_size));
    const size_t got = read_bytes(file, tail.data(), tail.size());
    tail.resize(got);

    // Ищем последнюю страницу OggS: в её заголовке granule position (8 байт LE).
    const unsigned char page[] = {'O', 'g', 'g', 'S'};
    auto it = std::find_end(tail.begin(), tail.end(), std::begin(page), std::end(page));
    if (it == tail.end() || static_cast<size_t>(tail.end() - it) < 14) {
        return 0.0;
    }
    const unsigned char* p = &*it;
    unsigned long long granule = 0;
    for (int i = 7; i >= 0; --i) {
        granule = (granule << 8) | p[6 + i];
    }
    if (granule == 0) {
        return 0.0;
    }
    return static_cast<double>(granule) / static_cast<double>(rate);
}

}  // namespace

Format sniff_format(const std::string& utf8_path) {
    unsigned char head[32] = {0};
    FILE* file = open_utf8(utf8_path);
    if (file == nullptr) {
        return Format::Unknown;
    }
    const size_t size = read_bytes(file, head, sizeof(head));
    std::fclose(file);
    if (size < 12) {
        // для mp3/ogg достаточно первых байт, остальные проверки пропускаем
        if (size >= 3 && std::memcmp(head, "ID3", 3) == 0) {
            return Format::Mp3;
        }
        if (size >= 2 && head[0] == 0xFF && (head[1] & 0xE0) == 0xE0 && (head[1] & 0x06) != 0x00) {
            return Format::Mp3;
        }
        return Format::Unknown;
    }
    if (std::memcmp(head, "RIFF", 4) == 0 && std::memcmp(head + 8, "WAVE", 4) == 0) {
        return Format::Wav;
    }
    if (std::memcmp(head, "fLaC", 4) == 0) {
        return Format::Flac;
    }
    if (std::memcmp(head, "OggS", 4) == 0) {
        return Format::Ogg;
    }
    if (std::memcmp(head, "ID3", 3) == 0) {
        return Format::Mp3;
    }
    if (head[0] == 0xFF && (head[1] & 0xE0) == 0xE0) {
        // 12-битный sync + layer=00 -> ADTS-AAC; 11-битный sync + layer!=00 -> MP3
        if ((head[1] & 0xF0) == 0xF0 && (head[1] & 0x06) == 0x00) {
            return Format::Aac;
        }
        if ((head[1] & 0x06) != 0x00) {
            return Format::Mp3;
        }
    }
    if (std::memcmp(head + 4, "ftyp", 4) == 0) {
        return Format::M4a;
    }
    if (std::memcmp(head, "\x30\x26\xb2\x75", 4) == 0) {
        return Format::Wma;
    }
    return Format::Unknown;
}

const char* format_name(Format format) {
    switch (format) {
        case Format::Wav:  return "WAV";
        case Format::Flac: return "FLAC";
        case Format::Ogg:  return "OGG/Vorbis";
        case Format::Mp3:  return "MP3";
        case Format::Aac:  return "AAC (ADTS)";
        case Format::M4a:  return "M4A/AAC";
        case Format::Wma:  return "WMA";
        case Format::Unknown: break;
    }
    return "неизвестный формат";
}

double estimate_mp3_duration(const std::string& utf8_path) {
    // Таблицы MPEG-1/2/2.5 Layer III.
    static const int kBitrateV1L3[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const int kBitrateV2L3[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const int kRateV1[3] = {44100, 48000, 32000};
    static const int kRateV2[3] = {22050, 24000, 16000};
    static const int kRateV25[3] = {11025, 12000, 8000};

    FILE* file = open_utf8(utf8_path);
    if (file == nullptr) {
        return 0.0;
    }
    std::vector<unsigned char> head(64 * 1024);
    const size_t size = read_bytes(file, head.data(), head.size());
    head.resize(size);
    if (_fseeki64(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        return 0.0;
    }
    const long long file_size = _ftelli64(file);
    std::fclose(file);
    if (head.size() < 5) {
        return 0.0;
    }

    size_t offset = 0;
    if (head.size() >= 10 && std::memcmp(head.data(), "ID3", 3) == 0) {   // пропускаем тег
        offset = 10 + ((static_cast<size_t>(head[6] & 0x7F) << 21) |
                       (static_cast<size_t>(head[7] & 0x7F) << 14) |
                       (static_cast<size_t>(head[8] & 0x7F) << 7) |
                       static_cast<size_t>(head[9] & 0x7F));
    }

    for (size_t i = offset; i + 4 < head.size(); ++i) {
        const unsigned char b1 = head[i + 1];
        const unsigned char b2 = head[i + 2];
        if (head[i] != 0xFF || (b1 & 0xE0) != 0xE0 || (b1 & 0x06) == 0x00) {
            continue;
        }
        const unsigned char version = static_cast<unsigned char>((b1 >> 3) & 0x03);
        const unsigned char layer = static_cast<unsigned char>((b1 >> 1) & 0x03);
        const unsigned char bitrate_index = static_cast<unsigned char>((b2 >> 4) & 0x0F);
        const unsigned char rate_index = static_cast<unsigned char>((b2 >> 2) & 0x03);
        if (layer != 0x01 || bitrate_index == 0 || bitrate_index == 15 || rate_index == 3) {
            continue;
        }
        int bitrate = 0;
        int rate = 0;
        if (version == 0x03) {
            bitrate = kBitrateV1L3[bitrate_index] * 1000;
            rate = kRateV1[rate_index];
        } else if (version == 0x02 || version == 0x00) {
            bitrate = kBitrateV2L3[bitrate_index] * 1000;
            rate = (version == 0x02) ? kRateV2[rate_index] : kRateV25[rate_index];
        }
        if (bitrate != 0 && rate != 0) {
            const double seconds = static_cast<double>(file_size - static_cast<long long>(i)) * 8.0 / bitrate;
            return std::max(0.0, seconds);
        }
    }
    return 0.0;
}

double probe_duration(const std::string& utf8_path) {
    // 1. Декодер miniaudio: точная длина для WAV/FLAC/MP3.
    ma_decoder decoder;
    if (ma_decoder_init_file_w(app::utf8_to_wide(utf8_path).c_str(),
                               nullptr, &decoder) == MA_SUCCESS) {
        ma_uint64 frames = 0;
        ma_uint32 rate = 0;
        ma_format format = ma_format_unknown;
        ma_uint32 channels = 0;
        if (ma_decoder_get_length_in_pcm_frames(&decoder, &frames) == MA_SUCCESS && frames > 0 &&
            ma_decoder_get_data_format(&decoder, &format, &channels, &rate, nullptr, 0) == MA_SUCCESS &&
            rate > 0) {
            ma_decoder_uninit(&decoder);
            return static_cast<double>(frames) / static_cast<double>(rate);
        }
        ma_decoder_uninit(&decoder);
    }

    const Format format = sniff_format(utf8_path);
    if (format == Format::Ogg) {                       // stb_vorbis длину не отдаёт
        FILE* file = open_utf8(utf8_path);
        if (file != nullptr) {
            const double seconds = ogg_duration(file);
            std::fclose(file);
            if (seconds > 0.0) {
                return seconds;
            }
        }
    }
    if (format == Format::Mp3) {                       // запасной путь: считаем по кадрам
        return estimate_mp3_duration(utf8_path);
    }
    return 0.0;
}

}  // namespace media
