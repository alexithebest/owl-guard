// guard.h - API do motor de protecao (userland)
#pragma once

#include <windows.h>
#include <string>

#define WM_GUARD_EVENT (WM_APP + 42)   // lParam = LogEvent*, receptor libera

enum class Severity { Info, Warn, Danger, Blocked };

struct LogEvent {
    Severity sev;
    std::wstring text;
};

struct GuardOptions {
    bool block_memory_scan = true;   // driver ring-0
    bool kill_suspects = false;      // termina processo com score alto
};

struct GuardStats {
    unsigned long long scanned;
    unsigned long long threats;
    unsigned long long blocked;
    bool driver_active;
};

bool guard_start(HWND hwnd, const GuardOptions& opt);
void guard_stop();
GuardStats guard_stats();

bool is_elevated();
bool driver_available();
bool driver_shutdown();

bool install_autostart(bool enable);
bool autostart_enabled();
bool install_driver_service();
