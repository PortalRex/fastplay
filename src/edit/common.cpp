// Edit mode shared helpers: Models.cs (StreamInfo/MediaInfo/Settings) and Util.cs.
#include "common.h"
#include "../settings.h"
#include <algorithm>
#include <charconv>
#include <cmath>

namespace edit {

// ---------------------------------------------------------------- internal helpers shared by the edit .cpp files
// (declared again where used: segio.cpp, media.cpp)
namespace detail {

// .NET char.IsWhiteSpace.
bool IsSpace(wchar_t c) {
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

std::wstring TrimW(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && IsSpace(s[a])) a++;
    while (b > a && IsSpace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

// Invariant double parse like .NET's NumberStyles.Float: surrounding white space, a leading sign, decimals and an
// exponent; the whole text must be consumed. Finite values only. Locale independent (std::from_chars).
bool ParseDouble(const std::string& text, double* out) {
    size_t a = 0, b = text.size();
    while (a < b && (text[a] == ' ' || (text[a] >= 0x09 && text[a] <= 0x0D))) a++;
    while (b > a && (text[b - 1] == ' ' || (text[b - 1] >= 0x09 && text[b - 1] <= 0x0D))) b--;
    if (a < b && text[a] == '+') a++;
    if (a >= b) return false;
    // from_chars also takes "inf"/"nan"; .NET's Float style wants digits here.
    char c0 = text[a] == '-' && a + 1 < b ? text[a + 1] : text[a];
    if (!((c0 >= '0' && c0 <= '9') || c0 == '.')) return false;
    double v = 0;
    auto r = std::from_chars(text.data() + a, text.data() + b, v, std::chars_format::general);
    if (r.ec != std::errc() || r.ptr != text.data() + b || !std::isfinite(v)) return false;
    *out = v;
    return true;
}

bool ParseDoubleW(const std::wstring& text, double* out) {
    std::wstring t = TrimW(text);
    std::string n;
    n.reserve(t.size());
    for (wchar_t c : t) {
        if (c > 127) return false;
        n += (char)c;
    }
    return ParseDouble(n, out);
}

// Fixed-point with up to 'decimals' digits, trailing zeros removed (.NET "0.###").
std::string Fixed(double v, int decimals) {
    if (!std::isfinite(v)) return "0";
    char buf[400];
    auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, decimals);
    if (r.ec != std::errc()) return "0";
    std::string s(buf, r.ptr);
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

}  // namespace detail

using detail::Fixed;

// ---------------------------------------------------------------- Models.cs

std::wstring StreamInfo::Details() const {
    std::wstring r;
    if (type == "video") {
        r = std::to_wstring(width) + L"x" + std::to_wstring(height) + L" " + util::Wide(Fixed(fps, 3)) + L"fps " +
            util::Wide(pixFmt);
        if (bitRate > 0) r += L" " + std::to_wstring(bitRate / 1000) + L"kb/s";
        if (rotation != 0) r += L" rot " + std::to_wstring(rotation) + L"°";
        if (isCoverArt) r += L" (cover)";
    } else if (type == "audio") {
        r = std::to_wstring(sampleRate) + L"Hz " + std::to_wstring(channels) + L"ch";
        if (bitRate > 0) r += L" " + std::to_wstring(bitRate / 1000) + L"kb/s";
    }
    if (!title.empty()) r += L" \"" + title + L"\"";
    return r;
}

const StreamInfo* MediaInfo::Video() const {
    for (const auto& s : streams)
        if (s.type == "video" && !s.isCoverArt) return &s;
    return nullptr;
}

const StreamInfo* MediaInfo::Audio() const {
    for (const auto& s : streams)
        if (s.type == "audio") return &s;
    return nullptr;
}

double MediaInfo::MediaDuration() const {
    double d = 0;
    for (const auto& s : streams)
        if ((s.type == "video" || s.type == "audio") && !s.isCoverArt) d = std::max(d, s.duration);
    return d > 0 ? d : duration;
}

// ---------------------------------------------------------------- EditSettings ([edit] in fastplay.ini)

namespace {
const wchar_t kSec[] = L"edit";

bool GetBool(const Ini& ini, const wchar_t* key, bool def) { return ini.GetInt(kSec, key, def ? 1 : 0) != 0; }
void SetBool(Ini& ini, const wchar_t* key, bool v) { ini.Set(kSec, key, v ? L"1" : L"0"); }
void SetNum(Ini& ini, const wchar_t* key, double v) { ini.Set(kSec, key, Num(v)); }
}  // namespace

void EditSettings::Load(const Ini& ini) {
    outDir = ini.Get(kSec, L"out_dir", outDir);
    format = ini.Get(kSec, L"format", format);
    merge = GetBool(ini, L"merge", merge);
    cutMode = ini.Get(kSec, L"cut_mode", cutMode);
    nameTemplate = ini.Get(kSec, L"name_template", nameTemplate);
    mergeTemplate = ini.Get(kSec, L"merge_template", mergeTemplate);
    preserveMetadata = GetBool(ini, L"preserve_metadata", preserveMetadata);
    preserveChapters = GetBool(ini, L"preserve_chapters", preserveChapters);
    chaptersFromSegments = GetBool(ini, L"chapters_from_segments", chaptersFromSegments);
    avoidNegativeTs = GetBool(ini, L"avoid_negative_ts", avoidNegativeTs);
    fastStart = GetBool(ini, L"fast_start", fastStart);
    confirmExport = GetBool(ini, L"confirm_export", confirmExport);
    verifyOutputs = GetBool(ini, L"verify_outputs", verifyOutputs);
    discordReady = GetBool(ini, L"discord_ready", discordReady);
    discordSizeMB = ini.GetInt(kSec, L"discord_size_mb", discordSizeMB);
    copyToClipboard = GetBool(ini, L"copy_to_clipboard", copyToClipboard);
    sameDiskTipAnswered = GetBool(ini, L"same_disk_tip_answered", sameDiskTipAnswered);
    batchMode = ini.Get(kSec, L"batch_mode", batchMode);
    batchTrimStart = ini.GetDouble(kSec, L"batch_trim_start", batchTrimStart);
    batchTrimEnd = ini.GetDouble(kSec, L"batch_trim_end", batchTrimEnd);
    snapshotFormat = ini.Get(kSec, L"snapshot_format", snapshotFormat);
    showThumbs = GetBool(ini, L"show_thumbs", showThumbs);
    showWave = GetBool(ini, L"show_wave", showWave);
    showKeyframes = GetBool(ini, L"show_keyframes", showKeyframes);
    autoSaveProject = GetBool(ini, L"auto_save_project", autoSaveProject);
    sidePanelWidth = ini.GetInt(kSec, L"side_panel_width", sidePanelWidth);
    seekSmall = ini.GetDouble(kSec, L"seek_small", seekSmall);
    seekLarge = ini.GetDouble(kSec, L"seek_large", seekLarge);
    frameJump = ini.GetInt(kSec, L"frame_jump", frameJump);
    nudgeFrames = ini.GetInt(kSec, L"nudge_frames", nudgeFrames);

    // A hand-edited ini must not produce values the UI can't use.
    if (cutMode != L"keyframe" && cutMode != L"exact" && cutMode != L"smart") cutMode = L"keyframe";
    if (batchMode != L"same" && batchMode != L"saved" && batchMode != L"trim") batchMode = L"same";
    if (format.empty()) format = L"same";
    if (snapshotFormat.empty()) snapshotFormat = L"jpg";
    if (discordSizeMB < 0) discordSizeMB = 0;
    if (!(seekSmall > 0)) seekSmall = 1;
    if (!(seekLarge > 0)) seekLarge = 10;
    if (frameJump < 1) frameJump = 10;
    if (nudgeFrames < 1) nudgeFrames = 1;
    if (!(batchTrimStart >= 0)) batchTrimStart = 0;
    if (!(batchTrimEnd >= 0)) batchTrimEnd = 0;
}

void EditSettings::Save(Ini& ini) const {
    ini.Set(kSec, L"out_dir", outDir);
    ini.Set(kSec, L"format", format);
    SetBool(ini, L"merge", merge);
    ini.Set(kSec, L"cut_mode", cutMode);
    ini.Set(kSec, L"name_template", nameTemplate);
    ini.Set(kSec, L"merge_template", mergeTemplate);
    SetBool(ini, L"preserve_metadata", preserveMetadata);
    SetBool(ini, L"preserve_chapters", preserveChapters);
    SetBool(ini, L"chapters_from_segments", chaptersFromSegments);
    SetBool(ini, L"avoid_negative_ts", avoidNegativeTs);
    SetBool(ini, L"fast_start", fastStart);
    SetBool(ini, L"confirm_export", confirmExport);
    SetBool(ini, L"verify_outputs", verifyOutputs);
    SetBool(ini, L"discord_ready", discordReady);
    ini.Set(kSec, L"discord_size_mb", std::to_wstring(discordSizeMB));
    SetBool(ini, L"copy_to_clipboard", copyToClipboard);
    SetBool(ini, L"same_disk_tip_answered", sameDiskTipAnswered);
    ini.Set(kSec, L"batch_mode", batchMode);
    SetNum(ini, L"batch_trim_start", batchTrimStart);
    SetNum(ini, L"batch_trim_end", batchTrimEnd);
    ini.Set(kSec, L"snapshot_format", snapshotFormat);
    SetBool(ini, L"show_thumbs", showThumbs);
    SetBool(ini, L"show_wave", showWave);
    SetBool(ini, L"show_keyframes", showKeyframes);
    SetBool(ini, L"auto_save_project", autoSaveProject);
    ini.Set(kSec, L"side_panel_width", std::to_wstring(sidePanelWidth));
    SetNum(ini, L"seek_small", seekSmall);
    SetNum(ini, L"seek_large", seekLarge);
    ini.Set(kSec, L"frame_jump", std::to_wstring(frameJump));
    ini.Set(kSec, L"nudge_frames", std::to_wstring(nudgeFrames));
}

// ---------------------------------------------------------------- Util.cs

std::wstring Fmt(double seconds, bool millis) {
    if (!std::isfinite(seconds)) seconds = 0;
    bool neg = seconds < 0;
    long long total = (long long)std::nearbyint(std::fabs(seconds) * 1000);  // Math.Round: to even
    long long h = total / 3600000, m = total / 60000 % 60, s = total / 1000 % 60, ms = total % 1000;
    wchar_t buf[64];
    if (millis) swprintf(buf, 64, L"%02lld:%02lld:%02lld.%03lld", h, m, s, ms);
    else swprintf(buf, 64, L"%02lld:%02lld:%02lld", h, m, s);
    return neg ? L"-" + std::wstring(buf) : std::wstring(buf);
}

std::wstring Num(double v) { return util::Wide(Fixed(v, 6)); }

bool TryParseTime(const std::wstring& text, double* seconds) {
    if (seconds) *seconds = 0;
    std::wstring t = detail::TrimW(text);
    std::replace(t.begin(), t.end(), L',', L'.');
    if (t.empty()) return false;
    bool neg = t[0] == L'-';
    if (neg) t.erase(0, 1);
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (;;) {
        size_t c = t.find(L':', start);
        parts.push_back(t.substr(start, c == std::wstring::npos ? std::wstring::npos : c - start));
        if (c == std::wstring::npos) break;
        start = c + 1;
    }
    if (parts.size() > 3) return false;
    double total = 0;
    for (const auto& p : parts) {
        double v;
        if (!detail::ParseDoubleW(p, &v) || v < 0) return false;
        total = total * 60 + v;
    }
    if (seconds) *seconds = neg ? -total : total;
    return true;
}

std::wstring SafeFileName(const std::wstring& s) {
    // Path.GetInvalidFileNameChars() on Windows.
    std::wstring r = s;
    for (auto& c : r)
        if (c < 32 || wcschr(L"\"<>|:*?\\/", c)) c = L'_';
    return detail::TrimW(r);
}

namespace {
bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
size_t NameStart(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/:");
    return i == std::wstring::npos ? 0 : i + 1;
}
}  // namespace

std::wstring UniquePath(const std::wstring& path) {
    if (!FileExists(path)) return path;
    size_t ns = NameStart(path);
    std::wstring dir = path.substr(0, ns), ext = Ext(path), name = BaseName(path);
    if (!ext.empty()) ext = path.substr(path.size() - ext.size());  // keep the original case
    for (int i = 2;; i++) {
        std::wstring p = dir + name + L" (" + std::to_wstring(i) + L")" + ext;
        if (!FileExists(p)) return p;
    }
}

void TryDelete(const std::wstring& path) { DeleteFileW(path.c_str()); }

std::wstring Ext(const std::wstring& path) {
    size_t ns = NameStart(path), dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || dot < ns || dot == path.size() - 1) return {};
    return util::Lower(path.substr(dot));
}

std::wstring BaseName(const std::wstring& path) {
    std::wstring name = path.substr(NameStart(path));
    size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}

const std::vector<uint32_t>& Palette() {
    static const std::vector<uint32_t> colors = {
        RGB(0x3b, 0x82, 0xf6), RGB(0x22, 0xc5, 0x5e), RGB(0xf5, 0x9e, 0x0b), RGB(0xec, 0x48, 0x99),
        RGB(0x8b, 0x5c, 0xf6), RGB(0x14, 0xb8, 0xa6), RGB(0xef, 0x44, 0x44), RGB(0x84, 0xcc, 0x16),
    };
    return colors;
}

}  // namespace edit
