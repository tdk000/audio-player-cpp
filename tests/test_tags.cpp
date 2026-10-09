// Тесты чтения метаданных: синтетические файлы + реальные mp3 + сверка
// с Python-эталоном (mutagen в старом проекте).
//
// Обычный консольный main(): печатает "OK  <имя>" / "FAIL <имя>",
// возвращает 1, если хоть одна проверка провалилась.
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/tags.h"

namespace {

int g_failed = 0;

void report(const std::string& name, bool ok, const std::string& detail = std::string()) {
    if (ok) {
        std::printf("OK  %s\n", name.c_str());
    } else {
        ++g_failed;
        std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : " :: ",
                    detail.c_str());
    }
    std::fflush(stdout);
}

std::string hex(const std::string& s) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (unsigned char c : s) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0xF]);
    }
    return out;
}

std::string show(const std::string& s) {
    return "\"" + s + "\" [" + hex(s) + "]";
}

// Сравнение ожидаемого и фактического с диагностикой.
bool expect_eq(const std::string& name, const std::string& got, const std::string& want) {
    const bool ok = (got == want);
    report(name, ok, ok ? "" : "got " + show(got) + " want " + show(want));
    return ok;
}

// ---------------------------------------------------------------------------
// Кодировки
// ---------------------------------------------------------------------------
std::string utf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(),
                                         static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                         nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), &out[0], size,
                        nullptr, nullptr);
    return out;
}

std::string utf16le_bytes(const std::wstring& wide) {
    std::string out;
    for (wchar_t ch : wide) {
        out.push_back(static_cast<char>(ch & 0xFF));
        out.push_back(static_cast<char>((ch >> 8) & 0xFF));
    }
    return out;
}

std::string utf16be_bytes(const std::wstring& wide) {
    std::string out;
    for (wchar_t ch : wide) {
        out.push_back(static_cast<char>((ch >> 8) & 0xFF));
        out.push_back(static_cast<char>(ch & 0xFF));
    }
    return out;
}

void test_encodings() {
    // latin1: "Café" -> C, a, f, 0xE9
    const std::string latin = std::string("Caf") + static_cast<char>(0xE9);
    expect_eq("latin1_to_utf8(Cafe-acute)", tags::latin1_to_utf8(latin),
              std::string("Caf") + "\xC3\xA9");

    // latin1: кириллица недоступна, но диапазон C0-FF должен давать 2 байта.
    const std::string latin2 = std::string(1, static_cast<char>(0xC0));
    expect_eq("latin1_to_utf8(0xC0)->U+00C0", tags::latin1_to_utf8(latin2),
              std::string("\xC3\x80"));

    expect_eq("latin1_to_utf8(ascii)", tags::latin1_to_utf8("Hello"), std::string("Hello"));
    expect_eq("latin1_to_utf8(empty)", tags::latin1_to_utf8(""), std::string(""));

    // UTF-16 LE с BOM: "Привет, мир" (BOM внутрь не входит — его снимает вызывающий).
    const std::wstring cyr = L"\u041F\u0440\u0438\u0432\u0435\u0442, \u043C\u0438\u0440";
    const std::string cyr_utf8 = utf8(cyr);
    expect_eq("utf16_to_utf8(LE, BOM-prefixed)",
              tags::utf16_to_utf8(std::string("\xFF\xFE") + utf16le_bytes(cyr), false),
              std::string("\xEF\xBB\xBF") + cyr_utf8);
    expect_eq("utf16_to_utf8(BE, BOM-prefixed)",
              tags::utf16_to_utf8(std::string("\xFE\xFF") + utf16be_bytes(cyr), true),
              std::string("\xEF\xBB\xBF") + cyr_utf8);

    // BMP-символ вне латиницы/кириллицы: "日本語".
    const std::wstring jp = L"\u65E5\u672C\u8A9E";
    expect_eq("utf16_to_utf8(LE, japanese)", tags::utf16_to_utf8(utf16le_bytes(jp), false),
              utf8(jp));
    expect_eq("utf16_to_utf8(BE, japanese)", tags::utf16_to_utf8(utf16be_bytes(jp), true),
              utf8(jp));

    // Суррогатная пара (U+1F600) -> 4 байта UTF-8.
    expect_eq("utf16_to_utf8(LE, surrogate pair)",
              tags::utf16_to_utf8(utf16le_bytes(L"\xD83D\xDE00"), false),
              std::string("\xF0\x9F\x98\x80"));

    expect_eq("utf16_to_utf8(empty)", tags::utf16_to_utf8("", false), std::string(""));
}

// ---------------------------------------------------------------------------
// Временные файлы
// ---------------------------------------------------------------------------
std::wstring temp_dir() {
    wchar_t buffer[MAX_PATH + 1] = {0};
    const DWORD len = GetTempPathW(MAX_PATH, buffer);
    if (len == 0) return L".\\";
    return std::wstring(buffer);
}

std::wstring make_temp_path(const std::wstring& stem, const std::wstring& ext) {
    static int counter = 0;
    wchar_t name[128] = {0};
    std::swprintf(name, 128, L"tagstest_%lu_%d_%ls%ls", GetCurrentProcessId(), counter++,
                  stem.c_str(), ext.c_str());
    return temp_dir() + name;
}

void write_file(const std::wstring& path, const std::string& data) {
    std::FILE* fp = _wfopen(path.c_str(), L"wb");
    if (!fp) return;
    if (!data.empty()) std::fwrite(data.data(), 1, data.size(), fp);
    std::fclose(fp);
}

std::string to_utf8_path(const std::wstring& path) {
    if (path.empty()) return std::string();
    const int size = WideCharToMultiByte(CP_UTF8, 0, path.c_str(),
                                         static_cast<int>(path.size()), nullptr, 0, nullptr,
                                         nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), static_cast<int>(path.size()), &out[0], size,
                        nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------------------
// Сборка синтетических контейнеров
// ---------------------------------------------------------------------------
void put_be32(std::string& out, unsigned long value) {
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

void put_le32(std::string& out, unsigned long value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 24) & 0xFF));
}

void put_be24(std::string& out, unsigned long value) {
    out.push_back(static_cast<char>((value >> 16) & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
    out.push_back(static_cast<char>(value & 0xFF));
}

// Synchsafe-представление 28-битного размера.
void put_synchsafe32(std::string& out, unsigned long value) {
    out.push_back(static_cast<char>((value >> 21) & 0x7F));
    out.push_back(static_cast<char>((value >> 14) & 0x7F));
    out.push_back(static_cast<char>((value >> 7) & 0x7F));
    out.push_back(static_cast<char>(value & 0x7F));
}

// Кадр ID3v2.3 (размер — обычный big-endian).
std::string id3v23_frame(const std::string& id, const std::string& body) {
    std::string frame = id;
    put_be32(frame, body.size());
    frame.push_back('\0');
    frame.push_back('\0');  // флаги
    frame += body;
    return frame;
}

// Полный тег ID3v2.3 с заголовком.
std::string id3v23_tag(const std::string& frames) {
    std::string tag = "ID3";
    tag.push_back(static_cast<char>(3));  // major
    tag.push_back('\0');                  // revision
    tag.push_back('\0');                  // flags
    put_synchsafe32(tag, frames.size());
    tag += frames;
    return tag;
}

// Vorbis Comment: vendor + count + записи "KEY=value".
std::string vorbis_comment(const std::vector<std::string>& entries,
                           const std::string& vendor = "test-vendor") {
    std::string out;
    put_le32(out, vendor.size());
    out += vendor;
    put_le32(out, entries.size());
    for (const std::string& entry : entries) {
        put_le32(out, entry.size());
        out += entry;
    }
    return out;
}

void test_id3v23() {
    const std::wstring cyr_title = L"\u0417\u0430\u0433\u043E\u043B\u043E\u0432\u043E\u043A";  // Заголовок
    const std::string artist = "Test Artist";

    // title — UTF-16 LE с BOM (encoding 1), artist — UTF-8 (encoding 3).
    std::string title_body;
    title_body.push_back(static_cast<char>(1));
    title_body += std::string("\xFF\xFE");
    title_body += utf16le_bytes(cyr_title);

    std::string artist_body;
    artist_body.push_back(static_cast<char>(3));
    artist_body += artist;

    std::string frames;
    frames += id3v23_frame("TIT2", title_body);
    frames += id3v23_frame("TPE1", artist_body);
    // Мусорный кадр, который нужно пропустить.
    frames += id3v23_frame("TALB", std::string(1, static_cast<char>(3)) + "Some Album");

    std::string data = id3v23_tag(frames);
    data.append(4096, '\0');  // «аудиоданные»
    data += "TAG";

    const std::wstring path = make_temp_path(L"id3v23", L".mp3");
    write_file(path, data);

    const tags::Info info = tags::read(to_utf8_path(path));
    expect_eq("id3v2.3 title (UTF-16 LE + BOM)", info.title, utf8(cyr_title));
    expect_eq("id3v2.3 artist (UTF-8)", info.artist, artist);

    DeleteFileW(path.c_str());
}

void test_id3v24() {
    // v2.4: synchsafe размеры кадров и многозначный TIT2 (разделитель NUL).
    const std::string first = "First Title";
    const std::string second = "Second Title";
    const std::string artist = "V24 Artist";

    std::string title_body;
    title_body.push_back(static_cast<char>(3));
    title_body += first;
    title_body.push_back('\0');
    title_body += second;

    std::string artist_body;
    artist_body.push_back(static_cast<char>(0));
    artist_body += "Caf";  // latin-1 (ниже добавим 0xE9)
    artist_body.push_back(static_cast<char>(0xE9));

    std::string frames;
    {
        std::string body = title_body;
        std::string frame = "TIT2";
        put_synchsafe32(frame, body.size());
        frame.push_back('\0');
        frame.push_back('\0');
        frame += body;
        frames += frame;
    }
    {
        std::string body = artist_body;
        std::string frame = "TPE1";
        put_synchsafe32(frame, body.size());
        frame.push_back('\0');
        frame.push_back('\0');
        frame += body;
        frames += frame;
    }

    std::string data = "ID3";
    data.push_back(static_cast<char>(4));
    data.push_back('\0');
    data.push_back('\0');
    put_synchsafe32(data, frames.size());
    data += frames;
    data.append(2048, '\0');

    const std::wstring path = make_temp_path(L"id3v24", L".mp3");
    write_file(path, data);

    const tags::Info info = tags::read(to_utf8_path(path));
    expect_eq("id3v2.4 title (synchsafe, first value)", info.title, first);
    expect_eq("id3v2.4 artist (latin-1)", info.artist,
              std::string("Caf") + "\xC3\xA9");

    DeleteFileW(path.c_str());
}

void test_id3v1() {
    std::string title = "Old Latin Title";
    std::string artist = "Old Latin Artist";
    title.resize(30, '\0');
    artist.resize(30, '\0');

    std::string data;
    data.append(2048, '\x55');  // «аудио» без тегов
    data += "TAG";
    data += title;
    data += artist;
    data += std::string(30, '\0');  // album
    data += std::string(4, '\0');   // year
    data += std::string(30, '\0');  // comment
    data.push_back(static_cast<char>(12));  // genre

    const std::wstring path = make_temp_path(L"id3v1", L".mp3");
    write_file(path, data);

    const tags::Info info = tags::read(to_utf8_path(path));
    expect_eq("id3v1 title", info.title, std::string("Old Latin Title"));
    expect_eq("id3v1 artist", info.artist, std::string("Old Latin Artist"));

    DeleteFileW(path.c_str());
}

void test_id3v1_latin1_cyrillic() {
    // ID3v1 в latin-1: 0xCF 0xE8 -> "Пи" (CP1251-байты тоже валидны как latin-1,
    // здесь проверяем именно latin-1 -> UTF-8).
    std::string title(30, '\0');
    title[0] = static_cast<char>(0xCF);
    title[1] = static_cast<char>(0xE8);
    std::string artist(30, '\0');
    artist[0] = static_cast<char>(0xC0);

    std::string data;
    data.append(1024, '\x11');
    data += "TAG";
    data += title;
    data += artist;
    data += std::string(65, '\0');

    const std::wstring path = make_temp_path(L"id3v1latin", L".mp3");
    write_file(path, data);

    const tags::Info info = tags::read(to_utf8_path(path));
    expect_eq("id3v1 latin-1 title", info.title, std::string("\xC3\x8F\xC3\xA8"));
    expect_eq("id3v1 latin-1 artist", info.artist, std::string("\xC3\x80"));

    DeleteFileW(path.c_str());
}

void test_flac() {
    const std::string title = "FLAC Title \xD0\x9A\xD0\xB8\xD1\x80";  // + "Кир"
    const std::string artist = "FLAC Artist";
    const std::string comments = vorbis_comment({"TITLE=" + title, "ARTIST=" + artist,
                                                 "ALBUM=Ignored"});

    std::string data = "fLaC";
    // Блок 0 (STREAMINFO), не последний.
    data.push_back('\0');
    put_be24(data, 34);
    data.append(34, '\0');
    // Блок 4 (VORBIS_COMMENT), последний.
    data.push_back(static_cast<char>(0x80 | 4));
    put_be24(data, comments.size());
    data += comments;

    const std::wstring path = make_temp_path(L"flac", L".flac");
    write_file(path, data);

    const tags::Info info = tags::read(to_utf8_path(path));
    expect_eq("flac/vorbis title", info.title, title);
    expect_eq("flac/vorbis artist", info.artist, artist);

    DeleteFileW(path.c_str());
}

void test_ogg() {
    const std::string title = "OGG Title";
    const std::string artist = "OGG Artist";
    const std::string comments = vorbis_comment({"ARTIST=" + artist, "TITLE=" + title,
                                                 "title=lowercase ignored"});

    std::string data;
    data += "OggS";
    data.append(100, '\x42');  // поддельная первая страница
    data += std::string("\x03vorbis", 7);
    data += comments;

    const std::wstring path = make_temp_path(L"ogg", L".ogg");
    write_file(path, data);

    const tags::Info info = tags::read(to_utf8_path(path));
    expect_eq("ogg/vorbis title", info.title, title);
    expect_eq("ogg/vorbis artist (case-insensitive key)", info.artist, artist);

    DeleteFileW(path.c_str());
}

void test_wav_info() {
    const std::string title = "WAV Info Title";
    const std::string artist = "WAV Info Artist";

    std::string list;
    list += "INFO";
    for (int i = 0; i < 2; ++i) {
        const std::string id = (i == 0) ? "INAM" : "IART";
        const std::string value = (i == 0) ? title : artist;
        std::string payload = value;
        payload.push_back('\0');  // строки INFO — C-строки в latin-1
        list += id;
        put_le32(list, payload.size());
        list += payload;
        if (payload.size() & 1u) list.push_back('\0');  // выравнивание
    }

    std::string body;
    body += "WAVE";
    body += "fmt ";
    put_le32(body, 16);
    body.append(16, '\0');
    body += "data";
    put_le32(body, 1024);
    body.append(1024, '\0');
    body += "LIST";
    put_le32(body, list.size());
    body += list;

    std::string data = "RIFF";
    put_le32(data, body.size());
    data += body;

    const std::wstring path = make_temp_path(L"wav", L".wav");
    write_file(path, data);

    const tags::Info info = tags::read(to_utf8_path(path));
    expect_eq("wav/list-info title (INAM)", info.title, title);
    expect_eq("wav/list-info artist (IART)", info.artist, artist);

    DeleteFileW(path.c_str());
}

void test_wav_id3_chunk() {
    const std::string title = "WAV id3 Title";
    const std::string artist = "WAV id3 Artist";

    std::string frames;
    frames += id3v23_frame("TIT2", std::string(1, static_cast<char>(3)) + title);
    frames += id3v23_frame("TPE1", std::string(1, static_cast<char>(3)) + artist);
    const std::string id3 = id3v23_tag(frames);

    std::string body;
    body += "WAVE";
    body += "fmt ";
    put_le32(body, 16);
    body.append(16, '\0');
    body += "id3 ";
    put_le32(body, id3.size());
    body += id3;
    if (id3.size() & 1u) body.push_back('\0');

    std::string data = "RIFF";
    put_le32(data, body.size());
    data += body;

    const std::wstring path = make_temp_path(L"wavid3", L".wav");
    write_file(path, data);

    const tags::Info info = tags::read(to_utf8_path(path));
    expect_eq("wav id3-chunk title", info.title, title);
    expect_eq("wav id3-chunk artist", info.artist, artist);

    DeleteFileW(path.c_str());
}

void test_no_tags() {
    std::string data("RIFF");
    put_le32(data, 64);
    data += "WAVEfmt ";
    put_le32(data, 16);
    data.append(16, '\0');
    data += "data";
    put_le32(data, 16);
    data.append(16, '\0');

    const std::wstring path = make_temp_path(L"notags", L".wav");
    write_file(path, data);

    const tags::Info info = tags::read(to_utf8_path(path));
    // Ожидаем имя файла без расширения — оно содержит наши префиксы.
    std::wstring stem = path;
    const size_t slash = stem.find_last_of(L"/\\");
    if (slash != std::wstring::npos) stem = stem.substr(slash + 1);
    const size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos) stem = stem.substr(0, dot);

    expect_eq("no-tags title = file name", info.title, utf8(stem));
    expect_eq("no-tags artist empty", info.artist, std::string(""));
    report("no-tags title is not the full file name",
           info.title.find(".wav") == std::string::npos);

    DeleteFileW(path.c_str());
}

void test_missing_file() {
    const std::string path = to_utf8_path(temp_dir() + L"definitely_missing_12345.mp3");
    const tags::Info info = tags::read(path);
    expect_eq("missing file title = stem", info.title, "definitely_missing_12345");
    expect_eq("missing file artist empty", info.artist, std::string(""));
}

void test_utf8_path_cyrillic() {
    // Путь с кириллицей (папка/имя файла), тег внутри.
    std::string frames;
    frames += id3v23_frame("TIT2", std::string(1, static_cast<char>(3)) + "Cyr Path Title");
    std::string data = id3v23_tag(frames);
    data.append(512, '\0');

    const std::wstring path = make_temp_path(L"\u0422\u0435\u0441\u0442_\u0444\u0430\u0439\u043B",
                                             L".mp3");
    write_file(path, data);

    const tags::Info info = tags::read(to_utf8_path(path));
    expect_eq("cyrillic utf-8 path", info.title, std::string("Cyr Path Title"));

    DeleteFileW(path.c_str());
}

// ---------------------------------------------------------------------------
// Реальные файлы
// ---------------------------------------------------------------------------
const char* kRealFiles[] = {
    "C:\\Users\\1\\Desktop\\audio-player-cpp\\test-media\\01-test-tone-440hz-10s.mp3",
    "C:\\Users\\1\\Desktop\\audio-player-cpp\\test-media\\02-test-sweep-80-12000hz-60s.mp3",
    "C:\\Users\\1\\Desktop\\audio-player-cpp\\test-media\\03-ode-to-joy-synth-40s.mp3",
};

std::vector<tags::Info> g_real_info;

bool file_exists(const char* path) {
    const DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void test_real_files() {
    g_real_info.clear();
    for (size_t i = 0; i < 3; ++i) {
        const char* path = kRealFiles[i];
        if (!file_exists(path)) {
            report(std::string("real file present: ") + path, false, "not found");
            g_real_info.push_back(tags::Info());
            continue;
        }
        const tags::Info info = tags::read(path);
        g_real_info.push_back(info);
        report(std::string("real file read: ") + path,
               !info.title.empty() && !info.artist.empty(),
               "title=" + show(info.title) + " artist=" + show(info.artist));
        std::printf("    real    title=%s artist=%s\n", info.title.c_str(),
                    info.artist.c_str());
    }

    // Известные значения из Python-эталона (см. вывод сверки ниже).
    const char* want_titles[3] = {"Test Tone 440 Hz", "Test Sweep 80 Hz - 12 kHz",
                                  "Ode to Joy (synth demo)"};
    const char* want_artist = "Arena Test Lab";
    for (size_t i = 0; i < 3; ++i) {
        if (g_real_info[i].title.empty() && g_real_info[i].artist.empty()) continue;
        expect_eq(std::string("real title #") + std::to_string(i + 1),
                  g_real_info[i].title, want_titles[i]);
        expect_eq(std::string("real artist #") + std::to_string(i + 1),
                  g_real_info[i].artist, want_artist);
    }
}

// ---------------------------------------------------------------------------
// Сверка с Python-эталоном
// ---------------------------------------------------------------------------
// Экранирует строку для литерала Python (обычный, не raw-литерал).
std::string py_literal(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out.push_back(static_cast<char>(c)); break;
        }
    }
    out += "\"";
    return out;
}

// Запускает Python-эталон и возвращает его stdout (как есть) или "".
std::string run_python_reference() {
    std::string script;
    script += "import sys\n";
    script += "sys.stdout.reconfigure(encoding='utf-8')\n";
    script += "sys.path.insert(0, r'C:\\Users\\1\\Desktop\\audio-player')\n";
    script += "import player\n";
    script += "files = [\n";
    for (size_t i = 0; i < 3; ++i) {
        script += "    " + py_literal(kRealFiles[i]) + ",\n";
    }
    script += "]\n";
    script += "for p in files:\n";
    script += "    print('\\x1f'.join(player.read_tags(p)))\n";

    const std::wstring wide_script = temp_dir() + L"tagstest_pyref.py";
    write_file(wide_script, script);

    // _popen идёт через cmd.exe, который ломает кавычки в путях — запускаем
    // процесс напрямую через CreateProcessW и читаем его stdout из канала.
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0)) return std::string();
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_pipe;
    si.hStdError = write_pipe;

    PROCESS_INFORMATION pi = {};
    std::wstring cmdline = L"\"C:\\Python314\\python.exe\" \"" + wide_script + L"\"";
    std::vector<wchar_t> cmdline_buffer(cmdline.begin(), cmdline.end());
    cmdline_buffer.push_back(L'\0');  // CreateProcessW правит буфер на месте

    const BOOL started = CreateProcessW(
        nullptr, cmdline_buffer.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    CloseHandle(write_pipe);  // иначе чтение из канала никогда не завершится

    std::string out;
    if (!started) {
        CloseHandle(read_pipe);
        return out;
    }

    char buffer[1024];
    DWORD got = 0;
    while (ReadFile(read_pipe, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
        out.append(buffer, got);
    }

    WaitForSingleObject(pi.hProcess, 10000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(read_pipe);
    DeleteFileW(wide_script.c_str());
    return out;
}

// Разбивает вывод Python на строки.
std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string line =
            (nl == std::string::npos) ? text.substr(start) : text.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}

void test_python_reference() {
    std::printf("    --- Python reference (player.read_tags) ---\n");
    const std::string raw = run_python_reference();
    const std::vector<std::string> lines = split_lines(raw);

    // Первые 3 строки — это наши файлы; лишние (предупреждения) печатаем как есть.
    for (const std::string& line : lines) {
        std::printf("    python  %s\n", line.c_str());
    }

    if (lines.size() < 3) {
        report("python reference produced 3 lines", false,
               "got " + std::to_string(lines.size()) + " line(s): " + raw);
        return;
    }

    bool all_match = true;
    for (size_t i = 0; i < 3; ++i) {
        const std::string& line = lines[i];
        const size_t sep = line.find('\x1f');
        if (sep == std::string::npos) {
            report(std::string("python line format #") + std::to_string(i + 1), false, line);
            all_match = false;
            continue;
        }
        const std::string py_title = line.substr(0, sep);
        const std::string py_artist = line.substr(sep + 1);

        const bool same_title = (py_title == g_real_info[i].title);
        const bool same_artist = (py_artist == g_real_info[i].artist);
        if (!same_title || !same_artist) all_match = false;

        report(std::string("python match title #") + std::to_string(i + 1), same_title,
               same_title ? "" : "cpp=" + show(g_real_info[i].title) +
                                     " py=" + show(py_title));
        report(std::string("python match artist #") + std::to_string(i + 1), same_artist,
               same_artist ? "" : "cpp=" + show(g_real_info[i].artist) +
                                      " py=" + show(py_artist));
    }
    report("python reference fully matches C++", all_match);
}

}  // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);

    test_encodings();
    test_id3v23();
    test_id3v24();
    test_id3v1();
    test_id3v1_latin1_cyrillic();
    test_flac();
    test_ogg();
    test_wav_info();
    test_wav_id3_chunk();
    test_no_tags();
    test_missing_file();
    test_utf8_path_cyrillic();
    test_real_files();
    test_python_reference();

    std::printf("\n%s (%d failure(s))\n", g_failed == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
                g_failed);
    return g_failed == 0 ? 0 : 1;
}
