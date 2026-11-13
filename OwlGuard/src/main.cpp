// ============================================================================
//  OPEN SORCES FREE JOIN https://discord.gg/MMtdav2EJy FOR MORE!
// ============================================================================
// main.cpp - Owl Guard, interface
//
// janela borderless desenhada em GDI, tema escuro, acento Discord blurple.
// a caixa de autostart precisa estar marcada antes de qualquer ativacao.

#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

#include "guard.h"
#include "overlay.h"

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

namespace {

constexpr wchar_t kClass[] = L"VantaGuardWindow";
constexpr wchar_t kDiscordUrl[] = L"https://discord.gg/MMtdav2EJy";
constexpr int kBaseW = 620;
constexpr int kBaseH = 748;

// paleta
const COLORREF C_BG_TOP   = RGB(10, 12, 17);
const COLORREF C_BG_BOT   = RGB(14, 18, 26);
const COLORREF C_PANEL    = RGB(18, 22, 30);
const COLORREF C_PANEL_2  = RGB(22, 27, 37);
const COLORREF C_LINE     = RGB(33, 40, 53);
const COLORREF C_LINE_HOT = RGB(52, 62, 82);
const COLORREF C_TXT      = RGB(232, 236, 244);
const COLORREF C_MUT      = RGB(139, 148, 165);
const COLORREF C_DIM      = RGB(92, 101, 120);
const COLORREF C_ACC      = RGB(88, 101, 242);
const COLORREF C_ACC_HOT  = RGB(108, 121, 250);
const COLORREF C_OK       = RGB(67, 199, 141);
const COLORREF C_WARN     = RGB(240, 177, 74);
const COLORREF C_BAD      = RGB(237, 66, 69);

constexpr UINT_PTR kTimerAnim = 1;

enum Hot { HOT_NONE, HOT_CLOSE, HOT_MIN, HOT_CHIP, HOT_ROW1, HOT_ROW2, HOT_ROW3, HOT_BTN, HOT_LINK };

struct Layout {
    RECT header, closeb, minb, chip, card, options, btn, logp, footer;
    RECT row[3];
};

struct UiLog {
    Severity sev;
    std::wstring time;
    std::wstring text;
};

HWND g_hwnd = nullptr;
float g_scale = 1.0f;
float g_phase = 0.0f;
float g_pulse = 0.0f;

bool g_running = false;
bool g_opt_autostart = false;
bool g_opt_driver = true;
bool g_opt_kill = false;
bool g_auto_pending = false;
bool g_overlay_demo = false;
bool g_pending_start = false;

int g_hot = HOT_NONE;
unsigned long long g_flash_until = 0;
unsigned long long g_last_stats = 0;
GuardStats g_stats{};

HFONT g_f_title = nullptr;
HFONT g_f_sub = nullptr;
HFONT g_f_card = nullptr;
HFONT g_f_body = nullptr;
HFONT g_f_small = nullptr;
HFONT g_f_label = nullptr;
HFONT g_f_btn = nullptr;
HFONT g_f_mono = nullptr;

HDC g_back_dc = nullptr;
HBITMAP g_back_bmp = nullptr;
HGDIOBJ g_back_old = nullptr;
HDC g_frame_dc = nullptr;
HBITMAP g_frame_bmp = nullptr;
HGDIOBJ g_frame_old = nullptr;
int g_buf_w = 0, g_buf_h = 0;

std::vector<UiLog> g_logs;

// ---------------------------------------------------------------- util

inline int S(float v) { return (int)std::lround(v * g_scale); }

void fill_round(HDC dc, RECT r, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ ob = SelectObject(dc, brush);
    HGDIOBJ op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(brush);
    DeleteObject(pen);
}

void stroke_round(HDC dc, RECT r, int radius, COLORREF color, int width = 1) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
}

void text_at(HDC dc, const std::wstring& s, RECT r, HFONT font, COLORREF color,
             UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    HGDIOBJ old = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, s.c_str(), (int)s.size(), &r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

void text_at(HDC dc, const wchar_t* s, RECT r, HFONT font, COLORREF color,
             UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) {
    text_at(dc, std::wstring(s), r, font, color, flags);
}

COLORREF blend(COLORREF a, COLORREF b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
               (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

std::wstring now_string() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[16]{};
    _snwprintf_s(buf, _TRUNCATE, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring format_count(unsigned long long v) {
    std::wstring s = std::to_wstring(v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, L" ");
    return s;
}

HFONT make_font(float px, int weight) {
    LOGFONTW lf{};
    lf.lfHeight = -S(px);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, L"Segoe UI", LF_FACESIZE);
    return CreateFontIndirectW(&lf);
}

HFONT make_mono(float px, int weight) {
    LOGFONTW lf{};
    lf.lfHeight = -S(px);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, L"Consolas", LF_FACESIZE);
    return CreateFontIndirectW(&lf);
}

void destroy_fonts() {
    HFONT* fonts[] = {&g_f_title, &g_f_sub, &g_f_card, &g_f_body,
                      &g_f_small, &g_f_label, &g_f_btn, &g_f_mono};
    for (auto* f : fonts) {
        if (*f) DeleteObject(*f);
        *f = nullptr;
    }
}

void build_fonts() {
    destroy_fonts();
    g_f_title = make_font(23, FW_SEMIBOLD);
    g_f_sub = make_font(10, FW_NORMAL);
    g_f_card = make_font(17, FW_SEMIBOLD);
    g_f_body = make_font(10.5f, FW_NORMAL);
    g_f_small = make_font(9, FW_NORMAL);
    g_f_label = make_font(11.5f, FW_SEMIBOLD);
    g_f_btn = make_font(13, FW_SEMIBOLD);
    g_f_mono = make_mono(9.5f, FW_NORMAL);
}

void free_buffers() {
    if (g_back_dc) { SelectObject(g_back_dc, g_back_old); DeleteDC(g_back_dc); DeleteObject(g_back_bmp); }
    if (g_frame_dc) { SelectObject(g_frame_dc, g_frame_old); DeleteDC(g_frame_dc); DeleteObject(g_frame_bmp); }
    g_back_dc = nullptr; g_back_bmp = nullptr; g_back_old = nullptr;
    g_frame_dc = nullptr; g_frame_bmp = nullptr; g_frame_old = nullptr;
    g_buf_w = g_buf_h = 0;
}

void ensure_buffers(HDC ref, int w, int h) {
    if (g_buf_w == w && g_buf_h == h && g_frame_dc) return;
    free_buffers();
    g_buf_w = w; g_buf_h = h;

    g_back_dc = CreateCompatibleDC(ref);
    g_back_bmp = CreateCompatibleBitmap(ref, w, h);
    g_back_old = SelectObject(g_back_dc, g_back_bmp);

    for (int y = 0; y < h; ++y) {
        float t = (float)y / (float)std::max(1, h - 1);
        COLORREF c = blend(C_BG_TOP, C_BG_BOT, t);
        HPEN pen = CreatePen(PS_SOLID, 1, c);
        HGDIOBJ old = SelectObject(g_back_dc, pen);
        MoveToEx(g_back_dc, 0, y, nullptr);
        LineTo(g_back_dc, w, y);
        SelectObject(g_back_dc, old);
        DeleteObject(pen);
    }

    g_frame_dc = CreateCompatibleDC(ref);
    g_frame_bmp = CreateCompatibleBitmap(ref, w, h);
    g_frame_old = SelectObject(g_frame_dc, g_frame_bmp);
}

// ---------------------------------------------------------------- layout

Layout make_layout() {
    Layout l{};
    l.header = {S(24), S(18), S(596), S(96)};
    l.minb = {S(510), S(16), S(548), S(44)};
    l.closeb = {S(554), S(16), S(592), S(44)};
    l.chip = {S(348), S(54), S(592), S(88)};
    l.card = {S(24), S(104), S(596), S(272)};
    l.options = {S(24), S(288), S(596), S(462)};
    l.btn = {S(24), S(478), S(596), S(532)};
    l.logp = {S(24), S(548), S(596), S(676)};
    l.footer = {S(24), S(690), S(596), S(726)};
    l.row[0] = {S(36), S(322), S(584), S(364)};
    l.row[1] = {S(36), S(368), S(584), S(410)};
    l.row[2] = {S(36), S(414), S(584), S(456)};
    return l;
}

bool point_in(const RECT& r, POINT p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}

int hit_test(const Layout& l, POINT p) {
    if (point_in(l.closeb, p)) return HOT_CLOSE;
    if (point_in(l.minb, p)) return HOT_MIN;
    if (point_in(l.chip, p)) return HOT_CHIP;
    if (point_in(l.footer, p) && p.x < S(430)) return HOT_LINK;
    if (point_in(l.btn, p)) return HOT_BTN;
    if (!g_running) {
        if (point_in(l.row[0], p)) return HOT_ROW1;
        if (point_in(l.row[1], p)) return HOT_ROW2;
        if (point_in(l.row[2], p)) return HOT_ROW3;
    }
    return HOT_NONE;
}

// ---------------------------------------------------------------- drawing

void draw_shield(HDC dc, int cx, int cy, float scale, COLORREF fill) {
    auto p = [&](float x, float y) {
        POINT pt{};
        pt.x = cx + (int)std::lround(x * scale * g_scale);
        pt.y = cy + (int)std::lround(y * scale * g_scale);
        return pt;
    };
    POINT pts[5] = {p(0, -16), p(13, -10), p(13, 4), p(0, 16), p(-13, 4)};
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, fill);
    HGDIOBJ ob = SelectObject(dc, brush);
    HGDIOBJ op = SelectObject(dc, pen);
    Polygon(dc, pts, 5);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(brush);
    DeleteObject(pen);

    HPEN check = CreatePen(PS_SOLID, S(2.6f), RGB(255, 255, 255));
    HGDIOBJ old = SelectObject(dc, check);
    POINT c1 = p(-6.5f, 0.5f), c2 = p(-1.5f, 6.5f), c3 = p(8.5f, -5.5f);
    MoveToEx(dc, c1.x, c1.y, nullptr);
    LineTo(dc, c2.x, c2.y);
    LineTo(dc, c3.x, c3.y);
    SelectObject(dc, old);
    DeleteObject(check);
}

void draw_checkbox(HDC dc, RECT row, bool checked, bool hot, bool locked) {
    int size = S(19);
    int cy = (row.top + row.bottom) / 2;
    RECT box{S(44), cy - size / 2, S(44) + size, cy + size / 2};

    COLORREF border = checked ? C_ACC : (hot ? C_LINE_HOT : C_LINE);
    COLORREF fillc = checked ? C_ACC : C_PANEL_2;
    if (locked) { fillc = checked ? blend(C_ACC, C_PANEL, 0.55f) : C_PANEL; border = C_LINE; }

    fill_round(dc, box, S(6), fillc);
    stroke_round(dc, box, S(6), border, S(1));

    if (checked) {
        HPEN pen = CreatePen(PS_SOLID, S(2.2f),
                             locked ? blend(RGB(255, 255, 255), C_PANEL, 0.4f) : RGB(255, 255, 255));
        HGDIOBJ old = SelectObject(dc, pen);
        int x = box.left, y = box.top;
        POINT pts[3] = {
            {x + S(4), y + S(9)},
            {x + S(8), y + S(13)},
            {x + S(15), y + S(5)},
        };
        Polyline(dc, pts, 3);
        SelectObject(dc, old);
        DeleteObject(pen);
    }
}

void draw_pill(HDC dc, RECT r, const wchar_t* label, COLORREF fg, bool hot) {
    fill_round(dc, r, (r.bottom - r.top) / 2, hot ? C_PANEL_2 : C_PANEL);
    stroke_round(dc, r, (r.bottom - r.top) / 2, hot ? C_LINE_HOT : C_LINE, S(1));
    RECT dot{r.left + S(12), (r.top + r.bottom) / 2 - S(3),
             r.left + S(18), (r.top + r.bottom) / 2 + S(3)};
    HBRUSH b = CreateSolidBrush(C_ACC);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, dot.left, dot.top, dot.right, dot.bottom);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);

    RECT txt{r.left + S(26), r.top, r.right - S(12), r.bottom};
    text_at(dc, label, txt, g_f_small, fg);
}

void draw_ring(HDC dc, int cx, int cy, int radius) {
    RECT inner{cx - radius, cy - radius, cx + radius, cy + radius};
    HBRUSH b = CreateSolidBrush(RGB(13, 16, 23));
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, inner.left, inner.top, inner.right, inner.bottom);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);

    // trilha
    HPEN track = CreatePen(PS_SOLID, S(5), RGB(30, 37, 50));
    HGDIOBJ old = SelectObject(dc, track);
    HGDIOBJ oldb = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, inner.left, inner.top, inner.right, inner.bottom);
    SelectObject(dc, old);
    SelectObject(dc, oldb);
    DeleteObject(track);

    if (g_running) {
        int base = (int)(g_phase * 57.3f) % 360;
        auto arc = [&](int start_deg, int sweep_deg, COLORREF color) {
            const double pi = 3.14159265358979323846;
            double a1 = start_deg * pi / 180.0;
            double a2 = (start_deg + sweep_deg) * pi / 180.0;
            int x1 = cx + (int)std::lround(radius * std::cos(a1));
            int y1 = cy - (int)std::lround(radius * std::sin(a1));
            int x2 = cx + (int)std::lround(radius * std::cos(a2));
            int y2 = cy - (int)std::lround(radius * std::sin(a2));
            HPEN pen = CreatePen(PS_SOLID, S(5), color);
            HGDIOBJ o1 = SelectObject(dc, pen);
            HGDIOBJ o2 = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Arc(dc, inner.left, inner.top, inner.right, inner.bottom, x1, y1, x2, y2);
            SelectObject(dc, o1);
            SelectObject(dc, o2);
            DeleteObject(pen);
        };
        arc(base, 130, C_ACC);
        arc((base + 180) % 360, 70, blend(C_ACC, C_PANEL, 0.45f));
    }

    // miolo
    int cr = S(9);
    COLORREF dot = g_running ? blend(C_OK, RGB(255, 255, 255), g_pulse * 0.35f) : C_DIM;
    b = CreateSolidBrush(dot);
    ob = SelectObject(dc, b);
    op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, cx - cr, cy - cr, cx + cr, cy + cr);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
}

void draw_button(HDC dc, const Layout& l) {
    bool enabled = g_opt_autostart;
    COLORREF fillc = !enabled ? RGB(25, 30, 42)
                    : (g_hot == HOT_BTN ? C_ACC_HOT : C_ACC);
    if (g_running && g_hot == HOT_BTN) fillc = RGB(196, 55, 62);
    else if (g_running) fillc = RGB(170, 47, 55);

    fill_round(dc, l.btn, S(14), fillc);
    if (!enabled) stroke_round(dc, l.btn, S(14), RGB(40, 47, 62), S(1));

    const wchar_t* label;
    COLORREF fg;
    if (g_running) { label = L"DESATIVAR PROTEÇÃO"; fg = RGB(255, 255, 255); }
    else if (!enabled) { label = L"MARQUE A CAIXA DE INICIALIZAÇÃO"; fg = RGB(96, 106, 128); }
    else { label = L"ATIVAR PROTEÇÃO"; fg = RGB(255, 255, 255); }

    if ((GetTickCount64() < g_flash_until) && !enabled) {
        // pulso no contorno quando tentam ativar sem a caixa
        stroke_round(dc, l.btn, S(14), C_WARN, S(2));
        fg = C_WARN;
    }
    text_at(dc, label, l.btn, g_f_btn, fg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void draw_option_row(HDC dc, RECT row, int index, bool checked, bool hot,
                     const wchar_t* title, const wchar_t* desc) {
    bool locked = g_running;
    // hover
    if (hot && !locked) {
        RECT hl = row;
        hl.left += S(2); hl.right -= S(2);
        fill_round(dc, hl, S(10), RGB(21, 26, 35));
    }
    draw_checkbox(dc, row, checked, hot, locked);

    int cy = (row.top + row.bottom) / 2;
    RECT t{S(76), cy - S(17), row.right - S(16), cy - S(1)};
    RECT d{S(76), cy - S(1), row.right - S(16), cy + S(15)};
    text_at(dc, title, t, g_f_label, locked ? C_MUT : C_TXT);
    text_at(dc, desc, d, g_f_small, C_DIM);

    if (index < 2) {
        RECT line{S(76), row.bottom - S(1), row.right - S(16), row.bottom};
        HBRUSH b = CreateSolidBrush(RGB(26, 32, 43));
        FillRect(dc, &line, b);
        DeleteObject(b);
    }
}

void draw_logs(HDC dc, const Layout& l) {
    fill_round(dc, l.logp, S(14), RGB(15, 19, 26));
    stroke_round(dc, l.logp, S(14), C_LINE, S(1));

    RECT title{l.logp.left + S(18), l.logp.top + S(10), l.logp.right - S(18), l.logp.top + S(28)};
    text_at(dc, L"EVENTOS", title, g_f_small, C_DIM);

    RECT head{l.logp.left + S(18), l.logp.top + S(10), l.logp.right - S(18), l.logp.top + S(28)};
    text_at(dc, g_running ? L"ao vivo" : L"parado", head, g_f_small, g_running ? C_OK : C_DIM,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    int top = l.logp.top + S(34);
    int line_h = S(20);
    int max_lines = (l.logp.bottom - top - S(6)) / line_h;
    int count = (int)g_logs.size();
    int shown = std::min(count, max_lines);

    SaveDC(dc);
    IntersectClipRect(dc, l.logp.left + S(10), top, l.logp.right - S(10), l.logp.bottom - S(4));
    for (int i = 0; i < shown; ++i) {
        const UiLog& e = g_logs[count - 1 - i];
        int y = top + i * line_h;
        COLORREF c = C_MUT;
        switch (e.sev) {
            case Severity::Warn: c = C_WARN; break;
            case Severity::Danger: c = C_BAD; break;
            case Severity::Blocked: c = C_OK; break;
            default: c = RGB(120, 132, 154); break;
        }
        RECT dot{l.logp.left + S(18), y + line_h / 2 - S(3),
                 l.logp.left + S(24), y + line_h / 2 + S(3)};
        HBRUSH b = CreateSolidBrush(c);
        HGDIOBJ ob = SelectObject(dc, b);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, dot.left, dot.top, dot.right, dot.bottom);
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(b);

        RECT tr{l.logp.left + S(32), y, l.logp.right - S(76), y + line_h};
        text_at(dc, e.text, tr, g_f_mono, c);

        RECT tm{l.logp.right - S(72), y, l.logp.right - S(18), y + line_h};
        text_at(dc, e.time, tm, g_f_mono, C_DIM, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    RestoreDC(dc, -1);
}

void paint(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);

    RECT client{};
    GetClientRect(hwnd, &client);
    int w = client.right, h = client.bottom;
    ensure_buffers(dc, w, h);

    Layout l = make_layout();

    HDC f = g_frame_dc;
    BitBlt(f, 0, 0, w, h, g_back_dc, 0, 0, SRCCOPY);

    // linha superior sutil
    {
        HPEN pen = CreatePen(PS_SOLID, S(1), RGB(26, 32, 44));
        HGDIOBJ old = SelectObject(f, pen);
        MoveToEx(f, S(24), S(96), nullptr);
        LineTo(f, w - S(24), S(96));
        SelectObject(f, old);
        DeleteObject(pen);
    }

    // -------- header
    draw_shield(f, S(43), S(47), 1.0f, C_ACC);

    RECT title{S(68), S(24), S(330), S(56)};
    text_at(f, L"OWL GUARD", title, g_f_title, C_TXT);
    RECT sub{S(68), S(56), S(340), S(74)};
    text_at(f, L"anti discord grabber · watchdog + ring-0 shield", sub, g_f_sub, C_MUT);

    // janela: minimizar / fechar
    {
        COLORREF cc = (g_hot == HOT_CLOSE) ? C_BAD : C_DIM;
        HPEN pen = CreatePen(PS_SOLID, S(1.6f), cc);
        HGDIOBJ old = SelectObject(f, pen);
        int x = (l.closeb.left + l.closeb.right) / 2;
        int y = (l.closeb.top + l.closeb.bottom) / 2;
        int d = S(5);
        MoveToEx(f, x - d, y - d, nullptr); LineTo(f, x + d, y + d);
        MoveToEx(f, x + d, y - d, nullptr); LineTo(f, x - d, y + d);
        SelectObject(f, old);
        DeleteObject(pen);

        COLORREF mc = (g_hot == HOT_MIN) ? C_TXT : C_DIM;
        HPEN mpen = CreatePen(PS_SOLID, S(1.6f), mc);
        old = SelectObject(f, mpen);
        MoveToEx(f, x - d - S(64), y, nullptr);
        LineTo(f, x + d - S(64), y);
        SelectObject(f, old);
        DeleteObject(mpen);
    }

    draw_pill(f, l.chip, L"discord.gg/MMtdav2EJy", g_hot == HOT_CHIP ? C_TXT : C_MUT,
              g_hot == HOT_CHIP);

    // -------- status card
    fill_round(f, l.card, S(16), C_PANEL);
    stroke_round(f, l.card, S(16), C_LINE, S(1));

    int cx = l.card.left + S(74), cy = l.card.top + S(84);
    draw_ring(f, cx, cy, S(44));

    RECT st{l.card.left + S(146), l.card.top + S(22), l.card.right - S(18), l.card.top + S(52)};
    text_at(f, g_running ? L"PROTEGIDO" : L"EM ESPERA", st, g_f_card,
            g_running ? C_OK : C_MUT);

    RECT l1{l.card.left + S(146), l.card.top + S(52), l.card.left + S(360), l.card.top + S(72)};
    RECT l2{l.card.left + S(146), l.card.top + S(72), l.card.left + S(360), l.card.top + S(92)};
    RECT l3{l.card.left + S(146), l.card.top + S(92), l.card.left + S(360), l.card.top + S(112)};
    text_at(f, g_running ? L"watchdog ativo · processos monitorados" : L"nenhum módulo carregado",
            l1, g_f_body, g_running ? C_TXT : C_DIM);
    text_at(f,
            g_running ? (g_stats.driver_active ? L"driver ring-0 ativo · VM_READ bloqueado"
                                               : L"modo watchdog · sem driver")
                      : L"marque a inicialização e ative",
            l2, g_f_body, g_stats.driver_active && g_running ? C_ACC : C_DIM);
    text_at(f,
            g_running ? L"leveldb, navegadores e handles vigiados"
                      : L"proteção pronta para subir",
            l3, g_f_small, C_DIM);

    // stats alinhados à direita
    struct Stat { const wchar_t* label; std::wstring value; COLORREF color; };
    Stat stats[3] = {
        {L"analisados", format_count(g_stats.scanned), C_TXT},
        {L"ameaças", format_count(g_stats.threats), g_stats.threats ? C_WARN : C_MUT},
        {L"bloqueios", format_count(g_stats.blocked), g_stats.blocked ? C_OK : C_MUT},
    };
    int sy = l.card.top + S(32);
    for (auto& s : stats) {
        RECT label{l.card.right - S(200), sy, l.card.right - S(90), sy + S(18)};
        RECT value{l.card.right - S(160), sy, l.card.right - S(20), sy + S(18)};
        text_at(f, s.label, label, g_f_small, C_DIM, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        text_at(f, s.value, value, g_f_mono, s.color, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        sy += S(24);
    }

    // -------- options
    fill_round(f, l.options, S(16), C_PANEL);
    stroke_round(f, l.options, S(16), C_LINE, S(1));

    RECT otitle{l.options.left + S(20), l.options.top + S(10),
                l.options.right - S(20), l.options.top + S(30)};
    text_at(f, L"MÓDULOS", otitle, g_f_small, C_DIM);

    draw_option_row(f, l.row[0], 0, g_opt_autostart, g_hot == HOT_ROW1,
                    L"Iniciar com o Windows",
                    L"obrigatório para ativar · tarefa elevada / Run key");
    draw_option_row(f, l.row[1], 1, g_opt_driver, g_hot == HOT_ROW2,
                    L"Bloqueio de memória do Discord",
                    L"driver ring-0 · remove VM_READ de handles externos");
    draw_option_row(f, l.row[2], 2, g_opt_kill, g_hot == HOT_ROW3,
                    L"Encerrar processos suspeitos",
                    L"kill automático quando o score de grabber passa do limite");

    draw_button(f, l);
    draw_logs(f, l);

    // -------- footer
    {
        RECT left{l.footer.left, l.footer.top, l.footer.left + S(260), l.footer.bottom};
        text_at(f, L"OPEN SOURCE · FREE", left, g_f_small, C_DIM);
        RECT link{l.footer.left + S(122), l.footer.top, l.footer.left + S(300), l.footer.bottom};
        text_at(f, L"discord.gg/MMtdav2EJy", link, g_f_small,
                g_hot == HOT_LINK ? C_ACC_HOT : C_ACC);
        RECT ver{l.footer.right - S(120), l.footer.top, l.footer.right, l.footer.bottom};
        text_at(f, L"v1.0 · ring-0", ver, g_f_small, C_DIM,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    BitBlt(dc, 0, 0, w, h, f, 0, 0, SRCCOPY);
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------- acoes

void push_log(Severity sev, std::wstring text) {
    g_logs.push_back(UiLog{sev, now_string(), std::move(text)});
    if (g_logs.size() > 200) g_logs.erase(g_logs.begin(), g_logs.begin() + 100);
}

void start_protection() {
    if (g_running) return;
    if (!g_opt_autostart) {
        g_flash_until = GetTickCount64() + 1400;
        push_log(Severity::Warn, L"marque \"Iniciar com o Windows\" antes de ativar");
        InvalidateRect(g_hwnd, nullptr, FALSE);
        return;
    }
    if (!install_autostart(true)) {
        push_log(Severity::Danger, L"falha ao registrar inicialização automática");
        return;
    }
    GuardOptions opt{};
    opt.block_memory_scan = g_opt_driver;
    opt.kill_suspects = g_opt_kill;
    if (!guard_start(g_hwnd, opt)) {
        push_log(Severity::Danger, L"motor não iniciou");
        return;
    }
    g_running = true;
    g_phase = 0.0f;
    overlay_start();
    if (!is_elevated())
        push_log(Severity::Warn, L"sem elevação · driver indisponível, watchdog segue ativo");
    push_log(Severity::Info, L"halo roxo + selo acoplados à janela do Discord");
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void stop_protection() {
    if (!g_running) return;
    overlay_stop();
    guard_stop();
    g_running = false;
    g_auto_pending = false;
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void toggle_autostart() {
    if (g_running) {
        push_log(Severity::Warn, L"pare a proteção para alterar a inicialização");
        return;
    }
    g_opt_autostart = !g_opt_autostart;
    if (g_opt_autostart) {
        bool ok = install_autostart(true);
        push_log(ok ? Severity::Info : Severity::Danger,
                 ok ? L"inicialização registrada · logon / tarefa elevada"
                    : L"não foi possível registrar a inicialização");
        if (!ok) g_opt_autostart = false;
    } else {
        install_autostart(false);
        push_log(Severity::Info, L"inicialização automática removida");
    }
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void on_click(POINT p) {
    Layout l = make_layout();
    switch (hit_test(l, p)) {
        case HOT_CLOSE:
            PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
            break;
        case HOT_MIN:
            ShowWindow(g_hwnd, SW_MINIMIZE);
            break;
        case HOT_CHIP:
        case HOT_LINK:
            ShellExecuteW(nullptr, L"open", kDiscordUrl, nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case HOT_BTN:
            g_running ? stop_protection() : start_protection();
            break;
        case HOT_ROW1:
            toggle_autostart();
            break;
        case HOT_ROW2:
            if (g_running) { push_log(Severity::Warn, L"pare a proteção para alterar módulos"); break; }
            g_opt_driver = !g_opt_driver;
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        case HOT_ROW3:
            if (g_running) { push_log(Severity::Warn, L"pare a proteção para alterar módulos"); break; }
            g_opt_kill = !g_opt_kill;
            InvalidateRect(g_hwnd, nullptr, FALSE);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------- wndproc

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCCALCSIZE:
            if (wParam) return 0;
            break;

        case WM_NCHITTEST: {
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            POINT local{p.x - wr.left, p.y - wr.top};
            Layout l = make_layout();
            if (point_in(l.closeb, local) || point_in(l.minb, local) ||
                point_in(l.chip, local) || point_in(l.footer, local) ||
                point_in(l.btn, local) || point_in(l.options, local))
                return HTCLIENT;
            if (local.y < S(100)) return HTCAPTION;
            return HTCLIENT;
        }

        case WM_CREATE: {
            g_hwnd = hwnd;
            UINT dpi = GetDpiForWindow(hwnd);
            g_scale = (float)(dpi ? dpi : 96) / 96.0f;
            build_fonts();
            SetTimer(hwnd, kTimerAnim, 33, nullptr);
            g_opt_autostart = autostart_enabled();
            if (g_auto_pending) {
                g_opt_autostart = true;
                g_pending_start = true;
            }
            push_log(Severity::Info, L"owl guard pronto · discord.gg/MMtdav2EJy");
            if (g_overlay_demo) {
                overlay_start();
                push_log(Severity::Info, L"demo do overlay ativa · só o selo, sem proteção");
            }
            return 0;
        }

        case WM_DPICHANGED: {
            g_scale = (float)HIWORD(wParam) / 96.0f;
            build_fonts();
            free_buffers();
            RECT* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_TIMER:
            if (wParam == kTimerAnim) {
                if (g_running) {
                    g_phase += 0.075f;
                    if (g_phase > 6.28318f * 16.0f) g_phase = 0.0f;
                    g_pulse = 0.5f + 0.5f * std::sin(g_phase * 2.2f);
                }
                unsigned long long now = GetTickCount64();
                if (now - g_last_stats > 400) {
                    g_last_stats = now;
                    if (g_running) g_stats = guard_stats();
                }
                if (g_pending_start && g_opt_autostart) {
                    g_pending_start = false;
                    start_protection();
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_MOUSEMOVE: {
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int hot = hit_test(make_layout(), p);
            if (hot != g_hot) {
                g_hot = hot;
                bool hand = hot == HOT_CHIP || hot == HOT_LINK || hot == HOT_BTN ||
                            hot == HOT_ROW1 || hot == HOT_ROW2 || hot == HOT_ROW3 ||
                            hot == HOT_CLOSE || hot == HOT_MIN;
                SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            return 0;
        }

        case WM_MOUSELEAVE:
            g_hot = HOT_NONE;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDOWN: {
            POINT p{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            on_click(p);
            return 0;
        }

        case WM_GUARD_EVENT: {
            auto* ev = reinterpret_cast<LogEvent*>(lParam);
            if (ev) {
                push_log(ev->sev, ev->text);
                delete ev;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        case WM_PAINT:
            paint(hwnd);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE && g_running) { stop_protection(); return 0; }
            break;

        case WM_CLOSE:
            stop_protection();
            overlay_stop();
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kTimerAnim);
            free_buffers();
            destroy_fonts();
            g_hwnd = nullptr;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetProcessShutdownParameters(0x3FF, SHUTDOWN_NORETRY);
    g_scale = (float)(GetDpiForSystem() ? GetDpiForSystem() : 96) / 96.0f;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (int i = 1; i < argc; ++i) {
            if (_wcsicmp(argv[i], L"--autostart") == 0) g_auto_pending = true;
            if (_wcsicmp(argv[i], L"--overlay-demo") == 0) g_overlay_demo = true;
        }
        LocalFree(argv);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DROPSHADOW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClass;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return 1;

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = (int)std::lround(kBaseW * g_scale);
    int h = (int)std::lround(kBaseH * g_scale);
    int x = work.right - w - 24;
    int y = work.bottom - h - 24;
    if (x < 0) x = (work.right - w) / 2;
    if (y < 0) y = (work.bottom - h) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kClass, L"Owl Guard",
                                WS_POPUP | WS_MINIMIZEBOX,
                                x, y, w, h, nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 2;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    int corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
