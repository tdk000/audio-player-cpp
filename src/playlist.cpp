#include "playlist.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <system_error>

#include "common.h"

namespace playlist {
namespace {

std::string lower_ascii(const std::string& text) {
    std::string result = text;
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return result;
}

// Разбиение имени на чередующиеся куски «число / не число».
std::vector<std::string> split_parts(const std::string& text) {
    std::vector<std::string> parts;
    std::string current;
    bool digits = false;
    for (const char ch : text) {
        const bool is_digit = ch >= '0' && ch <= '9';
        if (!current.empty() && is_digit != digits) {
            parts.push_back(current);
            current.clear();
        }
        digits = is_digit;
        current.push_back(ch);
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

std::string trim(const std::string& text) {
    const size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return std::string();
    }
    const size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

// Строгий, но быстрый тест на корректный UTF-8: нужен, чтобы отличить
// современный .m3u8 от старого .m3u в Windows-1251.
bool valid_utf8(const std::string& text) {
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        size_t extra = 0;
        if (ch < 0x80) {
            ++i;
            continue;
        } else if ((ch & 0xE0) == 0xC0) {
            extra = 1;
            if (ch < 0xC2) {
                return false;                    // переполнение (overlong)
            }
        } else if ((ch & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((ch & 0xF8) == 0xF0) {
            extra = 3;
            if (ch > 0xF4) {
                return false;
            }
        } else {
            return false;
        }
        if (i + extra >= text.size()) {
            return false;
        }
        for (size_t k = 1; k <= extra; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

std::string cp1251_to_utf8(const std::string& text) {
    if (text.empty()) {
        return std::string();
    }
    const int size = MultiByteToWideChar(1251, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) {
        return text;
    }
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(1251, 0, text.data(), static_cast<int>(text.size()), wide.data(), size);
    return app::wide_to_utf8(wide);
}

std::string lower_extension(const std::filesystem::path& path) {
    std::string extension = app::wide_to_utf8(path.extension().wstring());
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return extension;
}

}  // namespace

bool is_audio_file(const std::string& utf8_path) {
    const std::filesystem::path path = app::path_from_utf8(utf8_path);
    std::string extension = app::wide_to_utf8(path.extension().wstring());
    if (extension.empty()) {
        return false;
    }
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    for (const char* known : app::kAudioExtensions) {
        if (extension == known) {
            return true;
        }
    }
    return false;
}

bool natural_less(const std::string& left, const std::string& right) {
    const std::vector<std::string> a = split_parts(lower_ascii(left));
    const std::vector<std::string> b = split_parts(lower_ascii(right));
    const size_t common = std::min(a.size(), b.size());
    for (size_t i = 0; i < common; ++i) {
        const bool a_digits = !a[i].empty() && std::isdigit(static_cast<unsigned char>(a[i][0])) != 0;
        const bool b_digits = !b[i].empty() && std::isdigit(static_cast<unsigned char>(b[i][0])) != 0;
        if (a_digits && b_digits) {
            // Сравниваем как числа, отбрасывая ведущие нули.
            const size_t a_start = a[i].find_first_not_of('0');
            const size_t b_start = b[i].find_first_not_of('0');
            const std::string a_num = (a_start == std::string::npos) ? "0" : a[i].substr(a_start);
            const std::string b_num = (b_start == std::string::npos) ? "0" : b[i].substr(b_start);
            if (a_num.size() != b_num.size()) {
                return a_num.size() < b_num.size();
            }
            if (a_num != b_num) {
                return a_num < b_num;
            }
        } else if (a[i] != b[i]) {
            return a[i] < b[i];
        }
    }
    return a.size() < b.size();
}

std::vector<std::string> scan_folder(const std::string& utf8_folder) {
    std::vector<std::string> files;
    std::error_code ec;
    const std::filesystem::path folder = app::path_from_utf8(utf8_folder);
    std::filesystem::directory_iterator iterator(folder, ec);
    if (ec) {
        return files;
    }
    for (const std::filesystem::directory_entry& entry : iterator) {
        std::error_code entry_ec;
        if (!entry.is_regular_file(entry_ec)) {
            continue;
        }
        const std::string path = app::path_to_utf8(entry.path());
        if (is_audio_file(path)) {
            files.push_back(path);
        }
    }
    std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
        return natural_less(app::path_to_utf8(app::path_from_utf8(a).filename()),
                            app::path_to_utf8(app::path_from_utf8(b).filename()));
    });
    return files;
}

bool is_playlist_file(const std::string& utf8_path) {
    const std::string extension = lower_extension(app::path_from_utf8(utf8_path));
    return extension == ".m3u" || extension == ".m3u8";
}

std::vector<std::string> read_m3u(const std::string& utf8_file) {
    std::vector<std::string> result;
    std::ifstream input(app::path_from_utf8(utf8_file), std::ios::binary);
    if (!input) {
        return result;
    }
    std::string data((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();

    // BOM UTF-8: без него первая строка начнётся с мусорных байт и не найдётся.
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        data.erase(0, 3);
    }

    const std::filesystem::path folder = app::path_from_utf8(utf8_file).parent_path();
    std::set<std::string> seen;
    std::istringstream stream(data);
    std::string line;
    while (std::getline(stream, line)) {
        std::string text = trim(line);
        if (text.empty() || text[0] == '#') {       // #EXTM3U, #EXTINF и прочие директивы
            continue;
        }
        if (!valid_utf8(text)) {
            text = cp1251_to_utf8(text);
        }

        std::filesystem::path entry = app::path_from_utf8(text);
        if (entry.is_relative()) {
            entry = folder / entry;                 // путь относительно самого плейлиста
        }

        std::error_code ec;
        if (std::filesystem::is_directory(entry, ec)) {
            for (const std::string& track : scan_folder(app::path_to_utf8(entry))) {
                if (seen.insert(track).second) {
                    result.push_back(track);
                }
            }
        } else if (std::filesystem::is_regular_file(entry, ec)) {
            const std::string path = app::path_to_utf8(entry);
            if (is_audio_file(path) && seen.insert(path).second) {
                result.push_back(path);
            }
        }
    }
    return result;
}

bool write_m3u(const std::string& utf8_file, const std::vector<std::string>& tracks) {
    std::ofstream output(app::path_from_utf8(utf8_file), std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output << "#EXTM3U\n";
    for (const std::string& track : tracks) {
        const std::string title = app::path_to_utf8(app::path_from_utf8(track).stem());
        output << "#EXTINF:-1," << title << "\n";
        output << track << "\n";
    }
    output.flush();
    return output.good();
}

// --------------------------------------------------------------------------- //
//  Порядок воспроизведения
// --------------------------------------------------------------------------- //
void Queue::set_tracks(std::vector<std::string> tracks) {
    tracks_ = std::move(tracks);
    current_ = tracks_.empty() ? -1 : 0;
    rebuild_order(false);
}

// Добавление не должно сбивать воспроизведение: текущий трек и уже пройденная
// часть порядка остаются на месте, новые индексы дописываются в конец order_
// (в режиме перемешивания — в случайном порядке между собой).
int Queue::append_tracks(std::vector<std::string> tracks) {
    if (tracks.empty()) {
        return 0;
    }
    const int old_size = static_cast<int>(tracks_.size());
    int added = 0;
    for (std::string& track : tracks) {
        if (track.empty()) {
            continue;
        }
        if (std::find(tracks_.begin(), tracks_.end(), track) != tracks_.end()) {
            continue;                               // дубликаты в плейлисте не нужны
        }
        tracks_.push_back(std::move(track));
        ++added;
    }
    if (added == 0) {
        return 0;
    }
    if (old_size == 0) {
        current_ = 0;
        rebuild_order(false);
        return added;
    }

    std::vector<int> fresh;
    fresh.reserve(static_cast<size_t>(added));
    for (int i = old_size; i < static_cast<int>(tracks_.size()); ++i) {
        fresh.push_back(i);
    }
    if (shuffle_) {
        std::shuffle(fresh.begin(), fresh.end(), engine_);
    }
    order_.insert(order_.end(), fresh.begin(), fresh.end());
    return added;
}

void Queue::clear() {
    tracks_.clear();
    order_.clear();
    position_ = -1;
    current_ = -1;
}

const std::string& Queue::current_path() const {
    static const std::string empty;
    if (current_ < 0 || current_ >= static_cast<int>(tracks_.size())) {
        return empty;
    }
    return tracks_[static_cast<size_t>(current_)];
}

void Queue::shuffle_order() {
    std::shuffle(order_.begin(), order_.end(), engine_);
}

void Queue::set_random_seed(uint32_t seed) {
    engine_.seed(seed);
}

// Пересобрать порядок обхода. keep_current — оставить текущий трек играющим
// (переключение перемешивания не должно сбивать воспроизведение).
void Queue::rebuild_order(bool keep_current) {
    order_.resize(tracks_.size());
    for (size_t i = 0; i < tracks_.size(); ++i) {
        order_[i] = static_cast<int>(i);
    }
    if (order_.empty()) {
        position_ = -1;
        current_ = -1;
        return;
    }

    const bool keep = keep_current && current_ >= 0 && current_ < static_cast<int>(tracks_.size());
    if (!shuffle_) {
        // Обычный порядок: продолжаем с текущего трека по списку.
        position_ = keep ? current_ : 0;
        current_ = order_[static_cast<size_t>(position_)];
        return;
    }

    shuffle_order();
    if (keep) {
        // Текущий трек продолжает играть и становится первым в новом круге:
        // иначе круг начался бы с его середины и часть треков выпала бы.
        const auto found = std::find(order_.begin(), order_.end(), current_);
        if (found != order_.end()) {
            std::rotate(order_.begin(), found, found + 1);
        }
    }
    position_ = 0;
    current_ = order_[0];
}

void Queue::set_shuffle(bool on) {
    if (shuffle_ == on) {
        return;
    }
    shuffle_ = on;
    rebuild_order(true);
}

void Queue::cycle_repeat() {
    switch (repeat_) {
        case Repeat::Off: repeat_ = Repeat::All; break;
        case Repeat::All: repeat_ = Repeat::One; break;
        case Repeat::One: repeat_ = Repeat::Off; break;
    }
}

bool Queue::next(bool automatic) {
    if (order_.empty()) {
        return false;
    }
    if (automatic && repeat_ == Repeat::One) {
        return current_ >= 0;               // повтор трека: никуда не уходим
    }

    if (position_ + 1 < static_cast<int>(order_.size())) {
        ++position_;
        current_ = order_[static_cast<size_t>(position_)];
        return true;
    }

    // Круг закончился.
    if (automatic && repeat_ == Repeat::Off) {
        return false;                       // папка доиграна, повтор выключен
    }
    if (shuffle_) {
        const int played = current_;
        shuffle_order();
        if (order_.size() > 1 && order_[0] == played) {
            std::swap(order_[0], order_[1]);   // не играем тот же трек дважды подряд
        }
    }
    position_ = 0;
    current_ = order_[0];
    return true;
}

bool Queue::previous() {
    if (order_.empty()) {
        return false;
    }
    if (position_ > 0) {
        --position_;
    } else {
        position_ = static_cast<int>(order_.size()) - 1;   // кнопка «Пред» ходит по кругу
    }
    current_ = order_[static_cast<size_t>(position_)];
    return true;
}

bool Queue::jump_to(int index) {
    if (index < 0 || index >= static_cast<int>(tracks_.size())) {
        return false;
    }
    current_ = index;
    const auto found = std::find(order_.begin(), order_.end(), index);
    if (found != order_.end()) {
        position_ = static_cast<int>(found - order_.begin());
    } else {
        order_.push_back(index);
        position_ = static_cast<int>(order_.size()) - 1;
    }
    return true;
}

}  // namespace playlist
