#include "ui_slider.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "ui_theme.h"

namespace {

struct SliderState {
    bool vertical = false;
    bool integer_steps = false;
    double maximum = 1.0;
    double value = 0.0;
    double thumb_fraction = 0.0;   // доля дорожки под ползунок (для полосы прокрутки)
    bool dragging = false;
    bool hot = false;
    bool tracking = false;
    int dpi = 96;
};

struct CreateConfig {
    bool vertical = false;
    bool integer_steps = false;
    double maximum = 1.0;
    int dpi = 96;
};

struct Metrics {
    RECT track = {0, 0, 0, 0};
    int thumb_size = 0;
    int span = 1;
};

SliderState* state_of(HWND slider) {
    return reinterpret_cast<SliderState*>(GetWindowLongPtrW(slider, GWLP_USERDATA));
}

int px(int dpi, double logical) {
    return static_cast<int>(std::lround(logical * dpi / 96.0));
}


Metrics metrics_of(const SliderState& state, const RECT& client) {
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    Metrics result;
    if (state.vertical) {
        const int track_width = std::max(4, px(state.dpi, 8.0));
        const int left = (width - track_width) / 2;
        result.track = {left, 0, left + track_width, height};
        if (state.thumb_fraction > 0.0) {
            result.thumb_size = static_cast<int>(std::lround(height * state.thumb_fraction));
        } else {
            result.thumb_size = px(state.dpi, 24.0);
        }
        result.thumb_size = std::max(px(state.dpi, 16.0), std::min(result.thumb_size, height));
        result.span = std::max(1, height - result.thumb_size);
    } else {
        const int track_height = std::max(4, px(state.dpi, 8.0));
        const int top = (height - track_height) / 2;
        result.track = {0, top, width, top + track_height};
        result.thumb_size = std::min(std::max(4, width), px(state.dpi, 14.0));
        result.span = std::max(1, width - result.thumb_size);
    }
    return result;
}

int offset_of(const SliderState& state, const Metrics& metrics) {
    if (state.maximum <= 0.0) {
        return 0;
    }
    const double ratio = std::min(1.0, std::max(0.0, state.value / state.maximum));
    return static_cast<int>(std::lround(ratio * metrics.span));
}

double value_at(const SliderState& state, const Metrics& metrics, POINT point) {
    const int along = state.vertical ? point.y : point.x;
    const double ratio =
        static_cast<double>(along - metrics.thumb_size / 2) / static_cast<double>(metrics.span);
    return std::min(1.0, std::max(0.0, ratio)) * state.maximum;
}

void apply_user_value(HWND slider, SliderState& state, double raw) {
    double value = std::min(state.maximum, std::max(0.0, raw));
    if (state.integer_steps) {
        value = std::floor(value + 0.5);
    }
    if (value != state.value) {
        state.value = value;
        InvalidateRect(slider, nullptr, FALSE);
    }
}

void notify(HWND slider, UINT code) {
    HWND parent = GetParent(slider);
    if (parent != nullptr) {
        SendMessageW(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(slider), code),
                     reinterpret_cast<LPARAM>(slider));
    }
}

void paint(HWND slider, SliderState& state) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(slider, &ps);
    RECT client;
    GetClientRect(slider, &client);
    const int width = client.right;
    const int height = client.bottom;
    if (width <= 0 || height <= 0) {
        EndPaint(slider, &ps);
        return;
    }

    // Рисуем в памяти: без этого ползунок мигает при перетаскивании.
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, width, height);
    HGDIOBJ old_bitmap = SelectObject(memory, bitmap);
    HBRUSH background = CreateSolidBrush(theme::kBg);
    FillRect(memory, &client, background);
    DeleteObject(background);

    const bool enabled = IsWindowEnabled(slider) != FALSE;
    const Metrics metrics = metrics_of(state, client);
    const int offset = offset_of(state, metrics);
    const int radius = std::max(2, px(state.dpi, 3.0));

    if (state.vertical) {
            theme::fill_round_rect(memory, metrics.track, radius, theme::kPanel);
        RECT thumb = {metrics.track.left, metrics.track.top + offset, metrics.track.right,
                      metrics.track.top + offset + metrics.thumb_size};
        const COLORREF color = !enabled ? theme::kPanel
                                        : (state.hot || state.dragging ? theme::kFg : theme::kMuted);
            theme::fill_round_rect(memory, thumb, radius, color);
    } else {
            theme::fill_round_rect(memory, metrics.track, radius, theme::kPanel);
        const int center = offset + metrics.thumb_size / 2;
        if (center > metrics.track.left) {
            RECT done = {metrics.track.left, metrics.track.top, center, metrics.track.bottom};
            theme::fill_round_rect(memory, done, radius, enabled ? theme::kAccent : theme::kAccentDim);
        }
        const int thumb_y = (height - metrics.thumb_size) / 2;
        RECT thumb = {offset, thumb_y, offset + metrics.thumb_size, thumb_y + metrics.thumb_size};
        COLORREF color = theme::kFg;
        if (!enabled) {
            color = theme::kAccentDim;
        } else if (state.dragging || state.hot) {
            color = theme::kAccentHot;
        }
            theme::fill_round_rect(memory, thumb, radius, color);
    }

    BitBlt(dc, 0, 0, width, height, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old_bitmap);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(slider, &ps);
}

LRESULT CALLBACK slider_proc(HWND slider, UINT message, WPARAM wparam, LPARAM lparam) {
    SliderState* state = state_of(slider);
    switch (message) {
        case WM_NCCREATE: {
            const auto* config = static_cast<const CreateConfig*>(
                reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
            auto* created = new SliderState();
            if (config != nullptr) {
                created->vertical = config->vertical;
                created->integer_steps = config->integer_steps;
                created->maximum = config->maximum;
                created->dpi = config->dpi;
            }
            SetWindowLongPtrW(slider, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
            return TRUE;
        }
        case WM_DESTROY:
            delete state;
            SetWindowLongPtrW(slider, GWLP_USERDATA, 0);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (state != nullptr) {
                paint(slider, *state);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            if (state == nullptr || !IsWindowEnabled(slider)) {
                return 0;
            }
            RECT client;
            GetClientRect(slider, &client);
            const POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            state->dragging = true;
            SetCapture(slider);
            apply_user_value(slider, *state, value_at(*state, metrics_of(*state, client), point));
            notify(slider, Slider::kDragged);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (state == nullptr) {
                return 0;
            }
            if (!state->tracking) {
                TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, slider, 0};
                TrackMouseEvent(&track);
                state->tracking = true;
            }
            if (!state->hot) {
                state->hot = true;
                InvalidateRect(slider, nullptr, FALSE);
            }
            if (state->dragging) {
                RECT client;
                GetClientRect(slider, &client);
                const POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                apply_user_value(slider, *state, value_at(*state, metrics_of(*state, client), point));
                notify(slider, Slider::kDragged);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (state == nullptr || !state->dragging) {
                return 0;
            }
            state->dragging = false;
            ReleaseCapture();
            notify(slider, Slider::kReleased);
            InvalidateRect(slider, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (state != nullptr) {
                state->tracking = false;
                if (state->hot) {
                    state->hot = false;
                    InvalidateRect(slider, nullptr, FALSE);
                }
            }
            return 0;
        case WM_MOUSEWHEEL: {
            // Колесо над ползунком прокручивает список: решение принимает родитель.
            HWND parent = GetParent(slider);
            if (parent != nullptr) {
                SendMessageW(parent, WM_MOUSEWHEEL, wparam, lparam);
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT && IsWindowEnabled(slider)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        default:
            break;
    }
    return DefWindowProcW(slider, message, wparam, lparam);
}

}  // namespace

void Slider::register_class(HINSTANCE instance) {
    static bool registered = false;
    if (registered) {
        return;
    }
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = slider_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kClass;
    RegisterClassExW(&window_class);
    registered = true;
}

HWND Slider::create(HWND parent, int id, bool vertical, double maximum, bool integer_steps,
                    int dpi) {
    CreateConfig config;
    config.vertical = vertical;
    config.integer_steps = integer_steps;
    config.maximum = maximum;
    config.dpi = dpi;
    return CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 1, 1, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE)),
                           &config);
}

double Slider::value(HWND slider) {
    const SliderState* state = state_of(slider);
    return state != nullptr ? state->value : 0.0;
}

void Slider::set_value(HWND slider, double value) {
    SliderState* state = state_of(slider);
    if (state == nullptr) {
        return;
    }
    const double clamped = std::min(state->maximum, std::max(0.0, value));
    if (clamped != state->value) {
        state->value = clamped;
        InvalidateRect(slider, nullptr, FALSE);
    }
}

void Slider::set_maximum(HWND slider, double maximum) {
    SliderState* state = state_of(slider);
    if (state == nullptr) {
        return;
    }
    maximum = std::max(0.0, maximum);
    if (state->maximum == maximum) {
        return;                             // ничего не изменилось — не перерисовываемся
    }
    state->maximum = maximum;
    if (state->value > state->maximum) {
        state->value = state->maximum;
    }
    InvalidateRect(slider, nullptr, FALSE);
}

void Slider::set_thumb_fraction(HWND slider, double fraction) {
    SliderState* state = state_of(slider);
    if (state == nullptr) {
        return;
    }
    fraction = std::min(1.0, std::max(0.0, fraction));
    if (state->thumb_fraction == fraction) {
        return;
    }
    state->thumb_fraction = fraction;
    InvalidateRect(slider, nullptr, FALSE);
}

void Slider::set_dpi(HWND slider, int dpi) {
    SliderState* state = state_of(slider);
    if (state != nullptr && state->dpi != dpi) {
        state->dpi = dpi;
        InvalidateRect(slider, nullptr, FALSE);
    }
}
void Slider::set_enabled(HWND slider, bool enabled) {
    if ((IsWindowEnabled(slider) != FALSE) != enabled) {
        EnableWindow(slider, enabled ? TRUE : FALSE);
        InvalidateRect(slider, nullptr, FALSE);
    }
}
