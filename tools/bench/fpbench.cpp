// fpbench: time-to-first-frame for any Windows video player, measured the same way for every player.
//
// Starts the player (no activation), moves its window off every screen as soon as it exists, captures that window with
// Windows.Graphics.Capture and reports when the first composed frame shows video content in a sample region of the
// client area (mean/variance test, so black frames, blank windows and centred logos don't count). Times are from just
// before CreateProcess to the DWM composition time of that frame (SystemRelativeTime), so both players are judged on
// what actually reached the screen. Mutes the player's audio session, kills it after each run, and kills it at once
// if it ever becomes the foreground window.
//
//   fpbench --label NAME --exe PATH [--args "..."] [--env K=V]... [--runs N] [--timeout SEC] [--csv FILE]
//           [--region x0,y0,x1,y1] [--min-stddev N] [--save-frame DIR] [--pause-ms N] [--keep-onscreen]
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <psapi.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "dwmapi.lib")

using namespace winrt;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using namespace winrt::Windows::Graphics::DirectX::Direct3D11;

struct Options {
    std::wstring label = L"player", exe, args, csv, saveDir, file;  // file: quoted into args at {file} (or appended)
    std::vector<std::wstring> env;
    int runs = 5, timeoutSec = 30, pauseMs = 2000, measureSecs = 0;
    double region[4] = {0.05, 0.08, 0.30, 0.30};
    double minStd = 12;
    bool offscreen = true;
    DWORD targetPid = 0;  // --target-pid: watch this (already running) process's window instead of the launched one
    bool noFocusPatch = true;  // --no-focus-patch turns the debugger launch off
};

static double QpcMs() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}

// ---------------------------------------------------------------------------------------------- window lookup

struct FindCtx { DWORD pid; HWND found; };
static BOOL CALLBACK FindProc(HWND h, LPARAM l) {
    auto* c = (FindCtx*)l;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != c->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r;
    GetWindowRect(h, &r);
    if (r.right - r.left < 200 || r.bottom - r.top < 150) return TRUE;  // splash/tool windows
    c->found = h;
    return FALSE;
}

static bool OnAnyMonitor(HWND h) {
    RECT r;
    GetWindowRect(h, &r);
    return MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr;
}

static void MoveOffscreen(HWND h) {
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 200;
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    SetWindowPos(h, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---------------------------------------------------------------------------------------------- audio mute

static void MuteLoop(DWORD pid, std::atomic<bool>* stop) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        com_ptr<IMMDeviceEnumerator> en;
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), en.put_void());
        while (!*stop) {
            com_ptr<IMMDeviceCollection> devs;
            if (en && SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devs.put()))) {
                UINT n = 0;
                devs->GetCount(&n);
                for (UINT i = 0; i < n; i++) {
                    com_ptr<IMMDevice> d;
                    com_ptr<IAudioSessionManager2> mgr;
                    com_ptr<IAudioSessionEnumerator> sessions;
                    if (FAILED(devs->Item(i, d.put())) ||
                        FAILED(d->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, mgr.put_void())) ||
                        FAILED(mgr->GetSessionEnumerator(sessions.put())))
                        continue;
                    int cnt = 0;
                    sessions->GetCount(&cnt);
                    for (int k = 0; k < cnt; k++) {
                        com_ptr<IAudioSessionControl> c;
                        if (FAILED(sessions->GetSession(k, c.put()))) continue;
                        auto c2 = c.try_as<IAudioSessionControl2>();
                        DWORD spid = 0;
                        if (!c2 || FAILED(c2->GetProcessId(&spid)) || spid != pid) continue;
                        if (auto vol = c.try_as<ISimpleAudioVolume>()) {
                            BOOL m = FALSE;
                            vol->GetMute(&m);
                            if (!m) vol->SetMute(TRUE, nullptr);
                        }
                    }
                }
            }
            Sleep(3);
        }
    }
    CoUninitialize();
}

// ---------------------------------------------------------------------------------------------- capture

struct Detect {
    std::mutex mu;
    bool done = false;
    double frameMs = -1;  // QPC ms of the composed frame that first showed content
    int frames = 0;
    double lastStd = 0, lastMean = 0;
    std::vector<uint8_t> saved;  // BGRA of the sample region
    int savedW = 0, savedH = 0;
};

static void SaveBmp(const std::wstring& path, const std::vector<uint8_t>& px, int w, int h) {
    BITMAPFILEHEADER fh = {};
    BITMAPINFOHEADER ih = {};
    ih.biSize = sizeof ih;
    ih.biWidth = w;
    ih.biHeight = -h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + (DWORD)px.size();
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

// ---------------------------------------------------------------------------------------------- no-focus launch
// The player starts under a debugger. When its user32.dll is mapped (before any of its code runs), the exports a
// program can use to pull itself to the foreground are patched to return at once, then the debugger detaches.
// user32 sits at the same address in every process of the session, so export addresses are taken from our own copy.
// Same treatment for every player, so the comparison stays fair (the debug attach costs a few ms up front).
static bool PatchFocusAndDetach(const PROCESS_INFORMATION& pi) {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    struct Patch { const char* name; std::vector<uint8_t> code; };
    const std::vector<uint8_t> retTrue = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};  // mov eax,1; ret
    const std::vector<uint8_t> retZero = {0x31, 0xC0, 0xC3};                    // xor eax,eax; ret
    const std::vector<uint8_t> retVoid = {0xC3};
    const Patch patches[] = {
        {"SetForegroundWindow", retTrue}, {"BringWindowToTop", retTrue}, {"SwitchToThisWindow", retVoid},
        {"AttachThreadInput", retTrue},   {"AllowSetForegroundWindow", retTrue}, {"LockSetForegroundWindow", retTrue},
        {"keybd_event", retVoid},         {"mouse_event", retVoid},     {"SendInput", retZero},
    };
    // The dialog manager and window activation inside user32 call the win32u syscall stubs directly (a hidden modeless
    // dialog's WM_INITDIALOG focus is enough to activate a new process), so those stubs are patched too.
    HMODULE w32u = GetModuleHandleW(L"win32u.dll");
    const Patch stubs[] = {
        {"NtUserSetFocus", retZero}, {"NtUserSetActiveWindow", retZero}, {"NtUserSetForegroundWindow", retTrue},
        {"NtUserSetForegroundWindowForApplication", retTrue}, {"NtUserAttachThreadInput", retTrue},
        {"NtUserSendInput", retZero},
    };
    auto apply = [&](HMODULE mod, const Patch* list, size_t n) {
        bool ok = true;
        for (size_t i = 0; i < n; i++) {
            void* at = (void*)GetProcAddress(mod, list[i].name);
            if (!at) continue;
            DWORD old;
            SIZE_T w = 0;
            ok &= VirtualProtectEx(pi.hProcess, at, list[i].code.size(), PAGE_EXECUTE_READWRITE, &old) &&
                  WriteProcessMemory(pi.hProcess, at, list[i].code.data(), list[i].code.size(), &w) && w == list[i].code.size();
            VirtualProtectEx(pi.hProcess, at, list[i].code.size(), old, &old);
        }
        FlushInstructionCache(pi.hProcess, nullptr, 0);
        return ok;
    };
    bool u32Done = false, w32uDone = !w32u;
    bool patched = false, alive = true;
    double until = QpcMs() + 10000;
    DEBUG_EVENT ev;
    while (alive && !patched && QpcMs() < until) {
        if (!WaitForDebugEvent(&ev, 100)) continue;
        DWORD cont = DBG_CONTINUE;
        switch (ev.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT:
            if (ev.u.CreateProcessInfo.hFile) CloseHandle(ev.u.CreateProcessInfo.hFile);
            break;
        case LOAD_DLL_DEBUG_EVENT:
            if (ev.u.LoadDll.hFile) CloseHandle(ev.u.LoadDll.hFile);
            if (ev.u.LoadDll.lpBaseOfDll == (void*)u32) {
                if (!apply(u32, patches, std::size(patches))) alive = false;
                u32Done = true;
            } else if (w32u && ev.u.LoadDll.lpBaseOfDll == (void*)w32u) {
                if (!apply(w32u, stubs, std::size(stubs))) alive = false;
                w32uDone = true;
            }
            patched = alive && u32Done && w32uDone;
            break;
        case EXCEPTION_DEBUG_EVENT:
            if (ev.u.Exception.ExceptionRecord.ExceptionCode != EXCEPTION_BREAKPOINT) cont = DBG_EXCEPTION_NOT_HANDLED;
            break;
        case EXIT_PROCESS_DEBUG_EVENT:
            alive = false;
            break;
        }
        ContinueDebugEvent(ev.dwProcessId, ev.dwThreadId, cont);
    }
    DebugSetProcessKillOnExit(FALSE);
    DebugActiveProcessStop(pi.dwProcessId);
    return patched;
}

struct RunResult {
    double windowMs = -1, frameMs = -1;
    int frames = 0;
    double cpuPct = -1, privateMB = -1, workingMB = -1;  // while playing (--measure SEC)
    std::wstring status = L"ok";
};

static double CpuSeconds(HANDLE p) {
    FILETIME c, e, k, u;
    GetProcessTimes(p, &c, &e, &k, &u);
    auto v = [](FILETIME f) { return (((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e7; };
    return v(k) + v(u);
}

static RunResult RunOnce(const Options& o, int run, ID3D11Device* dev, IDirect3DDevice const& rtDev) {
    RunResult res;
    // Environment block: ours plus --env overrides.
    std::wstring envBlock;
    {
        wchar_t* cur = GetEnvironmentStringsW();
        for (wchar_t* p = cur; *p; p += wcslen(p) + 1) {
            std::wstring kv = p;
            bool overridden = false;
            for (auto& e : o.env) {
                std::wstring key = e.substr(0, e.find(L'=') + 1);
                if (_wcsnicmp(kv.c_str(), key.c_str(), key.size()) == 0) overridden = true;
            }
            if (!overridden) envBlock += kv + L'\0';
        }
        FreeEnvironmentStringsW(cur);
        for (auto& e : o.env) envBlock += e + L'\0';
        // A process created under a debugger gets the slow debug heap for life, even after we detach.
        if (o.noFocusPatch) envBlock += std::wstring(L"_NO_DEBUG_HEAP=1") + L'\0';
        envBlock += L'\0';
    }
    std::wstring args = o.args;
    if (!o.file.empty()) {
        std::wstring q = L"\"" + o.file + L"\"";
        size_t at = args.find(L"{file}");
        if (at != std::wstring::npos) args.replace(at, 6, q);
        else args += L" " + q;
    }
    std::wstring cmd = L"\"" + o.exe + L"\" " + args;
    std::wstring dir = o.exe.substr(0, o.exe.find_last_of(L"\\/"));
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNOACTIVATE;
    PROCESS_INFORMATION pi = {};
    double t0 = QpcMs();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT | (o.noFocusPatch ? DEBUG_ONLY_THIS_PROCESS : 0), envBlock.data(),
                        dir.c_str(), &si, &pi)) {
        res.status = L"launch failed";
        return res;
    }
    if (o.noFocusPatch && !PatchFocusAndDetach(pi)) {
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        res.status = L"no-focus patch failed";
        return res;
    }
    // With --target-pid the launched process only hands the file to a running (warm) player and exits.
    DWORD wpid = o.targetPid ? o.targetPid : pi.dwProcessId;
    HANDLE hw = o.targetPid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, o.targetPid) : pi.hProcess;
    if (!hw) hw = pi.hProcess;
    std::atomic<bool> stopMute{false};
    std::thread muter(MuteLoop, wpid, &stopMute);

    Detect det;
    HWND hwnd = nullptr;
    GraphicsCaptureItem item{nullptr};
    Direct3D11CaptureFramePool pool{nullptr};
    GraphicsCaptureSession session{nullptr};
    event_token token{};
    com_ptr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(ctx.put());
    double deadline = t0 + o.timeoutSec * 1000.0;

    while (QpcMs() < deadline) {
        if (WaitForSingleObject(hw, 0) == WAIT_OBJECT_0) { res.status = L"exited early"; break; }
        DWORD fg = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &fg);
        if (fg == wpid || fg == pi.dwProcessId) {
            wchar_t cls[64] = L"";
            GetClassNameW(GetForegroundWindow(), cls, 64);
            res.status = std::wstring(L"took foreground (window class ") + cls + L") - killed";
            break;
        }
        if (!hwnd) {
            FindCtx fc = {wpid, nullptr};
            EnumWindows(FindProc, (LPARAM)&fc);
            if (fc.found) {
                hwnd = fc.found;
                res.windowMs = QpcMs() - t0;
                if (o.offscreen && OnAnyMonitor(hwnd)) MoveOffscreen(hwnd);
                auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
                if (FAILED(interop->CreateForWindow(hwnd, guid_of<GraphicsCaptureItem>(), put_abi(item)))) {
                    res.status = L"capture failed";
                    break;
                }
                pool = Direct3D11CaptureFramePool::CreateFreeThreaded(rtDev, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
                                                                     item.Size());
                auto lastSize = item.Size();
                token = pool.FrameArrived([&, lastSize](Direct3D11CaptureFramePool const& sender, auto&&) mutable {
                    auto frame = sender.TryGetNextFrame();
                    if (!frame) return;
                    std::lock_guard<std::mutex> l(det.mu);
                    if (det.done) return;
                    det.frames++;
                    auto size = frame.ContentSize();
                    if (size.Width != lastSize.Width || size.Height != lastSize.Height) {
                        lastSize = size;
                        sender.Recreate(rtDev, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
                    }
                    // Sample region in capture coordinates: capture = DWM frame bounds; region = fraction of client.
                    RECT fb, cr;
                    POINT org = {0, 0};
                    if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &fb, sizeof fb)) ||
                        !GetClientRect(hwnd, &cr) || !ClientToScreen(hwnd, &org))
                        return;
                    int cw = cr.right, ch = cr.bottom;
                    int ox = org.x - fb.left, oy = org.y - fb.top;
                    int x0 = ox + (int)(cw * o.region[0]), y0 = oy + (int)(ch * o.region[1]);
                    int x1 = ox + (int)(cw * o.region[2]), y1 = oy + (int)(ch * o.region[3]);
                    x0 = std::max(0, x0); y0 = std::max(0, y0);
                    x1 = std::min(x1, size.Width); y1 = std::min(y1, size.Height);
                    if (x1 - x0 < 16 || y1 - y0 < 16) return;
                    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                    com_ptr<ID3D11Texture2D> tex;
                    access->GetInterface(__uuidof(ID3D11Texture2D), tex.put_void());
                    D3D11_TEXTURE2D_DESC td = {};
                    td.Width = x1 - x0;
                    td.Height = y1 - y0;
                    td.MipLevels = 1;
                    td.ArraySize = 1;
                    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    td.SampleDesc.Count = 1;
                    td.Usage = D3D11_USAGE_STAGING;
                    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    com_ptr<ID3D11Texture2D> st;
                    if (FAILED(dev->CreateTexture2D(&td, nullptr, st.put()))) return;
                    D3D11_BOX box = {(UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1};
                    ctx->CopySubresourceRegion(st.get(), 0, 0, 0, 0, tex.get(), 0, &box);
                    D3D11_MAPPED_SUBRESOURCE m;
                    if (FAILED(ctx->Map(st.get(), 0, D3D11_MAP_READ, 0, &m))) return;
                    double sum = 0, sum2 = 0;
                    int n = 0;
                    for (UINT y = 0; y < td.Height; y += 2) {
                        const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                        for (UINT x = 0; x < td.Width; x += 2) {
                            const uint8_t* p = row + x * 4;
                            double lum = 0.114 * p[0] + 0.587 * p[1] + 0.299 * p[2];
                            sum += lum;
                            sum2 += lum * lum;
                            n++;
                        }
                    }
                    double mean = sum / n, sd = std::sqrt(std::max(0.0, sum2 / n - mean * mean));
                    det.lastStd = sd;
                    det.lastMean = mean;
                    if (sd >= o.minStd) {
                        det.done = true;
                        // SystemRelativeTime: QPC-based time the frame was composed (100 ns units).
                        det.frameMs = frame.SystemRelativeTime().count() / 10000.0;
                        if (!o.saveDir.empty()) {
                            det.savedW = td.Width;
                            det.savedH = td.Height;
                            det.saved.resize((size_t)td.Width * td.Height * 4);
                            for (UINT y = 0; y < td.Height; y++)
                                memcpy(&det.saved[(size_t)y * td.Width * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch,
                                       (size_t)td.Width * 4);
                        }
                    }
                    ctx->Unmap(st.get(), 0);
                });
                session = pool.CreateCaptureSession(item);
                try { session.IsCursorCaptureEnabled(false); } catch (...) {}
                try { session.IsBorderRequired(false); } catch (...) {}
                session.StartCapture();
            }
        } else if (o.offscreen && OnAnyMonitor(hwnd)) {
            MoveOffscreen(hwnd);  // players that re-centre themselves after opening the file
        }
        {
            std::lock_guard<std::mutex> l(det.mu);
            if (det.done) break;
        }
        Sleep(1);
    }
    {
        std::lock_guard<std::mutex> l(det.mu);
        res.frames = det.frames;
        if (det.done) {
            // QPC ms of composition relative to launch: SystemRelativeTime is QPC in 100 ns units.
            res.frameMs = det.frameMs - t0;
        } else if (res.status == L"ok") {
            wchar_t b[96];
            swprintf(b, 96, L"timeout (frames=%d, last mean %.1f sd %.1f)", det.frames, det.lastMean, det.lastStd);
            res.status = b;
        }
        det.done = true;
    }
    if (session) session.Close();
    if (res.frameMs >= 0 && o.measureSecs > 0) {
        // Resource use during steady playback: CPU over the window (all cores = 100% * cores), memory at the end.
        double c0 = CpuSeconds(hw), w0 = QpcMs();
        bool stole = false;
        while (QpcMs() - w0 < o.measureSecs * 1000.0 && WaitForSingleObject(hw, 50) == WAIT_TIMEOUT) {
            DWORD fg = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &fg);
            if (fg == wpid) { stole = true; break; }
        }
        double c1 = CpuSeconds(hw), w1 = QpcMs();
        res.cpuPct = (c1 - c0) / ((w1 - w0) / 1000.0) * 100.0;
        PROCESS_MEMORY_COUNTERS_EX pm = {sizeof pm};
        if (K32GetProcessMemoryInfo(hw, (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm)) {
            res.privateMB = pm.PrivateUsage / 1048576.0;
            res.workingMB = pm.WorkingSetSize / 1048576.0;
        }
        if (stole) res.status = L"took foreground - killed";
    }
    if (pool) {
        pool.FrameArrived(token);
        pool.Close();
    }
    if (o.targetPid && res.status.find(L"foreground") == std::wstring::npos) {
        if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0);  // the warm player goes back to its tray state
        if (hw != pi.hProcess) CloseHandle(hw);
    } else if (o.targetPid) {
        TerminateProcess(hw, 0);
        if (hw != pi.hProcess) CloseHandle(hw);
    }
    TerminateProcess(pi.hProcess, 0);
    WaitForSingleObject(pi.hProcess, 10000);
    stopMute = true;
    muter.join();
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (!o.saveDir.empty() && !det.saved.empty()) {
        wchar_t name[64];
        swprintf(name, 64, L"\\%s-run%02d.bmp", o.label.c_str(), run);
        SaveBmp(o.saveDir + name, det.saved, det.savedW, det.savedH);
    }
    return res;
}

// --grab PID CLASS OUT.bmp: one Windows.Graphics.Capture still of that process's window of that class (works
// off-screen and includes GPU-presented video, unlike PrintWindow).
struct GrabCtx { DWORD pid; const wchar_t* cls; HWND found; };
static BOOL CALLBACK GrabFind(HWND h, LPARAM l) {
    auto* c = (GrabCtx*)l;
    DWORD pid = 0;
    wchar_t cls[64];
    GetWindowThreadProcessId(h, &pid);
    GetClassNameW(h, cls, 64);
    if (pid == c->pid && !wcscmp(cls, c->cls) && IsWindowVisible(h)) { c->found = h; return FALSE; }
    return TRUE;
}

static int Grab(DWORD pid, const wchar_t* cls, const wchar_t* out, ID3D11Device* dev, IDirect3DDevice const& rtDev) {
    GrabCtx gc = {pid, cls, nullptr};
    EnumWindows(GrabFind, (LPARAM)&gc);
    if (!gc.found) { fwprintf(stderr, L"no visible window of class %s\n", cls); return 1; }
    GraphicsCaptureItem item{nullptr};
    auto interop = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    check_hresult(interop->CreateForWindow(gc.found, guid_of<GraphicsCaptureItem>(), put_abi(item)));
    auto pool = Direct3D11CaptureFramePool::CreateFreeThreaded(rtDev, DirectXPixelFormat::B8G8R8A8UIntNormalized, 1, item.Size());
    auto session = pool.CreateCaptureSession(item);
    try { session.IsCursorCaptureEnabled(false); } catch (...) {}
    try { session.IsBorderRequired(false); } catch (...) {}
    session.StartCapture();
    Direct3D11CaptureFrame frame{nullptr};
    for (int i = 0; i < 300 && !frame; i++) {
        frame = pool.TryGetNextFrame();
        if (!frame) Sleep(10);
    }
    int rc = 1;
    if (frame) {
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        com_ptr<ID3D11Texture2D> tex;
        access->GetInterface(__uuidof(ID3D11Texture2D), tex.put_void());
        D3D11_TEXTURE2D_DESC td;
        tex->GetDesc(&td);
        auto size = frame.ContentSize();
        td.Width = std::min<UINT>(td.Width, size.Width);
        td.Height = std::min<UINT>(td.Height, size.Height);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.MiscFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        com_ptr<ID3D11Texture2D> st;
        com_ptr<ID3D11DeviceContext> ctx;
        dev->GetImmediateContext(ctx.put());
        if (SUCCEEDED(dev->CreateTexture2D(&td, nullptr, st.put()))) {
            D3D11_BOX box = {0, 0, 0, td.Width, td.Height, 1};
            ctx->CopySubresourceRegion(st.get(), 0, 0, 0, 0, tex.get(), 0, &box);
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(ctx->Map(st.get(), 0, D3D11_MAP_READ, 0, &m))) {
                std::vector<uint8_t> px((size_t)td.Width * td.Height * 4);
                for (UINT y = 0; y < td.Height; y++)
                    memcpy(&px[(size_t)y * td.Width * 4], (const uint8_t*)m.pData + (size_t)y * m.RowPitch, (size_t)td.Width * 4);
                ctx->Unmap(st.get(), 0);
                SaveBmp(out, px, td.Width, td.Height);
                rc = 0;
            }
        }
    }
    session.Close();
    pool.Close();
    return rc;
}

int wmain(int argc, wchar_t** argv) {
    Options o;
    DWORD grabPid = 0;
    std::wstring grabClass, grabOut;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&]() -> std::wstring { return i + 1 < argc ? argv[++i] : L""; };
        if (a == L"--label") o.label = next();
        else if (a == L"--exe") o.exe = next();
        else if (a == L"--args") o.args = next();
        else if (a == L"--file") o.file = next();
        else if (a == L"--env") o.env.push_back(next());
        else if (a == L"--runs") o.runs = _wtoi(next().c_str());
        else if (a == L"--timeout") o.timeoutSec = _wtoi(next().c_str());
        else if (a == L"--csv") o.csv = next();
        else if (a == L"--save-frame") o.saveDir = next();
        else if (a == L"--pause-ms") o.pauseMs = _wtoi(next().c_str());
        else if (a == L"--measure") o.measureSecs = _wtoi(next().c_str());
        else if (a == L"--min-stddev") o.minStd = _wtof(next().c_str());
        else if (a == L"--keep-onscreen") o.offscreen = false;
        else if (a == L"--no-focus-patch") o.noFocusPatch = false;
        else if (a == L"--target-pid") o.targetPid = (DWORD)_wtoi(next().c_str());
        else if (a == L"--grab") { grabPid = (DWORD)_wtoi(next().c_str()); grabClass = next(); grabOut = next(); }
        else if (a == L"--region") swscanf(next().c_str(), L"%lf,%lf,%lf,%lf", &o.region[0], &o.region[1], &o.region[2], &o.region[3]);
        else { fwprintf(stderr, L"unknown argument %s\n", a.c_str()); return 2; }
    }
    if (o.exe.empty() && !grabPid) {
        fwprintf(stderr, L"usage: fpbench --label NAME --exe PATH [--args ...] [--runs N] [--csv FILE]\n");
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    init_apartment(apartment_type::multi_threaded);
    com_ptr<ID3D11Device> dev;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                      D3D11_SDK_VERSION, dev.put(), nullptr, nullptr);
    if (!dev) { fwprintf(stderr, L"no D3D11 device\n"); return 1; }
    // The immediate context is used from capture callbacks only, one frame at a time (det.mu), never concurrently.
    auto dxgi = dev.as<IDXGIDevice>();
    IDirect3DDevice rtDev{nullptr};
    check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), reinterpret_cast<::IInspectable**>(put_abi(rtDev))));

    if (grabPid) return Grab(grabPid, grabClass.c_str(), grabOut.c_str(), dev.get(), rtDev);

    FILE* csv = nullptr;
    if (!o.csv.empty()) {
        bool exists = GetFileAttributesW(o.csv.c_str()) != INVALID_FILE_ATTRIBUTES;
        csv = _wfopen(o.csv.c_str(), L"a, ccs=UTF-8");
        if (csv && !exists) fwprintf(csv, L"label,run,window_ms,first_frame_ms,frames,cpu_pct,private_mb,working_mb,status\n");
    }
    std::vector<double> times;
    for (int r = 1; r <= o.runs; r++) {
        RunResult res = RunOnce(o, r, dev.get(), rtDev);
        wprintf(L"%s run %d: window %.0f ms, first frame %.0f ms", o.label.c_str(), r, res.windowMs, res.frameMs);
        if (res.cpuPct >= 0) wprintf(L", cpu %.1f%%, private %.0f MB, working set %.0f MB", res.cpuPct, res.privateMB, res.workingMB);
        wprintf(L" (%s)\n", res.status.c_str());
        fflush(stdout);
        if (csv) {
            fwprintf(csv, L"%s,%d,%.1f,%.1f,%d,%.1f,%.0f,%.0f,%s\n", o.label.c_str(), r, res.windowMs, res.frameMs, res.frames,
                     res.cpuPct, res.privateMB, res.workingMB, res.status.c_str());
            fflush(csv);
        }
        if (res.frameMs >= 0) times.push_back(res.frameMs);
        if (res.status.find(L"foreground") != std::wstring::npos) break;  // never keep going after a focus steal
        if (r < o.runs) Sleep(o.pauseMs);
    }
    if (csv) fclose(csv);
    if (!times.empty()) {
        std::sort(times.begin(), times.end());
        double med = times.size() % 2 ? times[times.size() / 2] : (times[times.size() / 2 - 1] + times[times.size() / 2]) / 2;
        wprintf(L"%s: median %.0f ms, min %.0f, max %.0f over %zu runs\n", o.label.c_str(), med, times.front(), times.back(),
                times.size());
    }
    return 0;
}
