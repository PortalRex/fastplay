#include "markers.h"
#include "edit/segio.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

// File format: UTF-8 text, a comment line naming the media file, then one marker per line: "<seconds>\t<label>".
static std::wstring MarkerFile(const std::wstring& mediaPath) {
    if (mediaPath.empty()) return L"";
    std::wstring dir;
    if (util::TestMode()) {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"FASTPLAY_MARKER_DIR", buf, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return L"";
        dir = buf;
    } else {
        dir = util::DataDir() + L"\\markers";
    }
    return dir + L"\\" + edit::segio::MediaKey(mediaPath) + L".txt";
}

static std::wstring CleanLabel(std::wstring s) {
    for (wchar_t& c : s)
        if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return util::Trim(s);
}

void MarkerList::Load(const std::wstring& mediaPath) {
    items_.clear();
    file_ = MarkerFile(mediaPath);
    if (file_.empty()) return;
    std::ifstream in(file_, std::ios::binary);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t tab = line.find('\t');
        char* end = nullptr;
        double t = strtod(line.c_str(), &end);
        if (end == line.c_str() || !std::isfinite(t) || t < 0) continue;
        items_.push_back({t, tab == std::string::npos ? L"" : CleanLabel(util::Wide(line.substr(tab + 1)))});
    }
    std::sort(items_.begin(), items_.end(), [](const Marker& a, const Marker& b) { return a.t < b.t; });
}

void MarkerList::Clear() {
    items_.clear();
    file_.clear();
}

void MarkerList::Save() const {
    if (file_.empty()) return;
    if (items_.empty()) {
        DeleteFileW(file_.c_str());
        return;
    }
    CreateDirectoryW(util::DirName(file_).c_str(), nullptr);
    std::ostringstream out;
    out << "# FastPlay markers: seconds<TAB>label\n";
    char num[32];
    for (const Marker& m : items_) {
        snprintf(num, sizeof num, "%.3f", m.t);
        out << num << '\t' << util::Utf8(m.label) << '\n';
    }
    // Write a temp file and swap it in, so a crash mid-write never leaves half a list.
    std::wstring tmp = file_ + L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f << out.str();
        if (!f) return;
    }
    MoveFileExW(tmp.c_str(), file_.c_str(), MOVEFILE_REPLACE_EXISTING);
}

int MarkerList::Add(double t, double minGap, int* existing) {
    int close = Nearest(t, minGap);
    if (close >= 0) {
        if (existing) *existing = close;
        return -1;
    }
    auto it = std::upper_bound(items_.begin(), items_.end(), t, [](double v, const Marker& m) { return v < m.t; });
    int i = (int)(it - items_.begin());
    items_.insert(it, {std::max(0.0, t), L""});
    Save();
    return i;
}

void MarkerList::Remove(int i) {
    if (i < 0 || i >= Count()) return;
    items_.erase(items_.begin() + i);
    Save();
}

void MarkerList::Rename(int i, const std::wstring& label) {
    if (i < 0 || i >= Count()) return;
    items_[i].label = CleanLabel(label);
    Save();
}

void MarkerList::RemoveAll() {
    items_.clear();
    Save();
}

int MarkerList::Nearest(double t, double tol) const {
    int best = -1;
    double bestD = tol;
    for (int i = 0; i < Count(); i++) {
        double d = std::fabs(items_[i].t - t);
        if (d <= bestD) {
            best = i;
            bestD = d;
        }
    }
    return best;
}

std::wstring MarkerList::Describe(int i) const {
    if (i < 0 || i >= Count()) return L"";
    std::wstring s = L"Marker " + std::to_wstring(i + 1) + L" of " + std::to_wstring(Count()) + L" (" +
                     util::FormatTime(items_[i].t) + L")";
    if (!items_[i].label.empty()) s += L": " + items_[i].label;
    return s;
}
