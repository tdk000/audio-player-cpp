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

    std::filesystem::remove_all(tmp, ec);

    if (g_failed != 0) {
        std::printf("\nПРОВАЛ: %d проверок не прошло\n", g_failed);
        return 1;
    }
    std::printf("\nВсе проверки пройдены\n");
    return 0;
}
