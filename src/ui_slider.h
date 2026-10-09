// Ползунок в тёмной теме: горизонтальный — позиция и громкость,
// вертикальный — полоса прокрутки плейлиста.
//
// Умеет то же, что tkinter.Scale в Python-версии: клик в любую точку дорожки
// сразу переставляет значение, перетаскивание — плавно, уведомления уходят
// родителю через WM_COMMAND (коды kDragged / kReleased).
#pragma once

#include <windows.h>

class Slider {
public:
    static constexpr const wchar_t* kClass = L"DshSlider";
    static constexpr UINT kDragged = 100;    // тянем: интерфейс показывает значение
    static constexpr UINT kReleased = 101;   // отпустили: можно применять

    static void register_class(HINSTANCE instance);

    static HWND create(HWND parent, int id, bool vertical, double maximum, bool integer_steps,
                       int dpi);

    static double value(HWND slider);
    static void set_value(HWND slider, double value);          // без уведомления родителю
    static void set_maximum(HWND slider, double maximum);
    static void set_thumb_fraction(HWND slider, double fraction);
    static void set_dpi(HWND slider, int dpi);
    static void set_enabled(HWND slider, bool enabled);
};
