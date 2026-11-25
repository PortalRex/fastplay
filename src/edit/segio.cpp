// SegmentIO.cs (+ Util.cs Json5): LosslessCut .llc projects, autosave location, segment list import/export, and the
// tolerant JSON reader shared by edit mode.
#include "segio.h"
#include <shlobj.h>
#include <algorithm>
#include <cmath>
#include <charconv>

namespace edit {
namespace detail {  // common.cpp
bool IsSpace(wchar_t c);
std::wstring TrimW(const std::wstring& s);
bool ParseDouble(const std::string& text, double* out);
bool ParseDoubleW(const std::wstring& text, double* out);
}  // namespace detail
}  // namespace edit

namespace edit::segio {

using detail::IsSpace;
using detail::TrimW;

// ================================================================ JSON

const Json* Json::Get(const char* key) const {
    if (kind != Object) return nullptr;
    for (const auto& kv : obj)
        if (kv.first == key) return kv.second.get();
    return nullptr;
}

std::string Json::Str(const char* key) const {
    const Json* v = Get(key);
    if (!v) return {};
    switch (v->kind) {
        case String:
        case Number: return v->s;
        case Bool: return v->b ? "true" : "false";
        default: return {};
    }
}

double Json::Num(const char* key) const {
    const Json* v = Get(key);
    if (!v) return 0;
    if (v->kind == Number) return v->n;
    double d = 0;
    if (v->kind == String && detail::ParseDouble(v->s, &d)) return d;
    return 0;
}

namespace {

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | cp >> 6);
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | cp >> 12);
        out += (char)(0x80 | (cp >> 6 & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | cp >> 18);
        out += (char)(0x80 | (cp >> 12 & 0x3F));
        out += (char)(0x80 | (cp >> 6 & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// Util.cs Json5: unquoted keys, single quotes, comments, trailing commas, Infinity/NaN. Also plain JSON (ffprobe).
class Json5 {
public:
    explicit Json5(const std::string& s) : s_(s) {}

    JsonPtr Parse() {
        if (s_.compare(0, 3, "\xEF\xBB\xBF") == 0) i_ = 3;
        return Value(0);
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    [[noreturn]] void Fail(const char* what) const {
        throw EditError(L"Invalid JSON: " + util::Wide(what) + L" at " + std::to_wstring(i_));
    }
    char Peek() const {
        if (i_ >= s_.size()) Fail("unexpected end");
        return s_[i_];
    }
    static bool IdentChar(char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '$' ||
               (unsigned char)c >= 0x80;  // non-ASCII letters (UTF-8)
    }

    void Ws() {
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == ' ' || (c >= 0x09 && c <= 0x0D)) {
                i_++;
            } else if ((unsigned char)c == 0xC2 && i_ + 1 < s_.size() && (unsigned char)s_[i_ + 1] == 0xA0) {
                i_ += 2;  // no-break space
            } else if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '/') {
                while (i_ < s_.size() && s_[i_] != '\n') i_++;
            } else if (c == '/' && i_ + 1 < s_.size() && s_[i_ + 1] == '*') {
                size_t e = s_.find("*/", i_ + 2);
                i_ = e == std::string::npos ? s_.size() : e + 2;
            } else {
                break;
            }
        }
    }

    void Expect(char c) {
        if (Peek() != c) {
            char msg[] = "expected 'x'";
            msg[10] = c;
            Fail(msg);
        }
        i_++;
    }

    JsonPtr Value(int depth) {
        if (depth > 256) Fail("nesting too deep");
        Ws();
        char c = Peek();
        if (c == '{') return Obj(depth);
        if (c == '[') return Arr(depth);
        auto v = std::make_shared<Json>();
        if (c == '"' || c == '\'') {
            v->kind = Json::String;
            v->s = Str();
            return v;
        }
        if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.') {
            v->kind = Json::Number;
            v->n = Number(&v->s);
            return v;
        }
        std::string id = Ident();
        if (id == "true" || id == "false") {
            v->kind = Json::Bool;
            v->b = id == "true";
        } else if (id == "Infinity" || id == "NaN") {
            v->kind = Json::Number;
            v->n = id == "NaN" ? NAN : INFINITY;
            v->s = id;
        } else if (id != "null") {
            Fail(("unexpected token " + id.substr(0, 40)).c_str());
        }
        return v;
    }

    JsonPtr Obj(int depth) {
        i_++;
        auto v = std::make_shared<Json>();
        v->kind = Json::Object;
        for (;;) {
            Ws();
            if (Peek() == '}') { i_++; return v; }
            std::string key = Peek() == '"' || Peek() == '\'' ? Str() : Ident();
            Ws();
            Expect(':');
            JsonPtr val = Value(depth + 1);
            // Duplicate keys: the last value wins, at the first key's position (like a Dictionary).
            auto it = std::find_if(v->obj.begin(), v->obj.end(), [&](const auto& kv) { return kv.first == key; });
            if (it != v->obj.end()) it->second = std::move(val);
            else v->obj.emplace_back(std::move(key), std::move(val));
            Ws();
            if (Peek() == ',') { i_++; continue; }
            Expect('}');
            return v;
        }
    }

    JsonPtr Arr(int depth) {
        i_++;
        auto v = std::make_shared<Json>();
        v->kind = Json::Array;
        for (;;) {
            Ws();
            if (Peek() == ']') { i_++; return v; }
            v->arr.push_back(Value(depth + 1));
            Ws();
            if (Peek() == ',') { i_++; continue; }
            Expect(']');
            return v;
        }
    }

    uint32_t Hex4() {
        if (i_ + 4 > s_.size()) Fail("bad \\u escape");
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else Fail("bad \\u escape");
        }
        return v;
    }

    std::string Str() {
        char q = s_[i_++];
        std::string out;
        for (;;) {
            char c = Peek();
            i_++;
            if (c == q) return out;
            if (c != '\\') { out += c; continue; }
            char e = Peek();
            i_++;
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case '\n': break;  // line continuation
                case '\r':
                    if (i_ < s_.size() && s_[i_] == '\n') i_++;
                    break;
                case 'u': {
                    uint32_t cp = Hex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 6 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        size_t save = i_;
                        i_ += 2;
                        uint32_t lo = Hex4();
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else i_ = save;
                    }
                    if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;  // lone surrogate
                    AppendUtf8(out, cp);
                    break;
                }
                default: out += e; break;
            }
        }
    }

    double Number(std::string* text) {
        size_t st = i_;
        while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || (s_[i_] >= 'a' && s_[i_] <= 'z') ||
                                  (s_[i_] >= 'A' && s_[i_] <= 'Z') || s_[i_] == '+' || s_[i_] == '-' || s_[i_] == '.'))
            i_++;
        *text = s_.substr(st, i_ - st);
        const std::string& t = *text;
        bool neg = !t.empty() && t[0] == '-';
        auto endsWith = [&](const char* suf) {
            size_t n = strlen(suf);
            return t.size() >= n && t.compare(t.size() - n, n, suf) == 0;
        };
        if (endsWith("Infinity")) return neg ? -INFINITY : INFINITY;
        if (endsWith("NaN")) return NAN;
        size_t body = (!t.empty() && (t[0] == '-' || t[0] == '+')) ? 1 : 0;
        if (t.size() > body + 2 && t[body] == '0' && (t[body + 1] == 'x' || t[body + 1] == 'X')) {  // JSON5 hex
            uint64_t v = 0;
            auto r = std::from_chars(t.data() + body + 2, t.data() + t.size(), v, 16);
            if (r.ec != std::errc() || r.ptr != t.data() + t.size()) Fail("bad number");
            return neg ? -(double)v : (double)v;
        }
        double v = 0;
        if (!detail::ParseDouble(t, &v)) Fail("bad number");
        return v;
    }

    std::string Ident() {
        size_t st = i_;
        while (i_ < s_.size() && IdentChar(s_[i_])) i_++;
        if (st == i_) {
            if (i_ >= s_.size()) Fail("unexpected end");
            char msg[] = "unexpected 'x'";
            msg[12] = s_[i_];
            Fail(msg);
        }
        return s_.substr(st, i_ - st);
    }
};

// JSON number text: shortest round-trip (like Utf8JsonWriter), no "-0".
std::string JsonNumber(double v) {
    if (!std::isfinite(v)) return "0";
    if (v == 0) return "0";
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v);
    return std::string(buf, r.ptr);
}

double Round6(double v) {
    if (!std::isfinite(v) || std::fabs(v) >= 1e15) return v;
    return std::nearbyint(v * 1e6) / 1e6;
}

}  // namespace

JsonPtr ParseJson(const std::string& text) { return Json5(text).Parse(); }

std::string JsonQuote(const std::string& utf8) {
    std::string r;
    r.reserve(utf8.size() + 2);
    r += '"';
    for (char c : utf8) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            case '\b': r += "\\b"; break;
            case '\f': r += "\\f"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof b, "\\u%04x", (unsigned char)c);
                    r += b;
                } else {
                    r += c;
                }
        }
    }
    r += '"';
    return r;
}

// ================================================================ project (.llc)

std::string WriteProject(const std::wstring& mediaPath, const std::vector<Segment>& segs) {
    // Same layout as FastCut's Utf8JsonWriter (indented by two, CRLF), non-ASCII kept as UTF-8.
    const char* nl = "\r\n";
    std::string o = "{";
    o += nl;
    o += "  \"version\": 1,";
    o += nl;
    o += "  \"mediaFileName\": " + JsonQuote(util::Utf8(util::FileName(mediaPath))) + ",";
    o += nl;
    o += "  \"cutSegments\": [";
    for (size_t i = 0; i < segs.size(); i++) {
        const Segment& s = segs[i];
        o += i ? "," : "";
        o += nl;
        o += "    {";
        o += nl;
        o += "      \"start\": " + JsonNumber(Round6(s.start)) + "," + nl;
        o += "      \"end\": " + JsonNumber(Round6(s.end)) + "," + nl;
        o += "      \"name\": " + JsonQuote(util::Utf8(s.name)) + "," + nl;
        if (!s.enabled) o += std::string("      \"selected\": false,") + nl;
        auto tags = ParseTags(s.tags);
        if (tags.empty()) {
            o += "      \"tags\": {}";
        } else {
            o += "      \"tags\": {";
            for (size_t k = 0; k < tags.size(); k++) {
                o += k ? "," : "";
                o += nl;
                o += "        " + JsonQuote(util::Utf8(tags[k].first)) + ": " + JsonQuote(util::Utf8(tags[k].second));
            }
            o += nl;
            o += "      }";
        }
        o += nl;
        o += "    }";
    }
    if (!segs.empty()) {
        o += nl;
        o += "  ";
    }
    o += "]";
    o += nl;
    o += "}";
    return o;
}

std::vector<Segment> ReadProject(const std::string& text, double duration) {
    JsonPtr root = ParseJson(text);
    if (!root || root->kind != Json::Object) throw EditError(L"Not a project file");
    std::vector<Segment> list;
    const Json* arr = root->Get("cutSegments");
    if (!arr || arr->kind != Json::Array) return list;
    for (const auto& o : arr->arr) {
        if (!o || o->kind != Json::Object) continue;
        const Json* a = o->Get("start");
        const Json* b = o->Get("end");
        const Json* name = o->Get("name");
        const Json* sel = o->Get("selected");
        Segment seg;
        seg.start = a && a->kind == Json::Number ? a->n : 0;
        seg.end = b && b->kind == Json::Number ? b->n : duration;
        seg.name = name && name->kind == Json::String ? util::Wide(name->s) : L"";
        if (sel && sel->kind == Json::Bool && !sel->b) seg.enabled = false;
        if (const Json* tags = o->Get("tags"); tags && tags->kind == Json::Object) {
            std::wstring t;
            for (const auto& kv : tags->obj) {
                if (!t.empty()) t += L", ";
                std::string v;
                if (kv.second->kind == Json::String || kv.second->kind == Json::Number) v = kv.second->s;
                else if (kv.second->kind == Json::Bool) v = kv.second->b ? "True" : "False";  // .NET bool.ToString()
                t += util::Wide(kv.first) + L"=" + util::Wide(v);
            }
            seg.tags = t;
        }
        list.push_back(std::move(seg));
    }
    return list;
}

std::vector<std::pair<std::wstring, std::wstring>> ParseTags(const std::wstring& tags) {
    std::vector<std::pair<std::wstring, std::wstring>> r;
    size_t start = 0;
    while (start <= tags.size()) {
        size_t c = tags.find(L',', start);
        if (c == std::wstring::npos) c = tags.size();
        std::wstring part = TrimW(tags.substr(start, c - start));
        start = c + 1;
        if (part.empty()) continue;
        size_t eq = part.find(L'=');
        if (eq != std::wstring::npos && eq > 0) r.push_back({TrimW(part.substr(0, eq)), TrimW(part.substr(eq + 1))});
        else r.push_back({part, L""});
    }
    return r;
}

// ================================================================ autosave

namespace {

// SHA-1 (FIPS 180-4), only for FastCut-compatible autosave names; avoids pulling in bcrypt/CryptoAPI.
void Sha1(const std::string& data, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m = data;
    uint64_t bits = (uint64_t)data.size() * 8;
    m += (char)0x80;
    while (m.size() % 64 != 56) m += '\0';
    for (int i = 7; i >= 0; i--) m += (char)(bits >> (i * 8));
    auto rol = [](uint32_t x, int n) { return x << n | x >> (32 - n); };
    for (size_t off = 0; off < m.size(); off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) {
            const auto* p = (const uint8_t*)m.data() + off + i * 4;
            w[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        }
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 4; k++) out[i * 4 + k] = (uint8_t)(h[i] >> (24 - k * 8));
}

// string.ToLowerInvariant (FastCut runs with InvariantGlobalization: simple Unicode case mapping).
std::wstring LowerInvariant(const std::wstring& s) {
    if (s.empty()) return s;
    std::wstring r(s.size(), L'\0');
    int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.data(), (int)s.size(), r.data(), (int)r.size(),
                          nullptr, nullptr, 0);
    if (n <= 0) return util::Lower(s);
    r.resize(n);
    return r;
}

// Models.cs Settings.Dir: FASTCUT_HOME, else %APPDATA%\FastCut.
std::wstring FastCutDir() {
    DWORD n = GetEnvironmentVariableW(L"FASTCUT_HOME", nullptr, 0);
    if (n > 1) {
        std::wstring home(n, L'\0');
        home.resize(GetEnvironmentVariableW(L"FASTCUT_HOME", home.data(), n));
        if (!home.empty()) return home;
    }
    std::wstring appData;
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DONT_VERIFY, nullptr, &p)) && p) appData = p;
    CoTaskMemFree(p);
    if (appData.empty()) {
        wchar_t buf[MAX_PATH * 2];
        DWORD got = GetEnvironmentVariableW(L"APPDATA", buf, (DWORD)std::size(buf));
        if (got && got < std::size(buf)) appData.assign(buf, got);
    }
    return appData + L"\\FastCut";
}

// Path.Combine for two parts.
std::wstring Combine(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    wchar_t last = a.back();
    return (last == L'\\' || last == L'/') ? a + b : a + L"\\" + b;
}

}  // namespace

namespace {
// SHA-1 of the UTF-8 lower-cased full path, first 16 upper-case hex digits (FastCut's SegmentIO.AutosavePath).
std::wstring AutosaveName(const std::wstring& mediaPath) { return MediaKey(mediaPath) + L".llc"; }
}  // namespace

std::wstring MediaKey(const std::wstring& mediaPath) {
    uint8_t digest[20];
    Sha1(util::Utf8(LowerInvariant(util::FullPath(mediaPath))), digest);
    wchar_t hex[17];
    for (int i = 0; i < 8; i++) swprintf(hex + i * 2, 3, L"%02X", digest[i]);
    return std::wstring(hex, 16);
}

std::wstring AutosavePath(const std::wstring& mediaPath) {
    return Combine(Combine(util::DataDir(), L"projects"), AutosaveName(mediaPath));
}

std::wstring LegacyAutosavePath(const std::wstring& mediaPath) {
    return Combine(Combine(FastCutDir(), L"projects"), AutosaveName(mediaPath));
}

std::wstring FindAutosave(const std::wstring& mediaPath) {
    for (const std::wstring& p : {AutosavePath(mediaPath), LegacyAutosavePath(mediaPath)})
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    return L"";
}

// ================================================================ import / export

const wchar_t kImportFilter[] =
    L"All supported\0*.llc;*.csv;*.txt;*.cue;*.srt;*.edl\0LosslessCut project (*.llc)\0*.llc\0"
    L"CSV (start,end,label)\0*.csv\0Timestamps / YouTube chapters (*.txt)\0*.txt\0CUE sheet (*.cue)\0*.cue\0"
    L"SubRip (*.srt)\0*.srt\0MPlayer EDL (*.edl)\0*.edl\0";

const wchar_t kExportFilter[] =
    L"LosslessCut project (*.llc)\0*.llc\0CSV (start,end,label)\0*.csv\0CSV with human timestamps\0*.csv\0"
    L"YouTube chapters (*.txt)\0*.txt\0CUE sheet (*.cue)\0*.cue\0SubRip (*.srt)\0*.srt\0MPlayer EDL (*.edl)\0*.edl\0"
    L"FFmpeg chapters metadata (*.txt)\0*.txt\0";

namespace {

bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> r;
    size_t start = 0;
    for (;;) {
        size_t c = s.find(sep, start);
        r.push_back(s.substr(start, c == std::wstring::npos ? std::wstring::npos : c - start));
        if (c == std::wstring::npos) return r;
        start = c + 1;
    }
}

std::wstring TrimEndCR(std::wstring s) {
    while (!s.empty() && s.back() == L'\r') s.pop_back();
    return s;
}

bool StartsWithI(const std::wstring& s, const wchar_t* prefix) {
    size_t n = wcslen(prefix);
    return s.size() >= n && _wcsnicmp(s.c_str(), prefix, n) == 0;
}

// int.TryParse: optional white space and sign around decimal digits.
bool ParseInt(const std::wstring& text, long long* out) {
    std::wstring t = TrimW(text);
    if (t.empty()) return false;
    size_t i = (t[0] == L'-' || t[0] == L'+') ? 1 : 0;
    if (i == t.size()) return false;
    for (size_t k = i; k < t.size(); k++)
        if (!IsDigit(t[k])) return false;
    *out = _wtoi64(t.c_str());
    return true;
}

// File.ReadAllText: BOM-detected UTF-8 / UTF-16, UTF-8 by default.
std::wstring ReadTextFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw EditError(L"Could not open " + path);
    LARGE_INTEGER size = {};
    GetFileSizeEx(h, &size);
    if (size.QuadPart > (256 << 20)) {
        CloseHandle(h);
        throw EditError(L"File too large: " + path);
    }
    std::string data((size_t)size.QuadPart, '\0');
    DWORD got = 0;
    BOOL ok = data.empty() || ReadFile(h, data.data(), (DWORD)data.size(), &got, nullptr);
    CloseHandle(h);
    if (!ok) throw EditError(L"Could not read " + path);
    data.resize(got);
    auto u = (const uint8_t*)data.data();
    if (data.size() >= 2 && ((u[0] == 0xFF && u[1] == 0xFE) || (u[0] == 0xFE && u[1] == 0xFF))) {
        bool be = u[0] == 0xFE;
        std::wstring w;
        for (size_t i = 2; i + 1 < data.size(); i += 2) w += (wchar_t)(be ? (u[i] << 8 | u[i + 1]) : (u[i] | u[i + 1] << 8));
        return w;
    }
    size_t skip = (data.size() >= 3 && u[0] == 0xEF && u[1] == 0xBB && u[2] == 0xBF) ? 3 : 0;
    if (data.size() == skip) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, data.data() + skip, (int)(data.size() - skip), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data.data() + skip, (int)(data.size() - skip), w.data(), n);
    return w;
}

// File.WriteAllText: UTF-8 without BOM.
void WriteTextFile(const std::wstring& path, const std::string& utf8) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw EditError(L"Could not write " + path);
    DWORD put = 0;
    BOOL ok = utf8.empty() || WriteFile(h, utf8.data(), (DWORD)utf8.size(), &put, nullptr);
    CloseHandle(h);
    if (!ok || put != utf8.size()) throw EditError(L"Could not write " + path);
}

std::wstring CsvField(const std::wstring& v) {
    if (v.find_first_of(L",\"\n") == std::wstring::npos) return v;
    std::wstring r = L"\"";
    for (wchar_t c : v) {
        if (c == L'"') r += L'"';
        r += c;
    }
    return r + L"\"";
}

std::vector<std::wstring> CsvSplit(const std::wstring& line) {
    std::vector<std::wstring> res;
    std::wstring cur;
    bool q = false;
    for (size_t i = 0; i < line.size(); i++) {
        wchar_t c = line[i];
        if (q) {
            if (c == L'"' && i + 1 < line.size() && line[i + 1] == L'"') { cur += L'"'; i++; }
            else if (c == L'"') q = false;
            else cur += c;
        } else if (c == L'"') {
            q = true;
        } else if (c == L',') {
            res.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    res.push_back(cur);
    return res;
}

std::vector<Segment> ImportCsv(const std::wstring& text, double duration) {
    std::vector<Segment> list;
    for (const auto& raw : Split(text, L'\n')) {
        std::wstring line = TrimEndCR(raw);
        if (TrimW(line).empty()) continue;
        auto f = CsvSplit(line);
        double start, end;
        if (!TryParseTime(f[0], &start)) continue;  // header row
        if (!(f.size() > 1 && TryParseTime(f[1], &end))) end = duration;
        Segment s;
        s.start = start;
        s.end = end;
        s.name = f.size() > 2 ? f[2] : L"";
        list.push_back(std::move(s));
    }
    return list;
}

// Hand-written equivalent of FastCut's StampRx:
//   ^\s*(?:[-*•]\s*)?\(?(\d{1,2}(?::\d{1,2}){1,2}(?:[.,]\d+)?|\d+(?:[.,]\d+)?)\)?\s*[-–—:|]?\s*(.*)$
// Everything after the time is optional and (.*) takes the rest, so the first way the time matches is the match.
bool MatchStamp(const std::wstring& line, std::wstring* time, std::wstring* rest) {
    size_t n = line.size(), i = 0;
    auto digits = [&](size_t at, size_t max) {
        size_t k = 0;
        while (at + k < n && k < max && IsDigit(line[at + k])) k++;
        return k;
    };
    while (i < n && IsSpace(line[i])) i++;
    if (i < n && (line[i] == L'-' || line[i] == L'*' || line[i] == L'•')) {
        i++;
        while (i < n && IsSpace(line[i])) i++;
    }
    if (i < n && line[i] == L'(') i++;
    size_t t0 = i, p = std::wstring::npos;
    size_t d = digits(i, 2);
    if (d && i + d < n && line[i + d] == L':') {  // h:mm(:ss)
        size_t q = i + d;
        int groups = 0;
        while (groups < 2 && q < n && line[q] == L':') {
            size_t dd = digits(q + 1, 2);
            if (!dd) break;
            q += 1 + dd;
            groups++;
        }
        if (groups) p = q;
    }
    if (p == std::wstring::npos) {  // plain seconds
        d = digits(i, SIZE_MAX);
        if (!d) return false;
        p = i + d;
    }
    if (p + 1 < n && (line[p] == L'.' || line[p] == L',') && IsDigit(line[p + 1])) p += 1 + digits(p + 1, SIZE_MAX);
    *time = line.substr(t0, p - t0);
    if (p < n && line[p] == L')') p++;
    while (p < n && IsSpace(line[p])) p++;
    if (p < n && wcschr(L"-–—:|", line[p])) p++;
    while (p < n && IsSpace(line[p])) p++;
    *rest = line.substr(p);
    return true;
}

std::vector<Segment> FromPoints(std::vector<std::pair<double, std::wstring>>& points, double duration) {
    std::vector<Segment> list;
    for (size_t i = 0; i < points.size(); i++) {
        Segment s;
        s.start = points[i].first;
        s.end = i + 1 < points.size() ? points[i + 1].first : duration;
        s.name = points[i].second;
        list.push_back(std::move(s));
    }
    return list;
}

// "0:00 Intro" lines: each timestamp starts a segment that ends at the next one.
std::vector<Segment> ImportTimestamps(const std::wstring& text, double duration) {
    std::vector<std::pair<double, std::wstring>> points;
    for (const auto& raw : Split(text, L'\n')) {
        std::wstring time, rest;
        double t;
        if (MatchStamp(TrimEndCR(raw), &time, &rest) && TryParseTime(time, &t)) points.push_back({t, TrimW(rest)});
    }
    std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    return FromPoints(points, duration);
}

std::vector<Segment> ImportCue(const std::wstring& text, double duration) {
    std::vector<std::pair<double, std::wstring>> points;
    std::wstring title;
    for (const auto& raw : Split(text, L'\n')) {
        std::wstring line = TrimW(raw);
        if (StartsWithI(line, L"TRACK ")) {
            title.clear();
        } else if (StartsWithI(line, L"TITLE ")) {
            title = TrimW(line.substr(6));
            size_t a = title.find_first_not_of(L'"'), b = title.find_last_not_of(L'"');
            title = a == std::wstring::npos ? L"" : title.substr(a, b - a + 1);
        } else if (StartsWithI(line, L"INDEX 01 ")) {
            auto p = Split(TrimW(line.substr(9)), L':');
            long long mm, ss, ff;
            if (p.size() == 3 && ParseInt(p[0], &mm) && ParseInt(p[1], &ss) && ParseInt(p[2], &ff))
                points.push_back({mm * 60 + ss + ff / 75.0, title});
        }
    }
    return FromPoints(points, duration);
}

// Regex.Split(text, @"\n\s*\n"): blank-line separated blocks.
std::vector<std::wstring> SrtBlocks(const std::wstring& text) {
    std::vector<std::wstring> blocks;
    size_t start = 0, i = 0;
    while (i < text.size()) {
        if (text[i] == L'\n') {
            size_t j = i + 1, lastNl = std::wstring::npos;
            while (j < text.size() && IsSpace(text[j])) {
                if (text[j] == L'\n') lastNl = j;
                j++;
            }
            if (lastNl != std::wstring::npos) {
                blocks.push_back(text.substr(start, i - start));
                start = i = lastNl + 1;
                continue;
            }
        }
        i++;
    }
    blocks.push_back(text.substr(start));
    return blocks;
}

// (\d+):(\d+):(\d+)[,.](\d+)\s*-->\s*(\d+):(\d+):(\d+)[,.](\d+), leftmost match.
bool MatchSrtTimes(const std::wstring& b, double* t1, double* t2, size_t* end) {
    size_t n = b.size();
    auto num = [&](size_t& p, std::wstring* out) {
        size_t s = p;
        while (p < n && IsDigit(b[p])) p++;
        if (p == s) return false;
        *out = b.substr(s, p - s);
        return true;
    };
    auto stamp = [&](size_t& p, double* t) {
        std::wstring h, m, s, f;
        if (!num(p, &h) || p >= n || b[p] != L':') return false;
        p++;
        if (!num(p, &m) || p >= n || b[p] != L':') return false;
        p++;
        if (!num(p, &s) || p >= n || (b[p] != L',' && b[p] != L'.')) return false;
        p++;
        if (!num(p, &f)) return false;
        double frac = 0;
        detail::ParseDoubleW(L"0." + f, &frac);
        *t = _wtoi64(h.c_str()) * 3600.0 + _wtoi64(m.c_str()) * 60.0 + _wtoi64(s.c_str()) + frac;
        return true;
    };
    for (size_t i = 0; i < n; i++) {
        if (!IsDigit(b[i])) continue;
        size_t p = i;
        if (!stamp(p, t1)) continue;
        while (p < n && IsSpace(b[p])) p++;
        if (b.compare(p, 3, L"-->") != 0) continue;
        p += 3;
        while (p < n && IsSpace(b[p])) p++;
        if (!stamp(p, t2)) continue;
        *end = p;
        return true;
    }
    return false;
}

std::vector<Segment> ImportSrt(const std::wstring& text) {
    std::wstring t;
    for (wchar_t c : text)
        if (c != L'\r') t += c;
    std::vector<Segment> list;
    for (const auto& b : SrtBlocks(t)) {
        double a, e;
        size_t end;
        if (!MatchSrtTimes(b, &a, &e, &end)) continue;
        std::wstring name;
        for (const auto& l : Split(b.substr(end), L'\n')) {
            std::wstring x = TrimW(l);
            if (x.empty()) continue;
            if (!name.empty()) name += L' ';
            name += x;
        }
        Segment s;
        s.start = a;
        s.end = e;
        s.name = name;
        list.push_back(std::move(s));
    }
    return list;
}

std::vector<Segment> ImportEdl(const std::wstring& text) {
    std::vector<Segment> list;
    for (const auto& line : Split(text, L'\n')) {
        std::vector<std::wstring> p;
        size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && IsSpace(line[i])) i++;
            size_t s = i;
            while (i < line.size() && !IsSpace(line[i])) i++;
            if (i > s) p.push_back(line.substr(s, i - s));
        }
        double a, b;
        if (p.size() >= 2 && detail::ParseDoubleW(p[0], &a) && detail::ParseDoubleW(p[1], &b)) {
            Segment s;
            s.start = a;
            s.end = b;
            list.push_back(std::move(s));
        }
    }
    return list;
}

// ;FFMETADATA1 chapters (our own "FFmpeg chapters metadata" export; FastCut read those .txt files as timestamps).
std::vector<Segment> ImportFfmetadata(const std::wstring& text) {
    std::vector<Segment> list;
    bool inChapter = false;
    double num = 1, den = 1000;
    Segment cur;
    auto flush = [&] {
        if (inChapter) list.push_back(cur);
        cur = Segment{};
        num = 1;
        den = 1000;
    };
    auto lines = Split(text, L'\n');
    for (size_t i = 0; i < lines.size(); i++) {
        std::wstring line = TrimEndCR(lines[i]);
        // A trailing unescaped backslash continues the value on the next line.
        while (!line.empty() && line.back() == L'\\' && i + 1 < lines.size()) {
            size_t bs = 0;
            for (size_t k = line.size(); k > 0 && line[k - 1] == L'\\'; k--) bs++;
            if (bs % 2 == 0) break;
            line += L'\n' + TrimEndCR(lines[++i]);
        }
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line[0] == L'[') {
            flush();
            inChapter = _wcsicmp(TrimW(line).c_str(), L"[CHAPTER]") == 0;
            continue;
        }
        if (!inChapter) continue;
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = util::Lower(line.substr(0, eq)), raw = line.substr(eq + 1), val;
        for (size_t k = 0; k < raw.size(); k++) {
            if (raw[k] == L'\\' && k + 1 < raw.size()) k++;
            val += raw[k];
        }
        double v;
        if (key == L"timebase") {
            size_t slash = val.find(L'/');
            double a, b;
            if (slash != std::wstring::npos && detail::ParseDoubleW(val.substr(0, slash), &a) &&
                detail::ParseDoubleW(val.substr(slash + 1), &b) && a > 0 && b > 0) {
                num = a;  // START/END follow TIMEBASE in ffmpeg's own files
                den = b;
            }
        } else if (key == L"start" && detail::ParseDoubleW(val, &v)) {
            cur.start = v * num / den;
        } else if (key == L"end" && detail::ParseDoubleW(val, &v)) {
            cur.end = v * num / den;
        } else if (key == L"title") {
            cur.name = val;
        }
    }
    flush();
    return list;
}

std::vector<Segment> SortedByStart(const std::vector<Segment>& segs) {
    std::vector<Segment> s = segs;
    std::stable_sort(s.begin(), s.end(), [](const Segment& a, const Segment& b) { return a.start < b.start; });
    return s;
}

// TimeSpan.FromSeconds (tick precision, truncated) split into parts.
void Clock(double t, long long* h, long long* m, long long* s, long long* ms) {
    long long ticks = (long long)(std::max(0.0, t) * 1e7);
    long long totalMs = ticks / 10000;
    *h = totalMs / 3600000;
    *m = totalMs / 60000 % 60;
    *s = totalMs / 1000 % 60;
    *ms = totalMs % 1000;
}

std::wstring ExportYouTube(const std::vector<Segment>& segs) {
    std::wstring r;
    auto sorted = SortedByStart(segs);
    if (!sorted.empty() && sorted[0].start > 0) r += L"0:00 Start\n";  // YouTube requires the first chapter at 0:00
    for (size_t i = 0; i < sorted.size(); i++) {
        long long h, m, s, ms;
        Clock(std::floor(sorted[i].start), &h, &m, &s, &ms);
        wchar_t ts[48];
        if (h >= 1) swprintf(ts, 48, L"%lld:%02lld:%02lld", h, m, s);
        else swprintf(ts, 48, L"%lld:%02lld", m, s);
        r += ts;
        r += L" " + (sorted[i].name.empty() ? L"Chapter " + std::to_wstring(i + 1) : sorted[i].name) + L"\n";
    }
    return r;
}

std::wstring ExportCue(const std::wstring& mediaPath, const std::vector<Segment>& segs) {
    std::wstring r = L"FILE \"" + util::FileName(mediaPath) + L"\" WAVE\n";
    auto sorted = SortedByStart(segs);
    for (size_t i = 0; i < sorted.size(); i++) {
        double t = sorted[i].start;
        int mm = (int)(t / 60), ss = (int)std::fmod(t, 60), ff = (int)std::nearbyint(std::fmod(t, 1) * 75) % 75;
        wchar_t b[96];
        swprintf(b, 96, L"  TRACK %02d AUDIO\n    TITLE \"", (int)(i + 1));
        r += b;
        r += sorted[i].name.empty() ? L"Track " + std::to_wstring(i + 1) : sorted[i].name;
        swprintf(b, 96, L"\"\n    INDEX 01 %02d:%02d:%02d\n", mm, ss, ff);
        r += b;
    }
    return r;
}

std::wstring SrtTime(double t) {
    long long h, m, s, ms;
    Clock(t, &h, &m, &s, &ms);
    wchar_t b[48];
    swprintf(b, 48, L"%02lld:%02lld:%02lld,%03lld", h, m, s, ms);
    return b;
}

std::wstring ExportSrt(const std::vector<Segment>& segs) {
    std::wstring r;
    for (size_t i = 0; i < segs.size(); i++) {
        const Segment& s = segs[i];
        r += std::to_wstring(i + 1) + L"\n" + SrtTime(s.start) + L" --> " + SrtTime(s.end) + L"\n" +
             (s.name.empty() ? L"Segment " + std::to_wstring(i + 1) : s.name) + L"\n\n";
    }
    return r;
}

std::wstring ExportFfmetadata(const std::vector<Segment>& segs) {
    std::wstring r = L";FFMETADATA1\n";
    for (const auto& s : SortedByStart(segs)) {
        std::wstring title;
        for (wchar_t c : s.name) {
            if (c == L'=' || c == L';' || c == L'#' || c == L'\\' || c == L'\n') title += L'\\';
            title += c;
        }
        r += L"[CHAPTER]\nTIMEBASE=1/1000\nSTART=" + std::to_wstring((long long)(s.start * 1000)) +
             L"\nEND=" + std::to_wstring((long long)(s.end * 1000)) + L"\ntitle=" + title + L"\n";
    }
    return r;
}

}  // namespace

std::vector<Segment> Import(const std::wstring& path, double duration) {
    std::wstring text = ReadTextFile(path);
    std::wstring ext = Ext(path);
    if (ext == L".llc") return ReadProject(util::Utf8(text), duration);
    if (text.compare(0, 12, L";FFMETADATA1") == 0) return ImportFfmetadata(text);
    if (ext == L".csv") return ImportCsv(text, duration);
    if (ext == L".cue") return ImportCue(text, duration);
    if (ext == L".srt") return ImportSrt(text);
    if (ext == L".edl") return ImportEdl(text);
    return ImportTimestamps(text, duration);
}

void Export(const std::wstring& path, int filterIndex, const std::wstring& mediaPath, const std::vector<Segment>& segs) {
    if (filterIndex == 1) {
        WriteTextFile(path, WriteProject(mediaPath, segs));
        return;
    }
    std::wstring text;
    switch (filterIndex) {
        case 2:
            for (const auto& s : segs) text += Num(s.start) + L"," + Num(s.end) + L"," + CsvField(s.name) + L"\n";
            break;
        case 3:
            for (const auto& s : segs) text += Fmt(s.start) + L"," + Fmt(s.end) + L"," + CsvField(s.name) + L"\n";
            break;
        case 4: text = ExportYouTube(segs); break;
        case 5: text = ExportCue(mediaPath, segs); break;
        case 6: text = ExportSrt(segs); break;
        case 7:
            for (const auto& s : segs) text += Num(s.start) + L" " + Num(s.end) + L" 0\n";
            break;
        default: text = ExportFfmetadata(segs); break;
    }
    WriteTextFile(path, util::Utf8(text));
}

}  // namespace edit::segio
