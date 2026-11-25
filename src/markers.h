// Timeline markers (like Steam game recording's player-added markers): points in a file you drop with a key, give
// an optional label, and jump between. Kept per file in <FastPlay data folder>\markers\<hash>.txt, the hash being
// the one Edit mode's autosave uses, so they come back whenever the file is opened again.
#pragma once
#include "util.h"

struct Marker {
    double t = 0;
    std::wstring label;  // optional
};

class MarkerList {
public:
    // Loads the markers saved for this file (none for an empty path). Test copies only keep markers in memory,
    // unless FASTPLAY_MARKER_DIR names a folder to use instead of the real one.
    void Load(const std::wstring& mediaPath);
    void Clear();  // forget everything (file closed); doesn't touch the saved file

    const std::vector<Marker>& Items() const { return items_; }
    bool Empty() const { return items_.empty(); }
    int Count() const { return (int)items_.size(); }

    // Adds a marker at t and saves. Returns its index, or -1 when one already sits within minGap seconds (then
    // *existing is set to that one).
    int Add(double t, double minGap, int* existing);
    void Remove(int i);
    void Rename(int i, const std::wstring& label);
    void RemoveAll();
    int Nearest(double t, double tol) const;  // closest marker within tol seconds, -1 if none
    std::wstring Describe(int i) const;       // "Marker 2 of 5 (01:23): label"

private:
    void Save() const;
    std::wstring file_;  // empty: not saved (no file open, or a test copy)
    std::vector<Marker> items_;
};
