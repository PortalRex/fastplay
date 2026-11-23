// FF.cs Probe/ProbeDuration/Keyframes/Waveform and Models.cs MediaInfo.Parse.
#include "media.h"
#include "ffrun.h"
#include "segio.h"
#include "../mp4index.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <list>
#include <mutex>
#include <thread>

namespace edit {
namespace detail {  // common.cpp
bool ParseDouble(const std::string& text, double* out);
}

namespace {

// ---------------------------------------------------------------- MP4 header index cache

// FastPlay's MP4 header index per file (null = no usable MP4/MOV header). Small LRU, revalidated by size + mtime so a
// file that is still being recorded is re-read.
struct IndexCache {
    struct Entry {
        std::wstring key;
        uint64_t size = 0, mtime = 0;
        std::shared_ptr<const MediaIndex> idx;
    };
    std::mutex mu;
    std::list<Entry> items;  // most recent first
    static constexpr size_t kMax = 16;
};

IndexCache& Cache() {
    static IndexCache c;
    return c;
}

bool StatFile(const std::wstring& path, uint64_t* size, uint64_t* mtime) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    *size = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
    *mtime = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
    return true;
}

std::shared_ptr<const MediaIndex> HeaderIndex(const std::wstring& path) {
    uint64_t size = 0, mtime = 0;
    if (!StatFile(path, &size, &mtime)) return nullptr;
    std::wstring key = util::Lower(util::FullPath(path));
    IndexCache& c = Cache();
    // Held while reading: a header read is a few small reads, and Probe/Keyframes/Waveform on one file then share it.
    std::lock_guard<std::mutex> lock(c.mu);
    for (auto it = c.items.begin(); it != c.items.end(); ++it) {
        if (it->key != key) continue;
        if (it->size == size && it->mtime == mtime) {
            c.items.splice(c.items.begin(), c.items, it);
            return c.items.front().idx;
        }
        c.items.erase(it);
        break;
    }
    auto idx = std::make_shared<MediaIndex>();
    std::shared_ptr<const MediaIndex> result;
    if (ReadMediaIndex(path, idx.get(), false)) result = std::move(idx);
    c.items.push_front({key, size, mtime, result});
    if (c.items.size() > IndexCache::kMax) c.items.pop_back();
    return result;
}

bool HasMfra(const std::shared_ptr<const MediaIndex>& idx) { return idx && idx->fragmented && !idx->keyOffsets.empty(); }

double Rate(const std::string& r) {
    size_t slash = r.find('/');
    double a, b;
    if (slash != std::string::npos && detail::ParseDouble(r.substr(0, slash), &a) &&
        detail::ParseDouble(r.substr(slash + 1), &b) && b != 0)
        return a / b;
    return 0;
}

}  // namespace

std::vector<std::wstring> FastInputOptions(const std::wstring& path) {
    if (HasMfra(HeaderIndex(path))) return {L"-use_mfra_for", L"pts"};
    return {};
}

// ---------------------------------------------------------------- probing

MediaInfo ParseProbe(const std::wstring& path, const std::string& json) {
    using segio::Json;
    segio::JsonPtr root = segio::ParseJson(json);
    MediaInfo m;
    m.path = path;
    m.rawJson = json;
    if (!root || root->kind != Json::Object) throw EditError(L"Unexpected ffprobe output");
    if (const Json* f = root->Get("format")) {
        m.duration = f->Num("duration");
        m.startTime = f->Num("start_time");
        m.formatName = f->Str("format_name");
        m.bitRate = (long long)f->Num("bit_rate");
    }
    if (const Json* streams = root->Get("streams"); streams && streams->kind == Json::Array) {
        for (const auto& stp : streams->arr) {
            const Json& st = *stp;
            if (st.kind != Json::Object) continue;
            StreamInfo s;
            s.index = (int)st.Num("index");
            s.type = st.Str("codec_type");
            s.codec = st.Str("codec_name");
            s.width = (int)st.Num("width");
            s.height = (int)st.Num("height");
            s.pixFmt = st.Str("pix_fmt");
            s.profile = st.Str("profile");
            s.bitRate = (long long)st.Num("bit_rate");
            s.sampleRate = (int)st.Num("sample_rate");
            s.channels = (int)st.Num("channels");
            s.startTime = st.Num("start_time");
            s.duration = st.Num("duration");
            double avg = Rate(st.Str("avg_frame_rate"));
            s.fps = avg > 0 ? avg : Rate(st.Str("r_frame_rate"));
            if (const Json* tags = st.Get("tags"); tags && tags->kind == Json::Object) {
                s.language = util::Wide(tags->Str("language"));
                s.title = util::Wide(tags->Str("title"));
                // Matroska keeps per-stream length in a DURATION tag ("00:00:14.616000000").
                double tagDur;
                if (s.duration <= 0 && TryParseTime(util::Wide(tags->Str("DURATION")), &tagDur)) s.duration = tagDur;
                std::string rot = tags->Str("rotate");
                double r;
                if (!rot.empty() && rot.find_first_of(".eE") == std::string::npos && detail::ParseDouble(rot, &r))
                    s.rotation = (int)r;
            }
            if (const Json* sd = st.Get("side_data_list"); sd && sd->kind == Json::Array) {
                for (const auto& d : sd->arr) {
                    const Json* r = d->Get("rotation");
                    if (r && r->kind == Json::Number) s.rotation = ((int)-r->n % 360 + 360) % 360;
                }
            }
            if (const Json* disp = st.Get("disposition"); disp && disp->Get("attached_pic") && disp->Num("attached_pic") == 1)
                s.isCoverArt = true;
            std::string tag = st.Str("codec_tag_string");
            s.encrypted = tag == "drmi" || tag == "drms" || tag == "drmz" || tag == "encv" || tag == "enca";
            // Data streams (tmcd, gpmd...) usually break remuxing to other containers, so default them off.
            s.keep = s.type == "video" || s.type == "audio" || s.type == "subtitle";
            m.streams.push_back(std::move(s));
        }
    }
    if (const Json* chapters = root->Get("chapters"); chapters && chapters->kind == Json::Array) {
        for (const auto& c : chapters->arr) {
            if (c->kind != Json::Object) continue;
            Chapter ch;
            const Json* t = c->Get("tags");
            ch.title = t ? util::Wide(t->Str("title")) : L"";
            ch.start = c->Num("start_time") - m.startTime;
            ch.end = c->Num("end_time") - m.startTime;
            m.chapters.push_back(std::move(ch));
        }
    }
    if (m.duration <= 0) m.duration = 0.001;
    return m;
}

MediaInfo Probe(const std::wstring& path, Cancel* ct) {
    auto idx = HeaderIndex(path);
    bool mfra = HasMfra(idx);
    std::vector<std::wstring> args = {L"-v", L"error", L"-print_format", L"json", L"-show_format", L"-show_streams",
                                      L"-show_chapters"};
    if (mfra) args.insert(args.end(), {L"-use_mfra_for", L"pts"});
    args.push_back(path);
    auto r = ff::Run(ff::Ffprobe(), args, ct);
    r.Check();
    MediaInfo m = ParseProbe(path, r.out);
    if (mfra) {
        // In mfra mode ffprobe only knows the duration up to the last random-access entry; the header index reads
        // the last fragment's real end.
        m.fragmentedMfra = true;
        if (idx->duration > 0) {
            // The index end is the video track's decode-time end; with B-frames presentation runs later by the
            // reorder delay, which (edit lists are ignored for fragmented files) is the video stream's start time.
            const StreamInfo* v = m.Video();
            double delay = v && v->startTime > 0 && v->startTime < 5 ? v->startTime : 0;
            m.duration = std::max(m.duration, idx->duration + delay - m.startTime);
            for (auto& s : m.streams)
                if ((s.type == "video" || s.type == "audio") && s.duration < idx->duration) s.duration = idx->duration;
            // ffprobe's bit_rate is the file size over its (partial) duration here, so it comes out thousands of times
            // too high; the size estimates and the Discord size budget need the real average.
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (m.duration > 0 && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
                double bytes = (double)(((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
                m.bitRate = (long long)(bytes * 8 / m.duration);
            }
        }
    }
    return m;
}

double ProbeDuration(const std::wstring& path, Cancel* ct) {
    auto idx = HeaderIndex(path);
    if (HasMfra(idx) && idx->duration > 0) return idx->duration;
    auto r = ff::Run(ff::Ffprobe(), {L"-v", L"error", L"-show_entries", L"format=duration", L"-of", L"csv=p=0", path}, ct,
                     {}, {}, false);
    double d;
    return detail::ParseDouble(r.out, &d) ? d : 0;
}

// ---------------------------------------------------------------- keyframes

KeyframeIndex Keyframes(const MediaInfo& m, int stream, Cancel* ct, const std::function<void(double)>& progress) {
    if (ct && ct->Cancelled()) throw Cancelled();

    // MP4/MOV keep a keyframe table in their header (sample tables, or mfra when fragmented): milliseconds instead
    // of reading every packet. The index covers the first video track, which is ffprobe's first video stream.
    const StreamInfo* v = m.Video();
    if (v && v->index == stream) {
        auto idx = HeaderIndex(m.path);
        if (idx && !idx->keyTimes.empty()) {
            KeyframeIndex k;
            k.pts.reserve(idx->keyTimes.size());
            k.dtsLag.reserve(idx->keyTimes.size());
            for (size_t i = 0; i < idx->keyTimes.size(); i++) {
                k.pts.push_back(idx->keyTimes[i] - m.startTime);
                k.dtsLag.push_back(i < idx->keyLags.size() ? idx->keyLags[i] : -1);
            }
            if (progress) progress(1);
            return k;
        }
    }

    // Packet flags only, no decoding.
    std::vector<std::pair<double, double>> list;
    size_t lines = 0;
    const double start = m.startTime, duration = m.duration;
    std::vector<std::wstring> args = {L"-v", L"error", L"-select_streams", std::to_wstring(stream), L"-show_entries",
                                      L"packet=pts_time,dts_time,flags", L"-of", L"csv=p=0"};
    auto fast = FastInputOptions(m.path);
    args.insert(args.end(), fast.begin(), fast.end());
    args.push_back(m.path);
    ff::Run(ff::Ffprobe(), args, ct,
            [&](const std::string& l) {
                size_t c1 = l.find(','), c2 = c1 == std::string::npos ? c1 : l.find(',', c1 + 1);
                bool key = c2 != std::string::npos && c2 + 1 < l.size() && l[c2 + 1] == 'K';
                // Packets arrive in file order, so the latest timestamp tells how far the scan is.
                if (!key && !(progress && ++lines % 400 == 0)) return;
                double t;
                if (!detail::ParseDouble(l.substr(0, c1), &t)) return;
                if (key) {
                    double d;
                    double lag = c2 != std::string::npos && detail::ParseDouble(l.substr(c1 + 1, c2 - c1 - 1), &d)
                                     ? std::max(0.0, t - d)
                                     : -1;
                    list.push_back({t - start, lag});
                }
                if (progress && duration > 0) progress(std::clamp((t - start) / duration, 0.0, 1.0));
            },
            {}, false);
    std::sort(list.begin(), list.end());
    KeyframeIndex k;
    k.pts.reserve(list.size());
    k.dtsLag.reserve(list.size());
    for (const auto& p : list) {
        k.pts.push_back(p.first);
        k.dtsLag.push_back(p.second);
    }
    return k;
}

// ---------------------------------------------------------------- waveform

void Waveform(const MediaInfo& m, int stream, float* target, size_t count, std::atomic<size_t>* filled, Cancel* ct) {
    if (filled) filled->store(0);
    const double duration = m.duration;
    int parts = duration < 120 ? 1 : std::clamp((int)std::thread::hardware_concurrency() / 2, 2, 4);
    const double len = duration / parts;
    const auto fast = FastInputOptions(m.path);

    auto part = [&](int p) {
        constexpr int rate = 8000, perPeak = rate / kWaveRate;
        double start = p * len, length = p == parts - 1 ? duration - p * len + 1 : len;
        size_t at = (size_t)std::llround(start * kWaveRate);
        std::vector<std::wstring> args = {L"-v", L"error"};
        if (start > 0) args.insert(args.end(), {L"-ss", Num(start)});
        args.insert(args.end(), {L"-t", Num(length)});
        args.insert(args.end(), fast.begin(), fast.end());
        args.insert(args.end(), {L"-i", m.path, L"-map", L"0:" + std::to_wstring(stream), L"-vn", L"-sn", L"-dn",
                                 L"-ac", L"1", L"-ar", std::to_wstring(rate), L"-f", L"s16le", L"-acodec",
                                 L"pcm_s16le", L"-"});
        int n = 0, peak = 0, carry = -1;
        size_t written = 0;
        auto put = [&](int value) {
            if (at < count) {
                target[at++] = value / 32768.f;
                written++;
            }
        };
        auto sample = [&](int16_t v) {
            int a = v < 0 ? -(int)v : v;
            if (a > peak) peak = a;
            if (++n == perPeak) {
                put(peak);
                n = 0;
                peak = 0;
            }
        };
        ff::RunStream(ff::Ffmpeg(), args, ct, [&](const uint8_t* d, size_t size) {
            size_t i = 0;
            if (carry >= 0) {
                sample((int16_t)(carry | d[0] << 8));
                i = 1;
                carry = -1;
            }
            for (; i + 1 < size; i += 2) sample((int16_t)(d[i] | d[i + 1] << 8));
            if (i < size) carry = d[i];
            if (filled && written) {
                filled->fetch_add(written);
                written = 0;
            }
            return true;
        });
        if (n > 0) put(peak);
        if (filled && written) filled->fetch_add(written);
    };

    // Part 0 on this thread, the rest in parallel; the first error (Cancelled included) is rethrown after all finish.
    std::vector<std::exception_ptr> errors(parts);
    std::vector<std::thread> threads;
    for (int p = 1; p < parts; p++)
        threads.emplace_back([&, p] {
            try { part(p); } catch (...) { errors[p] = std::current_exception(); }
        });
    try { part(0); } catch (...) { errors[0] = std::current_exception(); }
    for (auto& t : threads) t.join();
    for (auto& e : errors)
        if (e) std::rethrow_exception(e);
}

}  // namespace edit
