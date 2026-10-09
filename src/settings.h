// Сохранение состояния между запусками.
//
// Файл — обычный текст в UTF-8, по умолчанию
// %APPDATA%\AudioPlayerCpp\settings.ini. Разбор и сборка вынесены в чистые
// функции parse()/serialise(), поэтому проверяются тестами без файловой системы.
#pragma once

#include <string>
#include <vector>

namespace settings {

// Что переживает перезапуск: громкость, режимы, эквалайзер, плейлист и позиция.
struct State {
    double volume = 0.8;
    bool shuffle = false;
    int repeat = 0;                       // 0 — выкл, 1 — повтор папки, 2 — повтор трека
    bool eq_enabled = false;
    double eq_gain[3] = {0.0, 0.0, 0.0};  // низкие, средние, высокие: дБ
    std::string track;                    // трек, который играл
    double position = 0.0;                // позиция в нём, секунды
    std::vector<std::string> playlist;    // последний плейлист целиком
    int playlist_index = -1;              // индекс текущего трека в плейлисте
};

// Путь к файлу настроек. Пустая строка, если %APPDATA% недоступен.
std::string default_path();

// Текст файла <-> состояние. Значения вне диапазонов зажимаются,
// неизвестные ключи игнорируются, битые числа не портят остальной разбор.
std::string serialise(const State& state);
bool parse(const std::string& text, State& state);

// Чтение и запись файла. Ничего не бросают: нет файла — false и состояние
// остаётся нетронутым, нет папки — она создаётся.
bool load(const std::string& utf8_file, State& state);
bool save(const std::string& utf8_file, const State& state);

}  // namespace settings
