// ============================================================================
//  OPEN SORCES FREE JOIN https://discord.gg/MMtdav2EJy FOR MORE!
// ============================================================================
// overlay.cpp - moldura roxa + selo "OWL GUARD PROTECTING YOU!"
//
// duas janelas topmost click-through acompanham o retangulo do Discord:
//   frame  - WS_EX_LAYERED, per-pixel alpha, halo roxo com SDF arredondado
//   badge  - pilula roxa com o texto, sem foco, sem bloquear clique
//
// nao injeta nada no cliente. funciona em qualquer versao do Discord.

#include "overlay.h"

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>

#ifndef DWMWA_EXTENDED_FRAME_BOUNDS
#define DWMWA_EXTENDED_FRAME_BOUNDS 9
#endif

namespace {

constexpr wchar_t kFrameClass[] = L"OwlOverlayFrame";
constexpr wchar_t kBadgeClass[] = L"OwlOverlayBadge";
constexpr wchar_t kBadgeText[] = L"OWL GUARD PROTECTING YOU!";
constexpr int kMargin = 18;          // raio do halo
constexpr int kBadgeH = 32;
constexpr int kBadgePad = 15;
constexpr UINT_PTR kTrackTimer = 1;
constexpr UINT kTrackMs = 350;

std::thread g_thread;
std::atomic<bool> g_running{false};
DWORD g_thread_id = 0;
HANDLE g_ready = nullptr;

HWND g_frame = nullptr;
HWND g_badge = nullptr;
HWND g_target = nullptr;
RECT g_last{};
int g_badge_w = 260;
bool g_discord_found = false;

// GDI objects do selo
HFONT g_badge_font = nullptr;

struct FindCtx {
    HWND best = nullptr;
    long long area = 0;
};

std::wstring file_name_of_process(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2]{};
    DWORD len = (DWORD)std::size(buf);
    std::wstring path;
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) path.assign(buf, len);
    CloseHandle(h);
    auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

bool is_discord_name(const std::wstring& name) {
    return _wcsicmp(name.c_str(), L"Discord.exe") == 0 ||
           _wcsicmp(name.c_str(), L"DiscordCanary.exe") == 0 ||
           _wcsicmp(name.c_str(), L"DiscordPTB.exe") == 0;
}

bool window_rect(HWND hwnd, RECT& out) {
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                        &out, sizeof(out)))) {
        if (out.right - out.left > 100 && out.bottom - out.top > 100) return true;
    }
    if (!GetWindowRect(hwnd, &out)) return false;
    return out.right - out.left > 100 && out.bottom - out.top > 100;
}

BOOL CALLBACK enum_windows(HWND hwnd, LPARAM param) {
    auto* ctx = reinterpret_cast<FindCtx*>(param);

    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) return TRUE;
    if (GetWindow(hwnd, GW_OWNER)) return TRUE;

    wchar_t cls[128]{};
    GetClassNameW(hwnd, cls, (int)std::size(cls));
    if (_wcsicmp(cls, L"Chrome_WidgetWin_1") != 0) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return TRUE;
    if (!is_discord_name(file_name_of_process(pid))) return TRUE;

    RECT rect{};
    if (!window_rect(hwnd, rect)) return TRUE;

    long long area = (long long)(rect.right - rect.left) * (rect.bottom - rect.top);
    if (area > ctx->area) {
        ctx->area = area;
        ctx->best = hwnd;
    }
    return TRUE;
}

HWND find_discord() {
    FindCtx ctx{};
    EnumWindows(enum_windows, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best;
}

inline float sd_round_rect(float px, float py, float cx, float cy,
                           float half_w, float half_h, float radius) {
    float qx = std::fabs(px - cx) - (half_w - radius);
    float qy = std::fabs(py - cy) - (half_h - radius);
    float ax = qx > 0 ? qx : 0.0f;
    float ay = qy > 0 ? qy : 0.0f;
    float outside = std::sqrt(ax * ax + ay * ay);
    float inside = std::fmin(std::fmax(qx, qy), 0.0f);
    return outside + inside - radius;
}

void render_frame(int width, int height) {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib) {
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ old = SelectObject(mem, dib);

    auto* pixels = static_cast<unsigned int*>(bits);
    const float half_w = (width - 2.0f * kMargin) * 0.5f;
    const float half_h = (height - 2.0f * kMargin) * 0.5f;
    const float cx = kMargin + half_w;
    const float cy = kMargin + half_h;
    const float radius = 9.0f;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            float d = sd_round_rect(x + 0.5f, y + 0.5f, cx, cy, half_w, half_h, radius);
            unsigned int out = 0;
            if (d > 0.0f && d < (float)kMargin) {
                float t = 1.0f - d / (float)kMargin;
                float a = t * t * 188.0f;
                if (d < 1.6f) a = 232.0f;
                if (a > 1.0f) {
                    // gradiente diagonal: #7C3AED -> #A855F7
                    float g = (float)(x + y) / (float)(width + height);
                    float r = 124.0f + (168.0f - 124.0f) * g;
                    float gg = 58.0f + (85.0f - 58.0f) * g;
                    float b = 237.0f + (247.0f - 237.0f) * g;
                    unsigned int alpha = (unsigned int)(a + 0.5f);
                    auto premul = [&](float c) {
                        return (unsigned int)((c * alpha / 255.0f) + 0.5f);
                    };
                    out = (alpha << 24) | (premul(r) << 16) | (premul(gg) << 8) | premul(b);
                }
            }
            pixels[y * width + x] = out;
        }
    }

    HDC dst = GetDC(nullptr);
    POINT src_pos{0, 0};
    SIZE size{width, height};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(g_frame, dst, nullptr, &size, mem, &src_pos, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, dst);

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

void paint_badge(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);

    RECT client{};
    GetClientRect(hwnd, &client);

    HBRUSH fill = CreateSolidBrush(RGB(124, 58, 237));
    HPEN border = CreatePen(PS_SOLID, 1, RGB(167, 139, 250));
    HGDIOBJ old_brush = SelectObject(dc, fill);
    HGDIOBJ old_pen = SelectObject(dc, border);
    RoundRect(dc, 0, 0, client.right, client.bottom, 16, 16);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(fill);
    DeleteObject(border);

    // ponto verde pulsante fixo
    HBRUSH dot = CreateSolidBrush(RGB(134, 239, 172));
    HGDIOBJ ob = SelectObject(dc, dot);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    int cy = (client.top + client.bottom) / 2;
    Ellipse(dc, 13, cy - 3, 19, cy + 3);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(dot);

    RECT text{22, 0, client.right - 8, client.bottom};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    HGDIOBJ old_font = SelectObject(dc, g_badge_font);
    DrawTextW(dc, kBadgeText, -1, &text,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old_font);

    EndPaint(hwnd, &ps);
}

void size_badge() {
    HDC screen = GetDC(nullptr);
    HGDIOBJ old = SelectObject(screen, g_badge_font);
    SIZE sz{};
    GetTextExtentPoint32W(screen, kBadgeText, (int)wcslen(kBadgeText), &sz);
    SelectObject(screen, old);
    ReleaseDC(nullptr, screen);
    g_badge_w = sz.cx + 44;
    HRGN region = CreateRoundRectRgn(0, 0, g_badge_w + 1, kBadgeH + 1, 16, 16);
    SetWindowRgn(g_badge, region, TRUE);
}

void place_overlay(const RECT& rect) {
    int frame_w = (rect.right - rect.left) + kMargin * 2;
    int frame_h = (rect.bottom - rect.top) + kMargin * 2;

    SetWindowPos(g_frame, HWND_TOPMOST,
                 rect.left - kMargin, rect.top - kMargin,
                 frame_w, frame_h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    render_frame(frame_w, frame_h);

    int bx = rect.right - g_badge_w - 18;
    int by = rect.bottom - kBadgeH - 16;
    SetWindowPos(g_badge, HWND_TOPMOST, bx, by, g_badge_w, kBadgeH,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_badge, nullptr, FALSE);
}

void hide_overlay() {
    ShowWindow(g_frame, SW_HIDE);
    ShowWindow(g_badge, SW_HIDE);
    g_target = nullptr;
    g_discord_found = false;
}

void track() {
    HWND discord = find_discord();
    if (!discord) {
        if (g_discord_found) hide_overlay();
        return;
    }

    RECT rect{};
    if (!window_rect(discord, rect)) {
        hide_overlay();
        return;
    }
    // janela minimizada/coberta pelo desktop
    if (IsIconic(discord) || rect.right - rect.left < 200) {
        hide_overlay();
        return;
    }

    // so aparece quando o Discord esta em primeiro plano; nao vaza por cima
    // de outros apps quando a janela do Discord esta atras
    HWND foreground = GetForegroundWindow();
    DWORD discord_pid = 0, foreground_pid = 0;
    GetWindowThreadProcessId(discord, &discord_pid);
    if (foreground) GetWindowThreadProcessId(foreground, &foreground_pid);
    if (foreground && foreground_pid != discord_pid) {
        hide_overlay();
        return;
    }

    bool changed = discord != g_target ||
                   std::memcmp(&rect, &g_last, sizeof(RECT)) != 0;
    g_target = discord;
    g_last = rect;
    g_discord_found = true;
    if (!changed && IsWindowVisible(g_frame)) return;
    place_overlay(rect);
}

LRESULT CALLBACK frame_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TIMER:
            if (wParam == kTrackTimer) {
                track();
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK badge_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_PAINT:
            paint_badge(hwnd);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST:
            return HTTRANSPARENT;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void thread_main() {
    g_thread_id = GetCurrentThreadId();

    WNDCLASSEXW frame_cls{};
    frame_cls.cbSize = sizeof(frame_cls);
    frame_cls.lpfnWndProc = frame_proc;
    frame_cls.hInstance = GetModuleHandleW(nullptr);
    frame_cls.lpszClassName = kFrameClass;
    RegisterClassExW(&frame_cls);

    WNDCLASSEXW badge_cls{};
    badge_cls.cbSize = sizeof(badge_cls);
    badge_cls.lpfnWndProc = badge_proc;
    badge_cls.hInstance = GetModuleHandleW(nullptr);
    badge_cls.lpszClassName = kBadgeClass;
    RegisterClassExW(&badge_cls);

    g_frame = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
        WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kFrameClass, L"", WS_POPUP, 0, 0, 1, 1,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    g_badge = CreateWindowExW(
        WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kBadgeClass, L"", WS_POPUP, 0, 0, 10, 10,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!g_frame || !g_badge) {
        if (g_ready) SetEvent(g_ready);
        return;
    }

    g_badge_font = CreateFontW(-11, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    size_badge();

    SetTimer(g_frame, kTrackTimer, kTrackMs, nullptr);

    if (g_ready) SetEvent(g_ready);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_badge_font) DeleteObject(g_badge_font);
    if (g_frame) DestroyWindow(g_frame);
    if (g_badge) DestroyWindow(g_badge);
    g_frame = g_badge = nullptr;
    g_thread_id = 0;
}

} // namespace

void overlay_start() {
    if (g_running.exchange(true)) return;
    g_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = std::thread(thread_main);
    WaitForSingleObject(g_ready, 2000);
    CloseHandle(g_ready);
    g_ready = nullptr;
}

void overlay_stop() {
    if (!g_running.exchange(false)) return;
    while (g_thread_id == 0 && g_thread.joinable()) Sleep(10);
    if (g_thread_id) PostThreadMessageW(g_thread_id, WM_QUIT, 0, 0);
    if (g_thread.joinable()) g_thread.join();
    g_target = nullptr;
    g_discord_found = false;
}

bool overlay_active() {
    return g_running.load() && g_discord_found;
}
