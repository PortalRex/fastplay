// Private painting helper for the edit-mode GDI controls (timeline.cpp, segcards.cpp): a back buffer that is a
// top-down 32-bit DIB section, so GDI can draw text into it while translucent fills, anti-aliased rounded shapes and
// hatches are written straight into its pixels (plain GDI has no alpha blending without msimg32 / GDI+).
#pragma once
#include "../util.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace edit::ui {

// COLORREF (0x00BBGGRR) -> DIB pixel (0x00RRGGBB).
inline uint32_t Px(COLORREF c) { return (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c); }

// "00:01:02.500" -> "01:02.500" when the hours field is zero.
inline std::wstring StripZeroHours(std::wstring s) {
    if (s.rfind(L"00:", 0) == 0 && std::count(s.begin(), s.end(), L':') == 2) s.erase(0, 3);
    return s;
}

inline COLORREF Lighten(COLORREF c, float amount) {
    auto ch = [&](int v) { return (int)(v + (255 - v) * amount); };
    return RGB(ch(GetRValue(c)), ch(GetGValue(c)), ch(GetBValue(c)));
}

struct RectF {
    float x0, y0, x1, y1;
};

class Canvas {
public:
    Canvas() = default;
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;
    ~Canvas() { Free(); }

    // (Re)allocates the buffer when the size changes. Returns false if the size is empty or allocation failed.
    bool Ensure(int w, int h) {
        if (w <= 0 || h <= 0) return false;
        if (dc_ && w == w_ && h == h_) return true;
        Free();
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bmp_ = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp_) return false;
        dc_ = CreateCompatibleDC(nullptr);
        old_ = SelectObject(dc_, bmp_);
        SetBkMode(dc_, TRANSPARENT);
        px_ = (uint32_t*)bits;
        w_ = w;
        h_ = h;
        return true;
    }

    void Free() {
        if (dc_) {
            SelectObject(dc_, old_);
            DeleteDC(dc_);
        }
        if (bmp_) DeleteObject(bmp_);
        dc_ = nullptr;
        bmp_ = nullptr;
        old_ = nullptr;
        px_ = nullptr;
        w_ = h_ = 0;
    }

    HDC Dc() const { return dc_; }
    int W() const { return w_; }
    int H() const { return h_; }
    uint32_t* Row(int y) { return px_ + (size_t)y * w_; }
    // GDI batches its drawing: flush before touching pixels again after GDI drew text.
    void Sync() {
        if (!gdiPending_) return;
        GdiFlush();
        gdiPending_ = false;
    }

    void Text(HFONT f, const std::wstring& s, RECT r, COLORREF c, UINT flags) {
        if (r.right <= r.left || r.bottom <= r.top || s.empty()) return;
        HGDIOBJ o = SelectObject(dc_, f);
        SetTextColor(dc_, c);
        DrawTextW(dc_, s.c_str(), (int)s.size(), &r, flags | DT_NOPREFIX | DT_SINGLELINE);
        SelectObject(dc_, o);
        gdiPending_ = true;
    }

    // Left/top aligned text without DrawText's layout pass (several times cheaper; for labels drawn every frame).
    void TextAt(HFONT f, const std::wstring& s, int x, int y, COLORREF c) {
        if (s.empty()) return;
        HGDIOBJ o = SelectObject(dc_, f);
        SetTextColor(dc_, c);
        ExtTextOutW(dc_, x, y, 0, nullptr, s.c_str(), (UINT)s.size(), nullptr);
        SelectObject(dc_, o);
        gdiPending_ = true;
    }

    void Present(HDC dst) const { BitBlt(dst, 0, 0, w_, h_, dc_, 0, 0, SRCCOPY); }

    static inline void Blend(uint32_t& d, uint32_t s, int a) {  // a: 0..256
        if (a <= 0) return;
        if (a >= 256) { d = s; return; }
        uint32_t ia = 256 - (uint32_t)a;
        uint32_t rb = (((s & 0xFF00FF) * (uint32_t)a + (d & 0xFF00FF) * ia) >> 8) & 0xFF00FF;
        uint32_t g = (((s & 0x00FF00) * (uint32_t)a + (d & 0x00FF00) * ia) >> 8) & 0x00FF00;
        d = rb | g;
    }

    // Blends one color at one alpha (0..256) over n pixels; the source terms are computed once.
    static void BlendRow(uint32_t* d, int n, uint32_t s, int a) {
        if (a <= 0 || n <= 0) return;
        if (a >= 256) { std::fill(d, d + n, s); return; }
        uint32_t ia = 256 - (uint32_t)a, srb = (s & 0xFF00FF) * (uint32_t)a, sg = (s & 0x00FF00) * (uint32_t)a;
        for (int i = 0; i < n; i++) {
            uint32_t v = d[i];
            d[i] = ((((v & 0xFF00FF) * ia + srb) >> 8) & 0xFF00FF) | ((((v & 0x00FF00) * ia + sg) >> 8) & 0x00FF00);
        }
    }

    // Integer rectangle [x0,x1) x [y0,y1), alpha 0..255.
    void Fill(int x0, int y0, int x1, int y1, COLORREF c, int alpha = 255) {
        Sync();
        x0 = std::max(x0, 0); y0 = std::max(y0, 0);
        x1 = std::min(x1, w_); y1 = std::min(y1, h_);
        if (x0 >= x1 || y0 >= y1) return;
        uint32_t s = Px(c);
        int a = alpha >= 255 ? 256 : alpha * 256 / 255;
        for (int y = y0; y < y1; y++) BlendRow(Row(y) + x0, x1 - x0, s, a);
    }

    // Rectangle with fractional left/right edges (anti-aliased), e.g. the 2 px playhead.
    void FillF(float x0, float y0, float x1, float y1, COLORREF c, int alpha = 255) {
        Sync();
        int iy0 = std::max(0, (int)std::floor(y0 + 0.5f)), iy1 = std::min(h_, (int)std::floor(y1 + 0.5f));
        int ix0 = std::max(0, (int)std::floor(x0)), ix1 = std::min(w_, (int)std::ceil(x1));
        if (ix0 >= ix1 || iy0 >= iy1) return;
        uint32_t s = Px(c);
        auto colAlpha = [&](int x) {
            float cov = std::min((float)x + 1, x1) - std::max((float)x, x0);
            return (int)(alpha * std::clamp(cov, 0.f, 1.f) * 256 / 255);
        };
        // Only the first and last columns are partially covered.
        int aFirst = colAlpha(ix0), aLast = colAlpha(ix1 - 1), aMid = alpha * 256 / 255;
        for (int y = iy0; y < iy1; y++) {
            uint32_t* r = Row(y);
            Blend(r[ix0], s, aFirst);
            if (ix1 - 1 > ix0) Blend(r[ix1 - 1], s, aLast);
            BlendRow(r + ix0 + 1, ix1 - 1 - (ix0 + 1), s, aMid);
        }
    }

    // Anti-aliased rounded rectangle. stroke == 0 fills it; stroke > 0 draws an outline of that width centered on
    // the edge (like a GDI+ pen). shade(x, y, &color, &alpha) may override the color per pixel (hatches).
    template <class Shade>
    void RoundShaded(RectF r, float radius, float stroke, Shade shade) {
        Sync();
        float hw = (r.x1 - r.x0) / 2, hh = (r.y1 - r.y0) / 2;
        if (hw <= 0 || hh <= 0) return;
        radius = std::clamp(radius, 0.f, std::min(hw, hh));
        float cx = (r.x0 + r.x1) / 2, cy = (r.y0 + r.y1) / 2, pad = stroke / 2 + 1;
        int ix0 = std::max(0, (int)std::floor(r.x0 - pad)), ix1 = std::min(w_, (int)std::ceil(r.x1 + pad));
        int iy0 = std::max(0, (int)std::floor(r.y0 - pad)), iy1 = std::min(h_, (int)std::ceil(r.y1 + pad));
        // Pixels deeper inside than `inner` are fully covered (fill) or untouched (outline): handled per row span.
        float inner = stroke > 0 ? stroke / 2 + 0.5f : 0.5f;
        for (int y = iy0; y < iy1; y++) {
            uint32_t* row = Row(y);
            float qy = std::fabs(y + 0.5f - cy) - (hh - radius);
            int sx0 = ix1, sx1 = ix1;
            if (qy <= 0 && qy <= radius - inner && hw - inner > 0) {  // for such rows d = max(qx, qy) - radius
                sx0 = std::max(ix0, (int)std::ceil(cx - (hw - inner) - 0.5f));
                sx1 = std::max(sx0, std::min(ix1, (int)std::floor(cx + (hw - inner) - 0.5f) + 1));
            }
            for (int x = ix0; x < ix1; x++) {
                if (x == sx0 && sx1 > sx0) {
                    if (stroke <= 0)
                        for (; x < sx1; x++) {
                            COLORREF c;
                            int alpha;
                            shade(x, y, &c, &alpha);
                            Blend(row[x], Px(c), alpha * 256 / 255);
                        }
                    x = sx1 - 1;
                    continue;
                }
                float qx = std::fabs(x + 0.5f - cx) - (hw - radius);
                float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
                float d = (ox > 0 && oy > 0 ? std::sqrt(ox * ox + oy * oy) : ox + oy) + std::min(std::max(qx, qy), 0.f) -
                          radius;
                float cov = stroke > 0 ? stroke / 2 + 0.5f - std::fabs(d) : 0.5f - d;
                if (cov <= 0) continue;
                COLORREF c;
                int alpha;
                shade(x, y, &c, &alpha);
                Blend(row[x], Px(c), (int)(alpha * std::min(cov, 1.f) * 256 / 255));
            }
        }
    }

    void Round(RectF r, float radius, COLORREF c, int alpha = 255, float stroke = 0) {
        RoundShaded(r, radius, stroke, [&](int, int, COLORREF* pc, int* pa) { *pc = c; *pa = alpha; });
    }

    // Anti-aliased circle (filled, or an outline of the given width).
    void Circle(float cx, float cy, float radius, COLORREF c, int alpha = 255, float stroke = 0) {
        Round({cx - radius, cy - radius, cx + radius, cy + radius}, radius, c, alpha, stroke);
    }

    // Diagonal "/" stripes anchored to the window origin: `line` pixels of fg every `period` pixels, bg between.
    void Hatch(int x0, int y0, int x1, int y1, int period, int line, COLORREF fg, int fgA, COLORREF bg, int bgA) {
        Sync();
        x0 = std::max(x0, 0); y0 = std::max(y0, 0);
        x1 = std::min(x1, w_); y1 = std::min(y1, h_);
        uint32_t f = Px(fg), b = Px(bg);
        int fa = fgA * 256 / 255, ba = bgA * 256 / 255;
        for (int y = y0; y < y1; y++) {
            uint32_t* row = Row(y);
            for (int x = x0; x < x1; x++) {
                bool on = (x + y) % period < line;
                Blend(row[x], on ? f : b, on ? fa : ba);
            }
        }
    }

    // Copies a BGRX image (already at its final size) to (x, y), clipped.
    void Blit(const uint32_t* src, int sw, int sh, int x, int y) {
        Sync();
        int cx0 = std::max(0, -x), cy0 = std::max(0, -y);
        int cx1 = std::min(sw, w_ - x), cy1 = std::min(sh, h_ - y);
        if (cx0 >= cx1) return;
        for (int sy = cy0; sy < cy1; sy++)
            std::copy(src + (size_t)sy * sw + cx0, src + (size_t)sy * sw + cx1, Row(y + sy) + x + cx0);
    }

private:
    HDC dc_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HGDIOBJ old_ = nullptr;
    uint32_t* px_ = nullptr;
    int w_ = 0, h_ = 0;
    bool gdiPending_ = false;
};

// Text width/height on any DC (the canvas DC in practice).
inline SIZE TextSize(HDC dc, HFONT f, const std::wstring& s) {
    HGDIOBJ o = SelectObject(dc, f);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    SelectObject(dc, o);
    return sz;
}

}  // namespace edit::ui
