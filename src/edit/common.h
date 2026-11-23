// Edit mode (FastCut port): shared types. Everything under src/edit is only touched once the user switches to
// Edit mode, so View mode's start-up and memory stay exactly as they were.
#pragma once
#include "../util.h"
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

class Ini;

namespace edit {

struct Segment {
    double start = 0, end = 0;
    std::wstring name;  // label
    std::wstring tags;  // "key=value, key=value"
    bool enabled = true;
    int color = 0;      // palette index
    double Duration() const { return end - start; }
};

// Keyframe display times (seconds from the file's start, i.e. pts - format start_time, the same timeline mpv's
// time-pos and ffmpeg's "-copyts -start_at_zero" use) plus how far each keyframe's decode timestamp precedes it
// (-1 = unknown).
struct KeyframeIndex {
    std::vector<double> pts, dtsLag;
};

struct StreamInfo {
    int index = 0;
    std::string type;   // video, audio, subtitle, data, attachment
    std::string codec;
    std::wstring language, title;
    int width = 0, height = 0;
    double fps = 0;
    std::string pixFmt, profile;
    long long bitRate = 0;
    double startTime = 0, duration = 0;
    int sampleRate = 0, channels = 0;
    int rotation = 0;
    bool isCoverArt = false;
    bool encrypted = false;  // DRM (iTunes FairPlay "drmi"/"drms"...): ffmpeg can neither copy nor decode it
    bool keep = true;    // exported (Tracks tab)
    std::wstring Details() const;
};

struct Chapter {
    double start = 0, end = 0;
    std::wstring title;
};

struct MediaInfo {
    std::wstring path;
    double duration = 0, startTime = 0;
    std::string formatName;
    long long bitRate = 0;
    std::vector<StreamInfo> streams;
    std::vector<Chapter> chapters;
    std::string rawJson;          // ffprobe output (Stream details)
    bool fragmentedMfra = false;  // fragmented MP4 with an mfra index: ffmpeg/ffprobe get "-use_mfra_for pts"

    const StreamInfo* Video() const;  // first non-cover-art video stream
    const StreamInfo* Audio() const;
    double MediaDuration() const;     // longest audio/video stream (container duration can be stretched by subtitles)
};

// Edit-mode settings, stored in fastplay.ini section [edit] (FastCut's settings.json fields).
struct EditSettings {
    std::wstring outDir;                     // empty = next to the input file
    std::wstring format = L"same";
    bool merge = false;
    std::wstring cutMode = L"keyframe";      // keyframe | exact | smart
    std::wstring nameTemplate = L"{name}-{n}-{label}";
    std::wstring mergeTemplate = L"{name}-cut";
    bool preserveMetadata = true, preserveChapters = true, chaptersFromSegments = true;
    bool avoidNegativeTs = true, fastStart = true, confirmExport = true, verifyOutputs = true;
    bool discordReady = true;
    int discordSizeMB = 0;
    bool sameDiskTipAnswered = false;
    std::wstring batchMode = L"same";        // same | saved | trim
    double batchTrimStart = 0, batchTrimEnd = 0;
    std::wstring snapshotFormat = L"jpg";
    bool showThumbs = true, showWave = true, showKeyframes = true;
    bool autoSaveProject = true;
    int sidePanelWidth = 420;
    double seekSmall = 1, seekLarge = 10;
    int frameJump = 10, nudgeFrames = 1;

    void Load(const Ini& ini);
    void Save(Ini& ini) const;
};

// Progress from worker threads: frac in [0,1] (or < 0 = indeterminate) and a message. Must be thread-safe.
using ProgressFn = std::function<void(double frac, const std::wstring& msg)>;

// Cooperative cancellation; ff::Run kills the running tool and throws Cancelled when it is set.
struct Cancel {
    std::atomic<bool> flag{false};
    bool Cancelled() const { return flag.load(); }
};

// Errors carry a user-facing (wide) message.
struct EditError : std::runtime_error {
    std::wstring msg;
    explicit EditError(std::wstring m) : std::runtime_error("edit error"), msg(std::move(m)) {}
};
struct Cancelled : std::exception {};

// Util.cs
std::wstring Fmt(double seconds, bool millis = true);  // "1:02:03.456" / "02:03.456" like FastCut
std::wstring Num(double v);                            // invariant, up to 6 decimals ("0.######")
bool TryParseTime(const std::wstring& text, double* seconds);  // "1:02:03.5", "62.5", "1:02"
std::wstring SafeFileName(const std::wstring& s);
std::wstring UniquePath(const std::wstring& path);     // appends " (2)" etc. when taken
void TryDelete(const std::wstring& path);
std::wstring Ext(const std::wstring& path);            // lower-case extension with the dot, "" if none
std::wstring BaseName(const std::wstring& path);       // file name without extension
const std::vector<uint32_t>& Palette();                // segment colors (Timeline.Palette), COLORREF

}  // namespace edit
