#include "settings.h"
#include <algorithm>
#include <cstdio>
#include <cwctype>

// ---------------------------------------------------------------- Ini

static bool ReadFileUtf8(const std::wstring& path, std::wstring* out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    std::string data((size_t)sz.QuadPart, '\0');
    DWORD got = 0;
    BOOL ok = ReadFile(h, data.data(), (DWORD)data.size(), &got, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    data.resize(got);
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB) data.erase(0, 3);
    *out = util::Wide(data);
    return true;
}

static bool WriteFileUtf8(const std::wstring& path, const std::wstring& text) {
    std::string data = util::Utf8(text);
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr);
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

bool Ini::Load(const std::wstring& path) {
    secs_.clear();
    secs_.push_back({L"", {}});
    std::wstring text;
    if (!ReadFileUtf8(path, &text)) return false;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = text.size();
        std::wstring raw = text.substr(pos, nl - pos);
        if (!raw.empty() && raw.back() == L'\r') raw.pop_back();
        pos = nl + 1;
        std::wstring t = util::Trim(raw);
        if (t.size() >= 2 && t.front() == L'[' && t.back() == L']') {
            secs_.push_back({util::Lower(util::Trim(t.substr(1, t.size() - 2))), {}});
            continue;
        }
        size_t eq = t.find(L'=');
        if (t.empty() || t[0] == L';' || t[0] == L'#' || eq == std::wstring::npos) {
            if (pos <= text.size() || !raw.empty()) secs_.back().lines.push_back({L"", L"", raw, false});
            continue;
        }
        secs_.back().lines.push_back({util::Lower(util::Trim(t.substr(0, eq))), util::Trim(t.substr(eq + 1)), raw, true});
    }
    return true;
}

bool Ini::Save(const std::wstring& path) const {
    std::wstring out;
    for (const auto& s : secs_) {
        if (!s.name.empty()) out += L"[" + s.name + L"]\r\n";
        for (const auto& l : s.lines) out += (l.isKey ? l.key + L"=" + l.value : l.raw) + L"\r\n";
    }
    return WriteFileUtf8(path, out);
}

Ini::Sec* Ini::Find(const std::wstring& name) {
    for (auto& s : secs_)
        if (s.name == name) return &s;
    return nullptr;
}

const Ini::Sec* Ini::Find(const std::wstring& name) const {
    for (auto& s : secs_)
        if (s.name == name) return &s;
    return nullptr;
}

std::wstring Ini::Get(const std::wstring& sec, const std::wstring& key, const std::wstring& def) const {
    if (const Sec* s = Find(sec))
        for (const auto& l : s->lines)
            if (l.isKey && l.key == key) return l.value;
    return def;
}

bool Ini::Has(const std::wstring& sec, const std::wstring& key) const {
    if (const Sec* s = Find(sec))
        for (const auto& l : s->lines)
            if (l.isKey && l.key == key) return true;
    return false;
}

int Ini::GetInt(const std::wstring& sec, const std::wstring& key, int def) const {
    std::wstring v = Get(sec, key);
    if (v.empty()) return def;
    return (int)wcstol(v.c_str(), nullptr, 10);
}

double Ini::GetDouble(const std::wstring& sec, const std::wstring& key, double def) const {
    std::wstring v = Get(sec, key);
    if (v.empty()) return def;
    return wcstod(v.c_str(), nullptr);
}

void Ini::Set(const std::wstring& sec, const std::wstring& key, const std::wstring& value) {
    if (secs_.empty()) secs_.push_back({L"", {}});
    Sec* s = Find(sec);
    if (!s) {
        if (!secs_.back().lines.empty() && !util::Trim(secs_.back().lines.back().raw).empty())
            secs_.back().lines.push_back({L"", L"", L"", false});
        secs_.push_back({sec, {}});
        s = &secs_.back();
    }
    for (auto& l : s->lines)
        if (l.isKey && l.key == key) { l.value = value; return; }
    // Insert after the last key line so trailing blank lines stay at the end of the section.
    size_t at = s->lines.size();
    while (at > 0 && !s->lines[at - 1].isKey && util::Trim(s->lines[at - 1].raw).empty()) at--;
    s->lines.insert(s->lines.begin() + at, {key, value, L"", true});
}

std::vector<std::pair<std::wstring, std::wstring>> Ini::Section(const std::wstring& sec) const {
    std::vector<std::pair<std::wstring, std::wstring>> r;
    if (const Sec* s = Find(sec))
        for (const auto& l : s->lines)
            if (l.isKey) r.push_back({l.key, l.value});
    return r;
}

// ---------------------------------------------------------------- Settings

static std::wstring Num(double v) {
    wchar_t b[32];
    swprintf(b, 32, L"%g", v);
    return b;
}

void Settings::Load(const Ini& ini) {
    const std::wstring s = L"settings";
    singleInstance = ini.GetInt(s, L"single_instance", singleInstance) != 0;
    tray = ini.GetInt(s, L"tray", tray) != 0;
    rememberPosition = ini.GetInt(s, L"remember_position", rememberPosition) != 0;
    autofit = ini.GetInt(s, L"autofit", autofit) != 0;
    autofitPercent = ini.GetInt(s, L"autofit_percent", autofitPercent);
    exactSeek = ini.GetInt(s, L"exact_seek", exactSeek) != 0;
    mixAudioTracks = ini.GetInt(s, L"mix_audio_tracks", mixAudioTracks) != 0;
    discordClipMB = std::max(0, ini.GetInt(s, L"discord_clip_mb", discordClipMB));
    thumbnails = ini.GetInt(s, L"thumbnails", thumbnails) != 0;
    thumbCache = ini.GetInt(s, L"thumbnail_cache", thumbCache);
    forceWindow = ini.GetInt(s, L"early_video_output", forceWindow) != 0;
    prefetch = ini.GetInt(s, L"prefetch", prefetch) != 0;
    seekSmall = ini.GetDouble(s, L"seek_small", seekSmall);
    seekMedium = ini.GetDouble(s, L"seek_medium", seekMedium);
    seekLarge = ini.GetDouble(s, L"seek_large", seekLarge);
    speedStep = ini.GetDouble(s, L"speed_step", speedStep);
    volume = ini.GetInt(s, L"volume", volume);
    volumeStep = ini.GetInt(s, L"volume_step", volumeStep);
    mute = ini.GetInt(s, L"mute", mute) != 0;
    hwdec = ini.Get(s, L"hwdec", hwdec);
    vo = ini.Get(s, L"vo", vo);
    screenshotDir = ini.Get(s, L"screenshot_dir", screenshotDir);
    screenshotFormat = ini.Get(s, L"screenshot_format", screenshotFormat);
    mpvOptions = ini.Get(s, L"mpv_options", mpvOptions);
    afterPlayback = ini.GetInt(s, L"after_playback", afterPlayback);
    volumeMax = ini.GetInt(s, L"volume_max", volumeMax);
    taskbarProgress = ini.GetInt(s, L"taskbar_progress", taskbarProgress) != 0;
    ffmpegPath = ini.Get(s, L"ffmpeg_path", ffmpegPath);
    onTop = ini.GetInt(s, L"on_top", onTop) != 0;
    showControls = ini.GetInt(s, L"show_controls", showControls) != 0;
    showCaption = ini.GetInt(s, L"show_caption", showCaption) != 0;
    winW = ini.GetInt(s, L"window_width", winW);
    winH = ini.GetInt(s, L"window_height", winH);
    if (autofitPercent < 20 || autofitPercent > 100) autofitPercent = 75;
    if (thumbCache < 10) thumbCache = 10;
    if (volumeMax < 100 || volumeMax > 200) volumeMax = 100;
    if (volume < 0 || volume > volumeMax) volume = 100;
    if (afterPlayback < 0 || afterPlayback > 4) afterPlayback = 0;
}

void Settings::Save(Ini& ini) const {
    const std::wstring s = L"settings";
    ini.Set(s, L"single_instance", singleInstance ? L"1" : L"0");
    ini.Set(s, L"tray", tray ? L"1" : L"0");
    ini.Set(s, L"remember_position", rememberPosition ? L"1" : L"0");
    ini.Set(s, L"autofit", autofit ? L"1" : L"0");
    ini.Set(s, L"autofit_percent", std::to_wstring(autofitPercent));
    ini.Set(s, L"exact_seek", exactSeek ? L"1" : L"0");
    ini.Set(s, L"mix_audio_tracks", mixAudioTracks ? L"1" : L"0");
    ini.Set(s, L"discord_clip_mb", std::to_wstring(discordClipMB));
    ini.Set(s, L"thumbnails", thumbnails ? L"1" : L"0");
    ini.Set(s, L"thumbnail_cache", std::to_wstring(thumbCache));
    ini.Set(s, L"early_video_output", forceWindow ? L"1" : L"0");
    ini.Set(s, L"prefetch", prefetch ? L"1" : L"0");
    ini.Set(s, L"seek_small", Num(seekSmall));
    ini.Set(s, L"seek_medium", Num(seekMedium));
    ini.Set(s, L"seek_large", Num(seekLarge));
    ini.Set(s, L"speed_step", Num(speedStep));
    ini.Set(s, L"volume", std::to_wstring(volume));
    ini.Set(s, L"volume_step", std::to_wstring(volumeStep));
    ini.Set(s, L"mute", mute ? L"1" : L"0");
    ini.Set(s, L"hwdec", hwdec);
    ini.Set(s, L"vo", vo);
    ini.Set(s, L"screenshot_dir", screenshotDir);
    ini.Set(s, L"screenshot_format", screenshotFormat);
    ini.Set(s, L"mpv_options", mpvOptions);
    ini.Set(s, L"after_playback", std::to_wstring(afterPlayback));
    ini.Set(s, L"volume_max", std::to_wstring(volumeMax));
    ini.Set(s, L"taskbar_progress", taskbarProgress ? L"1" : L"0");
    ini.Set(s, L"ffmpeg_path", ffmpegPath);
    ini.Set(s, L"on_top", onTop ? L"1" : L"0");
    ini.Set(s, L"show_controls", showControls ? L"1" : L"0");
    ini.Set(s, L"show_caption", showCaption ? L"1" : L"0");
    ini.Set(s, L"window_width", std::to_wstring(winW));
    ini.Set(s, L"window_height", std::to_wstring(winH));
}

// ---------------------------------------------------------------- ResumeStore
// One entry per line: size<TAB>mtime<TAB>position<TAB>path. Most recent first.

void ResumeStore::Load(const std::wstring& file) {
    file_ = file;
    items_.clear();
    std::wstring text;
    if (!ReadFileUtf8(file, &text)) return;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find(L'\n', pos);
        if (nl == std::wstring::npos) nl = text.size();
        std::wstring line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t t1 = line.find(L'\t'), t2 = line.find(L'\t', t1 + 1), t3 = line.find(L'\t', t2 + 1);
        if (t1 == std::wstring::npos || t2 == std::wstring::npos || t3 == std::wstring::npos) continue;
        Entry e;
        e.size = wcstoull(line.substr(0, t1).c_str(), nullptr, 10);
        e.mtime = wcstoull(line.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
        e.pos = wcstod(line.substr(t2 + 1, t3 - t2 - 1).c_str(), nullptr);
        e.path = line.substr(t3 + 1);
        if (!e.path.empty()) items_.push_back(std::move(e));
    }
}

void ResumeStore::Save() {
    if (!dirty_ || file_.empty()) return;
    std::wstring out;
    wchar_t num[96];
    for (const auto& e : items_) {
        swprintf(num, 96, L"%llu\t%llu\t%.3f\t", (unsigned long long)e.size, (unsigned long long)e.mtime, e.pos);
        out += num + e.path + L"\r\n";
    }
    if (WriteFileUtf8(file_, out)) dirty_ = false;
}

double ResumeStore::Lookup(const std::wstring& path, uint64_t size, uint64_t mtime) const {
    std::wstring key = util::Lower(path);
    for (const auto& e : items_)
        if (util::Lower(e.path) == key) return (e.size == size && e.mtime == mtime) ? e.pos : -1;
    return -1;
}

void ResumeStore::Put(const std::wstring& path, uint64_t size, uint64_t mtime, double pos) {
    Remove(path);
    items_.push_front({path, size, mtime, pos});
    while (items_.size() > kMax) items_.pop_back();
    dirty_ = true;
}

void ResumeStore::Remove(const std::wstring& path) {
    std::wstring key = util::Lower(path);
    for (auto it = items_.begin(); it != items_.end(); ++it)
        if (util::Lower(it->path) == key) { items_.erase(it); dirty_ = true; return; }
}

std::vector<std::wstring> ResumeStore::Recent(size_t n) const {
    std::vector<std::wstring> r;
    for (const auto& e : items_) {
        if (r.size() >= n) break;
        r.push_back(e.path);
    }
    return r;
}

bool ResumeStore::Stat(const std::wstring& path, uint64_t* size, uint64_t* mtime) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    *size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    *mtime = ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    return true;
}
