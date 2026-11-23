// Console harness for src/edit/exporter.cpp: exports segments from a small H.264 MP4 in every cut mode (separate
// and merged), one Discord size-limited re-encode, a keyframe cut from a big fragmented OBS recording, track
// extraction, black detection and lossless joins (incl. a fragmented MP4 through the concat "option" directive).
// Every output goes under bench\edit-tests\export and is deleted at the end.
#include "../../src/edit/exporter.h"
#include "../../src/edit/ffrun.h"
#include "../../src/edit/media.h"
#include <chrono>
#include <cstdio>

using namespace edit;

static const wchar_t* kSmall = L"H:\\Claude\\p2cm-trailer.mp4";
static const wchar_t* kBig = L"D:\\Recordings\\Streams\\GTA\\2026-11-19 16-07-46.mp4";
static const wchar_t* kOut = L"H:\\Claude\\fastplay\\bench\\edit-tests\\export";

static int failures = 0;

static double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static std::string U(const std::wstring& w) { return util::Utf8(w); }

static long long Size(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return -1;
    return ((long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
}

static void PrintError(const char* what, const std::exception& e) {
    failures++;
    auto* ee = dynamic_cast<const EditError*>(&e);
    printf("  FAILED %s: %s\n", what, ee ? U(ee->msg).c_str() : e.what());
    std::wstring log = ff::LogText();
    if (log.size() > 3000) log = log.substr(log.size() - 3000);
    printf("  --- log tail ---\n%s\n  ---\n", U(log).c_str());
}

static void Describe(const std::wstring& path, double expected) {
    try {
        MediaInfo o = Probe(path, nullptr);
        const StreamInfo* v = o.Video();
        printf("    %s: %.3f s (expected %.3f), %.2f MB, %s %dx%d, %zu streams\n", U(util::FileName(path)).c_str(),
               o.MediaDuration(), expected, Size(path) / 1e6, v ? v->codec.c_str() : "no video", v ? v->width : 0,
               v ? v->height : 0, o.streams.size());
    } catch (const std::exception& e) {
        PrintError("probe output", e);
    }
}

static std::vector<int> AllStreams(const MediaInfo& m) {
    std::vector<int> r;
    for (auto& st : m.streams)
        if (st.keep) r.push_back(st.index);
    return r;
}

static void ExportCase(const char* name, const MediaInfo& m, const KeyframeIndex& kfi, EditSettings s,
                       const std::vector<Segment>& segs) {
    printf("\n[%s] mode=%s merge=%d discord=%d limit=%dMB\n", name, U(s.cutMode).c_str(), s.merge, s.discordReady,
           s.discordSizeMB);
    int progressCalls = 0;
    double lastFrac = 0;
    Exporter ex(m, s, AllStreams(m), -1, 1.0, kfi, [&](double f, const std::wstring&) {
        progressCalls++;
        if (f >= 0) lastFrac = f;
    });
    Cancel ct;
    try {
        double t0 = Now();
        auto outs = ex.Run(segs, &ct);
        double t1 = Now();
        auto issues = ex.Verify(segs, outs, &ct);
        double t2 = Now();
        printf("  run %.0f ms, verify %.0f ms, transcoded=%d, progress calls=%d (last %.2f)\n", (t1 - t0) * 1000,
               (t2 - t1) * 1000, ex.Transcoded(), progressCalls, lastFrac);
        bool merged = s.merge && segs.size() > 1;
        for (size_t i = 0; i < outs.size(); i++) {
            double expected = 0;
            if (merged)
                for (auto& g : segs) expected += g.Duration();
            else
                expected = segs[i].Duration();
            Describe(outs[i], expected);
        }
        if (issues.empty()) printf("  verify: OK\n");
        for (auto& is : issues) printf("  verify issue: %s\n", U(is).c_str());
    } catch (const std::exception& e) {
        PrintError(name, e);
    }
}

static int CountTempFiles() {
    int n = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((std::wstring(kOut) + L"\\.fastcut-*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do n++; while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static void CleanOutDir() {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((std::wstring(kOut) + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((std::wstring(kOut) + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    CreateDirectoryW(L"H:\\Claude\\fastplay\\bench", nullptr);
    CreateDirectoryW(L"H:\\Claude\\fastplay\\bench\\edit-tests", nullptr);
    CreateDirectoryW(kOut, nullptr);
    CleanOutDir();
    printf("ffmpeg: %s\n", U(ff::Ffmpeg()).c_str());

    double t0 = Now();
    bool nv = Exporter::NvencAvailable();
    printf("NVENC available: %d (%.0f ms first check)\n", nv, (Now() - t0) * 1000);
    t0 = Now();
    Exporter::NvencAvailable();
    printf("NVENC second check: %.3f ms (cached)\n", (Now() - t0) * 1000);

    // ---- small file ----
    MediaInfo m;
    KeyframeIndex kfi;
    try {
        t0 = Now();
        m = Probe(kSmall, nullptr);
        double t1 = Now();
        kfi = Keyframes(m, m.Video()->index, nullptr);
        printf("\nsmall: %.3f s, %zu streams, format %s, probe %.0f ms, %zu keyframes (%.0f ms):", m.duration,
               m.streams.size(), m.formatName.c_str(), (t1 - t0) * 1000, kfi.pts.size(), (Now() - t1) * 1000);
        for (size_t i = 0; i < kfi.pts.size() && i < 12; i++) printf(" %.3f", kfi.pts[i]);
        printf("\n");
    } catch (const std::exception& e) {
        PrintError("probe small", e);
        return 1;
    }

    EditSettings base;
    base.outDir = kOut;
    base.discordReady = false;
    std::vector<Segment> segs(2);
    segs[0].start = 1.3, segs[0].end = 4.7, segs[0].name = L"first";
    segs[1].start = 6.2, segs[1].end = 9.5;
    // The small file has keyframes only at 0 and 8.333: an exact ("normal") cut drops video from a segment with no
    // keyframe inside (by design, Verify says so), so exact mode gets segments that contain one.
    std::vector<Segment> segsExact(2);
    segsExact[0].start = 0, segsExact[0].end = 3;
    segsExact[1].start = 7, segsExact[1].end = 10.5;
    for (const wchar_t* mode : {L"keyframe", L"exact", L"smart"}) {
        for (bool merge : {false, true}) {
            EditSettings s = base;
            s.cutMode = mode;
            s.merge = merge;
            std::string name = U(mode) + (merge ? " merged" : " separate");
            ExportCase(name.c_str(), m, kfi, s, std::wstring(mode) == L"exact" ? segsExact : segs);
        }
    }
    {
        EditSettings s = base;
        s.discordReady = true;
        s.discordSizeMB = 1;
        std::vector<Segment> one(1);
        one[0].start = 0.5, one[0].end = 10.5;
        ExportCase("discord 1MB", m, kfi, s, one);
        s.merge = true;
        ExportCase("discord 1MB merged", m, kfi, s, segs);
    }
    printf("\ntemp files left after exports: %d\n", CountTempFiles());

    // ---- tools on the small file ----
    try {
        t0 = Now();
        auto outs = tools::ExtractTracks(m, m.streams, kOut, {}, nullptr);
        printf("\nExtractTracks: %zu file(s) in %.0f ms\n", outs.size(), (Now() - t0) * 1000);
        for (auto& o : outs) Describe(o, m.duration);
    } catch (const std::exception& e) {
        PrintError("ExtractTracks", e);
    }
    try {
        t0 = Now();
        auto found = tools::DetectBlack(m, 0.1, 0.10, {}, nullptr);
        printf("DetectBlack: %zu range(s) in %.0f ms:", found.size(), (Now() - t0) * 1000);
        for (auto& r : found) printf(" [%.3f-%.3f]", r.first, r.second);
        printf("\n");
    } catch (const std::exception& e) {
        PrintError("DetectBlack", e);
    }
    try {
        t0 = Now();
        auto snap = tools::Snapshot(m, 3.25, kOut, L"jpg", nullptr);
        printf("Snapshot: %s (%lld bytes) in %.0f ms\n", U(util::FileName(snap)).c_str(), Size(snap), (Now() - t0) * 1000);
    } catch (const std::exception& e) {
        PrintError("Snapshot", e);
    }

    // ---- lossless join of a fragmented MP4 (concat "option use_mfra_for pts" + duration) ----
    try {
        std::wstring frag = std::wstring(kOut) + L"\\frag.mp4";
        ff::Run(ff::Ffmpeg(), {L"-hide_banner", L"-nostdin", L"-y", L"-v", L"error", L"-i", kSmall, L"-c", L"copy", L"-movflags",
                               L"+frag_keyframe+empty_moov", frag}, nullptr).Check();
        auto fast = FastInputOptions(frag);
        std::wstring fo;
        for (auto& x : fast) fo += x + L" ";
        printf("\nfragmented copy: fast input options = [%s]\n", U(fo).c_str());
        std::vector<std::wstring> files = {frag, frag};
        t0 = Now();
        std::wstring why = tools::ConcatMismatch(files, {}, nullptr);
        printf("ConcatMismatch: \"%s\" (%.0f ms)\n", U(why).c_str(), (Now() - t0) * 1000);
        std::wstring joined = std::wstring(kOut) + L"\\joined.mp4";
        t0 = Now();
        tools::ConcatFiles(files, joined, false, 0, {}, nullptr);
        printf("ConcatFiles lossless: %.0f ms\n", (Now() - t0) * 1000);
        Describe(joined, 2 * m.duration);
        std::wstring joined2 = std::wstring(kOut) + L"\\joined-reencode.mp4";
        t0 = Now();
        tools::ConcatFiles({kSmall, frag}, joined2, true, 2, {}, nullptr);
        printf("ConcatFiles re-encode (2 MB limit): %.0f ms\n", (Now() - t0) * 1000);
        Describe(joined2, 2 * m.duration);
    } catch (const std::exception& e) {
        PrintError("concat", e);
    }

    // ---- big fragmented OBS recording on a hard disk ----
    try {
        t0 = Now();
        MediaInfo big = Probe(kBig, nullptr);
        double t1 = Now();
        printf("\nbig: %.3f s, fragmentedMfra=%d, probe %.0f ms\n", big.duration, big.fragmentedMfra, (t1 - t0) * 1000);
        KeyframeIndex bk = Keyframes(big, big.Video()->index, nullptr);
        printf("big: %zu keyframes in %.0f ms\n", bk.pts.size(), (Now() - t1) * 1000);
        EditSettings s = base;
        s.cutMode = L"keyframe";
        std::vector<Segment> one(1);
        one[0].start = 1200, one[0].end = 1210;
        ExportCase("big keyframe 1200-1210", big, bk, s, one);
        s.cutMode = L"smart";
        ExportCase("big smart 1200.5-1210", big, bk, s, {Segment{1200.5, 1210}});
    } catch (const std::exception& e) {
        PrintError("big", e);
    }

    printf("\ntemp files left: %d\n", CountTempFiles());
    CleanOutDir();
    RemoveDirectoryW(kOut);
    printf("%s (%d failure(s))\n", failures ? "DONE WITH FAILURES" : "DONE", failures);
    return failures ? 1 : 0;
}
