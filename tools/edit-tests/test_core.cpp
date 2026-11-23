// Console tests for the edit-mode core (common, ffrun, media, segio) and the mp4index keyLags addition.
//   build:  tools\edit-tests\build-core.cmd      run:  build\edit-tests\core\test_core.exe [--full-check]
// Temp files only under bench\edit-tests\core\ (removed at the end). Media files are only read.
#include "../../src/edit/common.h"
#include "../../src/edit/ffrun.h"
#include "../../src/edit/media.h"
#include "../../src/edit/segio.h"
#include "../../src/mp4index.h"
#include "../../src/settings.h"
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>
#include <fcntl.h>
#include <io.h>

using namespace edit;

static const wchar_t* kSmall = L"H:\\Claude\\p2cm-trailer.mp4";
static const wchar_t* kBig = L"D:\\Recordings\\Streams\\GTA\\2026-11-19 16-07-46.mp4";
static std::wstring g_tmp;
static int g_fail = 0, g_pass = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (cond) g_pass++;                               \
        else {                                            \
            g_fail++;                                     \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

static double Now() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / f.QuadPart;
}

static std::string U(const std::wstring& w) { return util::Utf8(w); }

// ---------------------------------------------------------------- child modes (this exe run by ff::Run)

static int Child(int argc, wchar_t** argv) {
    std::wstring mode = argv[1];
    _setmode(_fileno(stdout), _O_BINARY);  // exact bytes: the line splitter is under test
    _setmode(_fileno(stderr), _O_BINARY);
    if (mode == L"--echo-args") {
        // Re-parse the raw command line with CommandLineToArgvW (the contract), print each arg as hex UTF-8.
        int n = 0;
        wchar_t** a = CommandLineToArgvW(GetCommandLineW(), &n);
        for (int i = 2; i < n; i++) {
            std::string s = util::Utf8(a[i]);
            for (unsigned char c : s) printf("%02x", c);
            printf("\n");
        }
        LocalFree(a);
        return 0;
    }
    if (mode == L"--sleep") {
        Sleep(30000);
        return 0;
    }
    if (mode == L"--spam-err") {
        for (int i = 0; i < 300; i++) fprintf(stderr, i % 2 ? "err %d\n" : "err %d\r", i);
        fprintf(stdout, "a\r\nb\rc\n\nd");
        return 3;
    }
    if (mode == L"--flood") {
        std::vector<char> buf(1 << 16, 'x');
        for (int i = 0; i < 2000; i++) fwrite(buf.data(), 1, buf.size(), stdout);
        return 0;
    }
    return 1;
}

static std::wstring SelfExe() {
    wchar_t b[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH * 2);
    return std::wstring(b, n);
}

static std::string Hex(const std::wstring& w) {
    std::string r;
    char b[3];
    for (unsigned char c : util::Utf8(w)) {
        snprintf(b, 3, "%02x", c);
        r += b;
    }
    return r;
}

// ---------------------------------------------------------------- tests

static void TestUtil() {
    printf("[util]\n");
    CHECK(Fmt(3723.4567) == L"01:02:03.457", "Fmt %s", U(Fmt(3723.4567)).c_str());
    CHECK(Fmt(-1.5) == L"-00:00:01.500", "Fmt neg %s", U(Fmt(-1.5)).c_str());
    CHECK(Fmt(59.9996, false) == L"00:01:00", "Fmt nomillis %s", U(Fmt(59.9996, false)).c_str());
    CHECK(Num(1.5) == L"1.5" && Num(2) == L"2" && Num(0.1234567) == L"0.123457" && Num(-0.0000001) == L"0",
          "Num %s %s %s", U(Num(1.5)).c_str(), U(Num(0.1234567)).c_str(), U(Num(-0.0000001)).c_str());
    double t = 0;
    CHECK(TryParseTime(L"1:02:03.5", &t) && t == 3723.5, "parse h:m:s %f", t);
    CHECK(TryParseTime(L" 62,5 ", &t) && t == 62.5, "parse comma %f", t);
    CHECK(TryParseTime(L"-1:02", &t) && t == -62, "parse neg %f", t);
    CHECK(TryParseTime(L"00:00:14.616000000", &t) && std::fabs(t - 14.616) < 1e-9, "parse mkv %f", t);
    CHECK(!TryParseTime(L"1:2:3:4", &t) && !TryParseTime(L"abc", &t) && !TryParseTime(L"", &t) &&
              !TryParseTime(L"1:-2", &t) && !TryParseTime(L"inf", &t) && !TryParseTime(L"1:", &t),
          "parse rejects");
    CHECK(SafeFileName(L"  a:b*c?\"d<>|/\\e\t ") == L"a_b_c__d_____e_", "SafeFileName %s", U(SafeFileName(L"  a:b*c?\"d<>|/\\e\t ")).c_str());
    CHECK(Ext(L"C:\\x.y\\Movie.MP4") == L".mp4" && Ext(L"C:\\x.y\\noext") == L"" && Ext(L"a.") == L"", "Ext");
    CHECK(BaseName(L"C:\\x.y\\Movie.tar.gz") == L"Movie.tar" && BaseName(L"plain") == L"plain", "BaseName");
    CHECK(Palette().size() == 8 && Palette()[0] == RGB(0x3b, 0x82, 0xf6), "Palette");

    // UniquePath + TryDelete in the temp folder.
    std::wstring p = g_tmp + L"\\u.txt";
    CHECK(UniquePath(p) == p, "UniquePath free");
    CloseHandle(CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr));
    CHECK(UniquePath(p) == g_tmp + L"\\u (2).txt", "UniquePath taken %s", U(UniquePath(p)).c_str());
    TryDelete(p);
    CHECK(GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES, "TryDelete");

    StreamInfo v;
    v.type = "video"; v.width = 1920; v.height = 1080; v.fps = 30000.0 / 1001; v.pixFmt = "yuv420p"; v.bitRate = 6000123;
    v.rotation = 90; v.title = L"Cam";
    CHECK(v.Details() == L"1920x1080 29.97fps yuv420p 6000kb/s rot 90\u00B0 \"Cam\"", "Details %s", U(v.Details()).c_str());
    StreamInfo a;
    a.type = "audio"; a.sampleRate = 48000; a.channels = 2;
    CHECK(a.Details() == L"48000Hz 2ch", "Details audio");

    // EditSettings through an ini file.
    EditSettings s;
    s.outDir = L"D:\\Out dir"; s.merge = true; s.cutMode = L"smart"; s.discordSizeMB = 25; s.seekSmall = 0.5;
    s.batchTrimEnd = 12.25; s.showWave = false; s.sidePanelWidth = 500; s.nameTemplate = L"{name}-ü-{n}";
    Ini ini;
    s.Save(ini);
    std::wstring iniPath = g_tmp + L"\\t.ini";
    CHECK(ini.Save(iniPath), "ini save");
    Ini ini2;
    CHECK(ini2.Load(iniPath), "ini load");
    EditSettings r;
    r.Load(ini2);
    CHECK(r.outDir == s.outDir && r.merge && r.cutMode == L"smart" && r.discordSizeMB == 25 && r.seekSmall == 0.5 &&
              r.batchTrimEnd == 12.25 && !r.showWave && r.sidePanelWidth == 500 && r.nameTemplate == s.nameTemplate &&
              r.fastStart && r.format == L"same",
          "EditSettings round trip");
    TryDelete(iniPath);
}

static void TestRun() {
    printf("[ffrun]\n");
    printf("  ffmpeg  = %s\n  ffprobe = %s\n  available = %d\n", U(ff::Ffmpeg()).c_str(), U(ff::Ffprobe()).c_str(),
           ff::Available());
    CHECK(ff::Available(), "tools found");

    std::vector<std::wstring> args = {L"plain", L"", L"with space", L"quote\"in", L"trail\\", L"trail space\\",
                                      L"\\\\server\\share\\a b\\", L"back\\\\\"quote", L"tab\there", L"ünïcödé ✓",
                                      L"line\nbreak", L"a&b|c(d)'e;f", L"\"\"", L"\\"};
    double t0 = Now();
    auto r = ff::Run(SelfExe(), [&] { auto a = args; a.insert(a.begin(), L"--echo-args"); return a; }(), nullptr, {}, {}, false);
    double ms = Now() - t0;
    std::vector<std::string> lines;
    size_t st = 0;
    while (st < r.out.size()) {
        size_t e = r.out.find('\n', st);
        if (e == std::string::npos) e = r.out.size();
        std::string l = r.out.substr(st, e - st);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
        st = e + 1;
    }
    bool same = lines.size() == args.size();
    for (size_t i = 0; same && i < args.size(); i++) same = lines[i] == Hex(args[i]);
    CHECK(r.code == 0 && same, "argv round trip (code %d, %zu lines)", r.code, lines.size());
    printf("  argv round trip of %zu tricky args: %s (%.1f ms incl. process start)\n", args.size(), same ? "ok" : "MISMATCH", ms);

    std::vector<std::string> outLines, errLines;
    r = ff::Run(SelfExe(), {L"--spam-err"}, nullptr, [&](const std::string& l) { outLines.push_back(l); },
                [&](const std::string& l) { errLines.push_back(l); }, false);
    CHECK(r.code == 3, "exit code %d", r.code);
    CHECK(errLines.size() == 300 && errLines[0] == "err 0" && errLines[299] == "err 299", "stderr lines %zu", errLines.size());
    size_t tailCount = std::count(r.err.begin(), r.err.end(), '\n') + 1;
    CHECK(tailCount == 100 && r.err.rfind("err 200", 0) == 0, "stderr tail %zu", tailCount);
    CHECK((outLines == std::vector<std::string>{"a", "b", "c", "", "d"}), "stdout line split (%zu)", outLines.size());
    try {
        r.Check();
        CHECK(false, "Check should throw");
    } catch (const EditError& e) {
        CHECK(e.msg == L"err 294\nerr 295\nerr 296\nerr 297\nerr 298\nerr 299", "Check message %s", U(e.msg).c_str());
    }

    // Cancellation kills the child quickly.
    Cancel ct;
    std::thread canceller([&] { Sleep(200); ct.flag = true; });
    t0 = Now();
    bool threw = false;
    try { ff::Run(SelfExe(), {L"--sleep"}, &ct); } catch (const Cancelled&) { threw = true; }
    ms = Now() - t0;
    canceller.join();
    CHECK(threw && ms < 1000, "cancel (%.0f ms)", ms);
    printf("  cancel of a sleeping child: %.0f ms after start (cancel at 200 ms)\n", ms);

    // RunStream stopping early.
    size_t got = 0;
    t0 = Now();
    ff::RunStream(SelfExe(), {L"--flood"}, nullptr, [&](const uint8_t*, size_t n) { got += n; return got < (1 << 20); });
    ms = Now() - t0;
    CHECK(got >= (1 << 20) && got < (8 << 20), "RunStream stop (%zu bytes)", got);
    printf("  RunStream early stop after %zu bytes: %.1f ms\n", got, ms);

    // Cancel during RunStream.
    Cancel ct2;
    std::thread c2([&] { Sleep(150); ct2.flag = true; });
    threw = false;
    t0 = Now();
    try { ff::RunStream(SelfExe(), {L"--sleep"}, &ct2, [](const uint8_t*, size_t) { return true; }); }
    catch (const Cancelled&) { threw = true; }
    ms = Now() - t0;
    c2.join();
    CHECK(threw && ms < 1000, "RunStream cancel (%.0f ms)", ms);

    try {
        ff::Run(L"C:\\nope\\missing.exe", {}, nullptr);
        CHECK(false, "missing exe should throw");
    } catch (const EditError& e) {
        printf("  missing exe -> %s\n", U(e.msg).c_str());
    }
    std::wstring cl = ff::CmdLine(ff::Ffmpeg(), {L"-i", L"a b.mp4", L"x\"y"});
    CHECK(cl == L"ffmpeg -i \"a b.mp4\" \"x\\\"y\"", "CmdLine %s", U(cl).c_str());
    ff::AddLog(L"line1\nline2");
    CHECK(ff::LogText().find(L"line1\r\nline2") != std::wstring::npos, "log");
}

static void PrintMedia(const MediaInfo& m) {
    printf("  duration %.3f  start %.3f  format %s  mfra %d  streams %zu  chapters %zu  mediaDuration %.3f\n",
           m.duration, m.startTime, m.formatName.c_str(), m.fragmentedMfra, m.streams.size(), m.chapters.size(),
           m.MediaDuration());
    for (const auto& s : m.streams)
        printf("    #%d %s %s dur %.3f start %.3f  %s\n", s.index, s.type.c_str(), s.codec.c_str(), s.duration,
               s.startTime, U(s.Details()).c_str());
}

static std::vector<double> ScanKeyframes(const MediaInfo& m, int stream) {
    MediaInfo copy = m;  // the header path is only taken for the first video stream's index
    for (auto& s : copy.streams) s.index = -100 - s.index;
    return Keyframes(copy, stream, nullptr).pts;
}

static void CompareKeys(const std::vector<double>& a, const std::vector<double>& b, size_t n, const char* what) {
    size_t count = std::min({a.size(), b.size(), n});
    double maxDiff = 0;
    for (size_t i = 0; i < count; i++) maxDiff = std::max(maxDiff, std::fabs(a[i] - b[i]));
    printf("  %s: compared %zu keyframes, max diff %.6f s\n", what, count, maxDiff);
    CHECK(count > 0 && maxDiff <= 0.0015, "%s max diff %.6f", what, maxDiff);
}

static void TestSmall() {
    printf("[small file] %s\n", U(kSmall).c_str());
    double t0 = Now();
    auto fast = FastInputOptions(kSmall);
    double tFast = Now() - t0;
    CHECK(fast.empty(), "small file: no fast options");
    t0 = Now();
    MediaInfo m = Probe(kSmall, nullptr);
    double tProbe = Now() - t0;
    printf("  FastInputOptions %.1f ms, Probe %.1f ms\n", tFast, tProbe);
    PrintMedia(m);
    const StreamInfo* v = m.Video();
    CHECK(v && !m.fragmentedMfra, "video stream");
    if (!v) return;

    t0 = Now();
    KeyframeIndex hdr = Keyframes(m, v->index, nullptr);
    double tHdr = Now() - t0;
    t0 = Now();
    std::vector<double> scan = ScanKeyframes(m, v->index);
    double tScan = Now() - t0;
    printf("  keyframes: header %zu in %.1f ms, ffprobe scan %zu in %.1f ms; first %.3f, lag[0] %.4f\n", hdr.pts.size(),
           tHdr, scan.size(), tScan, hdr.pts.empty() ? -1 : hdr.pts[0], hdr.dtsLag.empty() ? -1 : hdr.dtsLag[0]);
    CHECK(hdr.pts.size() == scan.size(), "keyframe counts %zu vs %zu", hdr.pts.size(), scan.size());
    CompareKeys(hdr.pts, scan, SIZE_MAX, "small header vs scan");
    CHECK(hdr.dtsLag.size() == hdr.pts.size(), "lag size");

    // Lag vs ffprobe's own pts - dts on keyframes.
    MediaInfo copy = m;
    for (auto& s : copy.streams) s.index = -100 - s.index;
    KeyframeIndex scanFull = Keyframes(copy, v->index, nullptr);
    double lagDiff = 0;
    for (size_t i = 0; i < std::min(scanFull.dtsLag.size(), hdr.dtsLag.size()); i++)
        lagDiff = std::max(lagDiff, std::fabs(scanFull.dtsLag[i] - hdr.dtsLag[i]));
    printf("  dts lag header vs scan: max diff %.6f s\n", lagDiff);
    CHECK(lagDiff <= 0.0015, "lag diff %.6f", lagDiff);

    t0 = Now();
    double pd = ProbeDuration(kSmall, nullptr);
    printf("  ProbeDuration %.3f in %.1f ms\n", pd, Now() - t0);
    CHECK(std::fabs(pd - m.duration) < 0.01, "ProbeDuration");

    if (const StreamInfo* a = m.Audio()) {
        size_t count = (size_t)(m.duration * kWaveRate) + 1;
        std::vector<float> wave(count, -1.f);
        std::atomic<size_t> filled{0};
        t0 = Now();
        Waveform(m, a->index, wave.data(), count, &filled, nullptr);
        double tw = Now() - t0;
        float peak = *std::max_element(wave.begin(), wave.end());
        size_t unset = std::count(wave.begin(), wave.end(), -1.f);
        printf("  Waveform: %zu/%zu peaks in %.1f ms (max %.3f, %zu unset)\n", filled.load(), count, tw, peak, unset);
        CHECK(filled.load() > count * 0.9 && peak > 0.01f, "waveform filled %zu", filled.load());

        // Cancel mid-way.
        Cancel ct;
        ct.flag = true;
        bool threw = false;
        try { Waveform(m, a->index, wave.data(), count, &filled, &ct); } catch (const Cancelled&) { threw = true; }
        CHECK(threw, "waveform cancel");
    }
}

// Default-mode (no mfra options) keyframe scan through ffprobe directly.
static std::vector<double> DefaultScan(const std::wstring& path, int stream, double startTime) {
    std::vector<double> scan;
    ff::Run(ff::Ffprobe(),
            {L"-v", L"error", L"-select_streams", std::to_wstring(stream), L"-show_entries", L"packet=pts_time,flags",
             L"-of", L"csv=p=0", path},
            nullptr,
            [&](const std::string& l) {
                size_t c = l.find(',');
                if (c != std::string::npos && c + 1 < l.size() && l[c + 1] == 'K')
                    scan.push_back(atof(l.substr(0, c).c_str()) - startTime);
            },
            {}, false);
    std::sort(scan.begin(), scan.end());
    return scan;
}

// Generated 3-minute H.264 (B-frames, so an edit list and dts lag) + AAC sine: regular and fragmented (mfra).
static void TestSynthetic() {
    printf("[synthetic 180 s files with audio]\n");
    std::wstring reg = g_tmp + L"\\syn.mp4", frag = g_tmp + L"\\syn-frag.mp4";
    double t0 = Now();
    std::vector<std::wstring> enc = {L"-v", L"error", L"-y", L"-f", L"lavfi", L"-i", L"testsrc2=size=320x180:rate=30",
                                     L"-f", L"lavfi", L"-i", L"sine=frequency=440:sample_rate=48000", L"-t", L"180",
                                     L"-c:v", L"libx264", L"-preset", L"ultrafast", L"-g", L"90", L"-bf", L"2",
                                     L"-c:a", L"aac", L"-b:a", L"64k"};
    auto a1 = enc;
    a1.push_back(reg);
    ff::Run(ff::Ffmpeg(), a1, nullptr).Check();
    auto a2 = enc;
    a2.insert(a2.end(), {L"-movflags", L"+frag_keyframe+empty_moov", frag});
    ff::Run(ff::Ffmpeg(), a2, nullptr).Check();
    printf("  generated in %.0f ms\n", Now() - t0);

    for (const auto& path : {reg, frag}) {
        bool isFrag = path == frag;
        t0 = Now();
        MediaInfo m = Probe(path, nullptr);
        double tp = Now() - t0;
        printf("  %s: Probe %.1f ms\n", isFrag ? "fragmented" : "regular", tp);
        PrintMedia(m);
        CHECK(m.fragmentedMfra == isFrag, "mfra flag %d", m.fragmentedMfra);
        const StreamInfo* v = m.Video();
        const StreamInfo* a = m.Audio();
        CHECK(v && a, "streams");
        if (!v || !a) continue;
        KeyframeIndex hdr = Keyframes(m, v->index, nullptr);
        std::vector<double> scan = DefaultScan(path, v->index, m.startTime);
        printf("    keyframes header %zu, default-mode scan %zu; first %.4f lag %.4f\n", hdr.pts.size(), scan.size(),
               hdr.pts.empty() ? -1 : hdr.pts[0], hdr.dtsLag.empty() ? -9 : hdr.dtsLag[0]);
        CHECK(hdr.pts.size() == scan.size(), "count %zu vs %zu", hdr.pts.size(), scan.size());
        CompareKeys(hdr.pts, scan, SIZE_MAX, isFrag ? "fragmented header vs default scan" : "regular header vs scan");
        if (!isFrag) {
            MediaInfo copy = m;
            for (auto& s : copy.streams) s.index = -100 - s.index;
            KeyframeIndex sc = Keyframes(copy, v->index, nullptr);
            double lagDiff = 0;
            for (size_t i = 0; i < std::min(sc.dtsLag.size(), hdr.dtsLag.size()); i++)
                lagDiff = std::max(lagDiff, std::fabs(sc.dtsLag[i] - hdr.dtsLag[i]));
            printf("    dts lag header vs scan max diff %.6f (scan lag[0] %.4f)\n", lagDiff, sc.dtsLag.empty() ? -9 : sc.dtsLag[0]);
            CHECK(lagDiff <= 0.0015, "lag diff");
        } else {
            auto r = ff::Run(ff::Ffprobe(), {L"-v", L"error", L"-show_entries", L"format=duration", L"-of", L"csv=p=0", path},
                             nullptr, {}, {}, false);
            double d = atof(r.out.c_str());
            printf("    duration: default-mode ffprobe %.3f, Probe (index) %.3f\n", d, m.duration);
            CHECK(std::fabs(d - m.duration) < 0.05, "fragmented duration");
        }

        size_t count = (size_t)(m.duration * kWaveRate) + 1;
        std::vector<float> wave(count, -1.f);
        std::atomic<size_t> filled{0};
        t0 = Now();
        Waveform(m, a->index, wave.data(), count, &filled, nullptr);
        double tw = Now() - t0;
        size_t unset = std::count(wave.begin(), wave.end(), -1.f);
        float lo = 1, hi = 0;
        for (size_t i = 100; i + 200 < count; i++) {
            lo = std::min(lo, wave[i]);
            hi = std::max(hi, wave[i]);
        }
        printf("    Waveform (%d parts): %zu/%zu peaks in %.0f ms, %zu unset, steady-state peaks %.3f..%.3f\n",
               m.duration < 120 ? 1 : std::clamp((int)std::thread::hardware_concurrency() / 2, 2, 4), filled.load(),
               count, tw, unset, lo, hi);
        if (unset) {
            printf("    unset at:");
            for (size_t i = 0, shown = 0; i < count && shown < 12; i++)
                if (wave[i] == -1.f) { printf(" %zu", i); shown++; }
            printf("\n");
        }
        // FFmpeg's fragmented muxer leaves a 67 ms hole after the first AAC packet (priming, edit list ignored), so
        // part 0 decodes 40 ms short and its last few peaks stay unset (same with and without -use_mfra_for).
        CHECK(unset <= (isFrag ? 6u : 2u) && hi < 0.5f && (isFrag || lo > 0.05f), "waveform shape");
    }
    TryDelete(reg);
    TryDelete(frag);
}

static void TestBig(bool fullCheck) {
    printf("[big fragmented file] %s\n", U(kBig).c_str());
    if (GetFileAttributesW(kBig) == INVALID_FILE_ATTRIBUTES) {
        printf("  missing, skipped\n");
        return;
    }
    printf("  spinning disk: %d\n", util::IsOnSpinningDisk(kBig));
    double t0 = Now();
    auto fast = FastInputOptions(kBig);
    double tFast = Now() - t0;
    t0 = Now();
    MediaInfo m = Probe(kBig, nullptr);
    double tProbe = Now() - t0;
    printf("  FastInputOptions %.1f ms (%s), Probe (ffprobe, cached index) %.1f ms\n", tFast,
           fast.empty() ? "none" : U(fast[0] + L" " + fast[1]).c_str(), tProbe);
    PrintMedia(m);
    CHECK(m.fragmentedMfra && fast.size() == 2, "mfra detected");
    CHECK(std::fabs(m.duration - 5221.7) < 1.0, "duration %.3f", m.duration);
    CHECK(tFast + tProbe < 1000, "probe under a second (%.0f ms)", tFast + tProbe);
    const StreamInfo* v = m.Video();
    if (!v) return;
    t0 = Now();
    KeyframeIndex hdr = Keyframes(m, v->index, nullptr);
    double tk = Now() - t0;
    printf("  header keyframes %zu in %.2f ms; first %.3f last %.3f; lag[0] %.1f\n", hdr.pts.size(), tk,
           hdr.pts.empty() ? -1 : hdr.pts.front(), hdr.pts.empty() ? -1 : hdr.pts.back(),
           hdr.dtsLag.empty() ? 0 : hdr.dtsLag[0]);
    CHECK(hdr.pts.size() > 1200 && hdr.pts.size() < 1300, "keyframe count %zu", hdr.pts.size());

    // ffprobe packet scan over the first two minutes (mfra mode) must agree with the header times.
    std::vector<double> scan;
    t0 = Now();
    ff::Run(ff::Ffprobe(),
            {L"-v", L"error", L"-select_streams", std::to_wstring(v->index), L"-read_intervals", L"%+120",
             L"-show_entries", L"packet=pts_time,flags", L"-of", L"csv=p=0", L"-use_mfra_for", L"pts", kBig},
            nullptr,
            [&](const std::string& l) {
                size_t c = l.find(',');
                if (c != std::string::npos && c + 1 < l.size() && l[c + 1] == 'K')
                    scan.push_back(atof(l.substr(0, c).c_str()) - m.startTime);
            },
            {}, false);
    std::sort(scan.begin(), scan.end());
    printf("  ffprobe scan of the first 120 s: %zu keyframes in %.0f ms\n", scan.size(), Now() - t0);
    CompareKeys(hdr.pts, scan, scan.size(), "big header vs scan (120 s)");

    t0 = Now();
    double pd = ProbeDuration(kBig, nullptr);
    printf("  ProbeDuration %.3f in %.2f ms\n", pd, Now() - t0);

    if (fullCheck) {
        // Default (non-mfra) ffprobe: reads every fragment header. Slow on a hard disk; only with --full-check.
        t0 = Now();
        auto r = ff::Run(ff::Ffprobe(), {L"-v", L"error", L"-show_entries", L"format=duration,start_time:stream=index,duration,start_time",
                                         L"-of", L"compact", kBig}, nullptr, {}, {}, false);
        printf("  default-mode ffprobe (%.0f ms):\n%s", Now() - t0, r.out.c_str());
        t0 = Now();
        std::vector<double> scanDefault;
        ff::Run(ff::Ffprobe(),
                {L"-v", L"error", L"-select_streams", std::to_wstring(v->index), L"-read_intervals", L"%+120",
                 L"-show_entries", L"packet=pts_time,flags", L"-of", L"csv=p=0", kBig},
                nullptr,
                [&](const std::string& l) {
                    size_t c = l.find(',');
                    if (c != std::string::npos && c + 1 < l.size() && l[c + 1] == 'K')
                        scanDefault.push_back(atof(l.substr(0, c).c_str()) - m.startTime);
                },
                {}, false);
        std::sort(scanDefault.begin(), scanDefault.end());
        printf("  default-mode scan of the first 120 s: %zu keyframes in %.0f ms\n", scanDefault.size(), Now() - t0);
        CompareKeys(hdr.pts, scanDefault, scanDefault.size(), "big header vs default-mode scan (120 s)");
    }
}

static bool SameSegs(const std::vector<Segment>& a, const std::vector<Segment>& b, double tol, bool names, bool ends = true) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::fabs(a[i].start - b[i].start) > tol) return false;
        if (ends && std::fabs(a[i].end - b[i].end) > tol) return false;
        if (names && a[i].name != b[i].name) return false;
    }
    return true;
}

static std::wstring WriteTemp(const wchar_t* name, const std::string& utf8) {
    std::wstring p = g_tmp + L"\\" + name;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD put;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &put, nullptr);
    CloseHandle(h);
    return p;
}

static void TestSegio() {
    printf("[segio]\n");
    std::vector<Segment> segs(4);
    segs[0] = {0.0, 12.345678912, L"Intro, \"quoted\"", L"a=1, b = two", true, 0};
    segs[1] = {12.5, 70.25, L"Ünïcödé ✓ 日本", L"", false, 1};
    segs[2] = {3700.001, 3725.9, L"", L"solo", true, 2};
    segs[3] = {80, 95.5, L"back\\slash=semi;hash#", L"", true, 3};

    std::string proj = segio::WriteProject(L"C:\\media\\clip ü.mp4", segs);
    auto back = segio::ReadProject(proj, 100);
    bool ok = back.size() == 4 && std::fabs(back[0].end - 12.345679) < 1e-9 && back[1].name == segs[1].name &&
              !back[1].enabled && back[0].enabled && back[0].tags == L"a=1, b=two" && back[2].tags == L"solo=" &&
              back[3].name == segs[3].name;
    CHECK(ok, "project round trip");
    auto root = segio::ParseJson(proj);
    CHECK(root->Str("mediaFileName") == u8"clip ü.mp4" && root->Num("version") == 1, "project header");
    printf("  project sample:\n%.*s\n", 200, proj.c_str());

    // LosslessCut-style JSON5.
    const char* j5 = u8R"({
      // comment
      version: 1, mediaFileName: 'a.mp4',
      cutSegments: [
        { start: 1.5, end: 3, name: 'one', tags: { x: 'y', n: 5 }, },
        { start: 4, name: "it's \u00fc\ud83d\ude00", selected: false }, /* block */
        { end: +Infinity },
      ],
    })";
    auto s5 = segio::ReadProject(j5, 50);
    CHECK(s5.size() == 3 && s5[0].start == 1.5 && s5[0].end == 3 && s5[0].tags == L"x=y, n=5" && s5[1].end == 50 &&
              !s5[1].enabled && s5[1].name == L"it's \u00fc\U0001F600" && std::isinf(s5[2].end),
          "json5 project");
    bool threw = false;
    try { segio::ParseJson("{a: [1, 2"); } catch (const EditError&) { threw = true; }
    CHECK(threw, "malformed json throws");
    threw = false;
    try { segio::ReadProject("[1]", 1); } catch (const EditError&) { threw = true; }
    CHECK(threw, "non-object project throws");
    CHECK(segio::JsonQuote("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"", "JsonQuote");
    auto tags = segio::ParseTags(L" a = 1 ,, b=, =x , c ");
    CHECK(tags.size() == 4 && tags[0] == std::make_pair(std::wstring(L"a"), std::wstring(L"1")) && tags[1].second.empty() &&
              tags[2].first == L"=x" && tags[3].first == L"c",
          "ParseTags");

    // Every export format back through Import.
    const wchar_t* exts[] = {L"", L".llc", L".csv", L".csv", L".txt", L".cue", L".srt", L".edl", L".txt"};
    std::vector<Segment> sorted = segs;
    std::stable_sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.start < b.start; });
    for (int fi = 1; fi <= 8; fi++) {
        std::wstring p = g_tmp + L"\\export" + std::to_wstring(fi) + exts[fi];
        double t0 = Now();
        segio::Export(p, fi, L"C:\\media\\clip.mp4", segs);
        auto im = segio::Import(p, 4000);
        double ms = Now() - t0;
        bool good = false;
        switch (fi) {
            case 1: good = SameSegs(im, segs, 1e-6, true); break;
            case 2: good = SameSegs(im, segs, 1e-6, true); break;
            case 3: good = SameSegs(im, segs, 0.0006, true); break;
            case 4: {  // YouTube: "0:00 Start" added, starts floored, ends = next start
                good = im.size() == 4 && im[0].name == L"Intro, \"quoted\"" && im[1].start == 12 && im[2].start == 80 &&
                       im[3].start == 3700 && im[3].name == L"Chapter 4" && im[3].end == 4000;
                break;
            }
            case 5: good = im.size() == 4 && SameSegs(im, sorted, 1.0 / 75, false, false) && im[3].name == L"Track 4"; break;
            case 6: {
                good = SameSegs(im, segs, 0.0011, false) && im[2].name == L"Segment 3" && im[1].name == segs[1].name;
                break;
            }
            case 7: good = SameSegs(im, segs, 1e-6, false); break;
            case 8: good = SameSegs(im, sorted, 0.0011, false); break;
        }
        printf("  export/import filter %d (%s): %zu segments, %s, %.2f ms\n", fi, U(exts[fi]).c_str(), im.size(),
               good ? "ok" : "MISMATCH", ms);
        CHECK(good, "format %d", fi);
        if (fi == 8) CHECK(SameSegs(im, sorted, 0.0011, true), "ffmetadata titles unescaped");
        TryDelete(p);
    }

    // Hand-written inputs in the formats other tools produce.
    auto p = WriteTemp(L"yt.txt", u8"Chapters:\r\n0:00 Intro\r\n- 1:05 - Part one\r\n(12:34.5) | Second\r\n1:02:03 – Late\r\n90 plain seconds\r\nno time here\r\n");
    auto im = segio::Import(p, 5000);
    CHECK(im.size() == 5 && im[0].name == L"Intro" && im[1].start == 65 && im[1].name == L"Part one" &&
              im[2].start == 90 && im[2].name == L"plain seconds" && im[3].start == 754.5 && im[3].name == L"Second" &&
              im[4].start == 3723 && im[4].name == L"Late" && im[4].end == 5000,
          "timestamps import (%zu)", im.size());
    TryDelete(p);
    p = WriteTemp(L"s.srt", "1\r\n00:00:01,500 --> 00:00:03,000\r\nHello\r\n world \r\n\r\n \r\n2\r\n00:01:00.250-->00:01:02.5\r\nBye\r\n");
    im = segio::Import(p, 0);
    CHECK(im.size() == 2 && im[0].start == 1.5 && im[0].name == L"Hello world" && im[1].start == 60.25 && im[1].end == 62.5,
          "srt import (%zu)", im.size());
    TryDelete(p);
    p = WriteTemp(L"c.cue", "FILE \"x.wav\" WAVE\n  TRACK 01 AUDIO\n    TITLE \"One\"\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    INDEX 01 01:30:37\n");
    im = segio::Import(p, 200);
    CHECK(im.size() == 2 && im[0].name == L"One" && im[1].name.empty() && std::fabs(im[1].start - (90 + 37 / 75.0)) < 1e-9 &&
              im[1].end == 200,
          "cue import");
    TryDelete(p);
    p = WriteTemp(L"h.csv", "start,end,label\n1.5,2.5,\"a, \"\"b\"\"\"\n00:01:00.000,,x\n");
    im = segio::Import(p, 99);
    CHECK(im.size() == 2 && im[0].name == L"a, \"b\"" && im[1].start == 60 && im[1].end == 99, "csv import");
    TryDelete(p);

    // Autosave paths: FastCut's SHA-1 naming, in FastPlay's data folder; FastCut's own folder (FASTCUT_HOME honoured)
    // is only read. Expected names computed with .NET: SHA1(UTF8(Path.GetFullPath(p).ToLowerInvariant())) hex[..16].
    std::wstring ap = segio::AutosavePath(L"D:\\Recordings\\Streams\\GTA\\2026-11-19 16-07-46.mp4");
    printf("  AutosavePath = %s\n", U(ap).c_str());
    CHECK(util::FileName(ap) == L"C85D6BC5015E0D65.llc", "autosave name");
    CHECK(ap.rfind(util::DataDir() + L"\\projects\\", 0) == 0, "autosave in FastPlay's folder");
    CHECK(util::FileName(segio::AutosavePath(L"C:\\M\u00e9dia\\\u00dcBER \u00c4rger \u00c7A.mkv")) == L"42E3B019F90D6569.llc",
          "autosave name (non-ASCII)");
    SetEnvironmentVariableW(L"FASTCUT_HOME", L"X:\\fc-home");
    std::wstring ap2 = segio::LegacyAutosavePath(L"d:\\recordings\\streams\\gta\\2026-11-19 16-07-46.MP4");
    SetEnvironmentVariableW(L"FASTCUT_HOME", nullptr);
    printf("  LegacyAutosavePath (FASTCUT_HOME=X:\\fc-home, other case) = %s\n", U(ap2).c_str());
    CHECK(ap2.rfind(L"X:\\fc-home\\projects\\", 0) == 0 && util::FileName(ap2) == util::FileName(ap), "legacy home + case");
    // FindAutosave: FastPlay's own first, else FastCut's.
    std::wstring media = g_tmp + L"\\autosave-media.mp4", home = g_tmp + L"\\fc-home";
    SetEnvironmentVariableW(L"FASTCUT_HOME", home.c_str());
    std::wstring legacy = segio::LegacyAutosavePath(media);
    CreateDirectoryW(home.c_str(), nullptr);
    CreateDirectoryW((home + L"\\projects").c_str(), nullptr);
    if (FILE* f = _wfopen(legacy.c_str(), L"wb")) { fputs("{}", f); fclose(f); }
    CHECK(segio::FindAutosave(media) == legacy, "falls back to FastCut's autosave");
    DeleteFileW(legacy.c_str());
    CHECK(segio::FindAutosave(media).empty(), "nothing saved: empty");
    SetEnvironmentVariableW(L"FASTCUT_HOME", nullptr);
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && wcsncmp(argv[1], L"--", 2) == 0 && wcscmp(argv[1], L"--full-check") != 0) return Child(argc, argv);
    SetConsoleOutputCP(CP_UTF8);
    bool full = argc >= 2 && wcscmp(argv[1], L"--full-check") == 0;
    g_tmp = L"H:\\Claude\\fastplay\\bench\\edit-tests\\core";
    CreateDirectoryW(L"H:\\Claude\\fastplay\\bench\\edit-tests", nullptr);
    CreateDirectoryW(g_tmp.c_str(), nullptr);
    try {
        TestUtil();
        TestRun();
        TestSegio();
        TestSmall();
        TestSynthetic();
        TestBig(full);
    } catch (const EditError& e) {
        printf("EXCEPTION: %s\n", U(e.msg).c_str());
        g_fail++;
    } catch (const std::exception& e) {
        printf("EXCEPTION: %s\n", e.what());
        g_fail++;
    }
    RemoveDirectoryW(g_tmp.c_str());
    RemoveDirectoryW(L"H:\\Claude\\fastplay\\bench\\edit-tests");  // only succeeds when empty (other harnesses)
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
