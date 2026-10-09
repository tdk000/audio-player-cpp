#include "playlist.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <random>
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

// --------------------------------------------------------------------------- //
//  Порядок воспроизведения
// --------------------------------------------------------------------------- //
void Queue::set_tracks(std::vector<std::string> tracks) {
    tracks_ = std::move(tracks);
    current_ = tracks_.empty() ? -1 : 0;
    rebuild_order(false);
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
