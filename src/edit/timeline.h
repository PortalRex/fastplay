// Timeline.cs: zoomable timeline with ruler, thumbnails, waveform, keyframe marks and segment bars. Plain Win32 +
// GDI, double-buffered, per-monitor DPI aware. Mouse: click/drag seeks; drag on empty space creates a segment; drag
// a segment to move it, its edges to resize (Shift snaps to keyframes); wheel zooms around the cursor; Shift+wheel,
// middle drag or Alt+drag pans; Ctrl/Shift+click on a segment marks several.
#pragma once
#include "common.h"
#include "../thumbs.h"
#include "../markers.h"
#include <set>

namespace edit {

// Thumbnail source (FastPlay's keyframe decoder). Called on the UI thread.
struct TimelineThumbs {
    virtual ~TimelineThumbs() = default;
    // Best cached thumbnail within tol seconds of t, or null.
    virtual std::shared_ptr<const Thumb> Get(double t, double tol) = 0;
    // The thumbnails the visible strip wants (decode nearest to center first); replaces the previous wish list.
    virtual void Want(const std::vector<double>& times, double tol, double center) = 0;
};

struct TimelineCallbacks {
    std::function<void(double t)> seek;                     // click / scrub
    std::function<void(double t, POINT screen)> hover;      // mouse over the strip (preview popup)
    std::function<void()> hoverEnd;
    std::function<void(int index, uint32_t mods)> selectSegment;  // mods: MK_CONTROL | MK_SHIFT bits
    std::function<void()> editBegin;                        // before a drag changes segments (push undo)
    std::function<void()> editEnd;                          // after (autosave, refresh lists)
    std::function<Segment(double a, double b)> newSegment;  // makes a colored segment for drag-create
    std::function<std::wstring(double t)> timeText;         // ruler / tooltip labels (timecode offset aware)
};

class Timeline {
public:
    bool Create(HINSTANCE inst, HWND parent, TimelineCallbacks cb);
    HWND Hwnd() const { return hwnd_; }
    void SetDpi(int dpi);
    int PreferredHeight() const;  // depends on which rows are shown and the DPI
    void Invalidate();
    void WaveUpdated();           // more peaks arrived (cheap; redraws at most ~15x per second)

    void ResetView();
    void Zoom(double factor, double anchor);
    void ZoomTo(double start, double end);
    void Follow(double t);        // keep the playhead in view while playing
    double ViewStart() const { return viewStart_; }
    double ViewLength() const { return viewLen_; }

    // Data the editor owns; the timeline only reads it (segments are edited in place during drags, between the
    // editBegin and editEnd callbacks).
    double duration = 1, position = 0;
    std::vector<Segment>* segments = nullptr;
    int selected = -1;
    std::set<int>* marked = nullptr;
    std::vector<double> keyframes;
    const std::vector<Marker>* markers = nullptr;  // FastPlay's timeline markers (owned by App)
    const float* wave = nullptr;
    size_t waveCount = 0;
    TimelineThumbs* thumbs = nullptr;
    double thumbAspect = 16.0 / 9.0;
    bool showThumbs = true, showWave = true, showKeyframes = true, hasVideo = true, hasMedia = false;
    double keyframeProgress = -1, waveProgress = -1;  // < 0: not loading
    std::wstring cutMode = L"keyframe";               // draws the keyframe-cut lead-in / normal-cut no-video hints

    Timeline();   // defined in timeline.cpp (= default) so Impl stays private
    ~Timeline();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    HWND hwnd_ = nullptr;
    double viewStart_ = 0, viewLen_ = 1;
    friend struct Impl;
};

}  // namespace edit
