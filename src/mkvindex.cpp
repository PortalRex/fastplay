// Keyframe times from a Matroska/WebM file's Cues element (the seek index muxers write for video keyframes), found
// through the SeekHead: a few small reads instead of ffprobe walking every packet of a 20 GB OBS recording.
#include "mp4index.h"
#include <algorithm>

namespace {

using Buf = std::vector<uint8_t>;

enum : uint32_t {
    kEbml = 0x1A45DFA3, kSegment = 0x18538067, kSeekHead = 0x114D9B74, kSeek = 0x4DBB, kSeekId = 0x53AB,
    kSeekPos = 0x53AC, kInfo = 0x1549A966, kTimestampScale = 0x2AD7B1, kDuration = 0x4489, kTracks = 0x1654AE6B,
    kTrackEntry = 0xAE, kTrackNumber = 0xD7, kTrackType = 0x83, kVideo = 0xE0, kPixelWidth = 0xB0,
    kPixelHeight = 0xBA, kCues = 0x1C53BB6B, kCuePoint = 0xBB, kCueTime = 0xB3, kCueTrackPositions = 0xB7,
    kCueTrack = 0xF7, kCluster = 0x1F43B675,
};
constexpr uint64_t kUnknownSize = ~0ull;

struct Reader {
    HANDLE h = INVALID_HANDLE_VALUE;
    uint64_t size = 0;
    explicit Reader(const std::wstring& path) {
        h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
        LARGE_INTEGER s;
        if (h != INVALID_HANDLE_VALUE && GetFileSizeEx(h, &s)) size = (uint64_t)s.QuadPart;
    }
    ~Reader() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    bool Read(uint64_t off, Buf* b, size_t n) const {
        if (off >= size) return false;
        n = (size_t)std::min<uint64_t>(n, size - off);
        b->resize(n);
        OVERLAPPED o = {};
        o.Offset = (DWORD)off;
        o.OffsetHigh = (DWORD)(off >> 32);
        DWORD got = 0;
        if (!ReadFile(h, b->data(), (DWORD)n, &got, &o)) return false;
        b->resize(got);
        return got > 0;
    }
};

// EBML variable-length integers. IDs keep their length marker; sizes drop it (all ones = unknown size).
bool ReadId(const Buf& b, size_t& at, uint32_t* id) {
    if (at >= b.size() || !b[at]) return false;
    int len = 1;
    while (len <= 4 && !(b[at] & (0x80 >> (len - 1)))) len++;
    if (len > 4 || at + len > b.size()) return false;
    uint32_t v = 0;
    for (int i = 0; i < len; i++) v = v << 8 | b[at + i];
    at += len;
    *id = v;
    return true;
}

bool ReadSize(const Buf& b, size_t& at, uint64_t* size) {
    if (at >= b.size() || !b[at]) return false;
    int len = 1;
    while (len <= 8 && !(b[at] & (0x80 >> (len - 1)))) len++;
    if (len > 8 || at + len > b.size()) return false;
    uint64_t v = b[at] & (0xFF >> len);
    bool allOnes = v == (0xFFu >> len);
    for (int i = 1; i < len; i++) {
        v = v << 8 | b[at + i];
        allOnes = allOnes && b[at + i] == 0xFF;
    }
    at += len;
    *size = allOnes ? kUnknownSize : v;
    return true;
}

struct Elem {
    uint32_t id = 0;
    size_t body = 0, end = 0;  // end clamped to the buffer
    uint64_t size = 0;         // declared size (kUnknownSize possible)
};

bool ElemAt(const Buf& b, size_t at, size_t limit, Elem* e) {
    if (!ReadId(b, at, &e->id) || !ReadSize(b, at, &e->size)) return false;
    e->body = at;
    e->end = e->size == kUnknownSize ? limit : (size_t)std::min<uint64_t>(limit, at + e->size);
    return at <= limit;
}

uint64_t UInt(const Buf& b, const Elem& e) {
    uint64_t v = 0;
    for (size_t i = e.body; i < e.end && i < e.body + 8; i++) v = v << 8 | b[i];
    return v;
}

double Float(const Buf& b, const Elem& e) {
    if (e.end - e.body == 4) {
        uint32_t u = (uint32_t)UInt(b, e);
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
    if (e.end - e.body == 8) {
        uint64_t u = UInt(b, e);
        double d;
        memcpy(&d, &u, 8);
        return d;
    }
    return 0;
}

template <class F> void Children(const Buf& b, const Elem& parent, F&& f) {
    size_t at = parent.body;
    Elem e;
    while (at < parent.end && ElemAt(b, at, parent.end, &e)) {
        f(e);
        if (e.size == kUnknownSize) break;
        at = e.end;
    }
}

// Reads one whole element at file offset 'pos' (header + body, capped).
bool LoadElement(const Reader& r, uint64_t pos, uint32_t wantId, size_t cap, Buf* out, Elem* e) {
    Buf head;
    if (!r.Read(pos, &head, 16)) return false;
    Elem h;
    if (!ElemAt(head, 0, head.size(), &h) || h.id != wantId || h.size == kUnknownSize || h.size > cap) return false;
    if (!r.Read(pos, out, h.body + (size_t)h.size)) return false;
    return ElemAt(*out, 0, out->size(), e);
}

}  // namespace

bool ReadMkvIndex(const std::wstring& path, MediaIndex* out) {
    *out = MediaIndex{};
    Reader r(path);
    if (r.h == INVALID_HANDLE_VALUE) return false;
    Buf b;
    if (!r.Read(0, &b, 256 << 10)) return false;
    size_t at = 0;
    Elem ebml, seg;
    if (!ElemAt(b, at, b.size(), &ebml) || ebml.id != kEbml) return false;
    if (!ElemAt(b, ebml.end, b.size(), &seg) || seg.id != kSegment) return false;
    const uint64_t segData = seg.body;  // SeekPosition is relative to here

    // Level-1 elements in the first read; SeekHead says where the rest (Cues, often Info/Tracks too) live.
    uint64_t cuesPos = 0, infoPos = 0, tracksPos = 0;
    Buf info, tracks;
    Elem infoE, tracksE;
    bool haveInfo = false, haveTracks = false;
    for (size_t p = seg.body; p < b.size();) {
        Elem e;
        if (!ElemAt(b, p, b.size(), &e) || e.id == kCluster) break;
        if (e.id == kSeekHead) {
            Children(b, e, [&](const Elem& s) {
                if (s.id != kSeek) return;
                uint32_t id = 0;
                uint64_t pos = 0;
                Children(b, s, [&](const Elem& c) {
                    if (c.id == kSeekId) id = (uint32_t)UInt(b, c);
                    else if (c.id == kSeekPos) pos = UInt(b, c);
                });
                if (id == kCues) cuesPos = segData + pos;
                else if (id == kInfo) infoPos = segData + pos;
                else if (id == kTracks) tracksPos = segData + pos;
            });
        } else if ((e.id == kInfo || e.id == kTracks) && e.size != kUnknownSize && e.body + e.size <= b.size()) {
            Buf& dst = e.id == kInfo ? info : tracks;
            dst.assign(b.begin() + p, b.begin() + (size_t)(e.body + e.size));
            Elem& de = e.id == kInfo ? infoE : tracksE;
            ElemAt(dst, 0, dst.size(), &de);
            (e.id == kInfo ? haveInfo : haveTracks) = true;
        } else if (e.id == kCues && e.size != kUnknownSize) {
            cuesPos = p;
        }
        if (e.size == kUnknownSize) break;
        p = (size_t)(e.body + e.size);
    }
    if (!haveInfo && infoPos) haveInfo = LoadElement(r, infoPos, kInfo, 1 << 20, &info, &infoE);
    if (!haveTracks && tracksPos) haveTracks = LoadElement(r, tracksPos, kTracks, 4 << 20, &tracks, &tracksE);

    uint64_t scale = 1000000;  // ns per timestamp tick
    double durTicks = 0;
    if (haveInfo)
        Children(info, infoE, [&](const Elem& e) {
            if (e.id == kTimestampScale) scale = std::max<uint64_t>(1, UInt(info, e));
            else if (e.id == kDuration) durTicks = Float(info, e);
        });
    out->duration = durTicks * scale / 1e9;

    uint64_t videoTrack = 0;
    if (haveTracks)
        Children(tracks, tracksE, [&](const Elem& t) {
            if (t.id != kTrackEntry || videoTrack) return;
            uint64_t num = 0, type = 0;
            int w = 0, h = 0;
            Children(tracks, t, [&](const Elem& c) {
                if (c.id == kTrackNumber) num = UInt(tracks, c);
                else if (c.id == kTrackType) type = UInt(tracks, c);
                else if (c.id == kVideo)
                    Children(tracks, c, [&](const Elem& v) {
                        if (v.id == kPixelWidth) w = (int)UInt(tracks, v);
                        else if (v.id == kPixelHeight) h = (int)UInt(tracks, v);
                    });
            });
            if (type == 1) {
                videoTrack = num;
                out->width = w;
                out->height = h;
            }
        });

    // Cues: one CuePoint per indexed video keyframe. Only ever a subset of the real keyframes (muxers may index
    // fewer), which is still safe for cutting: every listed time is a keyframe.
    Buf cues;
    Elem cuesE;
    if (videoTrack && cuesPos && LoadElement(r, cuesPos, kCues, 64 << 20, &cues, &cuesE)) {
        Children(cues, cuesE, [&](const Elem& cp) {
            if (cp.id != kCuePoint) return;
            uint64_t t = 0;
            bool hasTime = false, forVideo = false;
            Children(cues, cp, [&](const Elem& c) {
                if (c.id == kCueTime) {
                    t = UInt(cues, c);
                    hasTime = true;
                } else if (c.id == kCueTrackPositions) {
                    Children(cues, c, [&](const Elem& tp) {
                        if (tp.id == kCueTrack && UInt(cues, tp) == videoTrack) forVideo = true;
                    });
                }
            });
            if (hasTime && forVideo) out->keyTimes.push_back(t * (double)scale / 1e9);
        });
        std::sort(out->keyTimes.begin(), out->keyTimes.end());
        out->keyTimes.erase(std::unique(out->keyTimes.begin(), out->keyTimes.end()), out->keyTimes.end());
        out->keyLags.assign(out->keyTimes.size(), -1);
    }
    return out->duration > 0 || !out->keyTimes.empty();
}
