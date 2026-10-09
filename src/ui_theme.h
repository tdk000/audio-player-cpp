// Тёмная тема: цвета и шрифты интерфейса.
#pragma once

#include <windows.h>

namespace theme {

inline constexpr COLORREF kBg = RGB(0x1e, 0x1e, 0x26);
inline constexpr COLORREF kPanel = RGB(0x2c, 0x2c, 0x38);
inline constexpr COLORREF kFg = RGB(0xec, 0xec, 0xf2);
inline constexpr COLORREF kMuted = RGB(0x9a, 0x9a, 0xa8);
inline constexpr COLORREF kAccent = RGB(0x5a, 0xa2, 0xff);
inline constexpr COLORREF kAccentHot = RGB(0x8c, 0xc0, 0xff);
inline constexpr COLORREF kAccentDim = RGB(0x3f, 0x6d, 0xa8);
inline constexpr COLORREF kWhite = RGB(0xff, 0xff, 0xff);

// Заливка скруглённого прямоугольника (кнопки, ползунки, полоса прокрутки).
inline void fill_round_rect(HDC dc, const RECT& rect, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_brush = SelectObject(dc, brush);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_brush);
    DeleteObject(pen);
    DeleteObject(brush);
}

// Segoe UI есть на любой Windows 7+ — как и в Python-версии.
inline HFONT create_font(int point_size, int dpi, bool bold = false) {
    return CreateFontW(-MulDiv(point_size, dpi, 72), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

struct Fonts {
    HFONT title = nullptr;    // 14 pt, полужирный — название трека
    HFONT normal = nullptr;   // 11 pt — кнопки, список, время
    HFONT small = nullptr;    // 10 pt — исполнитель, счётчик, громкость
    HFONT tiny = nullptr;     // 9 pt — строка состояния

    void create(int dpi) {
        destroy();
        title = create_font(14, dpi, true);
        normal = create_font(11, dpi);
        small = create_font(10, dpi);
        tiny = create_font(9, dpi);
    }

    void destroy() {
        for (HFONT* font : {&title, &normal, &small, &tiny}) {
            if (*font != nullptr) {
                DeleteObject(*font);
                *font = nullptr;
            }
        }
    }

    // Для списка и кнопок используем один и тот же «обычный» шрифт.
    HFONT ui() const { return normal; }
};

}  // namespace theme
