#include "app_win32.h"

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>
#include <system_error>

#include "common.h"
#include "media_probe.h"
#include "playlist.h"
#include "resources.h"
#include "settings.h"
#include "tags.h"
#include "ui_slider.h"

namespace {

constexpr const wchar_t* kWindowClass = L"DshAudioPlayerWindow";
constexpr const wchar_t* kOpenFileFilter =
    L"Аудио и плейлисты (*.mp3;*.wav;*.ogg;*.oga;*.flac;*.m3u;*.m3u8)\0"
    L"*.mp3;*.wav;*.ogg;*.oga;*.flac;*.m3u;*.m3u8\0"
    L"Аудио (*.mp3;*.wav;*.ogg;*.oga;*.flac)\0*.mp3;*.wav;*.ogg;*.oga;*.flac\0"
    L"Плейлисты (*.m3u;*.m3u8)\0*.m3u;*.m3u8\0"
    L"Все файлы (*.*)\0*.*\0\0";

// Куда сохранять плейлист: только M3U, чтобы список открывался любым плеером.
constexpr const wchar_t* kSavePlaylistFilter =
    L"Плейлист M3U (*.m3u)\0*.m3u\0"
    L"Плейлист M3U8 (*.m3u8)\0*.m3u8\0\0";

// Названия полос эквалайзера — для строки состояния.
constexpr const wchar_t* kEqBandNames[3] = {L"низкие", L"средние", L"высокие"};

typedef UINT(WINAPI* GetDpiForWindowProc)(HWND);

// GetDpiForWindow появился в Windows 10 1607 — если его нет, берём DPI экрана.
UINT dpi_of(HWND window) {
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
    static GetDpiForWindowProc get_dpi_for_window = reinterpret_cast<GetDpiForWindowProc>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    if (get_dpi_for_window != nullptr && window != nullptr) {
        const UINT dpi = get_dpi_for_window(window);
        if (dpi > 0) {
            return dpi;
        }
    }
    HDC dc = GetDC(nullptr);
    const UINT dpi = dc != nullptr ? static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)) : 96;
    if (dc != nullptr) {
        ReleaseDC(nullptr, dc);
    }
    return dpi > 0 ? dpi : 96;
}

std::wstring file_name(const std::string& utf8_path) {
    return app::path_from_utf8(utf8_path).filename().wstring();
}

std::wstring file_stem(const std::string& utf8_path) {
    return app::path_from_utf8(utf8_path).stem().wstring();
}

std::string parent_folder(const std::string& utf8_path) {
    return app::path_to_utf8(app::path_from_utf8(utf8_path).parent_path());
}

// Годится ли файл в плейлист: по расширению или по фактическому содержимому
// (переименованный WAV/FLAC тоже подойдёт, а .txt и прочий мусор — нет).
bool is_playable_candidate(const std::string& utf8_path) {
    if (playlist::is_audio_file(utf8_path)) {
        return true;
    }
    return media::sniff_format(utf8_path) != media::Format::Unknown;
}

// Выбор папки: современный IFileOpenDialog, при неудаче — старый SHBrowseForFolder.
// Используется и для «Открыть папку», и для «Добавить папку».
std::wstring pick_folder(HWND owner, const wchar_t* title, const std::string& start_dir) {
    std::wstring folder;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(title);
        if (!start_dir.empty()) {
            IShellItem* start = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(app::utf8_to_wide(start_dir).c_str(), nullptr,
                                                      IID_PPV_ARGS(&start)))) {
                dialog->SetFolder(start);
                start->Release();
            }
        }
        if (SUCCEEDED(dialog->Show(owner))) {
            IShellItem* result = nullptr;
            if (SUCCEEDED(dialog->GetResult(&result))) {
                PWSTR value = nullptr;
                if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &value))) {
                    folder = value;
                    CoTaskMemFree(value);
                }
                result->Release();
            }
        }
        dialog->Release();
    }

    if (folder.empty()) {                       // запасной путь для старых систем
        BROWSEINFOW browse{};
        browse.hwndOwner = owner;
        browse.lpszTitle = title;
        browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        LPITEMIDLIST item = SHBrowseForFolderW(&browse);
        if (item != nullptr) {
            wchar_t path[MAX_PATH] = {0};
            if (SHGetPathFromIDListW(item, path)) {
                folder = path;
            }
            CoTaskMemFree(item);
        }
    }
    return folder;
}

// Пути из объекта OLE-перетаскивания. Explorer и Проводник кладут список
// файлов форматом CF_HDROP — тот же, что приходит в WM_DROPFILES.
bool paths_from_data_object(IDataObject* data, std::vector<std::string>& paths) {
    if (data == nullptr) {
        return false;
    }
    FORMATETC format{};
    format.cfFormat = CF_HDROP;
    format.dwAspect = DVASPECT_CONTENT;
    format.lindex = -1;
    format.tymed = TYMED_HGLOBAL;

    STGMEDIUM medium{};
    if (FAILED(data->GetData(&format, &medium))) {
        return false;
    }

    bool ok = false;
    if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal != nullptr) {
        auto* drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
        if (drop != nullptr) {
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                if (length == 0) {
                    continue;
                }
                std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
                if (DragQueryFileW(drop, i, buffer.data(), length + 1) == 0) {
                    continue;
                }
                buffer.resize(length);
                paths.push_back(app::wide_to_utf8(buffer));
            }
            GlobalUnlock(medium.hGlobal);
            ok = true;
        }
    }
    ReleaseStgMedium(&medium);
    return ok;
}

}  // namespace

// Приём перетаскивания по правилам OLE: Windows сама сообщает о входе курсора
// в окно, движении и отпускании — поэтому зону приёма можно подсветить заранее,
// чего WM_DROPFILES не позволяет. Старый путь оставлен как запасной.
class PlayerDropTarget : public IDropTarget {
public:
    explicit PlayerDropTarget(PlayerApp* app) : app_(app) {}
    // Виртуальный деструктор: Release() удаляет объект через указатель на
    // интерфейс, без него удаление полиморфного класса — неопределённое поведение.
    virtual ~PlayerDropTarget() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (id == IID_IUnknown || id == IID_IDropTarget) {
            *object = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = InterlockedDecrement(&refs_);
        if (left == 0) {
            delete this;
        }
        return left;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        std::vector<std::string> paths;
        const bool has_files = paths_from_data_object(data, paths) && !paths.empty();
        app_->set_drop_active(has_files);        // подсветку включаем только под файлы
        if (effect != nullptr) {
            *effect = has_files ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        if (effect != nullptr) {
            *effect = app_->drop_active() ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        app_->set_drop_active(false);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL, DWORD* effect) override {
        app_->set_drop_active(false);
        if (effect != nullptr) {
            *effect = DROPEFFECT_COPY;
        }
        app_->accept_data_object(data);
        return S_OK;
    }

private:
    LONG refs_ = 1;
    PlayerApp* app_ = nullptr;
};

PlayerApp::PlayerApp(PlayerCore& core) : core_(core) {
    wchar_t profile[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) > 0) {
        last_dir_ = app::wide_to_utf8(profile);
    }
}

PlayerApp::~PlayerApp() {
    fonts_.destroy();
    if (list_brush_ != nullptr) {
        DeleteObject(list_brush_);
        list_brush_ = nullptr;
    }
}

int PlayerApp::px(double logical) const {
    return static_cast<int>(std::lround(logical * dpi_ / 96.0));
}

int PlayerApp::item_height() const {
    return px(26.0);
}

bool PlayerApp::create(HINSTANCE instance) {
    instance_ = instance;
    dpi_ = static_cast<int>(dpi_of(nullptr));
    Slider::register_class(instance);

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APPICON));
    window_class.hIconSm = window_class.hIcon;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = nullptr;      // фон рисуем сами
    window_class.lpszClassName = kWindowClass;
    if (RegisterClassExW(&window_class) == 0) {
        return false;
    }

    // WS_CLIPCHILDREN обязателен: без него перерисовка окна затирает кнопки,
    // список и ползунки — они мигают на каждом обновлении позиции.
    constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    // Задаём размеры по клиентской области, как geometry("540x520") в tkinter.
    RECT frame = {0, 0, px(540), px(520)};
    AdjustWindowRectEx(&frame, kWindowStyle, FALSE, 0);
    window_ = CreateWindowExW(0, kWindowClass, title_text().c_str(), kWindowStyle, CW_USEDEFAULT,
                              CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top,
                              nullptr, nullptr, instance, this);
    if (window_ != nullptr) {
        // Основной путь — OLE-перетаскивание: Windows сообщает о входе курсора
        // в окно, поэтому зону приёма видно до того, как файлы отпустят.
        // WM_DROPFILES (WS_EX_ACCEPTFILES) оставлен запасным: если регистрация
        // IDropTarget не удалась, перетаскивание всё равно работает.
        drop_target_ = new PlayerDropTarget(this);
        if (FAILED(RegisterDragDrop(window_, drop_target_))) {
            drop_target_->Release();
            drop_target_ = nullptr;
        }
        DragAcceptFiles(window_, TRUE);
        restore_state();                   // вернуть громкость, режимы, плейлист и позицию
    }
    return window_ != nullptr;
}

void PlayerApp::show() {
    if (window_ != nullptr) {
        ShowWindow(window_, SW_SHOW);
        UpdateWindow(window_);
    }
}

void PlayerApp::run() {
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (pre_dispatch(message)) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

// --------------------------------------------------------------------------- //
//  Горячие клавиши и курсор
// --------------------------------------------------------------------------- //
bool PlayerApp::pre_dispatch(MSG& message) {
    if (window_ == nullptr) {
        return false;
    }
    if (message.message == WM_KEYDOWN) {
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        return handle_key(message.wParam, ctrl, shift);
    }
    if (message.message == WM_MOUSEMOVE) {
        // Подсветка кнопки под курсором: у стандартной ownerdraw-кнопки её нет
        // без тем оформления, поэтому следим сами.
        POINT cursor{};
        GetCursorPos(&cursor);
        HWND under = WindowFromPoint(cursor);
        if (under != nullptr && (under == window_ || !IsChild(window_, under))) {
            under = nullptr;
        }
        if (under != nullptr) {
            wchar_t name[64] = {0};
            GetClassNameW(under, name, 63);
            if (_wcsicmp(name, L"Button") != 0) {
                under = nullptr;
            }
        }
        if (under != hot_button_) {
            HWND previous = hot_button_;
            hot_button_ = under;
            if (previous != nullptr) {
                InvalidateRect(previous, nullptr, FALSE);
            }
            if (under != nullptr) {
                InvalidateRect(under, nullptr, FALSE);
            }
        }
        return false;                       // сообщение кнопка должна получить сама
    }
    if (message.message == WM_SETCURSOR && LOWORD(message.lParam) == HTCLIENT) {
        wchar_t name[64] = {0};
        GetClassNameW(message.hwnd, name, 63);
        if (_wcsicmp(name, L"Button") == 0 || _wcsicmp(name, Slider::kClass) == 0) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return true;
        }
    }
    return false;
}

bool PlayerApp::handle_key(WPARAM key, bool ctrl, bool shift) {
    if (ctrl) {
        switch (key) {
            case 'O':
                // Ctrl+Shift+O — добавить папку к текущему плейлисту, не сбрасывая его.
                if (shift) {
                    add_folder_dialog();
                } else {
                    open_file_dialog();
                }
                return true;
            case 'S':
                // Ctrl+Shift+S — сохранить плейлист в M3U.
                if (shift) {
                    save_playlist_dialog();
                } else {
                    stop_playback();
                }
                return true;
            case VK_LEFT:
                prev_track();
                return true;
            case VK_RIGHT:
                next_track();
                return true;
            case '1':
            case '2':
            case '3': {
                eq_band_ = static_cast<int>(key - '1');   // выбрать полосу эквалайзера
                set_status(std::wstring(L"Полоса: ") + kEqBandNames[eq_band_] + L" — " +
                           app::utf8_to_wide(core_.eq_label()));
                return true;
            }
            case VK_UP:
            case VK_DOWN: {
                // Ctrl+стрелки двигают выбранную полосу, обычные — громкость.
                const double step =
                    key == VK_UP ? app::kEqStepDb : -app::kEqStepDb;
                core_.change_eq_gain(static_cast<PlayerCore::Band>(eq_band_), step);
                if (!core_.eq_enabled()) {
                    core_.set_eq_enabled(true);           // правка полосы включает эквалайзер
                }
                set_status(app::utf8_to_wide(core_.eq_label()));
                refresh();
                return true;
            }
            default:
                break;
        }
    }
    switch (key) {
        case VK_SPACE:
            toggle_playback();
            return true;
        case VK_LEFT:
            seek_by(-app::kSeekStep);
            return true;
        case VK_RIGHT:
            seek_by(app::kSeekStep);
            return true;
        case VK_UP:
            core_.change_volume(app::kVolumeStep);
            Slider::set_value(slider_volume_, std::lround(core_.volume() * 100.0));
            update_volume_label();
            return true;
        case VK_DOWN:
            core_.change_volume(-app::kVolumeStep);
            Slider::set_value(slider_volume_, std::lround(core_.volume() * 100.0));
            update_volume_label();
            return true;
        case VK_RETURN:
            play_selected();
            return true;
        case 'S':
            toggle_shuffle();
            return true;
        case 'R':
            cycle_repeat_mode();
            return true;
        case 'E':
            // Эквалайзер: включить/выключить. Полосы выбираются Ctrl+1..3,
            // усиление — Ctrl+стрелки вверх/вниз.
            core_.toggle_eq();
            set_status(app::utf8_to_wide(core_.eq_label()));
            refresh();
            return true;
        case VK_ESCAPE:
            SendMessageW(window_, WM_CLOSE, 0, 0);
            return true;
        default:
            break;
    }
    return false;
}

// --------------------------------------------------------------------------- //
//  Окно
// --------------------------------------------------------------------------- //
LRESULT CALLBACK PlayerApp::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    PlayerApp* app = reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        app = static_cast<PlayerApp*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (app != nullptr) {
        app->window_ = window;      // важно: WM_CREATE приходит раньше, чем вернётся CreateWindowExW
        return app->handle(window, message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT PlayerApp::handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_CREATE: {
            dpi_ = static_cast<int>(dpi_of(window));
            fonts_.create(dpi_);
            list_brush_ = CreateSolidBrush(theme::kPanel);
            create_children();
            layout();
            SetTimer(window, 1, app::kTickMs, nullptr);
            icon_ = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APPICON));
            if (icon_ != nullptr) {
                SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon_));
                SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon_));
            }
            return 0;
        }
        case WM_SIZE:
            layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            RECT frame = {0, 0, px(480), px(430)};
            AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, FALSE, 0);
            info->ptMinTrackSize.x = frame.right - frame.left;
            info->ptMinTrackSize.y = frame.bottom - frame.top;
            return 0;
        }
        case WM_DPICHANGED:
            apply_dpi(LOWORD(wparam), reinterpret_cast<const RECT*>(lparam));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window_, &ps);
            paint(dc);
            EndPaint(window_, &ps);
            return 0;
        }
        case WM_DRAWITEM:
            if (wparam == kIdList) {
                draw_list_item(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
            } else {
                draw_button(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
            }
            return TRUE;
        case WM_MEASUREITEM: {
            auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if (measure->CtlType == ODT_LISTBOX) {
                measure->itemHeight = static_cast<UINT>(item_height());
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, theme::kFg);
            SetBkColor(dc, theme::kPanel);
            if (list_brush_ != nullptr) {
                return reinterpret_cast<LRESULT>(list_brush_);
            }
            break;
        }
        case WM_COMMAND:
            on_command(LOWORD(wparam), HIWORD(wparam));
            return 0;
        case WM_DROPFILES: {
            const auto drop = reinterpret_cast<HDROP>(wparam);
            accept_dropped_files(drop);
            DragFinish(drop);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            const int lines = (delta > 0 ? -1 : 1) * 3;   // 3 строки за щелчок
            scroll_list_by(lines);
            return 0;
        }
        case WM_TIMER:
            if (wparam == 1) {
                on_timer();
            }
            return 0;
        case WM_SETFOCUS:
            return 0;
        case WM_CLOSE:
            closing_ = true;
            KillTimer(window_, 1);
            if (window_ != nullptr) {
                if (drop_target_ != nullptr) {
                    RevokeDragDrop(window_);
                    drop_target_->Release();       // OLE уже отпустил свою ссылку
                    drop_target_ = nullptr;
                }
                DragAcceptFiles(window_, FALSE);
            }
            persist_state();               // запомнить громкость, режимы, плейлист и позицию
            core_.close();
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            window_ = nullptr;
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void PlayerApp::create_children() {
    const DWORD button_style = WS_CHILD | WS_VISIBLE | BS_OWNERDRAW;
    open_file_ = CreateWindowExW(0, L"BUTTON", L"Открыть файл", button_style, 0, 0, 1, 1, window_,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdOpenFile)),
                                 instance_, nullptr);
    open_folder_ = CreateWindowExW(0, L"BUTTON", L"Открыть папку", button_style, 0, 0, 1, 1,
                                   window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdOpenFolder)),
                                   instance_, nullptr);
    prev_ = CreateWindowExW(0, L"BUTTON", L"« Пред", button_style | WS_DISABLED, 0, 0, 1, 1,
                            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPrev)),
                            instance_, nullptr);
    play_ = CreateWindowExW(0, L"BUTTON", L"▶ Играть", button_style | WS_DISABLED, 0, 0, 1, 1,
                            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPlay)),
                            instance_, nullptr);
    next_ = CreateWindowExW(0, L"BUTTON", L"След »", button_style | WS_DISABLED, 0, 0, 1, 1,
                            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdNext)),
                            instance_, nullptr);
    stop_ = CreateWindowExW(0, L"BUTTON", L"■ Стоп", button_style | WS_DISABLED, 0, 0, 1, 1,
                            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdStop)),
                            instance_, nullptr);
    shuffle_ = CreateWindowExW(0, L"BUTTON", L"Перемешать", button_style, 0, 0, 1, 1, window_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdShuffle)), instance_,
                               nullptr);
    repeat_ = CreateWindowExW(0, L"BUTTON", L"Повтор: выкл", button_style, 0, 0, 1, 1, window_,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdRepeat)), instance_,
                              nullptr);

    list_ = CreateWindowExW(0, L"LISTBOX", L"",
                            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | LBS_OWNERDRAWFIXED |
                                LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                            0, 0, 1, 1, window_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdList)), instance_,
                            nullptr);

    slider_position_ = Slider::create(window_, kIdPosition, false, 1.0, false, dpi_);
    slider_volume_ = Slider::create(window_, kIdVolume, false, 100.0, true, dpi_);
    scroll_ = Slider::create(window_, kIdScroll, true, 0.0, true, dpi_);
    Slider::set_value(slider_volume_, std::lround(core_.volume() * 100.0));
    Slider::set_enabled(slider_position_, false);
    Slider::set_enabled(scroll_, false);

    for (HWND control : {list_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.ui()), TRUE);
    }
    update_volume_label();      // приводим подпись громкости в соответствие с плеером
    update_mode_buttons();      // и подписи режимов (перемешивание/повтор)
    update_list_visibility();   // плейлист пуст — вместо списка будет подсказка
    UpdateWindow(window_);
}

void PlayerApp::apply_dpi(int dpi, const RECT* suggested) {
    if (dpi <= 0) {
        return;
    }
    dpi_ = dpi;
    fonts_.create(dpi_);
    for (HWND control : {open_file_, open_folder_, prev_, play_, next_, stop_, list_}) {
        if (control != nullptr) {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(fonts_.ui()), TRUE);
        }
    }
    for (HWND slider : {slider_position_, slider_volume_, scroll_}) {
        Slider::set_dpi(slider, dpi_);
    }
    if (suggested != nullptr) {
        SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    layout();
    InvalidateRect(window_, nullptr, FALSE);
}

void PlayerApp::layout() {
    if (window_ == nullptr) {
        return;
    }
    RECT client;
    GetClientRect(window_, &client);
    const int width = client.right;
    const int height = client.bottom;
    const int pad = px(16);
    const int gap = px(8);
    const int row_height = px(20);
    const int button_height = px(30);

    int y = px(14);
    layout_.title = {pad, y, width - pad - px(70), y + px(26)};
    layout_.count = {width - pad - px(70), y + px(4), width - pad, y + px(24)};
    y += px(26);
    layout_.artist = {pad, y, width - pad, y + px(18)};
    y += px(18) + px(10);

    const int time_width = px(46);
    layout_.current_time = {pad, y, pad + time_width, y + row_height};
    layout_.total_time = {width - pad - time_width, y, width - pad, y + row_height};
    layout_.position_slider = {pad + time_width + px(6), y + px(2), width - pad - time_width - px(6),
                               y + row_height - px(2)};
    y += row_height + px(12);

    const int open_width = px(140);
    int x = (width - (open_width * 2 + gap)) / 2;
    layout_.open_file = {x, y, x + open_width, y + button_height};
    x += open_width + gap;
    layout_.open_folder = {x, y, x + open_width, y + button_height};
    y += button_height + gap;

    const int prev_width = px(96);
    const int play_width = px(120);
    const int next_width = px(96);
    const int stop_width = px(96);
    const int controls = prev_width + play_width + next_width + stop_width + gap * 3;
    x = (width - controls) / 2;
    layout_.prev = {x, y, x + prev_width, y + button_height};
    x += prev_width + gap;
    layout_.play = {x, y, x + play_width, y + button_height};
    x += play_width + gap;
    layout_.next = {x, y, x + next_width, y + button_height};
    x += next_width + gap;
    layout_.stop = {x, y, x + stop_width, y + button_height};
    y += button_height + px(10);

    // Режимы: перемешивание и повтор (повтор — кнопка-переключатель трёх состояний).
    const int mode_width = px(150);
    x = (width - (mode_width * 2 + gap)) / 2;
    layout_.shuffle = {x, y, x + mode_width, y + button_height};
    x += mode_width + gap;
    layout_.repeat = {x, y, x + mode_width, y + button_height};
    y += button_height + px(10);

    const int volume_label_width = px(84);
    const int volume_value_width = px(52);
    layout_.volume_label = {pad, y, pad + volume_label_width, y + row_height};
    layout_.volume_value = {width - pad - volume_value_width, y, width - pad, y + row_height};
    layout_.volume_slider = {pad + volume_label_width + gap, y + px(2),
                             width - pad - volume_value_width - gap, y + row_height - px(2)};
    y += row_height + px(12);

    const int status_height = px(16);
    layout_.status = {pad, height - px(10) - status_height, width - pad, height - px(10)};
    const int scroll_width = px(10);
    const int list_bottom =
        std::max(y + px(40), static_cast<int>(layout_.status.top) - px(8));
    layout_.list = {pad, y, width - pad - scroll_width - px(2), list_bottom};
    layout_.scroll = {width - pad - scroll_width, y, width - pad, list_bottom};

    struct Item {
        HWND window;
        RECT rect;
    };
    const Item items[] = {{open_file_, layout_.open_file},   {open_folder_, layout_.open_folder},
                          {prev_, layout_.prev},             {play_, layout_.play},
                          {next_, layout_.next},             {stop_, layout_.stop},
                          {shuffle_, layout_.shuffle},       {repeat_, layout_.repeat},
                          {list_, layout_.list},             {slider_position_, layout_.position_slider},
                          {slider_volume_, layout_.volume_slider}, {scroll_, layout_.scroll}};
    for (const Item& item : items) {
        if (item.window != nullptr) {
            MoveWindow(item.window, item.rect.left, item.rect.top, item.rect.right - item.rect.left,
                       item.rect.bottom - item.rect.top, TRUE);
        }
    }
    if (list_ != nullptr) {
        SendMessageW(list_, LB_SETITEMHEIGHT, 0, item_height());
    }
    update_list_visibility();
    sync_scrollbar();
}

// --------------------------------------------------------------------------- //
//  Отрисовка
// --------------------------------------------------------------------------- //
void PlayerApp::draw_text(HDC dc, const std::wstring& text, const RECT& rect, HFONT font,
                          COLORREF color, UINT format) const {
    HGDIOBJ old_font = SelectObject(dc, font != nullptr ? font : fonts_.normal);
    const int old_mode = SetBkMode(dc, TRANSPARENT);
    const COLORREF old_color = SetTextColor(dc, color);
    RECT target = rect;
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &target, format);
    SetTextColor(dc, old_color);
    SetBkMode(dc, old_mode);
    SelectObject(dc, old_font);
}

void PlayerApp::paint(HDC dc) {
    RECT client;
    GetClientRect(window_, &client);
    const int width = client.right;
    const int height = client.bottom;
    if (width <= 0 || height <= 0) {
        return;
    }
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
    HBRUSH background = CreateSolidBrush(theme::kBg);
    FillRect(memory, &client, background);
    DeleteObject(background);

    draw_text(memory, title_label_, layout_.title, fonts_.title, theme::kFg,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    draw_text(memory, count_label_, layout_.count, fonts_.small, theme::kMuted,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    draw_text(memory, artist_label_, layout_.artist, fonts_.small, theme::kMuted,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    draw_text(memory, position_label_, layout_.current_time, fonts_.normal, theme::kMuted,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(memory, total_label_, layout_.total_time, fonts_.normal, theme::kMuted,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    draw_text(memory, L"Громкость", layout_.volume_label, fonts_.normal, theme::kMuted,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    draw_text(memory, volume_text_, layout_.volume_value, fonts_.normal, theme::kMuted,
              DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    draw_text(memory, status_, layout_.status, fonts_.tiny, theme::kMuted,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (queue_.empty()) {
        // Список скрыт, поэтому место плейлиста занимает подсказка.
        theme::fill_round_rect(memory, layout_.list, px(6.0), theme::kPanel);
        const wchar_t* hint = drop_active_ ? L"Отпустите — добавим в плейлист"
                                           : L"Перетащите сюда аудиофайлы или папку";
        draw_text(memory, hint, layout_.list, fonts_.normal,
                  drop_active_ ? theme::kAccentHot : theme::kMuted,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // Зона приёма: пока над окном несут файлы, обводим его рамкой. Рисуется
    // последней, чтобы оказаться поверх содержимого.
    if (drop_active_) {
        HBRUSH accent = CreateSolidBrush(theme::kAccent);
        RECT frame = client;
        const int thickness = px(3.0);
        for (int i = 0; i < thickness; ++i) {
            FrameRect(memory, &frame, accent);
            InflateRect(&frame, -1, -1);
        }
        DeleteObject(accent);
    }

    BitBlt(dc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
}

void PlayerApp::draw_button(const DRAWITEMSTRUCT& item) {
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool hot = (item.hwndItem == hot_button_) || ((item.itemState & ODS_HOTLIGHT) != 0);
    const bool checked = button_checked(item.hwndItem);   // включённый режим виден сразу

    HBRUSH background = CreateSolidBrush(theme::kBg);
    FillRect(item.hDC, &item.rcItem, background);
    DeleteObject(background);

    COLORREF fill = checked ? theme::kAccentDim : theme::kPanel;
    COLORREF text = checked ? theme::kWhite : theme::kFg;
    if (disabled) {
        fill = theme::kPanel;
        text = theme::kMuted;
    } else if (pressed) {
        fill = theme::kAccentDim;
        text = theme::kWhite;
    } else if (hot) {
        fill = theme::kAccent;
        text = theme::kWhite;
    }
    RECT rounded = item.rcItem;
    InflateRect(&rounded, -1, -1);
    theme::fill_round_rect(item.hDC, rounded, px(6), fill);

    wchar_t caption[64] = {0};
    GetWindowTextW(item.hwndItem, caption, 63);
    draw_text(item.hDC, caption, item.rcItem, fonts_.normal, text,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void PlayerApp::draw_list_item(const DRAWITEMSTRUCT& item) {
    if (item.itemID == static_cast<UINT>(-1)) {
        return;
    }
    const bool selected = (item.itemState & ODS_SELECTED) != 0;
    HBRUSH background = CreateSolidBrush(selected ? theme::kAccent : theme::kPanel);
    FillRect(item.hDC, &item.rcItem, background);
    DeleteObject(background);

    wchar_t text[512] = {0};
    SendMessageW(list_, LB_GETTEXT, item.itemID, reinterpret_cast<LPARAM>(text));
    RECT rect = item.rcItem;
    rect.left += px(10);
    rect.right -= px(6);
    draw_text(item.hDC, text, rect, fonts_.normal, selected ? theme::kWhite : theme::kFg,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// --------------------------------------------------------------------------- //
//  Команды интерфейса
// --------------------------------------------------------------------------- //
void PlayerApp::on_command(int id, int code) {
    switch (id) {
        case kIdOpenFile:
            open_file_dialog();
            return;
        case kIdOpenFolder:
            open_folder_dialog();
            return;
        case kIdPrev:
            prev_track();
            return;
        case kIdPlay:
            toggle_playback();
            return;
        case kIdNext:
            next_track();
            return;
        case kIdStop:
            stop_playback();
            return;
        case kIdShuffle:
            toggle_shuffle();
            return;
        case kIdRepeat:
            cycle_repeat_mode();
            return;
        case kIdList:
            if (code == LBN_DBLCLK) {
                play_selected();
            }
            return;
        case kIdPosition:
            if (code == static_cast<int>(Slider::kDragged)) {
                dragging_position_ = true;
                const std::wstring label =
                    app::utf8_to_wide(app::format_time(Slider::value(slider_position_)));
                if (label != position_label_) {
                    position_label_ = label;
                    InvalidateRect(window_, &layout_.current_time, FALSE);
                }
            } else if (code == static_cast<int>(Slider::kReleased)) {
                dragging_position_ = false;
                if (core_.seekable()) {
                    core_.seek(Slider::value(slider_position_));
                }
                refresh();
            }
            return;
        case kIdVolume:
            core_.set_volume(Slider::value(slider_volume_) / 100.0);
            update_volume_label();
            return;
        case kIdScroll:
            if (code == static_cast<int>(Slider::kDragged) ||
                code == static_cast<int>(Slider::kReleased)) {
                scroll_list_to(static_cast<int>(std::lround(Slider::value(scroll_))));
            }
            return;
        default:
            return;
    }
}

void PlayerApp::on_timer() {
    if (closing_) {
        return;
    }
    try {
        if (core_.poll()) {
            on_track_ended();
        }
        refresh();
    } catch (const std::exception& error) {
        log_crash(error.what());
    } catch (...) {
        log_crash("неизвестная ошибка");
    }
}

void PlayerApp::set_play_button_text() {
    if (play_ == nullptr) {
        return;
    }
    const wchar_t* caption = L"▶ Играть";
    if (core_.has_track()) {
        caption = core_.state() == PlayerCore::State::Playing ? L"|| Пауза" : L"▶ Играть";
    }
    wchar_t current[64] = {0};
    GetWindowTextW(play_, current, 63);
    if (_wcsicmp(current, caption) != 0) {
        SetWindowTextW(play_, caption);
        InvalidateRect(play_, nullptr, FALSE);
    }
}

// Перерисовываем только то, что действительно изменилось: иначе окно мигает
// на каждом тике таймера (5 раз в секунду).
void PlayerApp::refresh() {
    if (window_ == nullptr) {
        return;
    }
    const double position = core_.position();
    if (!dragging_position_) {
        Slider::set_value(slider_position_, core_.seekable() ? position : 0.0);
        const std::wstring label =
            app::utf8_to_wide(core_.seekable() ? app::format_time(position) : std::string("--:--"));
        if (label != position_label_) {
            position_label_ = label;
            InvalidateRect(window_, &layout_.current_time, FALSE);
        }
    }

    const bool has_track = core_.has_track();
    set_play_button_text();
    set_enabled(play_, has_track);
    set_enabled(stop_, has_track);
    set_enabled(prev_, has_track);
    set_enabled(next_, has_track);
    Slider::set_enabled(slider_position_, core_.seekable());

    const std::wstring title = track_title_.empty() ? L"Аудиоплеер" : track_title_;
    if (title != title_label_) {
        title_label_ = title;
        InvalidateRect(window_, &layout_.title, FALSE);
    }
    const std::wstring artist = track_artist_.empty() ? L"Откройте MP3-файл" : track_artist_;
    if (artist != artist_label_) {
        artist_label_ = artist;
        InvalidateRect(window_, &layout_.artist, FALSE);
    }
    const std::wstring total = total_time_text();
    if (total != total_label_) {
        total_label_ = total;
        InvalidateRect(window_, &layout_.total_time, FALSE);
    }
    const std::wstring count = count_text();
    if (count != count_label_) {
        count_label_ = count;
        InvalidateRect(window_, &layout_.count, FALSE);
    }

    sync_scrollbar();
}

void PlayerApp::set_enabled(HWND control, bool enabled) {
    if (control != nullptr && (IsWindowEnabled(control) != FALSE) != enabled) {
        EnableWindow(control, enabled ? TRUE : FALSE);
    }
}

void PlayerApp::update_volume_label() {
    if (volume_text_ != volume_label_text()) {
        volume_text_ = volume_label_text();
        InvalidateRect(window_, &layout_.volume_value, FALSE);
    }
}

std::wstring PlayerApp::volume_label_text() const {
    return std::to_wstring(static_cast<int>(std::lround(core_.volume() * 100.0))) + L" %";
}

void PlayerApp::set_status(const std::wstring& text) {
    if (status_ == text) {
        return;
    }
    status_ = text;
    if (window_ != nullptr) {
        InvalidateRect(window_, &layout_.status, FALSE);
    }
}

std::wstring PlayerApp::title_text() const {
    return std::wstring(L"Аудиоплеер ") + app::utf8_to_wide(app::kVersion);
}

std::wstring PlayerApp::total_time_text() const {
    if (!core_.seekable()) {
        return L"--:--";
    }
    return app::utf8_to_wide(app::format_time(core_.duration()));
}

std::wstring PlayerApp::count_text() const {
    if (queue_.size() > 1 && queue_.current() >= 0) {
        return std::to_wstring(queue_.current() + 1) + L"/" + std::to_wstring(queue_.size());
    }
    return std::wstring();
}

// --------------------------------------------------------------------------- //
//  Плейлист
// --------------------------------------------------------------------------- //
void PlayerApp::sync_playlist_view() {
    std::vector<std::wstring> names;
    names.reserve(queue_.tracks().size());
    for (const std::string& path : queue_.tracks()) {
        names.push_back(file_stem(path));
    }
    if (names != list_items_) {
        SendMessageW(list_, LB_RESETCONTENT, 0, 0);
        for (const std::wstring& name : names) {
            SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str()));
        }
        list_items_ = std::move(names);
        SendMessageW(list_, LB_SETITEMHEIGHT, 0, item_height());
    }
    update_list_visibility();
    highlight_current();
    sync_scrollbar();
}

// Пустой плейлист — вместо списка рисуем подсказку про перетаскивание.
void PlayerApp::update_list_visibility() {
    if (list_ == nullptr || scroll_ == nullptr) {
        return;
    }
    const bool visible = !queue_.empty();
    // Именно стиль окна, а не IsWindowVisible(): во время WM_CREATE родитель ещё
    // скрыт, и IsWindowVisible() вернул бы false для видимого списка.
    const bool shown = (GetWindowLongPtrW(list_, GWL_STYLE) & WS_VISIBLE) != 0;
    if (shown != visible) {
        ShowWindow(list_, visible ? SW_SHOW : SW_HIDE);
        ShowWindow(scroll_, visible ? SW_SHOW : SW_HIDE);
        InvalidateRect(window_, &layout_.list, FALSE);
    }
}

void PlayerApp::highlight_current() {
    if (list_ == nullptr) {
        return;
    }
    const int current = queue_.current();
    SendMessageW(list_, LB_SETCURSEL, current >= 0 ? static_cast<WPARAM>(current) : static_cast<WPARAM>(-1), 0);
    if (current < 0) {
        return;
    }
    RECT client;
    GetClientRect(list_, &client);
    const int list_height = static_cast<int>(client.bottom - client.top);
    const int visible = std::max(1, list_height / item_height());
    const int top = static_cast<int>(SendMessageW(list_, LB_GETTOPINDEX, 0, 0));
    if (current < top) {
        SendMessageW(list_, LB_SETTOPINDEX, static_cast<WPARAM>(current), 0);
    } else if (current >= top + visible) {
        SendMessageW(list_, LB_SETTOPINDEX, static_cast<WPARAM>(current - visible + 1), 0);
    }
}

void PlayerApp::sync_scrollbar() {
    if (scroll_ == nullptr || list_ == nullptr) {
        return;
    }
    RECT client;
    GetClientRect(list_, &client);
    const int list_height = static_cast<int>(client.bottom - client.top);
    const int visible = std::max(1, list_height / item_height());
    const int count = queue_.size();
    const int maximum = std::max(0, count - visible);
    Slider::set_maximum(scroll_, static_cast<double>(maximum));
    Slider::set_thumb_fraction(scroll_, count > 0 ? std::min(1.0, static_cast<double>(visible) / count) : 1.0);
    const int top = static_cast<int>(SendMessageW(list_, LB_GETTOPINDEX, 0, 0));
    Slider::set_value(scroll_, static_cast<double>(top));
    Slider::set_enabled(scroll_, maximum > 0);
}

void PlayerApp::scroll_list_to(int top) {
    if (list_ == nullptr) {
        return;
    }
    const int count = queue_.size();
    if (count == 0) {
        return;
    }
    RECT client;
    GetClientRect(list_, &client);
    const int list_height = static_cast<int>(client.bottom - client.top);
    const int visible = std::max(1, list_height / item_height());
    const int maximum = std::max(0, count - visible);
    top = std::min(maximum, std::max(0, top));
    SendMessageW(list_, LB_SETTOPINDEX, static_cast<WPARAM>(top), 0);
    sync_scrollbar();
}

void PlayerApp::scroll_list_by(int lines) {
    const int top = static_cast<int>(SendMessageW(list_, LB_GETTOPINDEX, 0, 0));
    scroll_list_to(top + lines);
}

void PlayerApp::play_selected() {
    const int index = static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0));
    if (index >= 0 && queue_.jump_to(index)) {
        play_from_queue(true, true);
    }
}

// --------------------------------------------------------------------------- //
//  Загрузка и воспроизведение
// --------------------------------------------------------------------------- //
bool PlayerApp::load(const std::string& path, bool autoplay, bool show_errors,
                     bool reset_playlist) {
    try {
        core_.open(path);
    } catch (const std::exception& error) {
        if (show_errors) {
            const std::wstring text = file_name(path) + L"\n\n" + app::utf8_to_wide(error.what());
            MessageBoxW(window_, text.c_str(), L"Не удалось открыть файл",
                        MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
            set_status(L"Ошибка загрузки");
        }
        return false;
    }

    if (reset_playlist) {
        queue_.set_tracks({path});
        list_items_.clear();
    }
    last_dir_ = parent_folder(core_.path());

    const tags::Info info = tags::read(path);
    track_title_ = info.title.empty() ? file_stem(path) : app::utf8_to_wide(info.title);
    track_artist_ = info.artist.empty() ? file_name(core_.path()) : app::utf8_to_wide(info.artist);

    const double duration = core_.duration();
    if (core_.seekable()) {
        Slider::set_maximum(slider_position_, duration);
        Slider::set_value(slider_position_, 0.0);
        Slider::set_enabled(slider_position_, true);
    } else {
        Slider::set_maximum(slider_position_, 1.0);
        Slider::set_value(slider_position_, 0.0);
        Slider::set_enabled(slider_position_, false);
    }

    if (autoplay) {
        core_.play();
        set_status(L"Играет");
    } else {
        set_status(L"Загружено");
    }
    sync_playlist_view();
    refresh();
    return true;
}

// Загружает текущий трек очереди, пропуская битые: молча при автопереходе,
// с диалогом — когда трек выбрал пользователь.
bool PlayerApp::play_from_queue(bool autoplay, bool interactive) {
    const int attempts = queue_.size();
    for (int attempt = 0; attempt < attempts; ++attempt) {
        const std::string path = queue_.current_path();
        if (path.empty()) {
            break;
        }
        if (load(path, autoplay, interactive, false)) {
            highlight_current();
            refresh();
            return true;
        }
        if (!queue_.next(false)) {          // ходить больше некуда
            break;
        }
    }
    if (queue_.empty()) {
        return false;
    }
    set_status(L"В плейлисте нечего играть");
    return false;
}

bool PlayerApp::load_folder(const std::string& folder) {
    std::vector<std::string> files = playlist::scan_folder(folder);
    if (files.empty()) {
        return false;
    }
    last_dir_ = folder;
    return start_playlist(std::move(files));
}

// Общий вход для папки и перетаскивания: поставить треки в плейлист и играть.
bool PlayerApp::start_playlist(std::vector<std::string> files) {
    if (files.empty()) {
        return false;
    }
    queue_.set_tracks(std::move(files));    // порядок учтёт включённое перемешивание
    list_items_.clear();
    update_mode_buttons();
    return play_from_queue(true, true);
}

// Добавить содержимое папки в конец текущего плейлиста, не сбивая воспроизведение.
bool PlayerApp::append_folder(const std::string& folder) {
    std::vector<std::string> files = playlist::scan_folder(folder);
    if (files.empty()) {
        return false;
    }
    last_dir_ = folder;
    const size_t before = static_cast<size_t>(queue_.size());
    const int added = queue_.append_tracks(std::move(files));
    if (added == 0) {
        set_status(L"Эти треки уже в плейлисте");
        return false;
    }
    list_items_.clear();
    update_mode_buttons();
    sync_playlist_view();
    refresh();
    const std::wstring text = std::wstring(queue_.empty() && before == 0 ? L"В плейлисте: " : L"Добавлено: ") +
                              std::to_wstring(added) + L" (всего " +
                              std::to_wstring(queue_.size()) + L")";
    set_status(text);
    if (before == 0) {                      // плейлист был пуст — сразу и играем
        play_from_queue(true, true);
    }
    return true;
}

// Единая точка входа для диалогов, командной строки и перетаскивания:
// плейлист-файл, папка или отдельный трек.
bool PlayerApp::open_path(const std::string& path, bool append) {
    std::error_code ec;
    if (playlist::is_playlist_file(path)) {
        std::vector<std::string> tracks = playlist::read_m3u(path);
        if (tracks.empty()) {
            set_status(L"В плейлисте нет доступных аудиофайлов");
            return false;
        }
        last_dir_ = parent_folder(path);
        if (append) {
            const int added = queue_.append_tracks(std::move(tracks));
            list_items_.clear();
            update_mode_buttons();
            sync_playlist_view();
            refresh();
            if (added == 0) {
                set_status(L"Эти треки уже в плейлисте");
                return false;
            }
            set_status(L"Добавлено из плейлиста: " + std::to_wstring(added));
            return true;
        }
        return start_playlist(std::move(tracks));
    }
    if (std::filesystem::is_directory(app::path_from_utf8(path), ec)) {
        return append ? append_folder(path) : load_folder(path);
    }
    if (append) {
        const int added = queue_.append_tracks({path});
        if (added > 0) {
            list_items_.clear();
            sync_playlist_view();
            refresh();
            set_status(L"Добавлено: " + file_name(path));
        }
        return added > 0;
    }
    return load(path);
}

// Собрать треки из произвольных путей: папки раскрываются, плейлисты читаются,
// не-аудио отбрасывается, дубликаты убираются. folders/skipped — для статуса.
std::vector<std::string> PlayerApp::collect_tracks(const std::vector<std::string>& paths,
                                                   int* folders, int* skipped) const {
    std::vector<std::string> tracks;
    std::set<std::string> seen;
    for (const std::string& path : paths) {
        std::error_code ec;
        std::vector<std::string> batch;
        if (std::filesystem::is_directory(app::path_from_utf8(path), ec)) {
            if (folders != nullptr) {
                ++*folders;
            }
            batch = playlist::scan_folder(path);      // внутри папки — естественный порядок
            if (batch.empty() && skipped != nullptr) {
                ++*skipped;
            }
        } else if (std::filesystem::is_regular_file(app::path_from_utf8(path), ec)) {
            if (playlist::is_playlist_file(path)) {
                batch = playlist::read_m3u(path);     // плейлист внутри плейлиста раскрываем
                if (batch.empty() && skipped != nullptr) {
                    ++*skipped;
                }
            } else if (is_playable_candidate(path)) {
                batch.push_back(path);
            } else if (skipped != nullptr) {
                ++*skipped;
            }
        }
        for (const std::string& track : batch) {
            if (seen.insert(track).second) {
                tracks.push_back(track);
            }
        }
    }
    return tracks;
}

// Общая обработка набора путей. Shift при перетаскивании — добавить, а не заменить.
void PlayerApp::accept_paths(const std::vector<std::string>& paths, bool dropped) {
    int folders = 0;
    int skipped = 0;
    std::vector<std::string> tracks = collect_tracks(paths, &folders, &skipped);
    const std::wstring verb = dropped ? L"Перетащено треков: " : L"Треков: ";

    if (tracks.empty()) {
        set_status(paths.size() > 1 ? L"В перетащенном нет аудиофайлов" : L"Это не аудиофайл");
        return;
    }

    const bool append = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (append) {
        const size_t before = tracks.size();
        const int added = queue_.append_tracks(std::move(tracks));
        list_items_.clear();
        update_mode_buttons();
        sync_playlist_view();
        refresh();
        if (added == 0) {
            set_status(L"Все эти треки уже в плейлисте");
            return;
        }
        std::wstring text = L"Добавлено: " + std::to_wstring(added) + L" из " +
                            std::to_wstring(before) + L" (всего " +
                            std::to_wstring(queue_.size()) + L")";
        if (skipped > 0) {
            text += L", пропущено: " + std::to_wstring(skipped);
        }
        set_status(text);
        return;
    }

    if (tracks.size() == 1) {                             // один файл — как «Открыть файл»
        load(tracks.front());
        return;
    }

    last_dir_ = parent_folder(tracks.front());
    const size_t total = tracks.size();
    if (start_playlist(std::move(tracks))) {
        std::wstring text = verb + std::to_wstring(total);
        if (folders > 1) {
            text += L" (папок: " + std::to_wstring(folders) + L")";
        }
        if (skipped > 0) {
            text += L", пропущено: " + std::to_wstring(skipped);
        }
        set_status(text);
    }
}

// Что делать с перетащенным в окно: HDROP освобождает вызывающий.
void PlayerApp::accept_dropped_files(HDROP drop) {
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    std::vector<std::string> paths;
    paths.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        const UINT length = DragQueryFileW(drop, i, nullptr, 0);
        if (length == 0) {
            continue;
        }
        std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
        if (DragQueryFileW(drop, i, buffer.data(), length + 1) == 0) {
            continue;
        }
        buffer.resize(length);
        paths.push_back(app::wide_to_utf8(buffer));
    }
    accept_paths(paths, true);
}

// Подсветка зоны приёма: рамка вокруг окна, пока над ним несут файлы.
void PlayerApp::set_drop_active(bool active) {
    if (drop_active_ == active) {
        return;
    }
    drop_active_ = active;
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void PlayerApp::accept_data_object(IDataObject* data) {
    std::vector<std::string> paths;
    if (!paths_from_data_object(data, paths) || paths.empty()) {
        set_status(L"Не удалось прочитать перетащенные файлы");
        return;
    }
    accept_paths(paths, true);
}

void PlayerApp::next_track() {
    if (queue_.empty()) {
        open_file_dialog();
        return;
    }
    if (queue_.next(false)) {               // кнопка «След» ходит по кругу
        play_from_queue(true, true);
    }
}

void PlayerApp::prev_track() {
    if (queue_.empty()) {
        return;
    }
    if (core_.position() > 3.0) {                // уже играли — сначала текущий трек
        core_.stop();
        core_.play();
        refresh();
        return;
    }
    if (queue_.previous()) {
        play_from_queue(true, true);
    }
}

void PlayerApp::toggle_shuffle() {
    queue_.set_shuffle(!queue_.shuffle());
    if (queue_.shuffle()) {
        set_status(queue_.size() > 1 ? L"Перемешивание включено" : L"Перемешивание: нужен плейлист");
    } else {
        set_status(L"Перемешивание выключено");
    }
    update_mode_buttons();
    highlight_current();
    sync_scrollbar();
}

void PlayerApp::cycle_repeat_mode() {
    queue_.cycle_repeat();
    switch (queue_.repeat()) {
        case playlist::Repeat::Off:
            set_status(L"Повтор выключен");
            break;
        case playlist::Repeat::All:
            set_status(L"Повтор папки включён");
            break;
        case playlist::Repeat::One:
            set_status(L"Повтор трека включён");
            break;
    }
    update_mode_buttons();
}

bool PlayerApp::button_checked(HWND control) const {
    if (control == shuffle_) {
        return queue_.shuffle();
    }
    if (control == repeat_) {
        return queue_.repeat() != playlist::Repeat::Off;
    }
    return false;
}

void PlayerApp::update_mode_buttons() {
    const wchar_t* shuffle_caption = queue_.shuffle() ? L"Перемешано" : L"Перемешать";
    const wchar_t* repeat_caption = L"Повтор: выкл";
    switch (queue_.repeat()) {
        case playlist::Repeat::All: repeat_caption = L"Повтор: папка"; break;
        case playlist::Repeat::One: repeat_caption = L"Повтор: трек"; break;
        case playlist::Repeat::Off: break;
    }
    struct Caption {
        HWND control;
        const wchar_t* text;
    };
    const Caption captions[] = {{shuffle_, shuffle_caption}, {repeat_, repeat_caption}};
    for (const Caption& item : captions) {
        if (item.control == nullptr) {
            continue;
        }
        wchar_t current[64] = {0};
        GetWindowTextW(item.control, current, 63);
        if (wcscmp(current, item.text) != 0) {
            SetWindowTextW(item.control, item.text);
            InvalidateRect(item.control, nullptr, FALSE);
        }
    }
}

void PlayerApp::toggle_playback() {
    if (!core_.has_track()) {
        open_file_dialog();
        return;
    }
    const PlayerCore::State state = core_.toggle();
    refresh();
    set_status(state == PlayerCore::State::Playing ? L"Играет" : L"Пауза");
}

void PlayerApp::stop_playback() {
    if (!core_.has_track()) {
        return;
    }
    core_.stop();
    refresh();
    set_status(L"Остановлено");
}

void PlayerApp::seek_by(double delta) {
    if (!core_.seekable()) {
        return;
    }
    core_.seek_by(delta);
    refresh();
}

void PlayerApp::on_track_ended() {
    // Что делать с доигранным треком, решает очередь: повтор трека, повтор папки,
    // следующий по списку или (когда играть больше нечего) сообщение в статусе.
    if (queue_.next(true)) {
        play_from_queue(true, false);
        return;
    }
    refresh();
    set_status(queue_.size() > 1 ? L"Плейлист закончился" : L"Трек закончился");
}

// --------------------------------------------------------------------------- //
//  Диалоги
// --------------------------------------------------------------------------- //
void PlayerApp::open_file_dialog() {
    wchar_t path[MAX_PATH * 4] = {0};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = kOpenFileFilter;
    dialog.lpstrFile = path;
    dialog.nMaxFile = sizeof(path) / sizeof(path[0]);
    dialog.lpstrTitle = L"Выберите аудиофайл";
    dialog.lpstrInitialDir = last_dir_.empty() ? nullptr : app::utf8_to_wide(last_dir_).c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog) != FALSE) {
        open_path(app::wide_to_utf8(path));      // файл, папка или плейлист .m3u
    }
}

void PlayerApp::open_folder_dialog() {
    const std::wstring folder = pick_folder(window_, L"Выберите папку с аудио", last_dir_);
    if (folder.empty()) {
        return;
    }

    const std::string utf8_folder = app::wide_to_utf8(folder);
    if (!load_folder(utf8_folder)) {
        std::wstring extensions;
        for (const char* extension : app::kAudioExtensions) {
            extensions += app::utf8_to_wide(extension) + L" ";
        }
        const std::wstring text =
            L"В папке нет аудиофайлов (" + extensions + L"):\n" + folder;
        MessageBoxW(window_, text.c_str(), L"Папка пуста", MB_OK | MB_ICONINFORMATION);
    }
}

// Добавить папку в конец текущего плейлиста (Ctrl+Shift+O).
void PlayerApp::add_folder_dialog() {
    const std::wstring folder = pick_folder(window_, L"Добавьте папку в плейлист", last_dir_);
    if (folder.empty()) {
        return;
    }
    const std::string utf8_folder = app::wide_to_utf8(folder);
    if (playlist::scan_folder(utf8_folder).empty()) {
        const std::wstring text = L"В папке нет аудиофайлов:\n" + folder;
        MessageBoxW(window_, text.c_str(), L"Папка пуста", MB_OK | MB_ICONINFORMATION);
        return;
    }
    append_folder(utf8_folder);        // «уже в плейлисте» append_folder скажет сам
}

// Сохранить текущий плейлист в M3U (Ctrl+Shift+S).
void PlayerApp::save_playlist_dialog() {
    if (queue_.empty()) {
        set_status(L"Плейлист пуст — сохранять нечего");
        return;
    }
    wchar_t path[MAX_PATH * 4] = {0};
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = kSavePlaylistFilter;
    dialog.lpstrFile = path;
    dialog.nMaxFile = sizeof(path) / sizeof(path[0]);
    dialog.lpstrTitle = L"Сохранить плейлист";
    dialog.lpstrDefExt = L"m3u";
    dialog.lpstrInitialDir = last_dir_.empty() ? nullptr : app::utf8_to_wide(last_dir_).c_str();
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&dialog) == FALSE) {
        return;                                  // пользователь отменил — это не ошибка
    }

    const std::string file = app::wide_to_utf8(path);
    if (playlist::write_m3u(file, queue_.tracks())) {
        last_dir_ = parent_folder(file);
        set_status(L"Плейлист сохранён: " + file_name(file));
    } else {
        MessageBoxW(window_, L"Не удалось записать файл плейлиста.", L"Ошибка сохранения",
                    MB_OK | MB_ICONERROR);
        set_status(L"Не удалось сохранить плейлист");
    }
}

void PlayerApp::open_from_command_line(const std::string& utf8_path) {
    std::error_code ec;
    const std::filesystem::path path = app::path_from_utf8(utf8_path);
    if (std::filesystem::is_directory(path, ec)) {
        if (!load_folder(utf8_path)) {
            set_status(L"В папке нет аудиофайлов");
        }
        return;
    }
    // Через open_path: в командной строке может прийти и .m3u.
    open_path(utf8_path);
}

// --------------------------------------------------------------------------- //
//  Состояние между запусками
// --------------------------------------------------------------------------- //
void PlayerApp::restore_state() {
    settings_path_ = settings::default_path();
    settings::State state;
    if (settings_path_.empty() || !settings::load(settings_path_, state)) {
        return;                                  // первый запуск: настроек ещё нет
    }

    core_.set_volume(state.volume);
    if (slider_volume_ != nullptr) {
        Slider::set_value(slider_volume_, std::lround(state.volume * 100.0));
    }
    update_volume_label();

    queue_.set_repeat(static_cast<playlist::Repeat>(state.repeat));
    queue_.set_shuffle(state.shuffle);
    core_.set_eq_gain(PlayerCore::Band::Low, state.eq_gain[0]);
    core_.set_eq_gain(PlayerCore::Band::Mid, state.eq_gain[1]);
    core_.set_eq_gain(PlayerCore::Band::High, state.eq_gain[2]);
    core_.set_eq_enabled(state.eq_enabled);
    update_mode_buttons();

    if (state.playlist.empty()) {
        refresh();
        return;
    }

    queue_.set_tracks(state.playlist);
    // Курсор ставим на трек, который играл. Если его в списке уже нет
    // (файл переименовали), берём сохранённый индекс.
    int index = -1;
    if (!state.track.empty()) {
        const std::vector<std::string>& tracks = queue_.tracks();
        for (size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i] == state.track) {
                index = static_cast<int>(i);
                break;
            }
        }
    }
    if (index < 0 && state.playlist_index >= 0 && state.playlist_index < queue_.size()) {
        index = state.playlist_index;
    }
    const bool has_track = index >= 0;
    const std::string track = has_track ? queue_.tracks()[static_cast<size_t>(index)] : std::string();
    if (has_track) {
        queue_.jump_to(index);
    }

    sync_playlist_view();
    if (!has_track || !load(track, false, false, false)) {
        set_status(L"Плейлист восстановлен");
        refresh();
        return;
    }

    // Загружено, но не играет: пользователь сам решит, продолжать ли с места.
    if (state.position > 0.0) {
        core_.seek(state.position);
        Slider::set_value(slider_position_, state.position);
        set_status(L"Продолжить с " + app::utf8_to_wide(app::format_time(state.position)) +
                   L" — пробел");
    } else {
        set_status(L"Плейлист восстановлен — пробел");
    }
    highlight_current();
    refresh();
}

void PlayerApp::persist_state() {
    if (settings_path_.empty()) {
        return;
    }
    settings::State state;
    state.volume = core_.volume();
    state.shuffle = queue_.shuffle();
    state.repeat = static_cast<int>(queue_.repeat());
    state.eq_enabled = core_.eq_enabled();
    state.eq_gain[0] = core_.eq_gain(PlayerCore::Band::Low);
    state.eq_gain[1] = core_.eq_gain(PlayerCore::Band::Mid);
    state.eq_gain[2] = core_.eq_gain(PlayerCore::Band::High);
    state.track = core_.path();
    state.position = core_.position();
    state.playlist = queue_.tracks();
    state.playlist_index = queue_.current();
    settings::save(settings_path_, state);
}

void PlayerApp::log_crash(const char* what) {
    if (crash_logged_) {
        return;
    }
    crash_logged_ = true;

    wchar_t temp[MAX_PATH] = {0};
    if (GetTempPathW(MAX_PATH, temp) == 0) {
        return;
    }
    const std::wstring path = std::wstring(temp) + L"audioplayer_crash.log";
    FILE* file = _wfopen(path.c_str(), L"a, ccs=UTF-8");
    if (file == nullptr) {
        return;
    }
    SYSTEMTIME time;
    GetLocalTime(&time);
    std::fwprintf(file, L"%04d-%02d-%02d %02d:%02d:%02d\n%s\n\n", time.wYear, time.wMonth,
                  time.wDay, time.wHour, time.wMinute, time.wSecond, app::utf8_to_wide(what).c_str());
    std::fclose(file);
    set_status(L"Ошибка интерфейса, лог: " + path);
}
