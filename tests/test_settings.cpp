// Проверка сохранения состояния между запусками: разбор, сборка и файл.
// Файловая система трогается только в самом конце — во временной папке.
//
//     dist\test_settings.exe
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "../src/common.h"
#include "../src/settings.h"

namespace {

int g_failed = 0;

void check(const std::string& name, bool condition, const std::string& extra = std::string()) {
    std::printf("  %s  %s%s\n", condition ? "OK  " : "FAIL", name.c_str(),
                extra.empty() ? "" : (" — " + extra).c_str());
    if (!condition) {
        ++g_failed;
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::printf("Значения по умолчанию\n");
    {
        settings::State state;
        check("громкость 0.8", state.volume == 0.8);
        check("перемешивание выключено", !state.shuffle);
        check("повтор выключен", state.repeat == 0);
        check("эквалайзер выключен", !state.eq_enabled);
        check("плейлист пуст", state.playlist.empty());
        check("индекс -1", state.playlist_index == -1);
    }

    std::printf("Сборка и разбор\n");
    {
        settings::State source;
        source.volume = 0.35;
        source.shuffle = true;
        source.repeat = 2;
        source.eq_enabled = true;
        source.eq_gain[0] = 4.5;
        source.eq_gain[1] = -3.0;
        source.eq_gain[2] = 12.0;
        source.track = "C:\\music\\track 1.mp3";
        source.position = 42.25;
        source.playlist = {"C:\\music\\track 1.mp3", "C:\\music\\track 2.mp3"};
        source.playlist_index = 1;

        const std::string text = settings::serialise(source);
        check("файл не пустой", !text.empty());
        check("заголовок на месте", text.rfind("#", 0) == 0);

        settings::State restored;
        check("текст разобран", settings::parse(text, restored));
        check("громкость сохранена", restored.volume == 0.35, std::to_string(restored.volume));
        check("перемешивание сохранено", restored.shuffle);
        check("повтор сохранён", restored.repeat == 2);
        check("эквалайзер сохранён", restored.eq_enabled);
        check("низкие сохранены", restored.eq_gain[0] == 4.5, std::to_string(restored.eq_gain[0]));
        check("средние сохранены", restored.eq_gain[1] == -3.0);
        check("высокие сохранены", restored.eq_gain[2] == 12.0);
        check("текущий трек сохранён (с пробелом и двоеточием)",
              restored.track == "C:\\music\\track 1.mp3", restored.track);
        check("позиция сохранена", restored.position == 42.25, std::to_string(restored.position));
        check("плейлист сохранён целиком", restored.playlist == source.playlist);
        check("индекс сохранён", restored.playlist_index == 1);
    }

    std::printf("Зажим значений и мусор в файле\n");
    {
        settings::State state;
        const std::string wild =
            "volume=7.5\n"
            "repeat=99\n"
            "eq_low=1000\n"
            "eq_mid=-1000\n"
            "position=-5\n";
        check("мусорные диапазоны разобраны", settings::parse(wild, state));
        check("громкость зажата к 1", state.volume == 1.0, std::to_string(state.volume));
        check("повтор зажат к 2", state.repeat == 2, std::to_string(state.repeat));
        check("усиление зажато сверху", state.eq_gain[0] == 12.0, std::to_string(state.eq_gain[0]));
        check("усиление зажато снизу", state.eq_gain[1] == -12.0, std::to_string(state.eq_gain[1]));
        check("отрицательная позиция зажата к 0", state.position == 0.0);

        settings::State garbage_before;
        garbage_before.volume = 0.5;
        const std::string garbage =
            "volume=не-число\n"
            "shuffle=может быть\n"
            "неизвестный_ключ=значение\n"
            "\n"
            "# комментарий\n";
        settings::State garbage_state;
        check("файл с мусором разобран", settings::parse(garbage, garbage_state));
        check("битая громкость оставила значение по умолчанию",
              garbage_state.volume == 0.8, std::to_string(garbage_state.volume));
        check("битый флаг читается как «выключено»", !garbage_state.shuffle);

        settings::State ignored;
        check("пустой текст не разбирается", !settings::parse("", ignored));
        check("только комментарии не разбираются", !settings::parse("# просто комментарий\n", ignored));
        check("только мусор не разбирается", !settings::parse("абракадабра\n", ignored));
    }

    std::printf("Файл настроек\n");
    {
        std::error_code ec;
        const std::filesystem::path tmp =
            std::filesystem::temp_directory_path() /
            ("audioplayer-settings-" + std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(tmp, ec);

        const std::string file = app::path_to_utf8(tmp / "sub" / "settings.ini");
        settings::State state;
        state.volume = 0.6;
        state.playlist = {"a.mp3", "b.mp3"};
        state.track = "a.mp3";
        check("сохранение создаёт папку и файл", settings::save(file, state));

        settings::State restored;
        check("чтение вернуло данные", settings::load(file, restored));
        check("громкость совпала", restored.volume == 0.6, std::to_string(restored.volume));
        check("плейлист совпал", restored.playlist == state.playlist);
        check("текущий трек совпал", restored.track == "a.mp3");

        std::error_code remove_ec;
        std::filesystem::remove_all(tmp, remove_ec);

        settings::State untouched;
        untouched.volume = 0.9;
        check("отсутствующий файл — это false", !settings::load(file, untouched));
        check("при этом состояние не портится", untouched.volume == 0.9);
        check("пустой путь — это false", !settings::load("", untouched));
    }

    if (g_failed != 0) {
        std::printf("\nПРОВАЛ: %d проверок не прошло\n", g_failed);
        return 1;
    }
    std::printf("\nВсе проверки пройдены\n");
    return 0;
}
