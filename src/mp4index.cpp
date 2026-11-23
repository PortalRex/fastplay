#include "mp4index.h"
#include <algorithm>
#include <numeric>

namespace {

using Buf = std::vector<uint8_t>;

constexpr uint32_t FourCC(const char (&s)[5]) {
    return (uint32_t)(uint8_t)s[0] << 24 | (uint32_t)(uint8_t)s[1] << 16 | (uint32_t)(uint8_t)s[2] << 8 | (uint8_t)s[3];
}
uint32_t Be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint64_t Be64(const uint8_t* p) { return (uint64_t)Be32(p) << 32 | Be32(p + 4); }

struct File {
    HANDLE h = INVALID_HANDLE_VALUE;
    uint64_t size = 0;
    explicit File(const std::wstring& path) {
        h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
        LARGE_INTEGER s;
        if (h != INVALID_HANDLE_VALUE && GetFileSizeEx(h, &s)) size = (uint64_t)s.QuadPart;
    }
    ~File() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
    bool Read(uint64_t off, void* buf, size_t n) const {
        if (off + n > size) return false;
        OVERLAPPED o = {};
        o.Offset = (DWORD)off;
        o.OffsetHigh = (DWORD)(off >> 32);
        DWORD got = 0;
        return ReadFile(h, buf, (DWORD)n, &got, &o) && got == n;
    }
};

struct Box {
    size_t start = 0, body = 0, end = 0;
    uint32_t type = 0;
};

// Box at 'at' inside a buffer limited to [.., limit). Truncated boxes are clamped to the limit.
bool BoxAt(const Buf& b, size_t at, size_t limit, Box* box) {
    if (at + 8 > limit) return false;
    uint64_t size = Be32(&b[at]);
    size_t hdr = 8;
    if (size == 1) {
        if (at + 16 > limit) return false;
        size = Be64(&b[at + 8]);
        hdr = 16;
    } else if (size == 0) {
        size = limit - at;
    }
    if (size < hdr) return false;
    box->start = at;
    box->body = at + hdr;
    box->end = (size_t)std::min<uint64_t>(limit, at + size);
    box->type = Be32(&b[at + 4]);
    return true;
}

bool Child(const Buf& b, const Box& parent, uint32_t type, Box* out, size_t skip = 0) {
    size_t p = parent.body + skip;
    Box x;
    while (BoxAt(b, p, parent.end, &x)) {
        if (x.type == type) { *out = x; return true; }
        if (x.end <= p) break;
        p = x.end;
    }
    return false;
}

template <class F>
void Children(const Buf& b, const Box& parent, uint32_t type, F&& fn) {
    size_t p = parent.body;
    Box x;
    while (BoxAt(b, p, parent.end, &x)) {
        if (x.type == type && !fn(x)) return;
        if (x.end <= p) break;
        p = x.end;
    }
}

// Scale (timescale) of an mvhd/mdhd box; duration returned through 'dur' in that scale.
uint32_t HeaderScale(const Buf& b, const Box& box, uint64_t* dur) {
    bool v1 = b[box.body] == 1;
    if (box.body + (v1 ? 32 : 20) > box.end) return 0;
    if (dur) *dur = v1 ? Be64(&b[box.body + 24]) : Be32(&b[box.body + 16]);
    return Be32(&b[box.body + (v1 ? 20 : 12)]);
}

struct Track {
    Box trak, stbl;
    uint32_t id = 0, scale = 0;
    uint64_t mdhdDuration = 0;
    uint32_t trexDuration = 0;  // mvex/trex default sample duration (fragmented files)
    double emptyDelay = 0;  // seconds of leading empty edits
    int64_t mediaTime = 0;  // first real edit's media_time (cut off the front)
    int width = 0, height = 0;
};

bool FindVideoTrack(const Buf& b, const Box& moov, uint32_t movieScale, Track* t) {
    bool found = false;
    Children(b, moov, FourCC("trak"), [&](const Box& trak) {
        Box mdia, hdlr, tkhd, mdhd;
        if (!Child(b, trak, FourCC("mdia"), &mdia) || !Child(b, mdia, FourCC("hdlr"), &hdlr)) return true;
        if (hdlr.body + 12 > hdlr.end || Be32(&b[hdlr.body + 8]) != FourCC("vide")) return true;
        if (!Child(b, mdia, FourCC("mdhd"), &mdhd)) return true;
        t->trak = trak;
        t->scale = HeaderScale(b, mdhd, &t->mdhdDuration);
        if (Child(b, trak, FourCC("tkhd"), &tkhd) && tkhd.end - tkhd.body >= 84) {
            bool v1 = b[tkhd.body] == 1;
            t->id = Be32(&b[tkhd.body + (v1 ? 20 : 12)]);
            t->width = (int)(Be32(&b[tkhd.end - 8]) >> 16);
            t->height = (int)(Be32(&b[tkhd.end - 4]) >> 16);
        }
        Box minf, edts, elst;
        if (Child(b, mdia, FourCC("minf"), &minf)) Child(b, minf, FourCC("stbl"), &t->stbl);
        if (Child(b, trak, FourCC("edts"), &edts) && Child(b, edts, FourCC("elst"), &elst)) {
            bool v1 = b[elst.body] == 1;
            uint32_t n = Be32(&b[elst.body + 4]);
            size_t p = elst.body + 8;
            for (uint32_t i = 0; i < n && p + (v1 ? 20 : 12) <= elst.end; i++) {
                int64_t dur = v1 ? (int64_t)Be64(&b[p]) : Be32(&b[p]);
                int64_t mt = v1 ? (int64_t)Be64(&b[p + 8]) : (int32_t)Be32(&b[p + 4]);
                p += v1 ? 20 : 12;
                if (mt == -1) { if (movieScale) t->emptyDelay += (double)dur / movieScale; continue; }
                t->mediaTime = mt;
                break;
            }
        }
        found = t->scale != 0;
        return !found;
    });
    return found;
}

double ToSeconds(const Track& t, int64_t mediaTicks) {
    return (double)(mediaTicks - t.mediaTime) / t.scale + t.emptyDelay;
}

// Fragment times: FFmpeg (and so mpv) ignores the edit list for fragmented MP4, so the raw media time is the pts.
double FragSeconds(const Track& t, int64_t mediaTicks) { return (double)mediaTicks / t.scale; }

// Regular MP4: walk stts/ctts up to each stss sync sample (FastCut's ReadTrack).
void ReadSampleTables(const Buf& b, const Track& t, MediaIndex* out) {
    Box stts, ctts, stss;
    if (!t.stbl.end || !Child(b, t.stbl, FourCC("stts"), &stts)) return;
    bool hasCtts = Child(b, t.stbl, FourCC("ctts"), &ctts);
    bool hasStss = Child(b, t.stbl, FourCC("stss"), &stss);
    std::vector<uint32_t> sync;
    if (hasStss) {
        uint32_t n = Be32(&b[stss.body + 4]);
        n = (uint32_t)std::min<uint64_t>(n, (stss.end - stss.body - 8) / 4);
        sync.resize(n);
        for (uint32_t i = 0; i < n; i++) sync[i] = Be32(&b[stss.body + 8 + i * 4]);
    }
    uint32_t sttsRuns = (uint32_t)std::min<uint64_t>(Be32(&b[stts.body + 4]), (stts.end - stts.body - 8) / 8);
    uint32_t cttsRuns = hasCtts ? (uint32_t)std::min<uint64_t>(Be32(&b[ctts.body + 4]), (ctts.end - ctts.body - 8) / 8) : 0;
    // FFmpeg shifts dts back so that pts >= dts; the most negative composition offset is that shift.
    int64_t minOff = 0;
    for (uint32_t i = 0; i < cttsRuns; i++) minOff = std::min<int64_t>(minOff, (int32_t)Be32(&b[ctts.body + 12 + i * 8]));
    const int64_t dtsShift = -minOff;
    uint32_t sIdx = 0, cIdx = 0, sLeft = 0, sDelta = 0, cLeft = 0;
    int64_t cOff = 0;
    auto nextStts = [&] {
        while (sLeft == 0 && sIdx < sttsRuns) {
            sLeft = Be32(&b[stts.body + 8 + sIdx * 8]);
            sDelta = Be32(&b[stts.body + 12 + sIdx * 8]);
            sIdx++;
        }
    };
    auto nextCtts = [&] {
        while (cLeft == 0 && cIdx < cttsRuns) {
            cLeft = Be32(&b[ctts.body + 8 + cIdx * 8]);
            cOff = (int32_t)Be32(&b[ctts.body + 12 + cIdx * 8]);  // negative offsets appear in v0 too
            cIdx++;
        }
    };
    nextStts();
    nextCtts();
    int64_t dts = 0;
    size_t next = 0;
    for (uint32_t sample = 1; sLeft > 0; sample++) {
        if (!hasStss || (next < sync.size() && sync[next] == sample)) {
            next++;
            out->keyTimes.push_back(ToSeconds(t, dts + cOff));
            out->keyLags.push_back((double)(cOff + dtsShift) / t.scale);
            if (hasStss && next >= sync.size()) break;
        }
        dts += sDelta;
        sLeft--;
        nextStts();
        if (cttsRuns > 0 && cLeft > 0) { cLeft--; nextCtts(); }
    }
}

// Fragmented MP4: the trailing mfra holds a tfra per track listing random-access points (time + moof offset).
bool ReadMfra(const File& f, const Track& t, MediaIndex* out) {
    uint8_t tail[16];
    if (f.size < 16 || !f.Read(f.size - 16, tail, 16) || Be32(tail + 4) != FourCC("mfro")) return false;
    uint32_t mfraSize = Be32(tail + 12);
    if (mfraSize < 16 || mfraSize > f.size || mfraSize > (64u << 20)) return false;
    Buf b(mfraSize);
    if (!f.Read(f.size - mfraSize, b.data(), mfraSize)) return false;
    Box mfra;
    if (!BoxAt(b, 0, b.size(), &mfra) || mfra.type != FourCC("mfra")) return false;
    bool got = false;
    Children(b, mfra, FourCC("tfra"), [&](const Box& tfra) {
        if (tfra.body + 16 > tfra.end) return true;
        bool v1 = b[tfra.body] == 1;
        uint32_t id = Be32(&b[tfra.body + 4]);
        if (t.id && id != t.id) return true;
        uint32_t lens = Be32(&b[tfra.body + 8]);
        uint32_t n = Be32(&b[tfra.body + 12]);
        size_t es = (v1 ? 16 : 8) + ((lens >> 4) & 3) + 1 + ((lens >> 2) & 3) + 1 + (lens & 3) + 1;
        size_t p = tfra.body + 16;
        for (uint32_t i = 0; i < n && p + es <= tfra.end; i++, p += es) {
            uint64_t time = v1 ? Be64(&b[p]) : Be32(&b[p]);
            uint64_t off = v1 ? Be64(&b[p + 8]) : Be32(&b[p + 4]);
            out->keyTimes.push_back(FragSeconds(t, (int64_t)time));
            out->keyOffsets.push_back(off);
            out->keyLags.push_back(-1);
        }
        got = true;
        return false;
    });
    return got && !out->keyTimes.empty();
}

// End time of the fragment at 'off' (tfdt + every sample's duration in the video traf). Fragmented files often
// carry no duration in their header, so this is where the real length comes from.
double FragmentEnd(const File& f, uint64_t off, const Track& t) {
    uint8_t h[16];
    if (!f.Read(off, h, 16) || Be32(h + 4) != FourCC("moof")) return -1;
    uint64_t size = Be32(h);
    if (size < 16 || size > (4u << 20)) return -1;
    Buf b((size_t)size);
    if (!f.Read(off, b.data(), b.size())) return -1;
    Box moof;
    BoxAt(b, 0, b.size(), &moof);
    double end = -1;
    Children(b, moof, FourCC("traf"), [&](const Box& traf) {
        Box tfhd, tfdt;
        if (!Child(b, traf, FourCC("tfhd"), &tfhd) || tfhd.body + 8 > tfhd.end) return true;
        if (t.id && Be32(&b[tfhd.body + 4]) != t.id) return true;
        if (!Child(b, traf, FourCC("tfdt"), &tfdt)) return false;
        uint64_t time = b[tfdt.body] == 1 ? Be64(&b[tfdt.body + 4]) : Be32(&b[tfdt.body + 4]);
        uint32_t tf = Be32(&b[tfhd.body]) & 0xFFFFFF;
        size_t p = tfhd.body + 8;
        if (tf & 0x01) p += 8;
        if (tf & 0x02) p += 4;
        uint32_t defDur = t.trexDuration;
        if ((tf & 0x08) && p + 4 <= tfhd.end) defDur = Be32(&b[p]);
        uint64_t total = 0;
        Children(b, traf, FourCC("trun"), [&](const Box& trun) {
            uint32_t rf = Be32(&b[trun.body]) & 0xFFFFFF;
            uint32_t n = Be32(&b[trun.body + 4]);
            size_t q = trun.body + 8 + ((rf & 0x01) ? 4 : 0) + ((rf & 0x04) ? 4 : 0);
            size_t per = ((rf & 0x100) ? 4 : 0) + ((rf & 0x200) ? 4 : 0) + ((rf & 0x400) ? 4 : 0) + ((rf & 0x800) ? 4 : 0);
            for (uint32_t i = 0; i < n && q + per <= trun.end; i++, q += per)
                total += (rf & 0x100) ? Be32(&b[q]) : defDur;
            return true;
        });
        end = FragSeconds(t, (int64_t)(time + total));
        return false;
    });
    return end;
}

// Fragmented MP4 without mfra: one small read per moof for its video traf's tfdt.
void ScanFragments(const File& f, uint64_t pos, const Track& t, MediaIndex* out) {
    uint8_t h[16];
    while (pos + 8 <= f.size && f.Read(pos, h, 16)) {
        uint64_t size = Be32(h);
        if (size == 1) size = Be64(h + 8);
        else if (size == 0) size = f.size - pos;
        if (size < 8) break;
        if (Be32(h + 4) == FourCC("moof") && size <= (4u << 20)) {
            Buf b((size_t)size);
            if (!f.Read(pos, b.data(), b.size())) break;
            Box moof;
            BoxAt(b, 0, b.size(), &moof);
            Children(b, moof, FourCC("traf"), [&](const Box& traf) {
                Box tfhd, tfdt;
                if (!Child(b, traf, FourCC("tfhd"), &tfhd) || (t.id && Be32(&b[tfhd.body + 4]) != t.id)) return true;
                if (!Child(b, traf, FourCC("tfdt"), &tfdt)) return false;
                uint64_t time = b[tfdt.body] == 1 ? Be64(&b[tfdt.body + 4]) : Be32(&b[tfdt.body + 4]);
                out->keyTimes.push_back(FragSeconds(t, (int64_t)time));
                out->keyOffsets.push_back(pos);
                out->keyLags.push_back(-1);
                return false;
            });
        }
        pos += size;
    }
}

}  // namespace

bool ReadMediaIndex(const std::wstring& path, MediaIndex* out, bool allowFragmentScan) {
    *out = MediaIndex{};
    File f(path);
    if (!f.ok()) return false;

    // Find the top-level moov, seeking past media data.
    uint64_t pos = 0, moovEnd = 0;
    Buf moovBuf;
    uint8_t h[16];
    bool first = true;
    while (pos + 8 <= f.size) {
        if (!f.Read(pos, h, std::min<uint64_t>(16, f.size - pos))) return false;
        uint64_t size = Be32(h);
        uint32_t type = Be32(h + 4);
        if (first) {
            static const uint32_t ok[] = {FourCC("ftyp"), FourCC("moov"), FourCC("mdat"), FourCC("free"),
                                          FourCC("skip"), FourCC("wide"), FourCC("pnot"), FourCC("uuid")};
            if (std::find(std::begin(ok), std::end(ok), type) == std::end(ok)) return false;
            first = false;
        }
        if (size == 1) size = Be64(h + 8);
        else if (size == 0) size = f.size - pos;
        if (size < 8) return false;
        if (type == FourCC("moov")) {
            if (size > (256u << 20)) return false;
            moovBuf.resize((size_t)size);
            if (!f.Read(pos, moovBuf.data(), moovBuf.size())) return false;
            moovEnd = pos + size;
            break;
        }
        pos += size;
    }
    if (moovBuf.empty()) return false;

    Box moov, mvhd, mvex;
    BoxAt(moovBuf, 0, moovBuf.size(), &moov);
    if (Child(moovBuf, moov, FourCC("cmov"), &mvhd)) return false;  // compressed header
    uint64_t movieDur = 0;
    uint32_t movieScale = Child(moovBuf, moov, FourCC("mvhd"), &mvhd) ? HeaderScale(moovBuf, mvhd, &movieDur) : 1000;
    if (!movieScale) movieScale = 1000;

    Track t;
    if (!FindVideoTrack(moovBuf, moov, movieScale, &t)) return false;
    out->width = t.width;
    out->height = t.height;
    out->fragmented = Child(moovBuf, moov, FourCC("mvex"), &mvex);

    if (out->fragmented) {
        Box trex;
        Children(moovBuf, mvex, FourCC("trex"), [&](const Box& x) {
            if (x.body + 16 <= x.end && (!t.id || Be32(&moovBuf[x.body + 4]) == t.id)) {
                t.trexDuration = Be32(&moovBuf[x.body + 12]);
                return false;
            }
            return true;
        });
        Box mehd;
        if (Child(moovBuf, mvex, FourCC("mehd"), &mehd)) {
            bool v1 = moovBuf[mehd.body] == 1;
            out->duration = (double)(v1 ? Be64(&moovBuf[mehd.body + 4]) : Be32(&moovBuf[mehd.body + 4])) / movieScale;
        }
        if (!ReadMfra(f, t, out) && allowFragmentScan) ScanFragments(f, moovEnd, t, out);
        if (!out->keyOffsets.empty()) {
            uint64_t last = *std::max_element(out->keyOffsets.begin(), out->keyOffsets.end());
            double end = FragmentEnd(f, last, t);
            if (end > out->duration) out->duration = end;
        }
    } else {
        if (t.mdhdDuration) out->duration = (double)t.mdhdDuration / t.scale;
        ReadSampleTables(moovBuf, t, out);
    }
    if (out->duration <= 0 && movieDur) out->duration = (double)movieDur / movieScale;

    // Keyframes can be listed out of display order in exotic files; keep times (and offsets, lags) ascending.
    if (!std::is_sorted(out->keyTimes.begin(), out->keyTimes.end())) {
        std::vector<size_t> order(out->keyTimes.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return out->keyTimes[a] < out->keyTimes[b]; });
        std::vector<double> times, lags;
        std::vector<uint64_t> offs;
        for (size_t i : order) {
            times.push_back(out->keyTimes[i]);
            if (!out->keyOffsets.empty()) offs.push_back(out->keyOffsets[i]);
            if (!out->keyLags.empty()) lags.push_back(out->keyLags[i]);
        }
        out->keyTimes.swap(times);
        out->keyOffsets.swap(offs);
        out->keyLags.swap(lags);
    }
    return true;
}
