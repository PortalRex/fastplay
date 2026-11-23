// SegmentIO.cs: LosslessCut .llc projects, per-file autosave, import/export of segment lists.
#pragma once
#include "common.h"

namespace edit::segio {

std::string WriteProject(const std::wstring& mediaPath, const std::vector<Segment>& segs);  // .llc (UTF-8 JSON5)
std::vector<Segment> ReadProject(const std::string& text, double duration);                 // tolerant JSON5
std::vector<std::pair<std::wstring, std::wstring>> ParseTags(const std::wstring& tags);

// Same folder and file naming as FastCut (FASTCUT_HOME honoured), so FastCut and FastPlay's Edit mode share
// autosaved segments for a file.
std::wstring AutosavePath(const std::wstring& mediaPath);

// GetOpenFileName / GetSaveFileName filters (double-NUL terminated), same choices and order as FastCut.
extern const wchar_t kImportFilter[];
extern const wchar_t kExportFilter[];
std::vector<Segment> Import(const std::wstring& path, double duration);  // by extension / content
void Export(const std::wstring& path, int filterIndex, const std::wstring& mediaPath, const std::vector<Segment>& segs);

// Small JSON helpers shared by edit-mode code (ffprobe output, projects). JSON5-tolerant reader.
struct Json;
using JsonPtr = std::shared_ptr<Json>;
struct Json {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;                                      // UTF-8 (String); numbers keep their source text here too
    std::vector<JsonPtr> arr;
    std::vector<std::pair<std::string, JsonPtr>> obj;   // insertion order
    const Json* Get(const char* key) const;             // object member or nullptr
    std::string Str(const char* key) const;             // member as string ("" if missing; numbers as source text)
    double Num(const char* key) const;                  // member as number (strings parsed, 0 if missing)
};
JsonPtr ParseJson(const std::string& text);             // throws EditError on malformed input
std::string JsonQuote(const std::string& utf8);         // "..." with escapes

}  // namespace edit::segio
