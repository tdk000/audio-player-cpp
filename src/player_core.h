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

    // Полосы эквалайзера. Реализованы узлами miniaudio: низкие и высокие —
    // полочные фильтры, середина — пиковый. 0 дБ означает «прозрачно».
    enum class Band { Low = 0, Mid = 1, High = 2 };
    static constexpr int kBandCount = 3;
    static constexpr double kMaxGainDb = 12.0;
    static constexpr int kFadeMs = 350;          // плавный вход и затухание, мс

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

    // Эквалайзер. Выключенный эквалайзер не снимается с тракта, а обнуляется:
    // пересборка графа на ходу щёлкает.
    bool eq_enabled() const { return eq_enabled_; }
    void set_eq_enabled(bool on);
    void toggle_eq();
    double eq_gain(Band band) const { return eq_gain_[static_cast<int>(band)]; }
    void set_eq_gain(Band band, double gain_db);
    void change_eq_gain(Band band, double delta_db);
    // Готова ли цепочка фильтров (устройству может не хватить ресурсов).
    bool eq_available() const { return eq_nodes_ready_; }
    // Текст для строки состояния: «Эквалайзер: выкл» или «НЧ +4 · СЧ 0 · ВЧ −2 дБ».
    std::string eq_label() const;

private:
    void unload();
    void release_sound();
    void stamp() { anchor_time_ = now(); }
    double clamp_position(double seconds) const;
    static double now();

    void create_eq_nodes();
    void destroy_eq_nodes();
    void wire_eq_input();                        // звук -> НЧ -> СЧ -> ВЧ -> выход
    void apply_eq();                             // пересчёт коэффициентов по усилениям
    void fade_in();                              // плавный вход при старте
    void fade_out();                             // плавное затухание при остановке

    ma_context context_{};
    bool context_ready_ = false;
    ma_engine engine_{};
    bool engine_ready_ = false;
    // Звук живём в куче: ma_sound нельзя ни копировать, ни переносить — внутри
    // граф узлов ссылается на собственные поля, и адрес должен быть стабильным.
    ma_sound* sound_ = nullptr;

    // Цепочка эквалайзера. Узлы живут столько же, сколько движок, и не
    // пересоздаются: при смене настроек меняются только коэффициенты.
    ma_loshelf_node eq_low_{};
    ma_peak_node eq_mid_{};
    ma_hishelf_node eq_high_{};
    bool eq_nodes_ready_ = false;
    bool eq_enabled_ = false;
    double eq_gain_[kBandCount] = {0.0, 0.0, 0.0};

    State state_ = State::Stopped;
    std::string path_;
    double duration_ = 0.0;
    double anchor_pos_ = 0.0;
    double anchor_time_ = 0.0;
    double volume_ = 0.0;
};
