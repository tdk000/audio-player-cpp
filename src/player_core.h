// Ядро плеера: весь звук здесь, интерфейса нет — легко тестировать.
//
// Обёртка над ma_engine/ma_sound из miniaudio с вменяемым состоянием и позицией.
// Позицию ведём сами (точка отсчёта + монотонное время), потому что у потокового
// звука внутренний курсор декодера читает файл вперёд и опережает то, что слышно.
#pragma once

#include <optional>
#include <stdexcept>
#include <string>

#include "common.h"
#include "miniaudio.h"

// Файл не загрузился; сообщение — человекочитаемое объяснение причины.
class AudioLoadError : public std::runtime_error {
public:
    explicit AudioLoadError(const std::string& message) : std::runtime_error(message) {}
};

class PlayerCore {
public:
    enum class State { Stopped, Playing, Paused };

    // Device::Null — «пустое» устройство вывода: движок работает в реальном
    // времени, но звук никуда не идёт (нужно тестам: тихо и без звуковой карты).
    enum class Device { Default, Null };

    explicit PlayerCore(double volume = app::kDefaultVolume, Device device = Device::Default);
    ~PlayerCore();

    PlayerCore(const PlayerCore&) = delete;
    PlayerCore& operator=(const PlayerCore&) = delete;

    // Загрузить файл (не запуская воспроизведение). Бросает AudioLoadError.
    // При неудаче ранее загруженный трек остаётся нетронутым.
    void open(const std::string& utf8_path);

    // start = nullopt — продолжить с текущей позиции (или снять с паузы).
    void play(std::optional<double> start = std::nullopt);
    void pause();
    State toggle();
    void stop();

    // Перемотка. Работает и на паузе, и в остановке.
    void seek(double seconds);
    void seek_by(double delta);

    // Дёргать из таймера интерфейса. true — трек доиграл до конца сам.
    bool poll();

    void close();

    State state() const { return state_; }
    bool has_track() const { return !path_.empty(); }
    bool seekable() const { return has_track() && duration_ > 0.0; }
    double position() const;
    double duration() const { return duration_; }
    double volume() const { return volume_; }
    void set_volume(double value);
    double change_volume(double delta);
    const std::string& path() const { return path_; }

private:
    void unload();
    void release_sound();
    void stamp() { anchor_time_ = now(); }
    double clamp_position(double seconds) const;
    static double now();

    ma_context context_{};
    bool context_ready_ = false;
    ma_engine engine_{};
    bool engine_ready_ = false;
    // Звук живём в куче: ma_sound нельзя ни копировать, ни переносить — внутри
    // граф узлов ссылается на собственные поля, и адрес должен быть стабильным.
    ma_sound* sound_ = nullptr;

    State state_ = State::Stopped;
    std::string path_;
    double duration_ = 0.0;
    double anchor_pos_ = 0.0;
    double anchor_time_ = 0.0;
    double volume_ = 0.0;
};
