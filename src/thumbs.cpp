#include "thumbs.h"
#include "mpvapi.h"
#include <algorithm>
#include <cmath>

void Thumbnailer::Start(HWND notify, UINT msg, const std::wstring& path, std::vector<double> keyTimes, double duration,
                        int thumbWidth, int maxCache, bool prefill) {
    Stop();
    notify_ = notify;
    msg_ = msg;
    path_ = path;
    keys_ = std::move(keyTimes);
    duration_ = duration;
    thumbW_ = std::clamp(thumbWidth, 64, 480);
    maxCache_ = maxCache;
    wantPrefill_ = prefill;
    stop_ = false;
    request_ = -1;
    cache_.clear();
    prefill_.clear();
    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread(&Thumbnailer::Run, this);
}

void Thumbnailer::Stop() {
    if (!thread_.joinable()) return;
    {
        std::lock_guard<std::mutex> l(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    if (frameEvent_) SetEvent(frameEvent_);
    thread_.join();
    CloseHandle(frameEvent_);
    frameEvent_ = nullptr;
    std::lock_guard<std::mutex> l(mu_);
    cache_.clear();
    prefill_.clear();
    keys_.clear();
}

void Thumbnailer::SetDuration(double d) {
    std::lock_guard<std::mutex> l(mu_);
    duration_ = d;
}

void Thumbnailer::SetKeyTimes(std::vector<double> keyTimes) {
    std::lock_guard<std::mutex> l(mu_);
    keys_ = std::move(keyTimes);
}

double Thumbnailer::Snap(double t) const {
    std::lock_guard<std::mutex> l(mu_);
    if (!keys_.empty()) {
        // Nearest keyframe at or before t (that is what a keyframe seek to t shows), else the first.
        auto it = std::upper_bound(keys_.begin(), keys_.end(), t);
        if (it == keys_.begin()) return *it;
        return *(it - 1);
    }
    double step = std::max(2.0, duration_ / 400);
    return std::max(0.0, std::floor(t / step) * step);
}

void Thumbnailer::Request(double t) {
    double s = Snap(t);
    {
        std::lock_guard<std::mutex> l(mu_);
        if (cache_.count(Key(s))) return;
        request_ = s;
    }
    cv_.notify_all();
}

std::shared_ptr<const Thumb> Thumbnailer::Get(double t) {
    double s = Snap(t);
    std::lock_guard<std::mutex> l(mu_);
    auto it = cache_.find(Key(s));
    if (it != cache_.end()) {
        it->second.used = ++tick_;
        return it->second.t;
    }
    // Nearest cached thumbnail within 2% of the duration stands in while the exact one decodes.
    double reach = std::max(10.0, duration_ * 0.02);
    auto lo = cache_.lower_bound(Key(s));
    std::shared_ptr<const Thumb> best;
    double bestD = reach;
    if (lo != cache_.end() && std::abs(lo->second.t->time - s) < bestD) { best = lo->second.t; bestD = std::abs(lo->second.t->time - s); }
    if (lo != cache_.begin()) {
        auto p = std::prev(lo);
        if (std::abs(p->second.t->time - s) < bestD) best = p->second.t;
    }
    return best;
}

static void OnRenderUpdate(void* ev) { SetEvent((HANDLE)ev); }

void Thumbnailer::Run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    mpv_handle* h = g_mpv.mpv_create();
    if (!h) return;
    const char* opts[][2] = {
        {"config", "no"}, {"load-scripts", "no"}, {"osc", "no"}, {"ytdl", "no"}, {"terminal", "no"},
        {"input-default-bindings", "no"}, {"input-vo-keyboard", "no"}, {"load-stats-overlay", "no"},
        {"load-console", "no"}, {"load-auto-profiles", "no"}, {"load-select", "no"}, {"load-positioning", "no"},
        {"load-commands", "no"}, {"load-context-menu", "no"},
        {"vo", "libmpv"}, {"hwdec", "no"}, {"audio", "no"}, {"sid", "no"}, {"pause", "yes"}, {"idle", "yes"},
        {"keep-open", "always"}, {"hr-seek", "no"}, {"cache", "no"}, {"demuxer-readahead-secs", "0"},
        {"demuxer-max-bytes", "4MiB"}, {"demuxer-max-back-bytes", "0"}, {"vd-lavc-threads", "1"},
        {"vd-lavc-skiploopfilter", "all"}, {"vd-lavc-fast", "yes"}, {"video-latency-hacks", "yes"},
        {"sub-auto", "no"}, {"audio-file-auto", "no"}, {"cover-art-auto", "no"}, {"osd-level", "0"},
        {"sws-scaler", "bilinear"}, {"stop-screensaver", "no"}, {"taskbar-progress", "no"},
    };
    for (auto& o : opts) g_mpv.mpv_set_option_string(h, o[0], o[1]);
    mpv_render_context* rc = nullptr;
    if (g_mpv.mpv_initialize(h) < 0) { g_mpv.mpv_terminate_destroy(h); return; }
    mpv_render_param cp[] = {{MPV_RENDER_PARAM_API_TYPE, (void*)MPV_RENDER_API_TYPE_SW}, {MPV_RENDER_PARAM_INVALID, nullptr}};
    if (g_mpv.mpv_render_context_create(&rc, h, cp) < 0) { g_mpv.mpv_terminate_destroy(h); return; }
    g_mpv.mpv_render_context_set_update_callback(rc, OnRenderUpdate, frameEvent_);

    std::string p = util::Utf8(path_);
    const char* load[] = {"loadfile", p.c_str(), nullptr};
    g_mpv.mpv_command(h, load);
    // Wait for the file to open (or fail) before taking requests.
    bool loaded = false;
    for (int i = 0; i < 400 && !loaded; i++) {
        {
            std::lock_guard<std::mutex> l(mu_);
            if (stop_) break;
        }
        mpv_event* e = g_mpv.mpv_wait_event(h, 0.05);
        if (e->event_id == MPV_EVENT_FILE_LOADED) loaded = true;
        else if (e->event_id == MPV_EVENT_END_FILE || e->event_id == MPV_EVENT_SHUTDOWN) break;
    }

    while (loaded) {
        double target = -1;
        {
            std::unique_lock<std::mutex> l(mu_);
            if (wantPrefill_ && prefill_.empty() && duration_ > 0) {
                // Coarse-to-fine order so the first few already cover the whole timeline.
                const int n = 64;
                std::vector<double> evenly;
                for (int i = 0; i < n; i++) {
                    double t = duration_ * (i + 0.5) / n;
                    if (!keys_.empty()) {
                        auto it = std::upper_bound(keys_.begin(), keys_.end(), t);
                        t = it == keys_.begin() ? *it : *(it - 1);
                    }
                    evenly.push_back(t);
                }
                for (int stride = n / 2; stride >= 1; stride /= 2)
                    for (int i = 0; i < n; i += stride)
                        if (std::find(prefill_.begin(), prefill_.end(), evenly[i]) == prefill_.end()) prefill_.push_back(evenly[i]);
                std::reverse(prefill_.begin(), prefill_.end());  // pop_back takes them in order
                wantPrefill_ = false;
            }
            cv_.wait(l, [&] { return stop_ || request_ >= 0 || !prefill_.empty(); });
            if (stop_) break;
            if (request_ >= 0) {
                target = request_;
                request_ = -1;
            } else {
                target = prefill_.back();
                prefill_.pop_back();
            }
            if (cache_.count(Key(target))) continue;
        }
        if (DecodeOne(h, rc, target)) PostMessageW(notify_, msg_, 0, 0);
    }
    g_mpv.mpv_render_context_free(rc);
    g_mpv.mpv_terminate_destroy(h);
}

bool Thumbnailer::DecodeOne(mpv_handle* h, mpv_render_context* rc, double t) {
    char ts[32];
    snprintf(ts, sizeof ts, "%.3f", t);
    const char* seek[] = {"seek", ts, "absolute+keyframes", nullptr};
    if (g_mpv.mpv_command(h, seek) < 0) return false;
    // Wait for the seek to finish (playback-restart), then for the new frame to reach the render context.
    bool restarted = false;
    for (int i = 0; i < 60 && !restarted; i++) {
        {
            std::lock_guard<std::mutex> l(mu_);
            if (stop_) return false;
        }
        mpv_event* e = g_mpv.mpv_wait_event(h, 0.05);
        if (e->event_id == MPV_EVENT_PLAYBACK_RESTART) restarted = true;
        if (e->event_id == MPV_EVENT_SHUTDOWN || e->event_id == MPV_EVENT_END_FILE) return false;
    }
    if (!restarted) return false;
    for (int i = 0; i < 20; i++) {
        if (g_mpv.mpv_render_context_update(rc) & MPV_RENDER_UPDATE_FRAME) break;
        WaitForSingleObject(frameEvent_, 25);
    }
    int64_t vw = 0, vh = 0;
    g_mpv.mpv_get_property(h, "dwidth", MPV_FORMAT_INT64, &vw);
    g_mpv.mpv_get_property(h, "dheight", MPV_FORMAT_INT64, &vh);
    if (vw <= 0 || vh <= 0) return false;
    auto th = std::make_shared<Thumb>();
    th->w = thumbW_;
    th->h = std::max(16, (int)(thumbW_ * vh / vw));
    th->time = t;
    th->px.resize((size_t)th->w * th->h);
    int size[2] = {th->w, th->h};
    size_t stride = (size_t)th->w * 4;
    mpv_render_param rp[] = {
        {MPV_RENDER_PARAM_SW_SIZE, size},
        {MPV_RENDER_PARAM_SW_FORMAT, (void*)"bgr0"},
        {MPV_RENDER_PARAM_SW_STRIDE, &stride},
        {MPV_RENDER_PARAM_SW_POINTER, th->px.data()},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    if (g_mpv.mpv_render_context_render(rc, rp) < 0) return false;

    std::lock_guard<std::mutex> l(mu_);
    cache_[Key(t)] = {th, ++tick_};
    while ((int)cache_.size() > maxCache_) {
        auto victim = std::min_element(cache_.begin(), cache_.end(),
                                       [](const auto& a, const auto& b) { return a.second.used < b.second.used; });
        cache_.erase(victim);
    }
    return true;
}
