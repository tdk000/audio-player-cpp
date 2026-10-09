// Проверка плейлиста и порядка воспроизведения: сортировка файлов папки,
// перемешивание, повтор папки и повтор трека. Окно и звук не нужны.
//
//     dist\test_playlist.exe
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "../src/common.h"
#include "../src/playlist.h"

namespace {

int g_failed = 0;

void check(const std::string& name, bool condition, const std::string& extra = std::string()) {
    std::printf("  %s  %s%s\n", condition ? "OK  " : "FAIL", name.c_str(),
                extra.empty() ? "" : (" — " + extra).c_str());
    if (!condition) {
        ++g_failed;
    }
}

// Очередь из n «треков» без обращения к диску.
playlist::Queue make_queue(int count, std::vector<std::string>* names = nullptr) {
    std::vector<std::string> tracks;
    for (int i = 0; i < count; ++i) {
        tracks.push_back("track" + std::to_string(i + 1) + ".mp3");
    }
    if (names != nullptr) {
        *names = tracks;
    }
    playlist::Queue queue;
    queue.set_tracks(std::move(tracks));
    return queue;
}

bool write_empty(const std::filesystem::path& path) {
    FILE* file = _wfopen(path.wstring().c_str(), L"wb");
    if (file == nullptr) {
        return false;
    }
    std::fclose(file);
    return true;
}

// Записать файл «как есть», байт в байт: нужно для проверки чтения
// плейлистов в разных кодировках.
bool write_bytes(const std::filesystem::path& path, const std::string& data) {
    FILE* file = _wfopen(path.wstring().c_str(), L"wb");
    if (file == nullptr) {
        return false;
    }
    const size_t written = std::fwrite(data.data(), 1, data.size(), file);
    std::fclose(file);
    return written == data.size();
}

std::string file_name_of(const std::string& utf8_path) {
    return app::path_to_utf8(app::path_from_utf8(utf8_path).filename());
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("Расширения и «человеческая» сортировка\n");
    check("mp3 распознан", playlist::is_audio_file("song.mp3"));
    check("MP3 в верхнем регистре", playlist::is_audio_file("SONG.MP3"));
    check("flac распознан", playlist::is_audio_file("a.flac"));
    check("txt не аудио", !playlist::is_audio_file("readme.txt"));
    check("без расширения не аудио", !playlist::is_audio_file("noext"));
    check("«2» раньше «10»", playlist::natural_less("track 2.mp3", "track 10.mp3"));
    check("«1» раньше «2»", playlist::natural_less("track 1.mp3", "track 2.mp3"));
    check("«10» позже «9»", !playlist::natural_less("track 10.mp3", "track 9.mp3"));

    std::printf("Сканирование папки\n");
    std::error_code ec;
    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() /
        ("audioplayer-playlist-" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(tmp, ec);
    for (const char* name : {"track 10.mp3", "track 2.mp3", "track 1.mp3", "album.flac",
                             "readme.txt"}) {
        write_empty(tmp / name);
    }
    std::filesystem::create_directories(tmp / "subfolder", ec);   // подпапки не берём
    write_empty(tmp / "subfolder" / "inner.mp3");

    const std::vector<std::string> found = playlist::scan_folder(app::path_to_utf8(tmp));
    check("найдено 4 аудиофайла", found.size() == 4, std::to_string(found.size()));
    const std::vector<std::string> expected = {"album.flac", "track 1.mp3", "track 2.mp3",
                                               "track 10.mp3"};
    bool order_ok = found.size() == expected.size();
    for (size_t i = 0; order_ok && i < expected.size(); ++i) {
        order_ok = app::path_from_utf8(found[i]).filename() == expected[i];
    }
    check("порядок естественный, подпапки пропущены", order_ok);

    std::printf("Обычный порядок\n");
    playlist::Queue queue = make_queue(4);
    check("текущий — первый", queue.current() == 0 && queue.size() == 4);
    check("путь текущего", queue.current_path() == "track1.mp3", queue.current_path());

    check("кнопка «След»: 0 -> 1", queue.next(false) && queue.current() == 1);
    check("кнопка «След»: 1 -> 2", queue.next(false) && queue.current() == 2);
    check("кнопка «След»: 2 -> 3", queue.next(false) && queue.current() == 3);
    check("кнопка «След» ходит по кругу", queue.next(false) && queue.current() == 0);
    check("кнопка «Пред»: 0 -> 3", queue.previous() && queue.current() == 3);
    check("кнопка «Пред»: 3 -> 2", queue.previous() && queue.current() == 2);

    std::printf("Конец плейлиста без повтора\n");
    queue = make_queue(3);
    check("автопереход 0 -> 1", queue.next(true) && queue.current() == 1);
    check("автопереход 1 -> 2", queue.next(true) && queue.current() == 2);
    check("после последнего — играть нечего", !queue.next(true));

    playlist::Queue single = make_queue(1);
    check("один трек: автопереход некуда", !single.next(true));
    check("один трек: кнопка «След» повторяет его", single.next(false) && single.current() == 0);

    std::printf("Повтор папки\n");
    queue = make_queue(3);
    queue.set_repeat(playlist::Repeat::All);
    check("автопереход 0 -> 1", queue.next(true) && queue.current() == 1);
    check("автопереход 1 -> 2", queue.next(true) && queue.current() == 2);
    check("после последнего — снова первый", queue.next(true) && queue.current() == 0);

    std::printf("Повтор трека\n");
    queue = make_queue(3);
    queue.set_repeat(playlist::Repeat::One);
    queue.next(true);
    const int current = queue.current();
    check("автопереход остаётся на треке", queue.next(true) && queue.current() == current);
    check("и ещё раз остаётся", queue.next(true) && queue.current() == current);
    check("кнопка «След» всё равно переключает", queue.next(false) && queue.current() != current);

    std::printf("Переключение режима повтора\n");
    queue = make_queue(2);
    check("по умолчанию выключен", queue.repeat() == playlist::Repeat::Off);
    queue.cycle_repeat();
    check("выкл -> папка", queue.repeat() == playlist::Repeat::All);
    queue.cycle_repeat();
    check("папка -> трек", queue.repeat() == playlist::Repeat::One);
    queue.cycle_repeat();
    check("трек -> выкл", queue.repeat() == playlist::Repeat::Off);

    std::printf("Выбор трека в списке\n");
    queue = make_queue(4);
    check("прыжок на третий", queue.jump_to(2) && queue.current() == 2);
    check("дальше по списку", queue.next(false) && queue.current() == 3);

    std::printf("Перемешивание\n");
    queue = make_queue(4);
    queue.set_random_seed(20260214);
    queue.jump_to(0);
    queue.set_shuffle(true);
    check("перемешивание включено", queue.shuffle());
    check("текущий трек не сменился", queue.current() == 0, std::to_string(queue.current()));

    std::set<int> visited;
    visited.insert(queue.current());
    for (int i = 0; i < queue.size() - 1; ++i) {
        check("следующий в перемешанном порядке", queue.next(false));
        visited.insert(queue.current());
    }
    check("круг обошёл все треки по одному разу", visited.size() == 4 && *visited.begin() == 0 &&
                                                      *visited.rbegin() == 3,
          std::to_string(visited.size()));

    std::printf("Перемешивание: конец круга\n");
    queue = make_queue(4);
    queue.set_random_seed(7);
    queue.set_shuffle(true);
    int played = 1;
    while (queue.next(true)) {
        ++played;
        if (played > 10) {
            break;
        }
    }
    check("без повтора круг заканчивается", played == 4, std::to_string(played));

    queue = make_queue(4);
    queue.set_random_seed(7);
    queue.set_shuffle(true);
    queue.set_repeat(playlist::Repeat::All);
    for (int i = 0; i < 3; ++i) {           // доходим до последнего трека круга
        check("автопереход в круге", queue.next(true));
    }
    const int last_of_round = queue.current();
    check("с повтором папки начинается новый круг", queue.next(true));
    const int new_first = queue.current();
    check("новый круг не повторяет трек подряд", new_first != last_of_round,
          std::to_string(last_of_round) + " -> " + std::to_string(new_first));

    queue.set_shuffle(false);
    check("выключение перемешивания не сбивает трек", queue.current() == new_first);
    const int expected_next = (new_first + 1) % queue.size();
    check("дальше идём по порядку", queue.next(false) && queue.current() == expected_next,
          std::to_string(queue.current()));

    std::printf("Пустой плейлист и один трек\n");
    playlist::Queue empty;
    check("пусто", empty.empty() && empty.size() == 0 && empty.current() == -1);
    check("next() некуда", !empty.next(true) && !empty.next(false));
    check("previous() некуда", !empty.previous());
    check("jump_to() невозможен", !empty.jump_to(0));
    check("пустой путь", empty.current_path().empty());

    playlist::Queue new_folder = make_queue(3);
    new_folder.set_shuffle(true);
    new_folder.set_tracks({"a.mp3", "b.mp3", "c.mp3"});
    check("новая папка в режиме перемешивания", new_folder.size() == 3 &&
                                                    new_folder.current() >= 0 &&
                                                    new_folder.current() < 3,
          std::to_string(new_folder.current()));

    std::printf("Добавление треков к текущему плейлисту\n");
    {
        playlist::Queue queue = make_queue(3);
        check("старт на первом", queue.current() == 0);
        check("next() перевёл на второй", queue.next(false) && queue.current() == 1);
        const int playing = queue.current();
        const int added = queue.append_tracks({"new1.mp3", "new2.mp3"});
        check("добавлено 2 трека", added == 2, std::to_string(added));
        check("размер стал 5", queue.size() == 5, std::to_string(queue.size()));
        check("текущий трек не сбился", queue.current() == playing);
        check("хвост играется по порядку", queue.next(false) && queue.current() == 2);

        const int again = queue.append_tracks({"new1.mp3", "track1.mp3"});
        check("дубликаты не добавляются", again == 0, std::to_string(again));
        check("размер не изменился", queue.size() == 5);

        playlist::Queue empty;
        check("добавление в пустой даёт 1", empty.append_tracks({"only.mp3"}) == 1);
        check("и сразу становится текущим", empty.current() == 0);
        check("пустой список ничего не делает", empty.append_tracks({}) == 0);

        playlist::Queue mixed;
        mixed.set_tracks({"a.mp3", "b.mp3"});
        mixed.set_shuffle(true);
        const int before = mixed.current();
        check("в перемешивании добавлено 3", mixed.append_tracks({"c.mp3", "d.mp3", "e.mp3"}) == 3);
        check("текущий трек уцелел", mixed.current() == before);
        check("итого 5", mixed.size() == 5);
        std::set<int> visited;
        visited.insert(mixed.current());
        for (int i = 0; i < 4; ++i) {
            check("шаг по кругу", mixed.next(false));
            visited.insert(mixed.current());
        }
        check("все 5 треков достижимы", visited.size() == 5, std::to_string(visited.size()));
    }

    std::printf("Плейлисты M3U\n");
    {
        check("m3u распознан", playlist::is_playlist_file("list.m3u"));
        check("m3u8 в верхнем регистре", playlist::is_playlist_file("LIST.M3U8"));
        check("mp3 не плейлист", !playlist::is_playlist_file("song.mp3"));

        // Файлы, на которые будет ссылаться плейлист.
        std::filesystem::create_directories(tmp / "m3u sub", ec);
        write_empty(tmp / "m3u one.mp3");
        write_empty(tmp / "m3u two.mp3");
        write_empty(tmp / "m3u sub" / "inner.mp3");
        write_empty(tmp / "ignore.txt");

        const std::string list_path = app::path_to_utf8(tmp / "list.m3u");
        const std::string body =
            "\xEF\xBB\xBF#EXTM3U\n"                 // BOM + заголовок
            "#EXTINF:-1,Первый\n"                   // директива игнорируется
            "m3u one.mp3\n"                         // относительный путь
            "\n"                                    // пустая строка
            "m3u sub\n"                             // папка раскрывается
            "m3u one.mp3\n"                         // дубликат
            "m3u two.mp3\r\n"                       // CRLF
            "нет-такого-файла.mp3\n"                // отсутствующий файл
            "ignore.txt\n";                         // не аудио
        check("плейлист записан", write_bytes(tmp / "list.m3u", body));

        const std::vector<std::string> loaded = playlist::read_m3u(list_path);
        check("прочитано 3 трека", loaded.size() == 3, std::to_string(loaded.size()));
        std::set<std::string> names;
        for (const std::string& path : loaded) {
            names.insert(file_name_of(path));
        }
        check("относительные пути разрешены",
              names.count("m3u one.mp3") == 1 && names.count("m3u two.mp3") == 1);
        check("папка внутри плейлиста раскрыта", names.count("inner.mp3") == 1);
        check("дубликат убран", names.size() == 3);

        // Старый .m3u в Windows-1251: «тест.mp3» побайтово.
        write_empty(tmp / std::filesystem::path(L"тест.mp3"));
        check("cp1251-плейлист записан",
              write_bytes(tmp / "cp1251.m3u", "#EXTM3U\n\xF2\xE5\xF1\xF2.mp3\n"));
        const std::vector<std::string> legacy = playlist::read_m3u(app::path_to_utf8(tmp / "cp1251.m3u"));
        check("строк в cp1251-плейлисте: 1", legacy.size() == 1, std::to_string(legacy.size()));
        if (!legacy.empty()) {
            check("имя из cp1251 прочитано верно",
                  file_name_of(legacy.front()) == app::wide_to_utf8(L"тест.mp3"),
                  file_name_of(legacy.front()));
        }

        // Запись и обратное чтение.
        const std::string saved = app::path_to_utf8(tmp / "saved.m3u");
        check("write_m3u отработал", playlist::write_m3u(saved, loaded));
        const std::vector<std::string> round_trip = playlist::read_m3u(saved);
        check("round-trip сохранил 3 трека", round_trip.size() == 3,
              std::to_string(round_trip.size()));
        check("round-trip сохранил порядок", round_trip == loaded);
        check("чужой путь не читается",
              playlist::read_m3u(app::path_to_utf8(tmp / "missing.m3u")).empty());
    }

    std::filesystem::remove_all(tmp, ec);

    if (g_failed != 0) {
        std::printf("\nПРОВАЛ: %d проверок не прошло\n", g_failed);
        return 1;
    }
    std::printf("\nВсе проверки пройдены\n");
    return 0;
}
