// fastplay.ini (settings + key bindings) and the resume-position store.
#pragma once
#include "util.h"
#include <map>
#include <list>

// Minimal INI: sections of key=value lines, comments start with ';' or '#'. Keeps file order on save.
class Ini {
public:
    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path) const;
    std::wstring Get(const std::wstring& sec, const std::wstring& key, const std::wstring& def = L"") const;
    int GetInt(const std::wstring& sec, const std::wstring& key, int def) const;
    double GetDouble(const std::wstring& sec, const std::wstring& key, double def) const;
    void Set(const std::wstring& sec, const std::wstring& key, const std::wstring& value);
    bool Has(const std::wstring& sec, const std::wstring& key) const;
    std::vector<std::pair<std::wstring, std::wstring>> Section(const std::wstring& sec) const;

private:
    struct Line { std::wstring key, value, raw; bool isKey; };
    struct Sec { std::wstring name; std::vector<Line> lines; };
    std::vector<Sec> secs_;
    Sec* Find(const std::wstring& name);
    const Sec* Find(const std::wstring& name) const;
};

struct Settings {
    bool singleInstance = true;
    bool tray = false;             // closing hides to the tray and keeps mpv warm
    bool rememberPosition = true;  // resume where you left off
    bool autofit = true;           // size the window to the video (fit to 75% of the screen)
    int autofitPercent = 75;
    bool exactSeek = false;        // keyboard seeks land on exact frames instead of keyframes
    bool thumbnails = true;
    int thumbCache = 150;          // max thumbnails held in memory
    bool forceWindow = true;       // create the video output while the file opens (overlap GPU init with I/O)
    bool prefetch = true;          // read the file's index regions in parallel with libmpv start-up
    double seekSmall = 1, seekMedium = 5, seekLarge = 20;
    double speedStep = 0.25;
    int volume = 100;
    int volumeStep = 5;
    bool mute = false;
    std::wstring hwdec = L"d3d11va";
    std::wstring vo = L"gpu-next";
    std::wstring screenshotDir;    // empty: Pictures\FastPlay
    std::wstring screenshotFormat = L"png";
    std::wstring mpvOptions;       // extra "name=value;name=value" passed to mpv before init
    int afterPlayback = 0;         // 0 nothing, 1 next file, 2 repeat file, 3 close, 4 exit (MPC-HC "After playback")
    int volumeMax = 100;           // up to 200 for boost
    bool taskbarProgress = true;   // progress + play/pause/prev/next buttons on the taskbar thumbnail
    std::wstring fastcutPath;      // FastCut.exe (empty: next to FastPlay, or the fastcut/dist folder beside it)
    std::wstring ffmpegPath;       // for lossless clip export (empty: ffmpeg.exe on PATH)
    bool onTop = false;
    bool showControls = true;
    bool showCaption = true;
    int winW = 0, winH = 0;        // last windowed client size (0 = default)

    void Load(const Ini& ini);
    void Save(Ini& ini) const;
};

// Resume positions keyed by path + size + mtime (never by content hash: hashing a 30 GB file is the slow path).
class ResumeStore {
public:
    struct Entry { std::wstring path; uint64_t size = 0, mtime = 0; double pos = 0; };
    void Load(const std::wstring& file);
    void Save();
    // Position for a file, or <0 when unknown / file changed since.
    double Lookup(const std::wstring& path, uint64_t size, uint64_t mtime) const;
    void Put(const std::wstring& path, uint64_t size, uint64_t mtime, double pos);
    void Remove(const std::wstring& path);
    std::vector<std::wstring> Recent(size_t n) const;
    static bool Stat(const std::wstring& path, uint64_t* size, uint64_t* mtime);

private:
    std::wstring file_;
    std::list<Entry> items_;  // most recent first
    bool dirty_ = false;
    static constexpr size_t kMax = 1000;
};
