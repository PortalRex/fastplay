// FF.cs: runs ffmpeg/ffprobe hidden, streams their output, keeps a log of every command.
#include "ffrun.h"
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

namespace edit::ff {

namespace {

bool IsFile(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Next to fastplay.exe, else the PATH directories (only those, like FastCut), else the bare name.
std::wstring FindTool(const wchar_t* exe) {
    std::wstring local = util::ExeDir() + L"\\" + exe;
    if (IsFile(local)) return local;
    DWORD n = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (n) {
        std::wstring path(n, L'\0');
        path.resize(GetEnvironmentVariableW(L"PATH", path.data(), n));
        wchar_t buf[MAX_PATH * 4];
        DWORD got = SearchPathW(path.c_str(), exe, nullptr, (DWORD)std::size(buf), buf, nullptr);
        if (got && got < std::size(buf)) return buf;
    }
    return exe;
}

std::wstring Utf8ToWide(const std::string& s) { return util::Wide(s); }

std::string LastLines(const std::string& s, size_t n) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= s.size()) {
        size_t e = s.find('\n', start);
        if (e == std::string::npos) e = s.size();
        if (e > start) lines.push_back(s.substr(start, e - start));
        start = e + 1;
    }
    std::string r;
    for (size_t i = lines.size() > n ? lines.size() - n : 0; i < lines.size(); i++) {
        if (!r.empty()) r += '\n';
        r += lines[i];
    }
    return r;
}

// Quoting for CreateProcess that round-trips through CommandLineToArgvW and the MSVC CRT.
void AppendArg(std::wstring& cmd, const std::wstring& a) {
    if (!cmd.empty()) cmd += L' ';
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += a;
        return;
    }
    cmd += L'"';
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            slashes++;
        } else if (c == L'"') {
            cmd.append(slashes * 2 + 1, L'\\');
            slashes = 0;
        } else {
            cmd.append(slashes, L'\\');
            slashes = 0;
        }
        if (c != L'\\') cmd += c;
    }
    cmd.append(slashes * 2, L'\\');  // backslashes before the closing quote
    cmd += L'"';
}

// Every tool belongs to this job: closing FastPlay (the last handle) kills whatever is still running.
HANDLE GlobalJob() {
    static HANDLE job = [] {
        HANDLE j = CreateJobObjectW(nullptr, nullptr);
        if (j) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof li);
        }
        return j;
    }();
    return job;
}

std::wstring SystemMessage(DWORD err) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   err, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring r = msg ? util::Trim(msg) : L"error " + std::to_wstring(err);
    if (msg) LocalFree(msg);
    return r;
}

// One running tool: hidden, stdout/stderr on pipes, stdin on NUL, only those three handles inherited (so parallel
// runs never hold each other's pipes open), in its own job (nested in the global one) so Kill takes the whole tree.
class Proc {
public:
    HANDLE process = nullptr, job = nullptr, out = nullptr, err = nullptr;

    void Start(const std::wstring& exe, const std::vector<std::wstring>& args) {
        SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
        HANDLE outW = nullptr, errW = nullptr, nul = INVALID_HANDLE_VALUE;
        auto fail = [&](DWORD e) {
            if (outW) CloseHandle(outW);
            if (errW) CloseHandle(errW);
            if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
            Close();
            throw EditError(L"Could not start " + exe + L": " + SystemMessage(e));
        };
        if (!CreatePipe(&out, &outW, &sa, 1 << 16) || !CreatePipe(&err, &errW, &sa, 1 << 16)) fail(GetLastError());
        SetHandleInformation(out, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(err, HANDLE_FLAG_INHERIT, 0);
        nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
        if (nul == INVALID_HANDLE_VALUE) fail(GetLastError());

        HANDLE inherit[3] = {nul, outW, errW};
        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        std::vector<uint8_t> attrBuf(attrSize);
        auto attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
        if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize)) fail(GetLastError());
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);

        STARTUPINFOEXW si = {};
        si.StartupInfo.cb = sizeof si;
        si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput = nul;
        si.StartupInfo.hStdOutput = outW;
        si.StartupInfo.hStdError = errW;
        si.lpAttributeList = attrs;

        std::wstring cmd;
        AppendArg(cmd, exe);
        for (const auto& a : args) AppendArg(cmd, a);
        bool resolved = exe.find_first_of(L"\\/") != std::wstring::npos;
        PROCESS_INFORMATION pi = {};
        BOOL ok = CreateProcessW(resolved ? exe.c_str() : nullptr, cmd.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                 &si.StartupInfo, &pi);
        DWORD e = GetLastError();
        DeleteProcThreadAttributeList(attrs);
        CloseHandle(outW);
        CloseHandle(errW);
        CloseHandle(nul);
        outW = errW = nullptr;
        nul = INVALID_HANDLE_VALUE;
        if (!ok) fail(e);
        process = pi.hProcess;
        if (HANDLE g = GlobalJob()) AssignProcessToJobObject(g, process);
        job = CreateJobObjectW(nullptr, nullptr);
        if (job && !AssignProcessToJobObject(job, process)) {
            CloseHandle(job);
            job = nullptr;
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
    }

    void Kill() {
        if (job) TerminateJobObject(job, 1);
        if (process) TerminateProcess(process, 1);
    }

    void Close() {
        for (HANDLE* h : {&process, &job, &out, &err})
            if (*h) { CloseHandle(*h); *h = nullptr; }
    }

    ~Proc() { Close(); }
};

// Splits a byte stream into lines like .NET's ReadLine: "\n", "\r\n" and a lone "\r" (ffmpeg's progress line) all end
// a line; a final unterminated line counts.
class LineSplitter {
public:
    template <class F>
    void Feed(const char* p, size_t n, F&& emit) {
        for (size_t i = 0; i < n; i++) {
            char c = p[i];
            if (c == '\n') {
                if (!lastCR) {  // "\r\n" already ended the line at '\r'
                    emit(cur);
                    cur.clear();
                }
                lastCR = false;
            } else if (c == '\r') {
                emit(cur);
                cur.clear();
                lastCR = true;
            } else {
                cur += c;
                lastCR = false;
            }
        }
    }
    template <class F>
    void Finish(F&& emit) {
        if (!cur.empty()) emit(cur);
        cur.clear();
    }

private:
    std::string cur;
    bool lastCR = false;
};

// Reads stderr to the end: lines to onErr, the last 100 kept.
struct ErrReader {
    std::deque<std::string> tail;
    std::exception_ptr error;

    void Run(HANDLE h, const LineFn& onErr, Proc& p) {
        try {
            LineSplitter split;
            auto emit = [&](const std::string& l) {
                if (onErr) onErr(l);
                tail.push_back(l);
                if (tail.size() > 100) tail.pop_front();
            };
            char buf[8192];
            DWORD n = 0;
            while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n) split.Feed(buf, n, emit);
            split.Finish(emit);
        } catch (...) {
            error = std::current_exception();
            p.Kill();
        }
    }

    std::string Text() const {
        std::string r;
        for (const auto& l : tail) {
            if (!r.empty()) r += '\n';
            r += l;
        }
        return r;
    }
};

struct Log {
    std::mutex mu;
    std::deque<std::wstring> lines;
};
Log& TheLog() {
    static Log log;
    return log;
}

}  // namespace

const std::wstring& Ffmpeg() {
    static const std::wstring p = FindTool(L"ffmpeg.exe");
    return p;
}

const std::wstring& Ffprobe() {
    static const std::wstring p = FindTool(L"ffprobe.exe");
    return p;
}

bool Available() {
    return Ffmpeg().find(L'\\') != std::wstring::npos && Ffprobe().find(L'\\') != std::wstring::npos;
}

const Result& Result::Check() const {
    if (code != 0) {
        std::string last = LastLines(err, 6);
        throw EditError(last.empty() ? L"exit code " + std::to_wstring(code) : Utf8ToWide(last));
    }
    return *this;
}

void AddLog(const std::wstring& text) {
    std::wstring t;
    t.reserve(text.size());
    for (size_t i = 0; i < text.size(); i++) {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) t += L'\r';
        t += text[i];
    }
    Log& log = TheLog();
    std::lock_guard<std::mutex> lock(log.mu);
    log.lines.push_back(std::move(t));
    if (log.lines.size() > 1000) log.lines.erase(log.lines.begin(), log.lines.begin() + 200);
}

std::wstring LogText() {
    Log& log = TheLog();
    std::lock_guard<std::mutex> lock(log.mu);
    std::wstring r;
    for (const auto& l : log.lines) {
        if (!r.empty()) r += L"\r\n";
        r += l;
    }
    return r;
}

std::wstring CmdLine(const std::wstring& exe, const std::vector<std::wstring>& args) {
    std::wstring name = util::FileName(exe);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    std::wstring r = name + L" ";
    for (size_t i = 0; i < args.size(); i++) {
        if (i) r += L' ';
        const std::wstring& a = args[i];
        if (!a.empty() && a.find_first_of(L" \"\t&|()';") == std::wstring::npos) {
            r += a;
        } else {
            r += L'"';
            for (wchar_t c : a) {
                if (c == L'"') r += L'\\';
                r += c;
            }
            r += L'"';
        }
    }
    return r;
}

Result Run(const std::wstring& exe, const std::vector<std::wstring>& args, Cancel* ct, LineFn onOut, LineFn onErr,
           bool logIt) {
    if (ct && ct->Cancelled()) throw Cancelled();
    if (logIt) AddLog(L"> " + CmdLine(exe, args));
    Proc p;
    p.Start(exe, args);

    Result r;
    std::exception_ptr outError;
    std::thread outThread([&] {
        try {
            LineSplitter split;
            auto emit = [&](const std::string& l) { onOut(l); };
            char buf[16384];
            DWORD n = 0;
            while (ReadFile(p.out, buf, sizeof buf, &n, nullptr) && n) {
                if (onOut) split.Feed(buf, n, emit);
                else r.out.append(buf, n);
            }
            if (onOut) split.Finish(emit);
        } catch (...) {
            outError = std::current_exception();
            p.Kill();
        }
    });
    ErrReader errReader;
    std::thread errThread([&] { errReader.Run(p.err, onErr, p); });

    bool cancelled = false;
    for (;;) {
        if (WaitForSingleObject(p.process, ct ? 50 : INFINITE) == WAIT_OBJECT_0) break;
        if (ct && ct->Cancelled()) {
            cancelled = true;
            p.Kill();
            WaitForSingleObject(p.process, 5000);
            break;
        }
    }
    outThread.join();
    errThread.join();
    DWORD code = 1;
    GetExitCodeProcess(p.process, &code);
    r.code = (int)code;
    r.err = errReader.Text();

    if (cancelled || (ct && ct->Cancelled())) throw Cancelled();
    if (outError) std::rethrow_exception(outError);
    if (errReader.error) std::rethrow_exception(errReader.error);
    if (logIt && r.code != 0) AddLog(L"  exit " + std::to_wstring(r.code) + L": " + Utf8ToWide(LastLines(r.err, 10)));
    return r;
}

void RunStream(const std::wstring& exe, const std::vector<std::wstring>& args, Cancel* ct,
               const std::function<bool(const uint8_t* data, size_t size)>& onData) {
    if (ct && ct->Cancelled()) throw Cancelled();
    Proc p;
    p.Start(exe, args);

    // stderr must be drained or the tool blocks once the pipe is full.
    std::thread errThread([h = p.err] {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n) {
        }
    });
    // The reading thread blocks in ReadFile, so a watcher kills the tool on cancel (which ends the read).
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread watcher;
    if (ct && stop) {
        watcher = std::thread([&p, ct, stop] {
            HANDLE hs[2] = {stop, p.process};
            while (WaitForMultipleObjects(2, hs, FALSE, 50) == WAIT_TIMEOUT)
                if (ct->Cancelled()) { p.Kill(); break; }
        });
    }

    std::exception_ptr error;
    try {
        std::vector<uint8_t> buf(1 << 16);
        DWORD n = 0;
        while (ReadFile(p.out, buf.data(), (DWORD)buf.size(), &n, nullptr) && n)
            if (!onData(buf.data(), n)) break;
    } catch (...) {
        error = std::current_exception();
    }
    p.Kill();  // FastCut kills in 'finally' too: stopping early must not leave the tool decoding
    if (stop) SetEvent(stop);
    if (watcher.joinable()) watcher.join();
    WaitForSingleObject(p.process, 5000);
    errThread.join();
    if (stop) CloseHandle(stop);

    if (ct && ct->Cancelled()) throw Cancelled();
    if (error) std::rethrow_exception(error);
}

}  // namespace edit::ff
