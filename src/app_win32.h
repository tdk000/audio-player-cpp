// Главное окно плеера (Win32 API, без внешних библиотек).
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "player_core.h"
#include "playlist.h"
#include "ui_theme.h"

class PlayerApp {
public:
    explicit PlayerApp(PlayerCore& core);
    ~PlayerApp();

    bool create(HINSTANCE instance);
    void show();
    void run();

    // Файл или папка, переданные в командной строке.
    void open_from_command_line(const std::string& utf8_path);

    // Горячие клавиши и курсор-«рука» перехватываются до DispatchMessage.
    bool pre_dispatch(MSG& message);

private:
    enum : int {
        kIdOpenFile = 1001,
        kIdOpenFolder,
        kIdPrev,
        kIdPlay,
        kIdNext,
        kIdStop,
        kIdShuffle,
        kIdRepeat,
        kIdList,
        kIdPosition,
        kIdVolume,
        kIdScroll,
    };

    struct Layout {
        RECT title{};
        RECT count{};
        RECT artist{};
        RECT current_time{};
        RECT total_time{};
        RECT position_slider{};
        RECT open_file{};
        RECT open_folder{};
        RECT prev{};
        RECT play{};
        RECT next{};
        RECT stop{};
        RECT shuffle{};
        RECT repeat{};
        RECT volume_label{};
        RECT volume_slider{};
        RECT volume_value{};
        RECT list{};
        RECT scroll{};
        RECT status{};
    };

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    void create_children();
    void apply_dpi(int dpi, const RECT* suggested);
    void layout();
    int px(double logical) const;
    int item_height() const;

    void paint(HDC dc);
    void draw_button(const DRAWITEMSTRUCT& item);
    void draw_list_item(const DRAWITEMSTRUCT& item);
    void draw_text(HDC dc, const std::wstring& text, const RECT& rect, HFONT font, COLORREF color,
                   UINT format) const;

    void on_command(int id, int code);
    void on_timer();
    void refresh();

    bool handle_key(WPARAM key, bool ctrl);

    void open_file_dialog();
    void open_folder_dialog();
    bool load_folder(const std::string& folder);
    bool start_playlist(std::vector<std::string> files);
    void accept_dropped_files(HDROP drop);
    bool load(const std::string& path, bool autoplay = true, bool show_errors = true,
              bool reset_playlist = true);
    bool play_from_queue(bool autoplay = true, bool interactive = false);
    void next_track();
    void prev_track();
    void toggle_playback();
    void stop_playback();
    void seek_by(double delta);
    void play_selected();
    void on_track_ended();
    void toggle_shuffle();
    void cycle_repeat_mode();
    void update_mode_buttons();
    bool button_checked(HWND control) const;

    void sync_playlist_view();
    void update_list_visibility();
    void highlight_current();
    void sync_scrollbar();
    void scroll_list_by(int lines);
    void scroll_list_to(int top);
    void update_volume_label();
    std::wstring volume_label_text() const;
    void set_enabled(HWND control, bool enabled);
    void set_status(const std::wstring& text);
    void log_crash(const char* what);
    void set_play_button_text();

    std::wstring title_text() const;
    std::wstring total_time_text() const;
    std::wstring count_text() const;

    PlayerCore& core_;

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    HWND open_file_ = nullptr;
    HWND open_folder_ = nullptr;
    HWND prev_ = nullptr;
    HWND play_ = nullptr;
    HWND next_ = nullptr;
    HWND stop_ = nullptr;
    HWND shuffle_ = nullptr;
    HWND repeat_ = nullptr;
    HWND list_ = nullptr;
    HWND slider_position_ = nullptr;
    HWND slider_volume_ = nullptr;
    HWND scroll_ = nullptr;
    HBRUSH list_brush_ = nullptr;
    HICON icon_ = nullptr;
    HWND hot_button_ = nullptr;      // кнопка под курсором (подсветку рисуем сами)

    theme::Fonts fonts_;
    Layout layout_;
    int dpi_ = 96;

    playlist::Queue queue_;             // порядок воспроизведения и режимы
    std::vector<std::wstring> list_items_;
    std::string last_dir_;

    std::wstring track_title_;
    std::wstring track_artist_;
    // Тексты, которые сейчас нарисованы в окне: сравниваем с новыми, чтобы
    // перерисовывать только изменившиеся надписи (иначе окно мигает).
    std::wstring status_ = L"Готово";
    std::wstring position_label_ = L"0:00";
    std::wstring title_label_ = L"Аудиоплеер";
    std::wstring artist_label_ = L"Откройте MP3-файл";
    std::wstring total_label_ = L"--:--";
    std::wstring count_label_;
    std::wstring volume_text_ = L"80 %";

    bool dragging_position_ = false;
    bool closing_ = false;
    bool crash_logged_ = false;
};
