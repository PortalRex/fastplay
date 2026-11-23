// Export.cs port: Exporter (lossless segment export, smart cut, Discord-ready re-encode, verify) and Tools.
#include "exporter.h"
#include "ffrun.h"
#include "media.h"
#include "segio.h"
#include <objbase.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

namespace edit {

namespace {

using Args = std::vector<std::wstring>;

void Add(Args& a, std::initializer_list<std::wstring> more) { a.insert(a.end(), more.begin(), more.end()); }
void Add(Args& a, const Args& more) { a.insert(a.end(), more.begin(), more.end()); }

bool Has(const Args& a, const wchar_t* v) { return std::find(a.begin(), a.end(), v) != a.end(); }

template <class T, class U>
bool OneOf(const T& v, std::initializer_list<U> set) {
    for (const auto& x : set)
        if (v == x) return true;
    return false;
}

const std::initializer_list<const wchar_t*> kMp4Like = {L".mp4", L".m4v", L".mov", L".m4a", L".3gp"};
const std::initializer_list<const char*> kBitmapSubs = {"hdmv_pgs_subtitle", "dvd_subtitle", "dvb_subtitle", "xsub"};
const std::initializer_list<const char*> kWebAudio = {"aac", "mp3"};

bool Mp4Like(const std::wstring& ext) { return OneOf(ext, kMp4Like); }

std::wstring W(const std::string& s) { return util::Wide(s); }
std::wstring I(long long v) { return std::to_wstring(v); }

Args Head() { return {L"-hide_banner", L"-nostdin", L"-y", L"-v", L"error", L"-progress", L"pipe:1", L"-nostats"}; }

// Input options for a source file plus "-i <path>" (the fast-open options must sit right before -i).
Args SrcIn(const std::wstring& path) {
    Args a = FastInputOptions(path);
    Add(a, {L"-i", path});
    return a;
}

std::wstring Combine(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    wchar_t last = dir.back();
    return last == L'\\' || last == L'/' ? dir + name : dir + L"\\" + name;
}

std::wstring ReplaceAll(std::wstring s, const std::wstring& from, const std::wstring& to) {
    for (size_t i = 0; (i = s.find(from, i)) != std::wstring::npos; i += to.size()) s.replace(i, from.size(), to);
    return s;
}

std::wstring Colons(const std::wstring& s) {
    std::wstring r = s;
    std::replace(r.begin(), r.end(), L':', L'.');
    return r;
}

std::wstring Fixed(double v, int decimals) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%.*f", decimals, v);
    return buf;
}

bool DirExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

long long FileSize(const std::wstring& p) {  // -1 when missing
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d) || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return -1;
    return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

void WriteText(const std::wstring& path, const std::string& utf8) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw EditError(L"Can't write " + path);
    DWORD wrote = 0;
    BOOL ok = WriteFile(h, utf8.data(), (DWORD)utf8.size(), &wrote, nullptr);
    CloseHandle(h);
    if (!ok || wrote != utf8.size()) throw EditError(L"Can't write " + path);
}

// ".fastcut-<guid>" in dir; callers append "-head.ts" etc.
std::wstring TempBase(const std::wstring& dir) {
    GUID g{};
    CoCreateGuid(&g);
    wchar_t buf[40];
    swprintf(buf, 40, L"%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3, g.Data4[0],
             g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return Combine(dir, std::wstring(L".fastcut-") + buf);
}

// Deletes the listed temp files when the scope ends, whether it finished, failed or was cancelled.
struct TempFiles {
    std::vector<std::wstring> paths;
    std::wstring Add(std::wstring p) { paths.push_back(p); return p; }
    ~TempFiles() {
        for (auto& p : paths) {
            try { TryDelete(p); } catch (...) {}
        }
    }
};

std::wstring ErrorText(const std::exception& e) {
    if (auto* ee = dynamic_cast<const EditError*>(&e)) return ee->msg;
    return W(e.what());
}

std::wstring FirstLine(const std::wstring& s) {
    size_t n = s.find(L'\n');
    return n == std::wstring::npos ? s : s.substr(0, n);
}

void Report(const ProgressFn& p, double frac, const std::wstring& msg) {
    if (p) p(frac, msg);
}

// "out_time_us=123456" -> seconds. "out_time_us=N/A" and other lines -> false.
bool OutTime(const std::string& l, double* sec) {
    static const char kKey[] = "out_time_us=";
    if (l.compare(0, sizeof kKey - 1, kKey) != 0) return false;
    const char* s = l.c_str() + sizeof kKey - 1;
    char* end = nullptr;
    long long us = strtoll(s, &end, 10);
    if (end == s) return false;
    *sec = us / 1e6;
    return true;
}

double Clamp01(double v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

bool ContainsNoCase(const std::string& hay, const char* needle) {
    std::string h = hay, n = needle;
    for (auto& c : h) c = (char)tolower((unsigned char)c);
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    return h.find(n) != std::string::npos;
}

// Number after "key" (spaces skipped), like the regex "key\s*(-?[\d.]+)".
bool NumberAfter(const std::string& line, const char* key, double* v) {
    size_t i = line.find(key);
    if (i == std::string::npos) return false;
    i += strlen(key);
    while (i < line.size() && isspace((unsigned char)line[i])) i++;
    std::string num;
    if (i < line.size() && line[i] == '-') num += line[i++];
    bool digit = false;
    for (; i < line.size() && (isdigit((unsigned char)line[i]) || line[i] == '.'); i++) {
        digit |= line[i] != '.';
        num += line[i];
    }
    if (!digit) return false;
    *v = strtod(num.c_str(), nullptr);
    return true;
}

// C#'s Math.Round (to even on ties) for the values FastCut rounds.
double RoundEven(double v) { return std::nearbyint(v); }

bool HasVideo(const MediaInfo& m, const std::vector<int>& streams) {
    for (auto& st : m.streams)
        if (st.type == "video" && !st.isCoverArt && std::count(streams.begin(), streams.end(), st.index)) return true;
    return false;
}

// Video that plays in Discord everywhere: 8-bit 4:2:0 H.264.
bool DiscordPlays(const StreamInfo& v) {
    return v.codec == "h264" && (v.pixFmt == "yuv420p" || v.pixFmt == "yuvj420p" || v.pixFmt.empty());
}

long long LimitBytes(const EditSettings& s) { return s.discordSizeMB > 0 ? s.discordSizeMB * 1000000LL : 0; }

ff::LineFn ProgressParser(double dur, const ProgressFn& progress, const std::wstring& msg) {
    return [dur, progress, msg](const std::string& l) {
        double t;
        if (progress && dur > 0 && OutTime(l, &t)) progress(Clamp01(t / dur), msg);
    };
}

// Runs a and b at the same time (b on this thread) and rethrows the first exception after both have finished.
void RunBoth(const std::function<void()>& a, const std::function<void()>& b) {
    std::mutex mu;
    std::exception_ptr first;
    auto guarded = [&](const std::function<void()>& f) {
        try {
            f();
        } catch (...) {
            std::lock_guard<std::mutex> lock(mu);
            if (!first) first = std::current_exception();
        }
    };
    std::thread t(guarded, std::cref(a));
    guarded(b);
    t.join();
    if (first) std::rethrow_exception(first);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------

struct Exporter::Impl {
    MediaInfo m;
    EditSettings s;
    std::vector<int> streams;
    int rotation;  // -1 = keep
    double speed;
    KeyframeIndex kfi;
    ProgressFn progress;
    const std::vector<double>& keyframes;

    bool transcode = false;    // decided once per Run, so merged pieces all match
    double rateFactor = 1;     // lowered when an encode overshoots the size limit
    double mergeSeconds = 0;   // total length of a merged export, for the shared size budget
    double maxGop = 10;        // computed up front: smart cut reads it from two threads

    Impl(const MediaInfo& m_, const EditSettings& s_, std::vector<int> st, int rot, double sp, const KeyframeIndex& k,
         ProgressFn p)
        : m(m_), s(s_), streams(std::move(st)), rotation(rot), speed(sp), kfi(k), progress(std::move(p)),
          keyframes(kfi.pts) {
        if (keyframes.size() > 1) {
            maxGop = 0;
            for (size_t i = 1; i < keyframes.size(); i++) maxGop = std::max(maxGop, keyframes[i] - keyframes[i - 1]);
        }
    }

    bool Kept(int index) const { return std::count(streams.begin(), streams.end(), index) > 0; }

    std::wstring OutExt() const {
        if (s.discordReady && HasVideo(m, streams)) return s.format == L"mov" ? L".mov" : L".mp4";  // what Discord embeds
        return s.format == L"same" ? Ext(m.path) : L"." + s.format;
    }

    bool IsMp4() const { return Mp4Like(OutExt()); }

    // Streams that the chosen container can actually take.
    std::vector<StreamInfo> KeptStreams() const {
        bool mp4 = IsMp4();
        std::vector<StreamInfo> r;
        for (auto& st : m.streams)
            if (Kept(st.index) && !(mp4 && st.type == "subtitle" && OneOf(st.codec, kBitmapSubs))) r.push_back(st);
        return r;
    }

    bool ReencodeAudio() const {
        // Only for the MP4/MOV that Discord-ready writes when there is video. An audio-only export keeps its own
        // container (.opus, .ogg, .flac...), which cannot hold AAC: copying is both lossless and the only thing
        // that works there (the AAC re-encode made ffmpeg write an empty file).
        if (!s.discordReady || !IsMp4()) return false;
        for (auto& st : KeptStreams())
            if (st.type == "audio" && !OneOf(st.codec, kWebAudio)) return true;
        return false;
    }

    std::wstring OutDir() const { return !s.outDir.empty() && DirExists(s.outDir) ? s.outDir : util::DirName(m.path); }
    std::wstring Base() const { return BaseName(m.path); }

    std::wstring OutName(const std::wstring& tmpl, const Segment* seg, int n, int count) const {
        std::wstring name;
        if (count == 1 && seg && tmpl.find(L"{n}") != std::wstring::npos && seg->name.empty() && s.nameTemplate == tmpl) {
            name = Base() + L"-cut";
        } else {
            name = ReplaceAll(tmpl, L"{name}", Base());
            name = ReplaceAll(name, L"{n}", std::to_wstring(n));
            name = ReplaceAll(name, L"{label}", seg ? seg->name : L"");
            name = ReplaceAll(name, L"{start}", seg ? Colons(Fmt(seg->start)) : L"");
            name = ReplaceAll(name, L"{end}", seg ? Colons(Fmt(seg->end)) : L"");
        }
        // Runs of separators collapse to their first one ("a - -b" -> "a b"), then the ends are trimmed.
        auto sep = [](wchar_t c) { return c == L'-' || c == L'_' || c == L' '; };
        std::wstring collapsed;
        bool inRun = false;
        for (wchar_t c : name) {
            if (sep(c)) {
                if (!inRun) collapsed += c;
                inRun = true;
            } else {
                collapsed += c;
                inRun = false;
            }
        }
        size_t a = 0, b = collapsed.size();
        while (a < b && sep(collapsed[a])) a++;
        while (b > a && sep(collapsed[b - 1])) b--;
        name = SafeFileName(collapsed.substr(a, b - a));
        if (name.empty()) name = Base() + L"-cut";
        std::wstring ext = OutExt();
        std::wstring path = Combine(OutDir(), name + ext);
        if (_wcsicmp(util::FullPath(path).c_str(), util::FullPath(m.path).c_str()) == 0)
            path = Combine(OutDir(), name + L"-cut" + ext);
        return UniquePath(path);
    }

    // Input options applied to the source file: rotation override and speed (FPS) change.
    Args SourceOptions() const {
        Args a;
        if (rotation >= 0) Add(a, {L"-display_rotation:v", Num(-rotation)});
        if (speed != 1) Add(a, {L"-itsscale", Num(1 / speed)});
        return a;
    }

    Args OutputOptions(const std::wstring& ext, const Args& maps, bool hasTextSubs, double dur,
                       const std::vector<StreamInfo>& sts) const {
        Args a = maps;
        Add(a, {L"-c", L"copy"});
        if (hasTextSubs && Mp4Like(ext)) Add(a, {L"-c:s", L"mov_text"});
        Add(a, HevcTag(ext));
        if (ReencodeAudio()) Add(a, {L"-c:a", L"aac", L"-b:a", L"160k"});
        // A copied FLAC stream keeps the source's STREAMINFO (total length), so a 5 s cut claimed the whole song.
        // Re-encoding FLAC is lossless and fast, and writes the right header.
        else if (util::Lower(ext) == L".flac") Add(a, {L"-c:a", L"flac"});
        Add(a, {L"-map_metadata", s.preserveMetadata ? L"0" : L"-1"});
        Add(a, {L"-map_chapters", s.preserveChapters ? L"0" : L"-1"});
        if (s.avoidNegativeTs) Add(a, {L"-avoid_negative_ts", L"make_zero"});
        Add(a, FastStartArgs(s.fastStart, ext, dur, sts));
        return a;
    }

    // HEVC in MP4/MOV as "hvc1" (parameter sets in the header): Apple devices refuse the "hev1" variant.
    Args HevcTag(const std::wstring& ext) const {
        if (!Mp4Like(ext)) return {};
        for (auto& st : KeptStreams())
            if (st.type == "video" && st.codec == "hevc") return {L"-tag:v", L"hvc1"};
        return {};
    }

    // FastStartArgs, except for size-limited re-encodes: the -moov_size reservation stays in the file as padding
    // (half a MB for a short clip) that the bitrate budget can't see, so those small files use +faststart instead.
    Args FastStartFor(bool enabled, const std::wstring& ext, double dur, const std::vector<StreamInfo>& sts) const {
        if (transcode && LimitBytes(s) > 0) return enabled && Mp4Like(ext) ? Args{L"-movflags", L"+faststart"} : Args{};
        return FastStartArgs(enabled, ext, dur, sts);
    }

    static Args Maps(const std::vector<StreamInfo>& sts, int input = 0) {
        Args a;
        for (auto& st : sts) Add(a, {L"-map", I(input) + L":" + I(st.index)});
        return a;
    }

    void RunFf(Args args, double dur, double baseFrac, double span, const std::wstring& msg, Cancel* ct) const {
        Report(progress, baseFrac, msg);
        auto once = [&] {
            return ff::Run(ff::Ffmpeg(), args, ct, [&](const std::string& l) {
                double t;
                if (dur > 0 && OutTime(l, &t)) Report(progress, baseFrac + span * Clamp01(t / dur), msg);
            });
        };
        ff::Result r = once();
        auto moov = std::find(args.begin(), args.end(), L"-moov_size");
        if (r.code != 0 && moov != args.end() && ContainsNoCase(r.err, "moov")) {
            ff::AddLog(L"Reserved index space was too small; writing with +faststart instead.");
            *moov = L"-movflags";
            *(moov + 1) = L"+faststart";
            r = once();
        }
        if (r.code != 0 && !args.empty()) {
            // Don't leave an empty or half-written file behind (ffmpeg creates the output before it fails).
            const std::wstring& out = args.back();
            if (out.find(L'\\') != std::wstring::npos && GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES)
                DeleteFileW(out.c_str());
        }
        r.Check();
    }

    void CutOne(const Segment& seg, const std::wstring& outPath, double bf, double span, const std::wstring& msg,
                Cancel* ct) {
        if (transcode) Reencode(seg, outPath, bf, span, msg, ct);
        else if (s.cutMode == L"smart") SmartCut(seg, outPath, bf, span, msg, ct);
        else Cut(seg, outPath, s.cutMode, bf, span, msg, ct);
    }

    // Discord-ready re-encode: H.264 (NVENC when available, else x264), AAC unless the audio already plays, MP4.
    // The video is decoded anyway, so the cut is frame-exact. With a size limit the bitrate is budgeted and the
    // picture downscaled (1080p, 720p...) only when there would be too few bits per pixel.
    void Reencode(const Segment& seg, const std::wstring& outPath, double bf, double span, const std::wstring& msg,
                  Cancel* ct) {
        std::vector<StreamInfo> all = KeptStreams(), kept;
        const StreamInfo* v = nullptr;
        for (auto& st : all)
            if (st.type == "video" && !st.isCoverArt) { v = &st; break; }
        if (!v) throw EditError(L"No video stream to encode");
        for (auto& st : all)
            if (!(speed != 1 && st.type == "audio")) kept.push_back(st);
        bool hasAudio = std::any_of(kept.begin(), kept.end(), [](auto& st) { return st.type == "audio"; });
        bool hasSubs = std::any_of(kept.begin(), kept.end(), [](auto& st) { return st.type == "subtitle"; });
        double dur = seg.Duration() / speed;
        double budgetSeconds = s.merge ? 0 : dur;  // merged: the whole export shares one budget (set below)
        long long limit = LimitBytes(s);

        // Output size after rotation; downscale targets the short side.
        int rot = ((rotation >= 0 ? rotation : v->rotation) % 180) != 0 ? 1 : 0;
        int outW = rot ? v->height : v->width, outH = rot ? v->width : v->height;
        long long videoBps = 0;
        std::wstring scale;
        int fpsCap = 0, pickH = 0;
        if (limit > 0) {
            double seconds = budgetSeconds > 0 ? budgetSeconds : std::max(dur, mergeSeconds);
            long long audioBps = std::count_if(kept.begin(), kept.end(), [](auto& st) { return st.type == "audio"; }) * 192000LL;
            videoBps = (long long)((limit * 8 * 0.92 / std::max(0.5, seconds) - audioBps - 30000) * rateFactor);
            videoBps = std::max(videoBps, 120000LL);
            double fps = v->fps > 0 ? v->fps : 30;
            int shortSide = std::min(outW, outH);
            // Smooth motion matters most in gameplay clips: keep the full frame rate down to 720p, then try 30 fps,
            // then go smaller. Pick the first option with enough bits per pixel.
            std::vector<int> sizes;
            for (int h : {shortSide, 1080, 720, 540, 360})
                if (h <= shortSide && std::find(sizes.begin(), sizes.end(), h) == sizes.end()) sizes.push_back(h);
            std::vector<std::pair<int, double>> options;
            for (int h : sizes)
                if (h >= 720 || h == shortSide) options.push_back({h, fps});
            if (fps > 31) {
                for (int h : sizes) options.push_back({h, 30.0});
            } else {
                for (int h : sizes)
                    if (h < 720 && h != shortSide) options.push_back({h, fps});
            }
            auto pick = options.back();
            for (auto& o : options) {
                double pixelsPerSec = (double)outW * outH * o.first * o.first / ((double)shortSide * shortSide) * o.second;
                if (videoBps / pixelsPerSec >= 0.04) { pick = o; break; }
            }
            if (pick.first < shortSide) {
                pickH = pick.first;
                scale = outW >= outH ? L"scale=-2:" + I(pickH) + L":flags=lanczos" : L"scale=" + I(pickH) + L":-2:flags=lanczos";
            }
            if (pick.second < fps) {
                fpsCap = 30;
                scale = scale.empty() ? L"fps=30" : scale + L",fps=30";
            }
        }

        Args input = Head();
        Add(input, SourceOptions());
        Add(input, {L"-hwaccel", L"auto", L"-ss", Num(seg.start)});
        Add(input, SrcIn(m.path));
        Add(input, {L"-t", Num(dur)});  // decoded, so exact
        Args tail = Maps(kept);
        if (!scale.empty()) Add(tail, {L"-vf", scale});
        double gop = (fpsCap > 0 ? fpsCap : v->fps > 0 ? v->fps : 30) * 2;
        Add(tail, {L"-fps_mode", fpsCap > 0 ? L"cfr" : L"passthrough", L"-g", I((long long)RoundEven(gop))});
        // Audio is re-encoded too: copied audio would start at the packet before the cut, hidden by an edit list that
        // merging ignores (half a second of extra sound). AAC encoding is nearly free next to the video.
        if (hasAudio) Add(tail, {L"-c:a", L"aac", L"-b:a", L"192k"});
        if (hasSubs) Add(tail, {L"-c:s", L"mov_text"});
        Add(tail, {L"-map_metadata", s.preserveMetadata ? L"0" : L"-1", L"-map_chapters", s.preserveChapters ? L"0" : L"-1"});
        Add(tail, FastStartFor(true, Ext(outPath), dur, kept));
        tail.push_back(outPath);
        std::wstring size = pickH > 0 ? L" " + I(pickH) + L"p" : L"";
        std::wstring label = msg + L" (H.264" + size + (fpsCap > 0 ? L" 30fps" : L"") + L")";

        if (NvencAvailable()) {
            try {
                Args g = input;
                if (videoBps > 0)
                    Add(g, {L"-c:v", L"h264_nvenc", L"-preset", L"p5", L"-tune", L"hq", L"-rc", L"vbr", L"-b:v", I(videoBps),
                            L"-maxrate", I(videoBps * 3 / 2), L"-bufsize", I(videoBps * 2)});
                else
                    Add(g, {L"-c:v", L"h264_nvenc", L"-preset", L"p4", L"-tune", L"hq", L"-rc", L"vbr", L"-cq", L"21", L"-b:v", L"0"});
                Add(g, {L"-profile:v", L"high", L"-pix_fmt", L"yuv420p"});
                Add(g, tail);
                RunFf(g, dur, bf, span, label + L" on GPU", ct);
                return;
            } catch (const Cancelled&) {
                throw;
            } catch (const std::exception& e) {
                ff::AddLog(L"GPU encode failed, using the CPU: " + ErrorText(e));
            }
        }
        Args c = input;
        if (videoBps > 0)
            Add(c, {L"-c:v", L"libx264", L"-preset", L"faster", L"-b:v", I(videoBps), L"-maxrate", I(videoBps * 3 / 2),
                    L"-bufsize", I(videoBps * 2)});
        else
            Add(c, {L"-c:v", L"libx264", L"-preset", L"faster", L"-crf", L"20"});
        Add(c, {L"-profile:v", L"high", L"-pix_fmt", L"yuv420p"});
        Add(c, tail);
        RunFf(c, dur, bf, span, label, ct);
    }

    // Containers without a seek index (MPEG-TS/PS, FLV, AVI, raw streams): ffmpeg's input seek can land after the
    // keyframe it needs, so cuts there seek well before it and trim precisely on the output side.
    bool LooseSeek() const {
        std::string f = m.formatName;
        size_t a = 0;
        while (a <= f.size()) {
            size_t b = f.find(',', a);
            if (b == std::string::npos) b = f.size();
            std::string part = f.substr(a, b - a);
            if (OneOf(part, {"mpegts", "mpeg", "mpegvideo", "flv", "avi", "h264", "hevc"})) return true;
            a = b + 1;
        }
        return false;
    }

    // How far before a target to start reading so the demuxer is sure to land before it.
    double Preroll() const { return LooseSeek() ? std::min(60.0, maxGop * 2 + 2) : 10; }

    // Output-side seeks on stream copy compare decode timestamps, which B-frames put before the keyframe's pts.
    // Back off by the keyframe's measured lag (plus a hair), or a guess that stays clear of the previous keyframe.
    double DtsMargin(int ki) const {
        if (ki == 0) return keyframes[0] + 1;  // first keyframe: copy from the very start
        double gapHalf = (keyframes[ki] - keyframes[ki - 1]) / 2;
        double lag = ki < (int)kfi.dtsLag.size() ? kfi.dtsLag[ki] : -1;
        // -start_at_zero counts from the earliest stream; the video may start a little later than that.
        const StreamInfo* v = m.Video();
        double videoOffset = v ? std::max(0.0, v->startTime - m.startTime) : 0;
        return lag >= 0 ? std::min(lag + videoOffset + 0.004, gapHalf) : std::min(0.5, gapHalf);
    }

    // Input arguments that copy [from, to] of the source. A fast input seek lands somewhere before 'from';
    // -copyts/-start_at_zero keep the source timeline (relative to its start) so the output-side -ss/-to are exact
    // whatever streams are mapped. Callers add -avoid_negative_ts make_zero so the output still starts at 0.
    Args CopyRange(double from, double to) const {
        Args a;
        if (from <= 0.0005) {  // from the very start: keep frames with negative dts
            Add(a, {L"-copyts", L"-start_at_zero"});
            Add(a, SrcIn(m.path));
            Add(a, {L"-to", Num(to)});
        } else {
            Add(a, {L"-ss", Num(std::max(0.0, from - Preroll())), L"-copyts", L"-start_at_zero"});
            Add(a, SrcIn(m.path));
            Add(a, {L"-ss", Num(from), L"-to", Num(to)});
        }
        return a;
    }

    static void EnsureMakeZero(Args& a) {
        if (!Has(a, L"-avoid_negative_ts")) Add(a, {L"-avoid_negative_ts", L"make_zero"});
    }

    int LastKeyframeAtOrBefore(double t) const {
        for (int i = (int)keyframes.size() - 1; i >= 0; i--)
            if (keyframes[i] <= t + 0.0005) return i;
        return -1;
    }
    int FirstKeyframeAtOrAfter(double t) const {
        for (int i = 0; i < (int)keyframes.size(); i++)
            if (keyframes[i] >= t - 0.0005) return i;
        return -1;
    }

    void Cut(const Segment& seg, const std::wstring& outPath, const std::wstring& mode, double bf, double span,
             const std::wstring& msg, Cancel* ct) const {
        std::vector<StreamInfo> kept = KeptStreams();
        Args a = Head();
        Add(a, SourceOptions());
        double dur = seg.Duration() / speed;
        bool whole = seg.start <= 0.0005 && seg.end >= m.duration - 0.0005;
        int ki = LastKeyframeAtOrBefore(seg.start);
        if (whole) {
            Add(a, SrcIn(m.path));
        } else if (speed != 1) {
            // Timestamps are rescaled, so keep the simple forms.
            if (mode == L"exact") {
                Add(a, SrcIn(m.path));
                Add(a, {L"-ss", Num(seg.start / speed), L"-t", Num(dur)});
            } else {
                Add(a, {L"-ss", Num(seg.start)});
                Add(a, SrcIn(m.path));
                Add(a, {L"-t", Num(dur)});
            }
        } else if (mode == L"exact") {
            Add(a, CopyRange(seg.start, seg.end));
        } else if (ki >= 0) {
            Add(a, CopyRange(keyframes[ki] - DtsMargin(ki), seg.end));  // starts exactly at the keyframe before
        } else {  // keyframes unknown: let the demuxer pick
            Add(a, {L"-ss", Num(seg.start)});
            Add(a, SrcIn(m.path));
            Add(a, {L"-t", Num(dur)});
        }
        if (speed != 1)  // audio cannot be retimed losslessly
            kept.erase(std::remove_if(kept.begin(), kept.end(), [](auto& st) { return st.type == "audio"; }), kept.end());
        bool subs = std::any_of(kept.begin(), kept.end(), [](auto& st) { return st.type == "subtitle"; });
        Add(a, OutputOptions(Ext(outPath), Maps(kept), subs, dur, kept));
        if (Has(a, L"-copyts")) EnsureMakeZero(a);
        a.push_back(outPath);
        RunFf(a, dur, bf, span, msg, ct);
    }

    // NVENC settings matched to the source, or none when the GPU can't encode it (no NVIDIA, 10-bit H.264, 4:2:2...).
    static std::optional<Args> GpuEncoderArgs(const StreamInfo& v, long long br) {
        const wchar_t* pix = nullptr;
        if ((v.codec == "h264" || v.codec == "hevc") && OneOf(v.pixFmt, {"yuv420p", "nv12", "yuvj420p", ""}))
            pix = L"yuv420p";
        else if (v.codec == "hevc" && OneOf(v.pixFmt, {"yuv420p10le", "p010le"}))
            pix = L"p010le";
        if (!pix || !NvencAvailable()) return std::nullopt;
        return Args{L"-c:v", v.codec == "hevc" ? L"hevc_nvenc" : L"h264_nvenc", L"-preset", L"p5", L"-tune", L"hq",
                    L"-rc", L"vbr", L"-cq", L"18", L"-b:v", I(br), L"-maxrate", I(br * 2), L"-bufsize", I(br * 2),
                    L"-pix_fmt", pix};
    }

    // First frame to end of last frame of a file's video, from packet timestamps.
    static double VideoSpan(const std::wstring& path, Cancel* ct) {
        double first = 1e300, last = -1e300;
        ff::Run(ff::Ffprobe(),
                {L"-v", L"error", L"-select_streams", L"v:0", L"-show_entries", L"packet=pts_time,duration_time", L"-of",
                 L"csv=p=0", path},
                ct,
                [&](const std::string& l) {
                    const char* s = l.c_str();
                    char* end = nullptr;
                    double t = strtod(s, &end);
                    if (end == s || (*end && *end != ',')) return;
                    double d = 0;
                    if (*end == ',') {
                        const char* s2 = end + 1;
                        char* end2 = nullptr;
                        double dd = strtod(s2, &end2);
                        if (end2 != s2) d = dd;
                    }
                    first = std::min(first, t);
                    last = std::max(last, t + d);
                },
                {}, false);
        return last > first ? last - first : 0;
    }

    // Re-encodes only the frames between the cut point and the next keyframe, copies the rest, then joins them.
    // A segment with no keyframe inside is re-encoded whole. H.264/HEVC; other codecs fall back to a keyframe cut.
    void SmartCut(const Segment& seg, const std::wstring& outPath, double bf, double span, const std::wstring& msg,
                  Cancel* ct) const {
        const StreamInfo* v = m.Video();
        const wchar_t* enc = !v ? nullptr : v->codec == "h264" ? L"libx264" : v->codec == "hevc" ? L"libx265" : nullptr;
        int ki = FirstKeyframeAtOrAfter(seg.start);
        double kf = ki >= 0 ? keyframes[ki] : 1e300;
        if (!v || !enc || !Kept(v->index) || speed != 1 || std::abs(kf - seg.start) < 0.002) {
            if (!enc && v) ff::AddLog(L"Smart cut does not support " + W(v->codec) + L"; using a keyframe cut.");
            Cut(seg, outPath, L"keyframe", bf, span, msg, ct);
            return;
        }
        bool noKeyframeInside = kf >= seg.end - 0.05;
        double headEnd = noKeyframeInside ? seg.end : kf;

        TempFiles temps;
        std::wstring tmp = TempBase(OutDir());
        const std::wstring head = temps.Add(tmp + L"-head.ts"), tail = temps.Add(tmp + L"-tail.ts"),
                           list = temps.Add(tmp + L"-list.txt"), rest = temps.Add(tmp + L"-rest.mkv");

        long long br = v->bitRate > 0 ? v->bitRate : m.bitRate > 0 ? m.bitRate : 8000000;
        Args encodeInput;
        if (LooseSeek()) {
            double preH = std::max(0.0, seg.start - Preroll());
            Add(encodeInput, {L"-ss", Num(preH)});
            Add(encodeInput, SrcIn(m.path));
            Add(encodeInput, {L"-ss", Num(seg.start - preH), L"-t", Num(headEnd - seg.start)});
        } else {
            Add(encodeInput, {L"-ss", Num(seg.start), L"-t", Num(headEnd - seg.start)});
            Add(encodeInput, SrcIn(m.path));
        }
        Add(encodeInput, {L"-map", L"0:" + I(v->index), L"-fps_mode", L"passthrough"});
        std::wstring encMsg = msg + (noKeyframeInside ? L" (encoding)" : L" (encoding start)");

        auto encode = [&] {
            // The GPU encoder (NVENC) when it can take this pixel format; the CPU encoder otherwise or if it fails.
            if (auto gpu = GpuEncoderArgs(*v, br)) {
                try {
                    Args g = Head();
                    Add(g, encodeInput);
                    Add(g, *gpu);
                    Add(g, {L"-f", L"mpegts", head});
                    RunFf(g, headEnd - seg.start, bf, span * 0.3, encMsg + L" on GPU", ct);
                    return;
                } catch (const Cancelled&) {
                    throw;
                } catch (const std::exception& e) {
                    ff::AddLog(L"GPU encode failed, using the CPU: " + ErrorText(e));
                }
            }
            Args c = Head();
            Add(c, encodeInput);
            Add(c, {L"-c:v", enc, L"-b:v", I(br), L"-maxrate", I(br * 2), L"-bufsize", I(br * 2), L"-preset", L"faster"});
            if (!v->pixFmt.empty()) Add(c, {L"-pix_fmt", W(v->pixFmt)});
            if (std::wstring(enc) == L"libx265") Add(c, {L"-x265-params", L"log-level=error"});
            Add(c, {L"-f", L"mpegts", head});
            RunFf(c, headEnd - seg.start, bf, span * 0.3, encMsg, ct);
        };

        // One read of the source for everything that is copied: the video from the keyframe on (output seek, so
        // packets before it are dropped even where the demuxer lands earlier) and the audio/subtitles from the exact
        // start (an input seek with stream copy would start them at the earlier keyframe).
        std::vector<StreamInfo> others;
        for (auto& st : KeptStreams())
            if (st.index != v->index) others.push_back(st);
        auto copy = [&] {
            if (noKeyframeInside && others.empty()) return;
            Args a = Head();
            double from = noKeyframeInside ? seg.start : std::min(seg.start, kf - DtsMargin(ki));
            Add(a, {L"-ss", Num(std::max(0.0, from - Preroll())), L"-copyts", L"-start_at_zero"});
            Add(a, SrcIn(m.path));
            if (!noKeyframeInside)
                Add(a, {L"-map", L"0:" + I(v->index), L"-c", L"copy", L"-ss", Num(std::max(0.0, kf - DtsMargin(ki))), L"-to",
                        Num(seg.end), L"-f", L"mpegts", tail});
            if (!others.empty()) {
                Add(a, Maps(others));
                Add(a, {L"-c", L"copy", L"-ss", Num(seg.start), L"-to", Num(seg.end), L"-avoid_negative_ts", L"make_zero", rest});
            }
            RunFf(a, seg.end - std::min(seg.start, kf), bf + span * 0.3, span * 0.4, msg + L" (copying)", ct);
        };

        RunBoth(encode, copy);

        // The real length of the encoded part keeps the copied part exactly on the frame grid.
        double headDur = VideoSpan(head, ct);
        if (headDur <= 0) headDur = headEnd - seg.start;
        std::string text = "ffconcat version 1.0\nfile '" + util::Utf8(ConcatPath(head)) + "'\n";
        if (!noKeyframeInside)
            text += "duration " + util::Utf8(Num(headDur)) + "\nfile '" + util::Utf8(ConcatPath(tail)) + "'\n";
        WriteText(list, text);

        Args a = Head();
        Add(a, SourceOptions());  // rotation override applies to the joined video
        Add(a, {L"-f", L"concat", L"-safe", L"0", L"-i", list});
        if (!others.empty()) Add(a, {L"-i", rest});
        Add(a, SrcIn(m.path));  // only for metadata
        int metaInput = others.empty() ? 1 : 2;
        Args maps = {L"-map", L"0:0"};
        if (!others.empty()) Add(maps, {L"-map", L"1"});
        bool subs = std::any_of(others.begin(), others.end(), [](auto& st) { return st.type == "subtitle"; });
        Args o = OutputOptions(Ext(outPath), maps, subs, seg.Duration(), KeptStreams());
        for (size_t i = 0; i + 1 < o.size(); i++) {
            if (o[i] == L"-map_metadata" && o[i + 1] == L"0") o[i + 1] = I(metaInput);
            if (o[i] == L"-map_chapters") o[i + 1] = L"-1";  // the original chapters would not line up with the cut
        }
        Add(a, o);
        a.push_back(outPath);
        RunFf(a, seg.Duration(), bf + span * 0.7, span * 0.3, msg + L" (joining)", ct);
    }

    static std::string EscapeMeta(const std::wstring& v) {
        std::wstring r;
        for (wchar_t c : v) {
            if (c == L'=' || c == L';' || c == L'#' || c == L'\\' || c == L'\n') r += L'\\';
            r += c;
        }
        return util::Utf8(r);
    }

    std::wstring Merge(const std::vector<Segment>& segs, Cancel* ct) {
        double total = 0;
        for (auto& g : segs) total += g.Duration();
        mergeSeconds = total / speed;
        std::wstring outPath = OutName(s.mergeTemplate, nullptr, 0, 1);
        std::wstring tmp = TempBase(OutDir()), ext = OutExt();
        TempFiles guard;
        std::vector<std::wstring> temps;
        const std::wstring list = guard.Add(tmp + L"-list.txt"), meta = guard.Add(tmp + L"-chapters.txt");
        int n = (int)segs.size();
        for (int i = 0; i < n; i++) {
            temps.push_back(guard.Add(tmp + L"-" + I(i) + ext));
            CutOne(segs[i], temps.back(), 0.9 * i / n, 0.9 / n, L"Segment " + I(i + 1) + L"/" + I(n), ct);
        }

        // Each piece's own audio/video length: the container duration can be stretched by a subtitle cue
        // running past the cut, which would otherwise leave a gap before the next piece.
        std::vector<double> lengths;
        for (auto& t : temps) {
            double d = 0;
            try {
                d = Probe(t, ct).MediaDuration();
            } catch (const Cancelled&) {
                throw;
            } catch (const std::exception&) {
            }
            lengths.push_back(d);
        }
        std::string text = "ffconcat version 1.0\n";
        for (int i = 0; i < n; i++) {
            text += "file '" + util::Utf8(ConcatPath(temps[i])) + "'\n";
            if (lengths[i] > 0) text += "duration " + util::Utf8(Num(lengths[i])) + "\n";
        }
        WriteText(list, text);

        Args a = Head();
        Add(a, {L"-f", L"concat", L"-safe", L"0", L"-i", list});
        bool chapters = s.chaptersFromSegments;
        if (chapters) {
            std::string ch = ";FFMETADATA1\n";
            double pos = 0;
            for (int i = 0; i < n; i++) {
                double d = lengths[i] > 0 ? lengths[i] : segs[i].Duration() / speed;
                std::wstring title = !segs[i].name.empty() ? segs[i].name : L"Segment " + I(i + 1);
                ch += "[CHAPTER]\nTIMEBASE=1/1000\nSTART=" + std::to_string((long long)(pos * 1000)) +
                      "\nEND=" + std::to_string((long long)((pos + d) * 1000)) + "\ntitle=" + EscapeMeta(title) + "\n";
                pos += d;
            }
            WriteText(meta, ch);
            Add(a, {L"-f", L"ffmetadata", L"-i", meta});
        }
        Add(a, {L"-map", L"0", L"-c", L"copy", L"-map_chapters", chapters ? L"1" : L"-1"});
        if (!transcode) Add(a, HevcTag(ext));
        Add(a, FastStartFor(s.fastStart, ext, total / speed + 1, KeptStreams()));
        a.push_back(outPath);
        RunFf(a, total / speed, 0.9, 0.1, L"Merging", ct);
        return outPath;
    }

    std::vector<std::wstring> Run(const std::vector<Segment>& segs, Cancel* ct) {
        if (segs.empty()) throw EditError(L"No segments to export");
        CreateDirectoryW(OutDir().c_str(), nullptr);
        bool merged = s.merge && segs.size() > 1;
        double sum = 0, longest = 0;
        for (auto& g : segs) {
            sum += g.Duration();
            longest = std::max(longest, g.Duration());
        }
        transcode = NeedsReencode(m, s, streams, (merged ? sum : longest) / speed);
        long long limit = LimitBytes(s);
        if (merged) {
            std::wstring outPath = Merge(segs, ct);
            // A single-pass encode can overshoot a little; redo it smaller when it does.
            for (int attempt = 0; transcode && limit > 0 && FileSize(outPath) > limit && attempt < 2; attempt++) {
                rateFactor *= limit * 0.94 / FileSize(outPath);
                TryDelete(outPath);
                outPath = Merge(segs, ct);
            }
            return {outPath};
        }
        std::vector<std::wstring> outputs;
        int n = (int)segs.size();
        for (int i = 0; i < n; i++) {
            std::wstring path = OutName(s.nameTemplate, &segs[i], i + 1, n);
            std::wstring msg = L"Segment " + I(i + 1) + L"/" + I(n);
            rateFactor = 1;
            CutOne(segs[i], path, (double)i / n, 1.0 / n, msg, ct);
            for (int attempt = 0; transcode && limit > 0 && FileSize(path) > limit && attempt < 2; attempt++) {
                rateFactor *= limit * 0.94 / FileSize(path);
                CutOne(segs[i], path, (double)i / n, 1.0 / n, msg + L" (fitting the size limit)", ct);
            }
            outputs.push_back(path);
        }
        return outputs;
    }

    // Checks each output: readable, expected tracks present, duration close to what the cut should give.
    std::vector<std::wstring> Verify(const std::vector<Segment>& segs, const std::vector<std::wstring>& outs, Cancel* ct) const {
        std::vector<std::wstring> issues;
        if (segs.empty()) return issues;
        std::vector<StreamInfo> kept;
        for (auto& st : KeptStreams())
            if (!(speed != 1 && st.type == "audio")) kept.push_back(st);
        bool merged = s.merge && segs.size() > 1;
        auto isAv = [](const StreamInfo& st) { return st.type == "video" || st.type == "audio" || st.type == "subtitle"; };
        bool wantVideo = std::any_of(kept.begin(), kept.end(), [](auto& st) { return st.type == "video" && !st.isCoverArt; });
        bool wantAudio = std::any_of(kept.begin(), kept.end(), [](auto& st) { return st.type == "audio"; });
        int want = (int)std::count_if(kept.begin(), kept.end(), isAv);
        long long limit = LimitBytes(s);
        for (size_t i = 0; i < outs.size(); i++) {
            std::wstring name = util::FileName(outs[i]);
            std::vector<Segment> group = merged ? segs : std::vector<Segment>{segs[std::min(i, segs.size() - 1)]};
            long long size = FileSize(outs[i]);
            if (size <= 0) {
                issues.push_back(name + L": file is missing or empty");
                continue;
            }
            MediaInfo o;
            try {
                o = Probe(outs[i], ct);
            } catch (const Cancelled&) {
                throw;
            } catch (const std::exception& e) {
                issues.push_back(name + L": can't be read (" + FirstLine(ErrorText(e)) + L")");
                continue;
            }

            double expected = 0;
            for (auto& g : group) expected += g.Duration();
            expected /= speed;
            // A keyframe cut legitimately starts earlier (at the keyframe before, audio a few frames before that);
            // a normal cut's video only starts at the next keyframe.
            double early = 0, late = 0;
            for (auto& g : group) {
                if (transcode) continue;  // re-encoded: frame-exact
                if (s.cutMode == L"keyframe") {
                    int k = LastKeyframeAtOrBefore(g.start);
                    if (k >= 0) early += g.start - keyframes[k] + DtsMargin(k);
                } else if (s.cutMode == L"exact") {
                    int k = FirstKeyframeAtOrAfter(g.start);
                    late += k >= 0 ? std::min(g.Duration(), keyframes[k] - g.start) : g.Duration();
                }
            }
            double len = o.MediaDuration(), slack = 0.25 + 0.1 * group.size();
            if (len < expected - late / speed - slack || len > expected + early / speed + slack)
                issues.push_back(name + L": " + Fixed(len, 2) + L"s long, expected about " + Fixed(expected + early / speed, 2) + L"s");

            const StreamInfo* ov = o.Video();
            if (wantVideo && !ov)
                issues.push_back(name + L": no video. The segment has no keyframe inside, so a normal cut drops it; use a keyframe or smart cut.");
            if (wantAudio && !o.Audio()) issues.push_back(name + L": no audio track");
            int got = (int)std::count_if(o.streams.begin(), o.streams.end(), isAv);
            bool reported = (wantVideo && !ov) || (wantAudio && !o.Audio());
            if (got < want && !reported) issues.push_back(name + L": " + I(got) + L" of " + I(want) + L" tracks");
            if (s.discordReady && ov && !DiscordPlays(*ov))
                issues.push_back(name + L": video is " + W(ov->codec) + L" (" + W(ov->pixFmt) + L"), which Discord may not play");
            if (s.discordReady && limit > 0 && size > limit)
                issues.push_back(name + L": " + Fixed(size / 1e6, 1) + L" MB, over the " + I(s.discordSizeMB) + L" MB upload limit");
        }
        return issues;
    }
};

// ---------------------------------------------------------------------------------------------------------------------

Exporter::Exporter(const MediaInfo& m, const EditSettings& s, std::vector<int> streams, int rotation, double speed,
                   const KeyframeIndex& kfi, ProgressFn progress)
    : impl_(std::make_shared<Impl>(m, s, std::move(streams), rotation, speed, kfi, std::move(progress))) {}

// Whether a Discord-ready export of 'seconds' has to re-encode the video: the codec won't play, or a lossless copy
// would be over the size limit.
bool Exporter::NeedsReencode(const MediaInfo& m, const EditSettings& s, const std::vector<int>& streams, double seconds) {
    if (!s.discordReady) return false;
    const StreamInfo* v = nullptr;
    for (auto& st : m.streams)
        if (st.type == "video" && !st.isCoverArt && std::count(streams.begin(), streams.end(), st.index)) { v = &st; break; }
    if (!v) return false;
    long long limit = LimitBytes(s);
    return !DiscordPlays(*v) || (limit > 0 && seconds * (double)std::max(m.bitRate, 1LL) / 8 > limit * 0.97);
}

std::vector<std::wstring> Exporter::Run(const std::vector<Segment>& segs, Cancel* ct) {
    auto outs = impl_->Run(segs, ct);
    transcode_ = impl_->transcode;
    return outs;
}

std::vector<std::wstring> Exporter::Verify(const std::vector<Segment>& segs, const std::vector<std::wstring>& outs, Cancel* ct) {
    return impl_->Verify(segs, outs, ct);
}

// "faststart" without rewriting the whole file afterwards: reserve room for the index (moov) at the front.
// The estimate is generous (unused room is a few MB of padding); if it is ever too small, RunFf retries with +faststart.
std::vector<std::wstring> Exporter::FastStartArgs(bool enabled, const std::wstring& ext, double dur,
                                                  const std::vector<StreamInfo>& streams) {
    if (!enabled || !Mp4Like(ext)) return {};
    double perSecond = 0;
    for (auto& st : streams) {
        if (st.type == "video") perSecond += (st.fps > 0 ? st.fps : 60) * 28;
        else if (st.type == "audio") perSecond += (st.sampleRate > 0 ? st.sampleRate / 1024.0 : 50) * 16;
        else perSecond += 8;
    }
    long long bytes = (long long)((std::max(0.0, dur) + 2) * perSecond * 1.3) + 512 * 1024;
    return {L"-moov_size", I(bytes)};
}

// Whether ffmpeg can open an NVIDIA encoder on this machine (checked once; concurrent callers wait for that check).
bool Exporter::NvencAvailable() {
    static const bool available = [] {
        try {
            Cancel never;
            auto r = ff::Run(ff::Ffmpeg(),
                             {L"-v", L"error", L"-f", L"lavfi", L"-i", L"color=black:s=256x256:d=0.2", L"-c:v", L"h264_nvenc",
                              L"-f", L"null", L"-"},
                             &never, {}, {}, false);
            return r.code == 0;
        } catch (...) {
            return false;
        }
    }();
    return available;
}

std::wstring Exporter::ConcatPath(const std::wstring& p) {
    std::wstring r;
    for (wchar_t c : p) {
        if (c == L'\\') r += L'/';
        else if (c == L'\'') r += L"'\\''";
        else r += c;
    }
    return r;
}

// ---------------------------------------------------------------------------------------------------------------------
// Everything else that shells out to ffmpeg: snapshots, track extraction, concat, download, detection.

namespace tools {

namespace {

const StreamInfo& VideoOrThrow(const MediaInfo& m) {
    const StreamInfo* v = m.Video();
    if (!v) throw EditError(L"No video stream");
    return *v;
}

std::wstring TrackExt(const StreamInfo& st) {
    const std::string& c = st.codec;
    if (st.type == "video") {
        if (OneOf(c, {"h264", "hevc", "av1", "mpeg4"})) return L".mp4";
        if (OneOf(c, {"vp8", "vp9"})) return L".webm";
        if (OneOf(c, {"mjpeg", "png"}) && st.isCoverArt) return c == "png" ? L".png" : L".jpg";
        return L".mkv";
    }
    if (st.type == "audio") {
        if (OneOf(c, {"aac", "alac"})) return L".m4a";
        if (c == "mp3") return L".mp3";
        if (c == "opus") return L".opus";
        if (c == "vorbis") return L".ogg";
        if (c == "flac") return L".flac";
        if (c == "ac3") return L".ac3";
        if (c == "eac3") return L".eac3";
        if (c == "dts") return L".dts";
        if (c.compare(0, 4, "pcm_") == 0) return L".wav";
        return L".mka";
    }
    if (st.type == "subtitle") {
        if (OneOf(c, {"subrip", "mov_text"})) return L".srt";
        if (OneOf(c, {"ass", "ssa"})) return L".ass";
        if (c == "webvtt") return L".vtt";
        if (c == "hdmv_pgs_subtitle") return L".sup";
        return L".mks";
    }
    return L".bin";
}

using Params = std::map<std::string, std::string>;

// Video/audio stream parameters that have to match for a lossless join (cover art left out).
std::vector<Params> StreamParams(const std::wstring& path, Cancel* ct) {
    Args a = {L"-v", L"error", L"-show_data_hash", L"CRC32", L"-of", L"json", L"-show_entries",
              L"stream=codec_type,codec_name,profile,width,height,pix_fmt,time_base,r_frame_rate,sample_rate,channels,"
              L"extradata_hash:stream_disposition=attached_pic"};
    Add(a, SrcIn(path));
    auto r = ff::Run(ff::Ffprobe(), a, ct, {}, {}, false);
    r.Check();
    std::vector<Params> list;
    auto doc = segio::ParseJson(r.out);
    const segio::Json* streams = doc ? doc->Get("streams") : nullptr;
    if (!streams || streams->kind != segio::Json::Array) return list;
    for (auto& st : streams->arr) {
        if (!st || st->kind != segio::Json::Object) continue;
        const segio::Json* disp = st->Get("disposition");
        if (disp && disp->Num("attached_pic") == 1) continue;
        Params d;
        for (auto& kv : st->obj)
            if (kv.second && kv.second->kind != segio::Json::Object) d[kv.first] = st->Str(kv.first.c_str());
        std::string type = d.count("codec_type") ? d["codec_type"] : "";
        if (type == "video" || type == "audio") list.push_back(std::move(d));
    }
    return list;
}

std::string Fps(const std::string& r) {
    size_t slash = r.find('/');
    if (slash == std::string::npos || r.find('/', slash + 1) != std::string::npos) return r;
    char *e1 = nullptr, *e2 = nullptr;
    std::string ns = r.substr(0, slash), ds = r.substr(slash + 1);
    double n = strtod(ns.c_str(), &e1), d = strtod(ds.c_str(), &e2);
    if (ns.empty() || ds.empty() || *e1 || *e2 || d == 0) return r;
    return util::Utf8(Num(RoundEven(n / d * 100) / 100));
}

// Why stream a differs from stream b, or "" when they match.
std::wstring Differs(const Params& a, const Params& b) {
    auto V = [](const Params& d, const char* k) {
        auto it = d.find(k);
        return it == d.end() ? std::string() : it->second;
    };
    auto diff = [&](const char* k) { return V(a, k) != V(b, k); };
    if (diff("codec_name")) return L"a different codec (" + W(V(a, "codec_name")) + L" vs " + W(V(b, "codec_name")) + L")";
    if (diff("width") || diff("height"))
        return L"a different resolution (" + W(V(a, "width")) + L"x" + W(V(a, "height")) + L" vs " + W(V(b, "width")) + L"x" +
               W(V(b, "height")) + L")";
    if (diff("sample_rate") || diff("channels")) return L"a different audio format";
    if (diff("pix_fmt") || diff("profile")) return L"a different pixel format or profile";
    if (diff("r_frame_rate"))
        return L"a different frame rate (" + W(Fps(V(a, "r_frame_rate"))) + L" vs " + W(Fps(V(b, "r_frame_rate"))) + L" fps)";
    if (diff("extradata_hash")) return L"different encoder settings";
    if (diff("time_base")) return L"a different time base";
    return L"";
}

void JoinReencode(const std::vector<std::wstring>& files, const std::wstring& outPath, int sizeMB, ProgressFn progress,
                  Cancel* ct) {
    std::vector<MediaInfo> infos;
    for (auto& f : files) infos.push_back(Probe(f, ct));
    bool video = infos[0].Video() != nullptr, audio = false;
    double total = 0;
    for (auto& i : infos) {
        if ((i.Video() != nullptr) != video) throw EditError(L"Files with video and files without video can't be joined.");
        audio |= i.Audio() != nullptr;
        total += i.MediaDuration();
    }

    // Output picture: the first file's (as displayed), at the highest frame rate of all files.
    int w = 0, h = 0;
    double fps = 0;
    if (const StreamInfo* v0 = infos[0].Video()) {
        bool turned = v0->rotation % 180 != 0;
        w = (turned ? v0->height : v0->width) & ~1;
        h = (turned ? v0->width : v0->height) & ~1;
        double best = 0;
        for (auto& i : infos) {
            double f = i.Video()->fps;
            best = std::max(best, f > 0 && f <= 240 ? f : 30);
        }
        fps = RoundEven(best * 1000) / 1000;
    }
    int channels = 0;
    for (auto& i : infos)
        if (const StreamInfo* a = i.Audio()) channels = std::max(channels, a->channels);
    const wchar_t* layout = channels == 1 ? L"mono" : channels >= 6 ? L"5.1" : L"stereo";

    Args inputs;
    std::wstring graph, pads;
    for (size_t i = 0; i < files.size(); i++) {
        if (video) Add(inputs, {L"-hwaccel", L"auto"});
        Add(inputs, SrcIn(files[i]));
        std::wstring n = I((long long)i);
        if (video) {
            graph += L"[" + n + L":v:0]scale=" + I(w) + L":" + I(h) + L":force_original_aspect_ratio=decrease:flags=lanczos,pad=" +
                     I(w) + L":" + I(h) + L":(ow-iw)/2:(oh-ih)/2,setsar=1,fps=" + Num(fps) + L",format=yuv420p[v" + n + L"];";
            pads += L"[v" + n + L"]";
        }
        if (audio) {
            // A file without sound gets silence of its length, so the following files stay in sync.
            graph += infos[i].Audio()
                         ? L"[" + n + L":a:0]aresample=48000,aformat=sample_fmts=fltp:channel_layouts=" + layout + L"[a" + n + L"];"
                         : L"anullsrc=r=48000:cl=" + std::wstring(layout) + L",atrim=duration=" + Num(infos[i].MediaDuration()) +
                               L"[a" + n + L"];";
            pads += L"[a" + n + L"]";
        }
    }
    graph += pads + L"concat=n=" + I((long long)files.size()) + L":v=" + (video ? L"1" : L"0") + L":a=" + (audio ? L"1" : L"0");
    if (video) graph += L"[v]";
    if (audio) graph += L"[a]";

    std::vector<StreamInfo> outStreams;
    for (auto& st : infos[0].streams)
        if ((st.type == "video" || st.type == "audio") && !st.isCoverArt) outStreams.push_back(st);
    std::wstring ext = Ext(outPath);
    long long limit = sizeMB > 0 ? sizeMB * 1000000LL : 0;
    double rateFactor = 1;
    for (int attempt = 0;; attempt++) {
        long long videoBps = 0;
        if (limit > 0 && video)
            videoBps = std::max(120000LL, (long long)((limit * 8 * 0.92 / std::max(0.5, total) - (audio ? 192000 : 0) - 30000) * rateFactor));

        Args tail = {L"-filter_complex", graph};
        if (video) Add(tail, {L"-map", L"[v]", L"-fps_mode", L"cfr", L"-g", I((long long)RoundEven(fps * 2))});
        if (audio) Add(tail, {L"-map", L"[a]", L"-c:a", L"aac", L"-b:a", L"192k"});
        Add(tail, {L"-map_metadata", L"0", L"-map_chapters", L"-1"});
        // Size-limited: +faststart, since -moov_size padding would count against the limit (see FastStartFor).
        if (limit > 0 && video) {
            if (Mp4Like(ext)) Add(tail, {L"-movflags", L"+faststart"});
        } else {
            Add(tail, Exporter::FastStartArgs(true, ext, total, outStreams));
        }
        tail.push_back(outPath);

        bool done = false;
        if (video && Exporter::NvencAvailable()) {
            Args g = Head();
            Add(g, inputs);
            if (videoBps > 0)
                Add(g, {L"-c:v", L"h264_nvenc", L"-preset", L"p5", L"-tune", L"hq", L"-rc", L"vbr", L"-b:v", I(videoBps),
                        L"-maxrate", I(videoBps * 3 / 2), L"-bufsize", I(videoBps * 2)});
            else
                Add(g, {L"-c:v", L"h264_nvenc", L"-preset", L"p5", L"-tune", L"hq", L"-rc", L"vbr", L"-cq", L"19", L"-b:v", L"0"});
            Add(g, {L"-profile:v", L"high", L"-pix_fmt", L"yuv420p"});
            Add(g, tail);
            auto r = ff::Run(ff::Ffmpeg(), g, ct, ProgressParser(total, progress, L"Joining files (re-encoding to H.264 on the GPU)"));
            if (r.code == 0) done = true;
            else ff::AddLog(L"GPU encode failed, using the CPU: " + W(r.err));
        }
        if (!done) {
            Args c = Head();
            Add(c, inputs);
            if (video) {
                if (videoBps > 0)
                    Add(c, {L"-c:v", L"libx264", L"-preset", L"faster", L"-b:v", I(videoBps), L"-maxrate", I(videoBps * 3 / 2),
                            L"-bufsize", I(videoBps * 2)});
                else
                    Add(c, {L"-c:v", L"libx264", L"-preset", L"faster", L"-crf", L"19"});
                Add(c, {L"-profile:v", L"high", L"-pix_fmt", L"yuv420p"});
            }
            Add(c, tail);
            ff::Run(ff::Ffmpeg(), c, ct, ProgressParser(total, progress, L"Joining files (re-encoding)")).Check();
        }

        // A single-pass encode can overshoot a little; redo it smaller when it does.
        long long size = FileSize(outPath);
        if (limit <= 0 || !video || size <= limit || attempt >= 2) return;
        rateFactor *= limit * 0.94 / size;
    }
}

Args DetectArgs(const MediaInfo& m, const wchar_t* map, const wchar_t* filterOpt, const std::wstring& filter) {
    Args a = {L"-hide_banner", L"-nostdin", L"-v", L"info", L"-progress", L"pipe:1", L"-nostats", L"-hwaccel", L"auto"};
    Add(a, SrcIn(m.path));
    Add(a, {L"-map", map, filterOpt, filter, L"-f", L"null", L"-"});
    return a;
}

}  // namespace

std::wstring Snapshot(const MediaInfo& m, double t, const std::wstring& outDir, const std::wstring& fmt, Cancel* ct) {
    const StreamInfo& v = VideoOrThrow(m);
    std::wstring outPath = UniquePath(Combine(outDir, BaseName(m.path) + L"-" + Colons(Fmt(t)) + L"." + fmt));
    Args a = {L"-hide_banner", L"-nostdin", L"-y", L"-v", L"error", L"-ss", Num(t)};
    Add(a, SrcIn(m.path));
    Add(a, {L"-map", L"0:" + I(v.index), L"-frames:v", L"1", L"-update", L"1"});
    if (fmt == L"jpg") Add(a, {L"-q:v", L"2"});
    a.push_back(outPath);
    ff::Run(ff::Ffmpeg(), a, ct).Check();
    return outPath;
}

void SnapshotsEvery(const MediaInfo& m, double interval, double start, double end, const std::wstring& outDir,
                    const std::wstring& fmt, ProgressFn progress, Cancel* ct) {
    const StreamInfo& v = VideoOrThrow(m);
    std::wstring pattern = Combine(outDir, BaseName(m.path) + L"-frame-%05d." + fmt);
    Args a = Head();
    Add(a, {L"-ss", Num(start), L"-t", Num(end - start)});
    Add(a, SrcIn(m.path));
    Add(a, {L"-map", L"0:" + I(v.index), L"-vf", L"fps=1/" + Num(interval)});
    if (fmt == L"jpg") Add(a, {L"-q:v", L"2"});
    a.push_back(pattern);
    ff::Run(ff::Ffmpeg(), a, ct, ProgressParser(end - start, progress, L"Snapshots")).Check();
}

std::vector<std::wstring> ExtractTracks(const MediaInfo& m, const std::vector<StreamInfo>& streams,
                                        const std::wstring& outDir, ProgressFn progress, Cancel* ct) {
    std::vector<StreamInfo> list;
    for (auto& st : streams)
        if (st.type == "video" || st.type == "audio" || st.type == "subtitle") list.push_back(st);
    std::vector<std::wstring> outputs;
    int n = (int)list.size();
    for (int i = 0; i < n; i++) {
        const StreamInfo& st = list[i];
        std::wstring msg = L"Extracting track " + I(i + 1) + L"/" + I(n) + L" (" + W(st.type) + L" " + W(st.codec) + L")";
        Report(progress, (double)i / n, msg);
        std::wstring lang = st.language.empty() ? L"" : L"-" + st.language;
        std::wstring outPath = UniquePath(Combine(
            outDir, BaseName(m.path) + L"-track" + I(st.index) + L"-" + W(st.type) + lang + TrackExt(st)));
        Args a = Head();
        Add(a, SrcIn(m.path));
        Add(a, {L"-map", L"0:" + I(st.index), L"-c", L"copy"});
        if (st.codec == "mov_text") Add(a, {L"-c:s", L"srt"});
        if (st.isCoverArt) Add(a, {L"-frames:v", L"1"});
        a.push_back(outPath);
        double dur = m.duration;
        ff::Run(ff::Ffmpeg(), a, ct, [&, i](const std::string& l) {
            double t;
            if (dur > 0 && OutTime(l, &t)) Report(progress, (i + Clamp01(t / dur)) / n, msg);
        }).Check();
        outputs.push_back(outPath);
    }
    return outputs;
}

// Why the files can't be joined losslessly, or "" when they can. A stream copy keeps the first file's codec header
// (H.264 SPS/PPS...) and time base, so a later file encoded differently (another recording, another encoder, a
// re-encoded clip) comes out as broken or missing video.
std::wstring ConcatMismatch(const std::vector<std::wstring>& files, ProgressFn progress, Cancel* ct) {
    std::vector<Params> first;
    bool haveFirst = false;
    for (size_t i = 0; i < files.size(); i++) {
        std::wstring name = util::FileName(files[i]);
        Report(progress, (double)i / files.size(),
               L"Checking file " + I((long long)i + 1) + L"/" + I((long long)files.size()) + L": " + name);
        auto streams = StreamParams(files[i], ct);
        if (!haveFirst) {
            first = std::move(streams);
            haveFirst = true;
            continue;
        }
        bool sameTypes = streams.size() == first.size();
        for (size_t k = 0; sameTypes && k < streams.size(); k++) sameTypes = streams[k]["codec_type"] == first[k]["codec_type"];
        if (!sameTypes) return name + L" has different tracks than the first file";
        for (size_t k = 0; k < streams.size(); k++) {
            std::wstring why = Differs(streams[k], first[k]);
            if (!why.empty()) return name + L" has " + why + L" than the first file";
        }
    }
    return L"";
}

// Joins whole files: losslessly when their codec parameters match, otherwise ('reencode') by decoding them all and
// encoding one H.264 + AAC stream (NVENC when available), scaled and padded to the first file's picture size and the
// highest frame rate. sizeMB > 0 fits the result under that size.
void ConcatFiles(const std::vector<std::wstring>& files, const std::wstring& outPath, bool reencode, int sizeMB,
                 ProgressFn progress, Cancel* ct) {
    if (files.empty()) throw EditError(L"No files to join");
    if (reencode) {
        JoinReencode(files, outPath, sizeMB, progress, ct);
        return;
    }
    TempFiles temps;
    const std::wstring list = temps.Add(TempBase(util::DirName(outPath)) + L"-list.txt");
    double total = 0;
    std::string withOptions = "ffconcat version 1.0\n", plain = withOptions;
    bool anyOptions = false;
    for (auto& f : files) {
        double d = ProbeDuration(f, ct);
        total += d;
        std::string entry = "file '" + util::Utf8(Exporter::ConcatPath(f)) + "'\n";
        // Fast-open options go to the file's own demuxer ("option" directive). For a fragmented MP4 opened that way
        // the container reports only its first fragment's length, so the real duration is given explicitly or the
        // next file would start far too early.
        Args fast = FastInputOptions(f);
        std::string opts;
        for (size_t k = 0; k + 1 < fast.size(); k += 2)
            if (!fast[k].empty() && fast[k][0] == L'-') opts += "option " + util::Utf8(fast[k].substr(1)) + " " + util::Utf8(fast[k + 1]) + "\n";
        std::string dur = !opts.empty() && d > 0 ? "duration " + util::Utf8(Num(d)) + "\n" : "";
        anyOptions |= !opts.empty();
        withOptions += entry + opts + dur;
        plain += entry + dur;
    }
    WriteText(list, withOptions);
    Args a = Head();
    bool mkv = Ext(outPath) == L".mkv";
    Add(a, {L"-f", L"concat", L"-safe", L"0", L"-i", list, L"-map", L"0:v?", L"-map", L"0:a?"});
    if (mkv) Add(a, {L"-map", L"0:s?"});
    Add(a, {L"-c", L"copy", L"-map_metadata", L"0"});
    if (!mkv) Add(a, Exporter::FastStartArgs(true, Ext(outPath), total, Probe(files[0], ct).streams));
    a.push_back(outPath);
    auto r = ff::Run(ff::Ffmpeg(), a, ct, ProgressParser(total, progress, L"Merging files"));
    if (r.code != 0 && anyOptions && ContainsNoCase(r.err, "unknown keyword")) {
        // An ffmpeg older than 4.4 has no "option" directive: join without the fast-open options.
        ff::AddLog(L"This ffmpeg's concat demuxer has no \"option\" directive; joining without fast-open options.");
        WriteText(list, plain);
        r = ff::Run(ff::Ffmpeg(), a, ct, ProgressParser(total, progress, L"Merging files"));
    }
    r.Check();
}

// Remuxes a URL (e.g. an HLS .m3u8) to a local file without re-encoding.
void Download(const std::wstring& url, const std::wstring& outPath, ProgressFn progress, Cancel* ct) {
    Args a = Head();
    Add(a, {L"-i", url, L"-map", L"0:v?", L"-map", L"0:a?", L"-c", L"copy"});
    if (Ext(outPath) == L".mp4") Add(a, {L"-bsf:a", L"aac_adtstoasc", L"-movflags", L"+faststart"});
    a.push_back(outPath);
    ff::Run(ff::Ffmpeg(), a, ct, [&](const std::string& l) {
        double t;
        if (OutTime(l, &t)) Report(progress, -1, L"Downloaded " + Fmt(t, false));
    }).Check();
}

std::vector<std::pair<double, double>> DetectBlack(const MediaInfo& m, double minDur, double pixTh, ProgressFn progress,
                                                   Cancel* ct) {
    std::vector<std::pair<double, double>> res;
    std::mutex mu;
    auto a = DetectArgs(m, L"0:v:0", L"-vf", L"scale=320:-2,blackdetect=d=" + Num(minDur) + L":pix_th=" + Num(pixTh));
    ff::Run(ff::Ffmpeg(), a, ct, ProgressParser(m.duration, progress, L"Detecting black scenes"), [&](const std::string& l) {
        double b, e;
        if (NumberAfter(l, "black_start:", &b) && NumberAfter(l, "black_end:", &e)) {
            std::lock_guard<std::mutex> lock(mu);
            res.push_back({b - m.startTime, e - m.startTime});
        }
    });
    return res;
}

std::vector<std::pair<double, double>> DetectSilence(const MediaInfo& m, double noiseDb, double minDur, ProgressFn progress,
                                                     Cancel* ct) {
    std::vector<std::pair<double, double>> res;
    std::mutex mu;
    std::optional<double> start;
    auto a = DetectArgs(m, L"0:a:0", L"-af", L"silencedetect=n=" + Num(noiseDb) + L"dB:d=" + Num(minDur));
    ff::Run(ff::Ffmpeg(), a, ct, ProgressParser(m.duration, progress, L"Detecting silence"), [&](const std::string& l) {
        double t;
        std::lock_guard<std::mutex> lock(mu);
        if (NumberAfter(l, "silence_start:", &t)) {
            start = t - m.startTime;
            return;
        }
        if (NumberAfter(l, "silence_end:", &t) && start) {
            res.push_back({std::max(0.0, *start), t - m.startTime});
            start.reset();
        }
    });
    if (start) res.push_back({*start, m.duration});
    return res;
}

std::vector<double> DetectScenes(const MediaInfo& m, double threshold, ProgressFn progress, Cancel* ct) {
    std::vector<double> res;
    std::mutex mu;
    auto a = DetectArgs(m, L"0:v:0", L"-vf", L"scale=320:-2,select='gt(scene," + Num(threshold) + L")',showinfo");
    ff::Run(ff::Ffmpeg(), a, ct, ProgressParser(m.duration, progress, L"Detecting scene changes"), [&](const std::string& l) {
        double t;
        if (l.find("Parsed_showinfo") != std::string::npos && NumberAfter(l, "pts_time:", &t)) {
            std::lock_guard<std::mutex> lock(mu);
            res.push_back(t - m.startTime);
        }
    });
    std::sort(res.begin(), res.end());
    return res;
}

}  // namespace tools

}  // namespace edit
