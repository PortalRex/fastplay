// Start-up path, libmpv set-up, file loading and mpv events. Window/input code is in window.cpp.
#include "app.h"
#include "controls.h"
#include "edit/editor.h"
#include <algorithm>

App* g_app = nullptr;

namespace {

struct IndexMsg {
    MediaIndex idx;
    bool ok = false;
    bool spinning = false;
};

enum : uint64_t {
    kPropPause = 1, kPropDuration, kPropVolume, kPropMute, kPropSpeed, kPropAbA, kPropAbB,
    kPropChapters, kPropDWidth, kPropDHeight, kPropIdle, kPropEof,
};

void WakeupCallback(void* d) { PostMessageW((HWND)d, WM_APP_MPV, 0, 0); }

}  // namespace

// ------------------------------------------------------------------------------------------------ start-up

int App::Run(HINSTANCE inst, const std::vector<std::wstring>& args) {
    inst_ = inst;
    std::vector<std::wstring> files;
    bool startInTray = false;
    for (const auto& a : args) {
        if (a == L"--tray") startInTray = true;
        else if (a == L"--bench-exit") benchExit_ = true;
        else if (a.rfind(L"--start=", 0) == 0) startOverride_ = _wtof(a.c_str() + 8);
        else if (!a.empty() && a[0] != L'-') files.push_back(a.find(L"://") != std::wstring::npos ? a : util::FullPath(a));
    }
    std::wstring file = files.empty() ? L"" : files[0];

    iniPath_ = util::DataDir() + L"\\fastplay.ini";
    ini_.Load(iniPath_);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(iniPath_.c_str(), GetFileExInfoStandard, &fa)) iniTime_ = fa.ftLastWriteTime;
    s_.Load(ini_);
    keys_.Load(ini_);
    bar_.volume = s_.volume;
    bar_.volumeMax = s_.volumeMax;
    if (startInTray) s_.tray = true;  // started as the warm instance: closing goes back to the tray

    // Single instance: hand the file to the running player and leave. Test runs always get their own instance.
    if (s_.singleInstance && (!util::TestMode() || !util::InstanceSuffix().empty())) {
        std::wstring mutexName = L"Local\\FastPlay.SingleInstance" + util::InstanceSuffix();
        instanceMutex_ = CreateMutexW(nullptr, FALSE, mutexName.c_str());
        bool another = GetLastError() == ERROR_ALREADY_EXISTS;
        std::wstring all;  // several files go over as one newline-separated message (the receiver makes a playlist)
        for (const auto& f : files) all += (all.empty() ? L"" : L"\n") + f;
        if (another && HandOffToRunningInstance(all)) return 0;
    }
    util::TimingLog("start");

    if (!util::TestMode()) resume_.Load(util::DataDir() + L"\\history.txt");
    hwndReady_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (files.size() > 1) {
        plist_ = files;
        plPos_ = 0;
    }
    if (!file.empty()) {
        pendingStart_ = PrepareOpen(file);  // also starts the header prefetch
        std::lock_guard<std::mutex> l(pendingMu_);
        pendingFile_ = file;
    }
    // Window first (~8 ms), then libmpv loads and initialises on its own thread. Loading the 120 MB DLL in parallel
    // with window creation measured slower for the window (loader-lock contention) and no faster for the first frame.
    CreateWindows(startInTray);
    util::TimingLog("window shown");
    initThread_ = std::thread(&App::InitThread, this);
    SetEvent(hwndReady_);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (PreTranslateKey(msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (instanceMutex_) CloseHandle(instanceMutex_);
    return 0;
}

bool App::HandOffToRunningInstance(const std::wstring& file) {
    // The first instance may still be creating its window; give it a moment.
    HWND other = nullptr;
    for (int i = 0; i < 100 && !other; i++) {
        other = FindWindowW(util::MainClassName().c_str(), nullptr);
        if (!other) Sleep(20);
    }
    if (!other) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(other, &pid);
    AllowSetForegroundWindow(pid);  // we were started by the user, so we may pass the foreground on
    COPYDATASTRUCT cd = {kCopyDataOpen, (DWORD)((file.size() + 1) * sizeof(wchar_t)), (void*)file.c_str()};
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, &result) && result;
}

void App::SetMpvOptions(mpv_handle* h) {
    std::wstring cache = util::LocalCacheDir() + L"\\shaders";
    CreateDirectoryW(cache.c_str(), nullptr);
    std::string cache8 = util::Utf8(cache);
    std::string vol = std::to_string(s_.volume), volMax = std::to_string(s_.volumeMax);
    std::string hwdec = util::Utf8(s_.hwdec), vo = util::Utf8(s_.vo);
    std::wstring shotDir = s_.screenshotDir;
    if (shotDir.empty()) {
        wchar_t up[MAX_PATH];
        if (GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH)) shotDir = std::wstring(up) + L"\\Pictures\\FastPlay";
    }
    std::string shot8 = util::Utf8(shotDir), fmt8 = util::Utf8(s_.screenshotFormat);
    const char* opts[][2] = {
        // No config files, scripts, OSC, youtube-dl or built-in key bindings: we are the UI.
        {"config", "no"}, {"load-scripts", "no"}, {"osc", "no"}, {"ytdl", "no"}, {"terminal", "no"},
        {"input-default-bindings", "no"}, {"input-builtin-bindings", "no"}, {"input-vo-keyboard", "no"},
        {"input-cursor", "no"}, {"input-media-keys", "no"}, {"cursor-autohide", "no"},
        {"load-stats-overlay", "no"}, {"load-console", "no"}, {"load-auto-profiles", "no"}, {"load-select", "no"},
        {"load-positioning", "no"}, {"load-commands", "no"}, {"load-context-menu", "no"},
        // GPU path: d3d11 gpu-next with zero-copy d3d11va decoding; cached shaders.
        {"vo", vo.c_str()}, {"gpu-api", "d3d11"}, {"gpu-context", "d3d11"}, {"hwdec", hwdec.c_str()},
        {"gpu-shader-cache", "yes"}, {"gpu-shader-cache-dir", cache8.c_str()},
        {"force-window", s_.forceWindow ? "yes" : "no"},
        {"idle", "yes"}, {"keep-open", "yes"},
        // Lighter buffers than mpv's defaults (150 MiB ahead / 50 MiB back).
        {"demuxer-max-bytes", "64MiB"}, {"demuxer-max-back-bytes", "16MiB"},
        // Fragmented MP4 (OBS): take fragment times from the trailing mfra index instead of reading every moof
        // header at open (1254 seeks for a 32 GB / 87 min recording: ~10 s on a cold hard disk). "pts" keeps
        // timestamps identical to the default path for OBS files (checked packet by packet).
        {"demuxer-lavf-o", "use_mfra_for=pts"},
        {"audio-file-auto", "no"}, {"cover-art-auto", "no"}, {"sub-auto", "exact"},
        {"volume", vol.c_str()}, {"volume-max", volMax.c_str()}, {"loop-file", s_.afterPlayback == 2 ? "inf" : "no"}, {"mute", (s_.mute || util::TestMode()) ? "yes" : "no"},
        {"audio-pitch-correction", "yes"}, {"audio-client-name", "FastPlay"},
        {"osd-on-seek", "msg"}, {"osd-duration", "1200"}, {"osd-font-size", "30"}, {"osd-border-size", "2"},
        {"screenshot-directory", shot8.c_str()}, {"screenshot-format", fmt8.c_str()},
        {"screenshot-template", "%F-%wH.%wM.%wS.%wT"},
        {"taskbar-progress", "no"}, {"stop-screensaver", "yes"},
    };
    for (auto& o : opts) g_mpv.mpv_set_option_string(h, o[0], o[1]);
    // Extra options from fastplay.ini (mpv_options=name=value;name=value) and FASTPLAY_MPV_OPTS (experiments).
    std::wstring extra = s_.mpvOptions;
    wchar_t env[2048];
    if (GetEnvironmentVariableW(L"FASTPLAY_MPV_OPTS", env, 2048)) extra += L";" + std::wstring(env);
    size_t pos = 0;
    while (pos < extra.size()) {
        size_t semi = extra.find(L';', pos);
        if (semi == std::wstring::npos) semi = extra.size();
        std::wstring kv = util::Trim(extra.substr(pos, semi - pos));
        size_t eq = kv.find(L'=');
        if (eq != std::wstring::npos)
            g_mpv.mpv_set_option_string(h, util::Utf8(kv.substr(0, eq)).c_str(), util::Utf8(kv.substr(eq + 1)).c_str());
        pos = semi + 1;
    }
}

void App::InitThread() {
    util::TimingLog("init: begin");
    bool ok = g_mpv.Load(util::ExeDir() + L"\\libmpv-2.dll");
    util::TimingLog("init: libmpv loaded");
    mpv_handle* h = ok ? g_mpv.mpv_create() : nullptr;
    if (h) SetMpvOptions(h);
    WaitForSingleObject(hwndReady_, INFINITE);
    if (!h) {
        PostMessageW(hwnd_, WM_APP_MPV_READY, 0, 0);
        return;
    }
    int64_t wid = (int64_t)(intptr_t)video_;
    g_mpv.mpv_set_option(h, "wid", MPV_FORMAT_INT64, &wid);
    g_mpv.mpv_set_wakeup_callback(h, WakeupCallback, hwnd_);
    struct { uint64_t id; const char* name; mpv_format f; } props[] = {
        {kPropPause, "pause", MPV_FORMAT_FLAG}, {kPropDuration, "duration", MPV_FORMAT_DOUBLE},
        {kPropVolume, "volume", MPV_FORMAT_DOUBLE}, {kPropMute, "mute", MPV_FORMAT_FLAG},
        {kPropSpeed, "speed", MPV_FORMAT_DOUBLE}, {kPropAbA, "ab-loop-a", MPV_FORMAT_DOUBLE},
        {kPropAbB, "ab-loop-b", MPV_FORMAT_DOUBLE}, {kPropChapters, "chapter-list", MPV_FORMAT_NODE},
        {kPropDWidth, "dwidth", MPV_FORMAT_INT64}, {kPropDHeight, "dheight", MPV_FORMAT_INT64},
        {kPropIdle, "idle-active", MPV_FORMAT_FLAG}, {kPropEof, "eof-reached", MPV_FORMAT_FLAG},
    };
    for (auto& p : props) g_mpv.mpv_observe_property(h, p.id, p.name, p.f);
    if (g_mpv.mpv_initialize(h) < 0) {
        g_mpv.mpv_terminate_destroy(h);
        PostMessageW(hwnd_, WM_APP_MPV_READY, 0, 0);
        return;
    }
    util::TimingLog("init: mpv initialized");
    std::wstring file;
    double start;
    {
        std::lock_guard<std::mutex> l(pendingMu_);
        file.swap(pendingFile_);
        start = pendingStart_;
    }
    if (!file.empty()) {
        IssueLoad(h, file, start);
        util::TimingLog("init: loadfile issued");
    }
    PostMessageW(hwnd_, WM_APP_MPV_READY, 1, (LPARAM)h);
}

void App::OnMpvReady(bool ok) {
    if (initThread_.joinable()) initThread_.join();
    if (!ok) {
        MessageBoxW(hwnd_, L"Could not load libmpv-2.dll (it must sit next to fastplay.exe).", L"FastPlay", MB_ICONERROR);
        return;
    }
    DrainEvents();
    std::wstring file;
    double start;
    {
        std::lock_guard<std::mutex> l(pendingMu_);
        file.swap(pendingFile_);
        start = pendingStart_;
    }
    if (!file.empty()) IssueLoad(mpv_, file, start);  // opened while libmpv was still starting
}

// ------------------------------------------------------------------------------------------------ files

double App::PrepareOpen(const std::wstring& path) {
    SaveResume();
    RetireThumbnails();
    path_ = path;
    fileSize_ = fileMtime_ = 0;
    ResumeStore::Stat(path, &fileSize_, &fileMtime_);
    loadSerial_++;
    firstFrameLogged_ = false;
    fitPending_ = true;
    indexReady_ = false;
    index_ = MediaIndex{};
    vidW_ = vidH_ = 0;
    bar_ = BarState{bar_.hasFile, true, bar_.mute, bar_.remaining, 0, 0, bar_.speed, -1, -1, bar_.volume, bar_.volumeMax, {}};
    bar_.hasFile = true;
    panX_ = panY_ = zoom_ = 0;

    double start = 0;
    if (startOverride_ >= 0) {
        start = startOverride_;
        startOverride_ = -1;
    } else if (s_.rememberPosition && !util::TestMode()) {
        double p = resume_.Lookup(path, fileSize_, fileMtime_);
        if (p > 0) start = p;
    }
    if (!util::TestMode()) resume_.Put(path, fileSize_, fileMtime_, start);  // shows up in Recent right away
    StartPrefetch(path, start, loadSerial_);
    if (EditActive()) editor_->OnFileOpened(path);
    if (hwnd_) {
        UpdateTitle();
        RefreshBar();
    }
    return start;
}

void App::IssueLoad(mpv_handle* h, const std::wstring& path, double start) {
    std::string p = util::Utf8(path);
    char opts[64] = "";
    // Resume at the keyframe before the saved position: an exact seek would first decode up to a whole GOP
    // (4 s / 250 frames in OBS recordings) before showing anything. hr-seek is back to normal after this file.
    if (start > 0) snprintf(opts, sizeof opts, "start=%.3f,hr-seek=no", start);
    mpvx::SetFlag(h, "pause", false);
    mpvx::Cmd(h, {"loadfile", p.c_str(), "replace", "-1", opts});
}

void App::Open(const std::wstring& rawPath) {
    std::wstring path = rawPath.find(L"://") != std::wstring::npos ? rawPath : util::FullPath(rawPath);
    // A file that's in the playlist plays as that entry; anything else replaces the playlist (single files then
    // navigate their folder, like MPC-HC).
    auto it = std::find_if(plist_.begin(), plist_.end(), [&](const std::wstring& p) { return util::Lower(p) == util::Lower(path); });
    if (it != plist_.end()) plPos_ = (int)(it - plist_.begin());
    else {
        plist_.clear();
        plPos_ = -1;
    }
    eofHandled_ = false;
    double start = PrepareOpen(path);
    RefreshPlaylistWindow();
    if (!mpv_) {
        std::lock_guard<std::mutex> l(pendingMu_);
        pendingFile_ = path;
        pendingStart_ = start;
        return;
    }
    IssueLoad(mpv_, path, start);
    if (EditActive()) mpvx::SetFlag(mpv_, "pause", true);  // Edit mode opens files paused, like FastCut
}

void App::SaveResume() {
    if (path_.empty() || !mpv_ || util::TestMode()) return;
    if (s_.rememberPosition) {
        double pos = mpvx::GetDouble(mpv_, "time-pos", -1), dur = mpvx::GetDouble(mpv_, "duration", 0);
        if (pos >= 0) {
            // Near the start or the end means "watched from the beginning next time".
            if (pos < 5 || (dur > 0 && pos > dur - 10)) pos = 0;
            resume_.Put(path_, fileSize_, fileMtime_, pos);
        }
    }
    resume_.Save();
}

void App::CloseFile() {
    if (EditActive()) editor_->OnFileClosed();
    SaveResume();
    RetireThumbnails();
    mpvx::Cmd(mpv_, {"stop"});
    path_.clear();
    bar_ = BarState{false, true, bar_.mute, bar_.remaining, 0, 0, bar_.speed, -1, -1, bar_.volume, bar_.volumeMax, {}};
    UpdateTitle();
    RefreshBar();
}

void App::RetireThumbnails() {
    if (!thumbs_) return;
    // Stopping joins the decoder thread and destroys its mpv instance; do that off the UI thread.
    Thumbnailer* t = thumbs_.release();
    retiring_.emplace_back([t] { delete t; });
}

static bool IsMediaExt(const std::wstring& name) {
    static const wchar_t* exts[] = {L"mp4", L"m4v", L"mkv", L"webm", L"mov", L"avi", L"wmv", L"flv", L"ts", L"m2ts",
                                    L"mts", L"mpg", L"mpeg", L"vob", L"ogv", L"3gp", L"3g2", L"asf", L"rm", L"rmvb",
                                    L"f4v", L"divx", L"hevc", L"h264", L"y4m", L"mp3", L"flac", L"wav", L"m4a", L"aac",
                                    L"ogg", L"opus", L"wma", L"ac3", L"dts", L"mka", L"ape", L"wv", L"aiff", L"gif"};
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring e = util::Lower(name.substr(dot + 1));
    for (auto x : exts)
        if (e == x) return true;
    return false;
}

void App::StepFile(int dir) {
    if (path_.empty()) return;
    std::wstring folder = util::DirName(path_);
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((folder + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsMediaExt(fd.cFileName)) names.push_back(fd.cFileName);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(), -1,
                               b.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN;
    });
    std::wstring cur = util::Lower(util::FileName(path_));
    int idx = -1;
    for (size_t i = 0; i < names.size(); i++)
        if (util::Lower(names[i]) == cur) idx = (int)i;
    int next = idx + dir;
    if (idx < 0 || next < 0 || next >= (int)names.size()) {
        mpvx::ShowText(mpv_, dir > 0 ? L"No next file" : L"No previous file");
        return;
    }
    Open(folder + L"\\" + names[next]);
}

void App::StartPrefetch(const std::wstring& path, double start, unsigned serial) {
    bool readAhead = s_.prefetch;
    HANDLE ready = hwndReady_;
    std::thread([this, path, start, serial, readAhead, ready] {
        auto* m = new IndexMsg;
        m->ok = ReadMediaIndex(path, &m->idx, false);
        util::TimingLog(m->ok ? "prefetch: index read" : "prefetch: no mp4 index");
        if (readAhead) {
            // Warm the OS cache for where playback starts, while libmpv is still loading and initialising.
            uint64_t off = 0;
            if (m->ok && !m->idx.keyOffsets.empty()) {
                auto it = std::upper_bound(m->idx.keyTimes.begin(), m->idx.keyTimes.end(), start);
                size_t i = it == m->idx.keyTimes.begin() ? 0 : (size_t)(it - m->idx.keyTimes.begin() - 1);
                off = m->idx.keyOffsets[i];
            }
            HANDLE fh = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (fh != INVALID_HANDLE_VALUE) {
                const DWORD chunk = 1 << 20;
                std::vector<uint8_t> buf(chunk);
                for (int i = 0; i < 2; i++) {
                    OVERLAPPED o = {};
                    uint64_t at = off + (uint64_t)i * chunk;
                    o.Offset = (DWORD)at;
                    o.OffsetHigh = (DWORD)(at >> 32);
                    DWORD got = 0;
                    if (!ReadFile(fh, buf.data(), chunk, &got, &o) || got < chunk) break;
                }
                CloseHandle(fh);
            }
            util::TimingLog("prefetch: start region read");
        }
        m->spinning = util::IsOnSpinningDisk(path);
        WaitForSingleObject(ready, INFINITE);
        if (!PostMessageW(hwnd_, WM_APP_INDEX, serial, (LPARAM)m)) delete m;
    }).detach();
}

void App::OnIndex(void* raw, unsigned serial) {
    std::unique_ptr<IndexMsg> m((IndexMsg*)raw);
    if (serial != loadSerial_) return;
    spinning_ = m->spinning;
    if (!m->ok) return;
    index_ = std::move(m->idx);
    indexReady_ = true;
    if (index_.duration > bar_.dur) {
        bar_.dur = index_.duration;
        if (thumbs_) thumbs_->SetDuration(bar_.dur);
        RefreshBar();
    }
    if (vidW_ == 0 && index_.width > 0) {
        // Size the window from the header before libmpv has decoded anything.
        vidW_ = index_.width;
        vidH_ = index_.height;
        if (fitPending_ && s_.autofit && !EditActive()) ResizeToVideo(0, true);
    }
    if (thumbs_) thumbs_->SetKeyTimes(index_.keyTimes);
}

void App::StartThumbnails() {
    if (!s_.thumbnails || path_.empty() || !mpv_ || thumbs_) return;
    if (mpvx::GetInt(mpv_, "dwidth", 0) <= 0) return;  // audio only
    thumbs_ = std::make_unique<Thumbnailer>();
    int w = std::min(320, MulDiv(176, dpi_, 96));
    // Prefill only on SSDs: on a spinning disk the extra seeks would compete with playback.
    thumbs_->Start(hwnd_, WM_APP_THUMB, path_, indexReady_ ? index_.keyTimes : std::vector<double>{}, bar_.dur, w,
                   s_.thumbCache, !spinning_);
}

// ------------------------------------------------------------------------------------------------ mpv events

void App::DrainEvents() {
    if (!mpv_) return;
    while (true) {
        mpv_event* e = g_mpv.mpv_wait_event(mpv_, 0);
        if (e->event_id == MPV_EVENT_NONE) break;
        switch (e->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE:
            OnProperty((mpv_event_property*)e->data);
            break;
        case MPV_EVENT_FILE_LOADED:
            util::TimingLog("file loaded");
            UpdateTitle();
            break;
        case MPV_EVENT_VIDEO_RECONFIG:
            break;
        case MPV_EVENT_PLAYBACK_RESTART:
            if (!firstFrameLogged_) {
                firstFrameLogged_ = true;
                util::TimingLog("first frame (playback-restart)");
                if (benchExit_) {
                    benchExit_ = false;
                    SetTimer(hwnd_, 9, 1500, nullptr);  // let an external frame grabber see it, then exit
                }
                SetTimer(hwnd_, 3, 1500, nullptr);  // start thumbnails once playback has settled
                mpvx::SetStr(mpv_, "hr-seek", "default");  // the resume seek used keyframes only
            }
            bar_.pos = mpvx::GetDouble(mpv_, "time-pos", bar_.pos);
            {
                char b[96];
                snprintf(b, sizeof b, "playback restart at %.3f", bar_.pos);
                util::TimingLog(b);
            }
            RefreshBar();
            break;
        case MPV_EVENT_END_FILE: {
            auto* ef = (mpv_event_end_file*)e->data;
            if (ef->reason == MPV_END_FILE_REASON_ERROR) {
                std::wstring msg = L"Could not open: " + util::FileName(path_) + L" (" +
                                   util::Wide(g_mpv.mpv_error_string(ef->error)) + L")";
                mpvx::ShowText(mpv_, msg, 4000);
                util::TimingLog("load error");
                if (benchExit_) PostMessageW(hwnd_, WM_CLOSE, 0, 0);
            }
            break;
        }
        case MPV_EVENT_SHUTDOWN:
            return;
        default:
            break;
        }
    }
}

void App::OnProperty(mpv_event_property* p) {
    auto dbl = [&](double def) { return p->format == MPV_FORMAT_DOUBLE && p->data ? *(double*)p->data : def; };
    auto flag = [&] { return p->format == MPV_FORMAT_FLAG && p->data && *(int*)p->data != 0; };
    auto i64 = [&] { return p->format == MPV_FORMAT_INT64 && p->data ? *(int64_t*)p->data : 0; };
    if (!strcmp(p->name, "pause")) bar_.paused = flag();
    else if (!strcmp(p->name, "duration")) {
        // Fragmented MP4 opened via mfra: libmpv only learns the length as it reads; the header index knows it.
        bar_.dur = std::max(dbl(0), indexReady_ ? index_.duration : 0.0);
        if (thumbs_) thumbs_->SetDuration(bar_.dur);
    } else if (!strcmp(p->name, "volume")) bar_.volume = (int)std::lround(dbl(bar_.volume));
    else if (!strcmp(p->name, "mute")) bar_.mute = flag();
    else if (!strcmp(p->name, "speed")) bar_.speed = dbl(1);
    else if (!strcmp(p->name, "ab-loop-a")) bar_.abA = dbl(-1);
    else if (!strcmp(p->name, "ab-loop-b")) bar_.abB = dbl(-1);
    else if (!strcmp(p->name, "chapter-list")) {
        bar_.chapters.clear();
        if (p->format == MPV_FORMAT_NODE && p->data) {
            auto* n = (mpv_node*)p->data;
            if (n->format == MPV_FORMAT_NODE_ARRAY)
                for (int i = 0; i < n->u.list->num; i++) {
                    mpv_node& c = n->u.list->values[i];
                    if (c.format != MPV_FORMAT_NODE_MAP) continue;
                    for (int k = 0; k < c.u.list->num; k++)
                        if (!strcmp(c.u.list->keys[k], "time") && c.u.list->values[k].format == MPV_FORMAT_DOUBLE)
                            bar_.chapters.push_back(c.u.list->values[k].u.double_);
                }
        }
    } else if (!strcmp(p->name, "dwidth") || !strcmp(p->name, "dheight")) {
        int64_t v = i64();
        if (!strcmp(p->name, "dwidth")) vidW_ = v ? v : vidW_;
        else vidH_ = v ? v : vidH_;
        if (fitPending_ && vidW_ > 0 && vidH_ > 0 && mpvx::GetInt(mpv_, "dwidth", 0) > 0) {
            if (s_.autofit && !EditActive()) ResizeToVideo(0, true);
            fitPending_ = false;
        }
    } else if (!strcmp(p->name, "idle-active")) {
        idle_ = flag();
        if (idle_) bar_.hasFile = false;
    } else if (!strcmp(p->name, "eof-reached")) {
        // keep-open stops on the last frame; then the playlist / "after playback" decides what happens.
        bool eof = flag();
        if (eof && !eofHandled_ && bar_.hasFile) {
            eofHandled_ = true;
            OnEndOfFile();
        } else if (!eof) {
            eofHandled_ = false;
        }
    }
    if (!idle_ && !path_.empty()) bar_.hasFile = true;
    RefreshBar();
}

void App::UpdateTitle() {
    std::wstring app = EditActive() ? L"FastPlay (Edit)" : L"FastPlay";
    std::wstring t = path_.empty() ? app : util::FileName(path_) + L" - " + app;
    SetWindowTextW(hwnd_, t.c_str());
}

void App::RefreshBar() {
    if (barWnd_) barWnd_->Invalidate();
}
