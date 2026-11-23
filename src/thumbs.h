// Seekbar thumbnails: a second, hidden libmpv instance decodes keyframes only (no audio, software render API into a
// small BGRA buffer) on a below-normal-priority thread. Requests coalesce: only the latest hover position waits.
#pragma once
#include "util.h"
#include <memory>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <map>
#include <atomic>

struct Thumb {
    int w = 0, h = 0;
    double time = 0;
    std::vector<uint32_t> px;  // BGRX, top-down
};

class Thumbnailer {
public:
    ~Thumbnailer() { Stop(); }
    // keyTimes may be empty (then hover times are bucketed). prefill: decode an evenly spread set in the background.
    void Start(HWND notify, UINT msg, const std::wstring& path, std::vector<double> keyTimes, double duration,
               int thumbWidth, int maxCache, bool prefill);
    void Stop();
    bool Running() const { return thread_.joinable(); }
    void SetDuration(double d);
    void SetKeyTimes(std::vector<double> keyTimes);
    double Snap(double t) const;               // keyframe (or bucket) time a hover at t will show
    void Request(double t);                    // decode Snap(t) soon (replaces the previous request)
    std::shared_ptr<const Thumb> Get(double t);// exact match, else the nearest cached one within reach, else null

private:
    void Run();
    bool DecodeOne(struct mpv_handle* h, struct mpv_render_context* rc, double t);
    int64_t Key(double t) const { return (int64_t)(t * 1000 + 0.5); }

    HWND notify_ = nullptr;
    UINT msg_ = 0;
    std::wstring path_;
    std::thread thread_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::vector<double> keys_;
    double duration_ = 0;
    double request_ = -1;
    std::vector<double> prefill_;
    bool wantPrefill_ = false;
    int thumbW_ = 192, maxCache_ = 150;
    struct Slot { std::shared_ptr<const Thumb> t; uint64_t used; };
    std::map<int64_t, Slot> cache_;
    uint64_t tick_ = 0;
    HANDLE frameEvent_ = nullptr;
};
