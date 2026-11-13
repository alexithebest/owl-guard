// guard.cpp - motor de protecao anti-token-grabber
//
// camadas:
//   1. process watch    - todo processo novo tem a imagem analisada (IOC por familia,
//                         assinatura digital, padrao de token, webhook)
//   2. handle watch     - varre a tabela de handles do sistema e detecta qualquer
//                         processo nao confiavel lendo a memoria do Discord
//   3. file watch       - vigia leveldb do Discord e perfis de navegador, usa
//                         Restart Manager para saber quem abriu o arquivo
//   4. driver ring-0    - ObRegisterCallbacks remove VM_READ/VM_WRITE das handles
//                         abertas contra o Discord (bloqueio real, nao so alerta)

#include "guard.h"
#include "shared.h"

#include <windows.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <restartmanager.h>
#include <softpub.h>
#include <wintrust.h>
#include <shlobj.h>
#include <shellapi.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr NTSTATUS k_status_info_mismatch = (NTSTATUS)0xC0000004L;
constexpr ULONG k_system_extended_handle_info = 64;
constexpr size_t k_max_image_scan = 12ull * 1024 * 1024;

std::atomic<bool>  g_run{false};
HWND               g_hwnd = nullptr;
GuardOptions       g_opt{};
std::vector<std::thread> g_threads;

std::atomic<unsigned long long> g_scanned{0};
std::atomic<unsigned long long> g_threats{0};
std::atomic<unsigned long long> g_blocked{0};
std::atomic<bool> g_driver_active{false};

std::mutex g_prot_mtx;
std::set<DWORD> g_protected;

std::mutex g_driver_mtx;
HANDLE g_driver = INVALID_HANDLE_VALUE;

// ---------------------------------------------------------------- utilitarios

std::wstring lower_copy(std::wstring s) {
    for (auto& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

bool contains_ci(const std::wstring& hay, const wchar_t* needle) {
    return lower_copy(hay).find(lower_copy(needle)) != std::wstring::npos;
}

bool starts_with_ci(const std::wstring& hay, const std::wstring& prefix) {
    if (hay.size() < prefix.size()) return false;
    return _wcsnicmp(hay.c_str(), prefix.c_str(), prefix.size()) == 0;
}

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = buf;
    auto pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? p : p.substr(0, pos);
}

std::wstring known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) return {};
    std::wstring out = raw;
    CoTaskMemFree(raw);
    return out;
}

std::wstring process_path(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2]{};
    DWORD len = (DWORD)std::size(buf);
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) out.assign(buf, len);
    CloseHandle(h);
    return out;
}

std::wstring known_exe_name(const std::wstring& path) {
    auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

void post(Severity sev, std::wstring text) {
    auto* ev = new LogEvent{sev, std::move(text)};
    if (!g_hwnd || !PostMessageW(g_hwnd, WM_GUARD_EVENT, 0, (LPARAM)ev)) delete ev;
}

void postf(Severity sev, const wchar_t* fmt, ...) {
    wchar_t buf[1024]{};
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, std::size(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    post(sev, buf);
}

// ---------------------------------------------------------------- assinatura

bool is_signed(const std::wstring& path) {
    WINTRUST_FILE_INFO file_info{};
    file_info.cbStruct = sizeof(file_info);
    file_info.pcwszFilePath = path.c_str();

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file_info;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_SAFER_FLAG;

    LONG status = WinVerifyTrust(nullptr, &action, &data);
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &data);
    return status == ERROR_SUCCESS;
}

bool in_trusted_dir(const std::wstring& path) {
    wchar_t win[MAX_PATH]{}, pf[MAX_PATH]{}, pf86[MAX_PATH]{};
    GetWindowsDirectoryW(win, MAX_PATH);
    GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
    GetEnvironmentVariableW(L"ProgramFiles(x86)", pf86, MAX_PATH);
    return starts_with_ci(path, win) || starts_with_ci(path, pf) ||
           (!pf86[0] ? false : starts_with_ci(path, pf86));
}

// ---------------------------------------------------------------- IOC

const wchar_t* k_family_iocs[] = {
    L"blankgrabber", L"redline", L"raccoon", L"lumma", L"vidar", L"stealc",
    L"metastealer", L"risepro", L"aurora stealer", L"warzone", L"predator",
    L"kematian", L"pandora", L"meduza", L"sharptooth", L"whitesnake",
    L"onyx stealer", L"jetlogger", L"grabber", L"tokenlogger", L"stealer"
};

const char* k_binary_iocs[] = {
    "Local Storage\\leveldb",
    "discord\\Local Storage",
    "discordapp.com/api/webhooks",
    "discord.com/api/webhooks",
    "/api/webhooks/",
    "DumpTokens",
    "GrabTokens",
    "GetDiscordToken",
    "Login Data",
    "Local State",
    "GetAsyncKeyState",
    "sqlite3"
};

// token: base64(id) '.' base64(timestamp) '.' base64(hmac) - 24..28 / 6..7 / 25..40
bool contains_discord_token(const std::string& s) {
    auto cls = [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-';
    };
    const size_t n = s.size();
    size_t i = 0;
    while (i + 60 < n) {
        if (!cls((unsigned char)s[i])) { ++i; continue; }
        size_t j = i;
        while (j < n && cls((unsigned char)s[j])) ++j;
        size_t l1 = j - i;
        if (l1 >= 24 && l1 <= 28 && j < n && s[j] == '.') {
            size_t k = j + 1;
            while (k < n && cls((unsigned char)s[k])) ++k;
            size_t l2 = k - j - 1;
            if (l2 >= 6 && l2 <= 7 && k < n && s[k] == '.') {
                size_t l = k + 1;
                while (l < n && cls((unsigned char)s[l])) ++l;
                size_t l3 = l - k - 1;
                if (l3 >= 25 && l3 <= 40) return true;
                i = k;
                continue;
            }
        }
        i = j;
    }
    return false;
}

bool read_image(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size)) { CloseHandle(h); return false; }
    size_t n = (size_t)std::min<long long>(size.QuadPart, (long long)k_max_image_scan);
    if (n == 0) { CloseHandle(h); return true; }
    out.resize(n);
    DWORD got = 0;
    BOOL ok = ReadFile(h, out.data(), (DWORD)n, &got, nullptr);
    out.resize(ok ? got : 0);
    CloseHandle(h);
    return ok != FALSE;
}

// ---------------------------------------------------------------- driver

void driver_close() {
    std::lock_guard<std::mutex> lock(g_driver_mtx);
    if (g_driver != INVALID_HANDLE_VALUE) {
        CloseHandle(g_driver);
        g_driver = INVALID_HANDLE_VALUE;
    }
}

HANDLE driver_open() {
    std::lock_guard<std::mutex> lock(g_driver_mtx);
    if (g_driver != INVALID_HANDLE_VALUE) return g_driver;
    g_driver = CreateFileW(VANTA_USER_DEVICE, GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    return g_driver;
}

bool driver_ioctl(DWORD code, void* in, DWORD in_len) {
    HANDLE h = driver_open();
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD returned = 0;
    return DeviceIoControl(h, code, in, in_len, nullptr, 0, &returned, nullptr) != FALSE;
}

bool driver_service_start() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    bool ok = false;
    SC_HANDLE svc = OpenServiceW(scm, VANTA_SERVICE_NAME,
                                 SERVICE_START | SERVICE_QUERY_STATUS);
    if (svc) {
        if (StartServiceW(svc, 0, nullptr)) ok = true;
        else {
            SERVICE_STATUS st{};
            if (QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING) ok = true;
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok;
}

// ---------------------------------------------------------------- autostart

bool run_key_set(bool enable) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return false;
    bool ok;
    if (enable) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring quoted = L"\"" + std::wstring(path) + L"\"";
        ok = RegSetValueExW(key, L"OwlGuard", 0, REG_SZ,
                            (const BYTE*)quoted.c_str(),
                            (DWORD)((quoted.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        ok = RegDeleteValueW(key, L"OwlGuard") == ERROR_SUCCESS ||
             GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    RegCloseKey(key);
    return ok;
}

bool task_run(const std::wstring& args) {
    std::wstring cmd = L"schtasks.exe " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring mutable_cmd = cmd;
    if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 15000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

bool task_exists() {
    return task_run(L"/Query /TN \"OwlGuard\"");
}

void task_delete() {
    task_run(L"/Delete /TN \"OwlGuard\" /F");
}

std::wstring xml_escape(std::wstring s) {
    auto replace_all = [&](const std::wstring& from, const std::wstring& to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::wstring::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replace_all(L"&", L"&amp;");
    replace_all(L"<", L"&lt;");
    replace_all(L">", L"&gt;");
    return s;
}

bool task_create() {
    if (!is_elevated()) return false;
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
        L"  <RegistrationInfo><Description>Owl Guard - anti discord grabber</Description></RegistrationInfo>\n"
        L"  <Triggers><LogonTrigger><Enabled>true</Enabled><Delay>PT4S</Delay></LogonTrigger></Triggers>\n"
        L"  <Principals><Principal id=\"Author\"><LogonType>InteractiveToken</LogonType>"
        L"<RunLevel>HighestAvailable</RunLevel></Principal></Principals>\n"
        L"  <Settings>\n"
        L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\n"
        L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\n"
        L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\n"
        L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\n"
        L"    <Enabled>true</Enabled>\n"
        L"  </Settings>\n"
        L"  <Actions Context=\"Author\"><Exec><Command>" + xml_escape(self) + L"</Command>"
        L"<Arguments>--autostart</Arguments></Exec></Actions>\n"
        L"</Task>\n";

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    std::wstring file = std::wstring(temp) + L"owl_guard_task.xml";

    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const WORD bom = 0xFEFF;
    DWORD written = 0;
    bool ok = WriteFile(h, &bom, sizeof(bom), &written, nullptr) != FALSE;
    ok = ok && WriteFile(h, xml.data(), (DWORD)(xml.size() * sizeof(wchar_t)),
                         &written, nullptr) != FALSE;
    CloseHandle(h);
    if (!ok) { DeleteFileW(file.c_str()); return false; }

    ok = task_run(L"/Create /TN \"OwlGuard\" /XML \"" + file + L"\" /F");
    DeleteFileW(file.c_str());
    return ok;
}

// ---------------------------------------------------------------- analise

struct OwnerVerdict {
    bool trusted = false;
    unsigned long long at = 0;
};
std::mutex g_owner_mtx;
std::map<DWORD, OwnerVerdict> g_owner_cache;

bool owner_trusted(DWORD pid) {
    if (pid == 0 || pid == 4) return true;
    if (pid == GetCurrentProcessId()) return true;
    {
        std::lock_guard<std::mutex> lock(g_owner_mtx);
        auto it = g_owner_cache.find(pid);
        if (it != g_owner_cache.end() &&
            GetTickCount64() - it->second.at < 120000)
            return it->second.trusted;
    }
    std::wstring path = process_path(pid);
    bool trusted = true;
    if (!path.empty()) {
        std::wstring name = known_exe_name(path);
        static const wchar_t* k_safe_names[] = {
            L"chrome.exe", L"msedge.exe", L"brave.exe", L"opera.exe", L"firefox.exe",
            L"discord.exe", L"discordcanary.exe", L"discordptb.exe", L"explorer.exe",
            L"svchost.exe", L"taskmgr.exe", L"powershell.exe", L"cmd.exe",
            L"code.exe", L"devenv.exe", L"steam.exe", L"dwm.exe", L"csrss.exe"
        };
        trusted = in_trusted_dir(path) || is_signed(path);
        for (auto* safe : k_safe_names)
            if (_wcsicmp(name.c_str(), safe) == 0) { trusted = true; break; }
    }
    {
        std::lock_guard<std::mutex> lock(g_owner_mtx);
        g_owner_cache[pid] = OwnerVerdict{trusted, GetTickCount64()};
    }
    return trusted;
}

void protect_pid(DWORD pid) {
    {
        std::lock_guard<std::mutex> lock(g_prot_mtx);
        g_protected.insert(pid);
    }
    if (g_driver_active) driver_ioctl(IOCTL_VANTA_PROTECT_PID, &pid, sizeof(pid));
}

bool is_protected_pid(DWORD pid) {
    std::lock_guard<std::mutex> lock(g_prot_mtx);
    return g_protected.count(pid) != 0;
}

void terminate_suspect(DWORD pid, const std::wstring& why) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) return;
    if (TerminateProcess(h, 0)) {
        g_blocked.fetch_add(1);
        postf(Severity::Blocked, L"processo encerrado · pid %lu · %s", pid, why.c_str());
    }
    CloseHandle(h);
}

void analyze_image(DWORD pid, const std::wstring& path, const std::wstring& name) {
    g_scanned.fetch_add(1);
    std::wstring combined = path + L" " + name;
    int score = 0;
    std::wstring reasons;

    for (auto* ioc : k_family_iocs) {
        if (contains_ci(combined, ioc)) {
            score += 60;
            reasons += L"IOC familia ";
            reasons += ioc;
            reasons += L" · ";
            break;
        }
    }

    std::string data;
    if (read_image(path, data)) {
        if (contains_discord_token(data)) {
            score += 45;
            reasons += L"padrao de token no binario · ";
        }
        int hits = 0;
        for (auto* ioc : k_binary_iocs) {
            if (data.find(ioc) != std::string::npos) ++hits;
        }
        if (hits >= 3) {
            score += 35;
            reasons += L"strings de coleta de credenciais · ";
        } else if (hits >= 1) {
            score += 12;
        }
    }

    if (!in_trusted_dir(path)) {
        std::wstring roaming = known_folder(FOLDERID_RoamingAppData);
        std::wstring local = known_folder(FOLDERID_LocalAppData);
        std::wstring temp = known_folder(FOLDERID_LocalAppData);
        if ((!roaming.empty() && starts_with_ci(path, roaming)) ||
            (!local.empty() && starts_with_ci(path, local)) ||
            (!temp.empty() && contains_ci(path, L"\\temp\\"))) {
            score += 20;
            reasons += L"executa de diretorio de usuario · ";
        }
        if (!is_signed(path)) {
            score += 15;
            reasons += L"binario sem assinatura · ";
        } else {
            score -= 25;
        }
    } else {
        score -= 40;
    }

    if (reasons.size() >= 3) reasons.resize(reasons.size() - 3);

    if (score >= 90) {
        g_threats.fetch_add(1);
        postf(Severity::Danger, L"GRABBER provavel · pid %lu · %s (%s)",
              pid, name.c_str(), reasons.c_str());
        if (g_opt.kill_suspects) terminate_suspect(pid, L"score " + std::to_wstring(score));
    } else if (score >= 55) {
        g_threats.fetch_add(1);
        postf(Severity::Warn, L"comportamento suspeito · pid %lu · %s (%s)",
              pid, name.c_str(), reasons.c_str());
    }
}

// ---------------------------------------------------------------- process watch

std::map<DWORD, std::wstring> snapshot_processes() {
    std::map<DWORD, std::wstring> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0) continue;
            out[pe.th32ProcessID] = pe.szExeFile;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

void protect_discord_pids(const std::map<DWORD, std::wstring>& procs) {
    bool found = false;
    for (auto& [pid, name] : procs) {
        if (_wcsicmp(name.c_str(), L"Discord.exe") == 0 ||
            _wcsicmp(name.c_str(), L"DiscordCanary.exe") == 0 ||
            _wcsicmp(name.c_str(), L"DiscordPTB.exe") == 0) {
            found = true;
            protect_pid(pid);
        }
    }
    if (g_driver_active && found)
        g_driver_active.store(driver_open() != INVALID_HANDLE_VALUE);
}

void process_thread() {
    std::map<DWORD, std::wstring> previous;
    for (int i = 0; i < 4 && g_run; ++i) Sleep(150);
    previous = snapshot_processes();
    protect_pid(GetCurrentProcessId());

    while (g_run) {
        auto current = snapshot_processes();
        for (auto& [pid, name] : current) {
            if (previous.count(pid)) continue;
            if (pid == GetCurrentProcessId() || pid < 8) continue;
            if (_wcsicmp(name.c_str(), L"conhost.exe") == 0) continue;
            std::wstring path = process_path(pid);
            if (path.empty()) continue;
            analyze_image(pid, path, name);
        }
        protect_discord_pids(current);
        previous = std::move(current);
        for (int i = 0; i < 4 && g_run; ++i) Sleep(150);
    }
}

// ---------------------------------------------------------------- handle watch

using NtQuerySystemInformationFn = NTSTATUS(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

struct HandleEntryEx {
    PVOID Object;
    ULONG_PTR UniqueProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
};

std::mutex g_alert_mtx;
std::map<std::pair<DWORD, DWORD>, unsigned long long> g_alerts;

bool alert_due(DWORD offender, DWORD target) {
    std::lock_guard<std::mutex> lock(g_alert_mtx);
    auto key = std::make_pair(offender, target);
    auto it = g_alerts.find(key);
    unsigned long long now = GetTickCount64();
    if (it != g_alerts.end() && now - it->second < 30000) return false;
    g_alerts[key] = now;
    return true;
}

void scan_handles() {
    auto* query = (NtQuerySystemInformationFn)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    if (!query) return;

    ULONG len = 1u << 20;
    std::vector<unsigned char> buffer;
    NTSTATUS status = k_status_info_mismatch;

    for (int attempt = 0; attempt < 6; ++attempt) {
        buffer.resize(len);
        status = query(k_system_extended_handle_info, buffer.data(), len, &len);
        if (status != k_status_info_mismatch) break;
        len *= 2;
    }
    if (status != 0) return;

    auto* header = reinterpret_cast<ULONG_PTR*>(buffer.data());
    ULONG_PTR count = header[0];
    auto* entries = reinterpret_cast<HandleEntryEx*>(header + 2);

    const DWORD self = GetCurrentProcessId();
    int inspected = 0;

    for (ULONG_PTR i = 0; i < count && inspected < 5000 && g_run; ++i) {
        auto& e = entries[i];
        DWORD owner = (DWORD)e.UniqueProcessId;
        if (owner == 0 || owner == 4 || owner == self) continue;
        if (!(e.GrantedAccess & PROCESS_VM_READ)) continue;
        if (is_protected_pid(owner)) continue;
        if (owner_trusted(owner)) continue;

        HANDLE src = OpenProcess(PROCESS_DUP_HANDLE, FALSE, owner);
        if (!src) continue;
        HANDLE dup = nullptr;
        ++inspected;
        if (DuplicateHandle(src, (HANDLE)e.HandleValue, GetCurrentProcess(), &dup,
                            PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
            DWORD target = GetProcessId(dup);
            if (is_protected_pid(target) && target != 0) {
                std::wstring opath = process_path(owner);
                std::wstring oname = opath.empty() ? L"?" : known_exe_name(opath);
                if (alert_due(owner, target)) {
                    g_threats.fetch_add(1);
                    if (g_driver_active) {
                        g_blocked.fetch_add(1);
                        postf(Severity::Blocked,
                              L"leitura de memoria do Discord BLOQUEADA pelo driver · %s (pid %lu)",
                              oname.c_str(), owner);
                    } else {
                        postf(Severity::Danger,
                              L"processo lendo memoria do Discord · %s (pid %lu) · %s",
                              oname.c_str(), owner, opath.c_str());
                    }
                    if (g_opt.kill_suspects) terminate_suspect(owner, L"handle VM_READ no Discord");
                }
            }
            CloseHandle(dup);
        }
        CloseHandle(src);
    }
}

void handle_thread() {
    for (int i = 0; i < 6 && g_run; ++i) Sleep(250);
    while (g_run) {
        scan_handles();
        for (int i = 0; i < 20 && g_run; ++i) Sleep(150);
    }
}

// ---------------------------------------------------------------- file watch

bool safe_file_owner(const std::wstring& exe) {
    static const wchar_t* k_safe[] = {
        L"chrome.exe", L"msedge.exe", L"brave.exe", L"opera.exe", L"firefox.exe",
        L"discord.exe", L"discordcanary.exe", L"discordptb.exe", L"explorer.exe",
        L"searchindexer.exe", L"antimalware", L"msmpeng.exe", L"backup"
    };
    std::wstring low = lower_copy(exe);
    for (auto* s : k_safe)
        if (low.find(s) != std::wstring::npos) return true;
    return false;
}

void restart_manager_check(const std::wstring& path, const std::wstring& label) {
    DWORD session = 0;
    wchar_t key[CCH_RM_SESSION_KEY + 1]{};
    if (RmStartSession(&session, 0, key) != ERROR_SUCCESS) return;

    LPCWSTR files[1] = {path.c_str()};
    DWORD result = RmRegisterResources(session, 1, files, 0, nullptr, 0, nullptr);
    if (result == ERROR_SUCCESS) {
        UINT needed = 0, count = 0;
        DWORD reason = RmRebootReasonNone;
        result = RmGetList(session, &needed, &count, nullptr, &reason);
        if (result == ERROR_MORE_DATA && needed > 0) {
            std::vector<RM_PROCESS_INFO> list(needed);
            count = needed;
            result = RmGetList(session, &needed, &count, list.data(), &reason);
            if (result == ERROR_SUCCESS) {
                for (UINT i = 0; i < count; ++i) {
                    std::wstring exe = list[i].strAppName;
                    if (safe_file_owner(exe)) continue;
                    g_threats.fetch_add(1);
                    postf(Severity::Danger,
                          L"acesso a %s por processo nao confiavel · %s (pid %lu)",
                          label.c_str(), exe.c_str(), list[i].Process.dwProcessId);
                    if (g_opt.kill_suspects)
                        terminate_suspect(list[i].Process.dwProcessId, L"acesso a arquivo de token");
                }
            }
        }
    }
    RmEndSession(session);
}

void file_thread(std::wstring dir, std::wstring label,
                 std::vector<std::wstring> filters) {
    if (dir.empty()) return;
    HANDLE change = CreateFileW(dir.c_str(), FILE_LIST_DIRECTORY,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                                nullptr);
    if (change == INVALID_HANDLE_VALUE) return;

    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<unsigned char> buffer(16384);
    std::map<std::wstring, unsigned long long> recent;

    while (g_run) {
        ResetEvent(ov.hEvent);
        DWORD returned = 0;
        BOOL ok = ReadDirectoryChangesW(change, buffer.data(), (DWORD)buffer.size(),
                                        TRUE,
                                        FILE_NOTIFY_CHANGE_LAST_WRITE |
                                        FILE_NOTIFY_CHANGE_FILE_NAME |
                                        FILE_NOTIFY_CHANGE_SIZE,
                                        &returned, &ov, nullptr);
        if (!ok) break;

        DWORD wait = WaitForSingleObject(ov.hEvent, 500);
        if (!g_run) {
            CancelIoEx(change, &ov);
            break;
        }
        if (wait != WAIT_OBJECT_0) continue;
        if (!GetOverlappedResult(change, &ov, &returned, FALSE) || returned == 0) continue;

        auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buffer.data());
        while (true) {
            std::wstring name(info->FileName, info->FileNameLength / sizeof(wchar_t));
            std::wstring low = lower_copy(name);
            bool match = filters.empty();
            for (auto& f : filters)
                if (low.find(lower_copy(f)) != std::wstring::npos) { match = true; break; }
            if (match) {
                std::wstring full = dir + L"\\" + name;
                unsigned long long now = GetTickCount64();
                auto& stamp = recent[low];
                if (now - stamp > 4000) {
                    stamp = now;
                    restart_manager_check(full, label + L" / " + name);
                }
                if (recent.size() > 256) recent.clear();
            }
            if (info->NextEntryOffset == 0) break;
            info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(
                reinterpret_cast<unsigned char*>(info) + info->NextEntryOffset);
        }
    }
    CloseHandle(ov.hEvent);
    CloseHandle(change);
}

void spawn_file_watchers() {
    std::wstring roaming = known_folder(FOLDERID_RoamingAppData);
    std::wstring local = known_folder(FOLDERID_LocalAppData);

    struct Watch {
        std::wstring dir;
        std::wstring label;
        std::vector<std::wstring> filters;
    };

    std::vector<Watch> watches = {
        {roaming + L"\\discord\\Local Storage\\leveldb", L"discord leveldb",
         {L".log", L".ldb", L".sst"}},
        {roaming + L"\\discordcanary\\Local Storage\\leveldb", L"discord canary",
         {L".log", L".ldb", L".sst"}},
        {roaming + L"\\discordptb\\Local Storage\\leveldb", L"discord ptb",
         {L".log", L".ldb", L".sst"}},
        {local + L"\\Google\\Chrome\\User Data\\Default", L"chrome profile",
         {L"login data", L"local state", L"cookies", L"web data"}},
        {local + L"\\Microsoft\\Edge\\User Data\\Default", L"edge profile",
         {L"login data", L"local state", L"cookies", L"web data"}},
        {local + L"\\BraveSoftware\\Brave-Browser\\User Data\\Default", L"brave profile",
         {L"login data", L"local state", L"cookies", L"web data"}},
        {local + L"\\Opera Software\\Opera GX Stable", L"opera gx",
         {L"login data", L"local state", L"cookies", L"web data"}},
        {roaming + L"\\Mozilla\\Firefox\\Profiles", L"firefox profiles",
         {L"logins.json", L"cookies.sqlite", L"key4.db"}},
    };

    for (auto& w : watches) {
        if (GetFileAttributesW(w.dir.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        g_threads.emplace_back(file_thread, w.dir, w.label, w.filters);
    }
}

} // namespace

// ---------------------------------------------------------------- API

bool is_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    bool ok = GetTokenInformation(token, TokenElevation, &elevation, size, &size) &&
              elevation.TokenIsElevated;
    CloseHandle(token);
    return ok;
}

bool driver_available() {
    return driver_open() != INVALID_HANDLE_VALUE;
}

bool install_driver_service() {
    if (!is_elevated()) return false;
    std::wstring sys = exe_dir() + L"\\OwlGuard.sys";
    if (GetFileAttributesW(sys.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) return false;
    SC_HANDLE svc = CreateServiceW(
        scm, VANTA_SERVICE_NAME, L"Owl Guard ring-0 shield",
        SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS,
        SERVICE_KERNEL_DRIVER, SERVICE_SYSTEM_START, SERVICE_ERROR_NORMAL,
        sys.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    DWORD error = GetLastError();
    if (!svc && error == ERROR_SERVICE_EXISTS)
        svc = OpenServiceW(scm, VANTA_SERVICE_NAME,
                           SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
    bool ok = false;
    if (svc) {
        if (StartServiceW(svc, 0, nullptr)) ok = true;
        else {
            SERVICE_STATUS st{};
            ok = QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING;
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok;
}

bool driver_shutdown() {
    std::lock_guard<std::mutex> lock(g_prot_mtx);
    for (DWORD pid : g_protected) {
        if (g_driver != INVALID_HANDLE_VALUE)
            DeviceIoControl(g_driver, IOCTL_VANTA_UNPROTECT_PID, &pid, sizeof(pid),
                            nullptr, 0, nullptr, nullptr);
    }
    driver_close();

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, VANTA_SERVICE_NAME,
                                 SERVICE_STOP | SERVICE_QUERY_STATUS);
    bool ok = false;
    if (svc) {
        SERVICE_STATUS st{};
        ok = ControlService(svc, SERVICE_CONTROL_STOP, &st) != FALSE;
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return ok;
}

bool install_autostart(bool enable) {
    bool ok = false;
    if (enable) {
        if (is_elevated() && task_create()) {
            run_key_set(false);   // tarefa elevada substitui a chave Run
            ok = true;
        }
        if (!ok) ok = run_key_set(true);
    } else {
        task_delete();
        run_key_set(false);
        ok = true;
    }
    return ok;
}

bool autostart_enabled() {
    HKEY key = nullptr;
    bool run = false;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        run = RegQueryValueExW(key, L"OwlGuard", nullptr, nullptr, nullptr, nullptr)
              == ERROR_SUCCESS;
        RegCloseKey(key);
    }
    return run || task_exists();
}

GuardStats guard_stats() {
    return GuardStats{g_scanned.load(), g_threats.load(), g_blocked.load(),
                      g_driver_active.load()};
}

bool guard_start(HWND hwnd, const GuardOptions& opt) {
    if (g_run.load()) return true;
    g_hwnd = hwnd;
    g_opt = opt;
    g_scanned = 0;
    g_threats = 0;
    g_blocked = 0;
    g_driver_active = false;
    {
        std::lock_guard<std::mutex> lock(g_prot_mtx);
        g_protected.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_alert_mtx);
        g_alerts.clear();
    }

    post(Severity::Info, L"watchdog online · monitorando criacao de processos");

    if (opt.block_memory_scan) {
        bool driver = driver_open() != INVALID_HANDLE_VALUE;
        if (!driver) driver = install_driver_service();
        if (!driver) driver = driver_service_start();
        if (!driver) driver = driver_open() != INVALID_HANDLE_VALUE;

        g_driver_active = driver;
        if (driver) {
            post(Severity::Info,
                 L"driver ring-0 ativo · VM_READ contra Discord sera removido");
        } else {
            post(Severity::Warn,
                 L"driver inativo · bloqueio em modo watchdog (inicie como admin e "
                 L"coloque OwlGuard.sys na pasta do exe)");
        }
    } else {
        post(Severity::Info, L"bloqueio de memoria desmarcado · apenas deteccao");
    }

    g_run = true;
    g_threads.emplace_back(process_thread);
    g_threads.emplace_back(handle_thread);
    spawn_file_watchers();

    post(Severity::Info, L"vigia de arquivos: discord leveldb + perfis de navegador");
    post(Severity::Info, L"protecao ativa · ouvindo eventos");
    return true;
}

void guard_stop() {
    if (!g_run.exchange(false)) return;
    for (auto& t : g_threads) if (t.joinable()) t.join();
    g_threads.clear();
    driver_shutdown();
    g_driver_active = false;
    {
        std::lock_guard<std::mutex> lock(g_prot_mtx);
        g_protected.clear();
    }
    post(Severity::Info, L"protecao desativada · watchdog descansando");
}
