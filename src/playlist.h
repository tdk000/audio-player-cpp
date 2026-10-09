// Плейлист: отбор аудиофайлов папки, «человеческая» сортировка и порядок
// воспроизведения (обычный или перемешанный, с повтором папки и трека).
#pragma once

#include <random>
#include <string>
#include <vector>

namespace playlist {

// Аудиофайл по расширению (.mp3/.wav/.ogg/.oga/.flac, регистр не важен).
bool is_audio_file(const std::string& utf8_path);

// Ключ сортировки, при котором «track 2» идёт раньше «track 10».
bool natural_less(const std::string& left, const std::string& right);

// Аудиофайлы папки (без подпапок), отсортированные по-человечески.
std::vector<std::string> scan_folder(const std::string& utf8_folder);

// Плейлист-файл: .m3u или .m3u8 (регистр не важен).
bool is_playlist_file(const std::string& utf8_path);

// Чтение M3U/M3U8. Относительные пути считаются от папки самого плейлиста,
// вложенные папки раскрываются, отсутствующие файлы и мусор пропускаются,
// дубликаты убираются. Кодировка — UTF-8 (с BOM или без), при невалидном
// UTF-8 строка читается как Windows-1251: старые плейлисты пишут именно так.
std::vector<std::string> read_m3u(const std::string& utf8_file);

// Запись плейлиста в M3U (UTF-8, с #EXTM3U и #EXTINF).
bool write_m3u(const std::string& utf8_file, const std::vector<std::string>& tracks);

enum class Repeat { Off, All, One };

// Порядок воспроизведения. Знает про перемешивание и повтор, ничего не знает
// про звук и окно — поэтому его легко прогнать тестами.
class Queue {
public:
    void set_tracks(std::vector<std::string> tracks);

    // Добавить треки в конец, не сбрасывая текущий и не перемешивая уже
    // сыгранную часть (режим «добавить папку к текущему плейлисту»).
    // Возвращает число реально добавленных треков: дубликаты отбрасываются.
    int append_tracks(std::vector<std::string> tracks);

    void clear();

    bool empty() const { return tracks_.empty(); }
    int size() const { return static_cast<int>(tracks_.size()); }
    const std::vector<std::string>& tracks() const { return tracks_; }

    // Индекс текущего трека в tracks() или -1, если играть нечего.
    int current() const { return current_; }
    const std::string& current_path() const;

    bool shuffle() const { return shuffle_; }
    void set_shuffle(bool on);

    Repeat repeat() const { return repeat_; }
    void set_repeat(Repeat mode) { repeat_ = mode; }
    void cycle_repeat();                      // выкл -> папка -> трек -> выкл

    // Следующий трек. automatic = true — трек доиграл сам: тогда повтор трека
    // остаётся на нём же, а конец списка без повтора даёт false («играть нечего»).
    // automatic = false — кнопка «След»: ходит по кругу всегда.
    bool next(bool automatic);
    bool previous();
    bool jump_to(int index);                  // выбор трека в списке

    void set_random_seed(uint32_t seed);      // детерминированный порядок для тестов

private:
    void rebuild_order(bool keep_current);
    void shuffle_order();

    std::vector<std::string> tracks_;
    std::vector<int> order_;                  // порядок обхода tracks_
    int position_ = -1;                       // где мы в order_
    int current_ = -1;                        // индекс в tracks_
    bool shuffle_ = false;
    Repeat repeat_ = Repeat::Off;
    std::mt19937 engine_{std::random_device{}()};
};

}  // namespace playlist
