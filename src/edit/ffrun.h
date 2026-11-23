// FF.cs: runs ffmpeg/ffprobe (hidden, no console), streams their output, keeps a log of every command.
#pragma once
#include "common.h"

namespace edit::ff {

// ffmpeg_path from fastplay.ini (ffprobe.exe is looked for in the same folder). Call before the first Ffmpeg().
void SetFfmpegPath(const std::wstring& ffmpegExe);
const std::wstring& Ffmpeg();   // ffmpeg_path, else next to fastplay.exe, else on PATH, else "ffmpeg.exe"
const std::wstring& Ffprobe();
bool Available();               // both tools found
std::wstring MissingToolsMessage();  // what to do when they aren't

struct Result {
    int code = -1;
    std::string out;  // stdout (UTF-8) when no line callback was given
    std::string err;  // last ~100 stderr lines (UTF-8)
    const Result& Check() const;  // throws EditError(last 6 stderr lines) when code != 0
};

using LineFn = std::function<void(const std::string& line)>;  // UTF-8, without the line break; worker threads

// Runs to completion (blocking). Kills the process tree and throws Cancelled when ct is set. Children are in a job
// object, so they die with FastPlay too.
Result Run(const std::wstring& exe, const std::vector<std::wstring>& args, Cancel* ct, LineFn onOut = {},
           LineFn onErr = {}, bool logIt = true);

// Raw stdout to onData (return false to stop early, which kills the tool). Throws Cancelled when ct is set.
void RunStream(const std::wstring& exe, const std::vector<std::wstring>& args, Cancel* ct,
               const std::function<bool(const uint8_t* data, size_t size)>& onData);

std::wstring CmdLine(const std::wstring& exe, const std::vector<std::wstring>& args);  // for the log
void AddLog(const std::wstring& text);
std::wstring LogText();

}  // namespace edit::ff
