#include "util.h"
#include <winioctl.h>
#include <cstdio>
#include <cwctype>

namespace util {

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    std::wstring p(buf, n);
    return p.substr(0, p.find_last_of(L"\\/"));
}

// Environment variables instead of SHGetKnownFolderPath: keeps shell32/ole32 out of the startup path.
static std::wstring EnvDir(const wchar_t* name) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(name, buf, (DWORD)std::size(buf));
    return (n && n < std::size(buf)) ? std::wstring(buf, n) : ExeDir();
}

const std::wstring& DataDir() {
    static std::wstring dir = [] {
        std::wstring exe = ExeDir();
        if (GetFileAttributesW((exe + L"\\fastplay.ini").c_str()) != INVALID_FILE_ATTRIBUTES) return exe;
        std::wstring d = EnvDir(L"APPDATA") + L"\\FastPlay";
        CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }();
    return dir;
}

std::wstring LocalCacheDir() {
    std::wstring d = EnvDir(L"LOCALAPPDATA") + L"\\FastPlay";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

std::wstring FullPath(const std::wstring& p) {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetFullPathNameW(p.c_str(), (DWORD)std::size(buf), buf, nullptr);
    return (n && n < std::size(buf)) ? std::wstring(buf, n) : p;
}

std::wstring FileName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

std::wstring DirName(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"." : p.substr(0, i);
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return {};
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring FormatTime(double sec, bool millis) {
    if (!(sec >= 0)) sec = 0;
    long long ms = (long long)(sec * 1000 + 0.5);
    long long h = ms / 3600000, m = ms / 60000 % 60, s = ms / 1000 % 60;
    wchar_t buf[48];
    if (h) swprintf(buf, 48, L"%lld:%02lld:%02lld", h, m, s);
    else swprintf(buf, 48, L"%02lld:%02lld", m, s);
    std::wstring r = buf;
    if (millis) {
        swprintf(buf, 48, L".%03lld", ms % 1000);
        r += buf;
    }
    return r;
}

bool ParseTime(const std::wstring& s, double* out) {
    double total = 0, part = 0;
    bool any = false;
    std::wstring cur;
    for (size_t i = 0; i <= s.size(); i++) {
        wchar_t c = i < s.size() ? s[i] : L':';
        if (c == L':') {
            if (cur.empty()) return false;
            wchar_t* end = nullptr;
            part = wcstod(cur.c_str(), &end);
            if (*end) return false;
            total = total * 60 + part;
            cur.clear();
            any = true;
        } else if (!iswspace(c)) {
            cur += c;
        }
    }
    if (!any) return false;
    *out = total;
    return true;
}

double MsSinceProcessStart() {
    static ULONGLONG created = [] {
        FILETIME c, e, k, u;
        GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        return ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
    }();
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    ULONGLONG n = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    return (double)(n - created) / 10000.0;
}

void TimingLog(const char* what) {
    static wchar_t path[MAX_PATH] = {};
    static int state = 0;  // 0 unknown, 1 on, 2 off
    if (state == 0) state = GetEnvironmentVariableW(L"FASTPLAY_TIMING", path, MAX_PATH) ? 1 : 2;
    if (state != 1) return;
    double ms = MsSinceProcessStart();
    FILE* f = _wfopen(path, L"a");
    if (!f) return;
    fprintf(f, "%lu\t%.1f\t%s\n", GetCurrentProcessId(), ms, what);
    fclose(f);
}

bool TestMode() {
    static int v = [] {
        wchar_t b[8];
        return GetEnvironmentVariableW(L"FASTPLAY_TEST", b, 8) && b[0] == L'1' ? 1 : 0;
    }();
    return v != 0;
}

const std::wstring& InstanceSuffix() {
    static std::wstring s = [] {
        wchar_t b[64];
        DWORD n = GetEnvironmentVariableW(L"FASTPLAY_INSTANCE", b, 64);
        return n && n < 64 ? std::wstring(L".") + b : std::wstring();
    }();
    return s;
}

std::wstring MainClassName() { return L"FastPlay.Main" + InstanceSuffix(); }

bool IsOnSpinningDisk(const std::wstring& path) {
    wchar_t vol[MAX_PATH];
    if (!GetVolumePathNameW(path.c_str(), vol, MAX_PATH)) return false;
    std::wstring dev = L"\\\\.\\" + std::wstring(vol);
    if (!dev.empty() && dev.back() == L'\\') dev.pop_back();
    HANDLE h = CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    STORAGE_PROPERTY_QUERY q = {};
    q.PropertyId = StorageDeviceSeekPenaltyProperty;
    q.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR d = {};
    DWORD got = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &d, sizeof d, &got, nullptr);
    CloseHandle(h);
    return ok && got >= sizeof d && d.IncursSeekPenalty;
}

}  // namespace util
