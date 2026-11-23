// The player: main window, libmpv instance, input, menus. One instance per process (single-instance by default).
#pragma once
#include "util.h"
#include "mpvapi.h"
#include "settings.h"
#include "keys.h"
#include "mp4index.h"
#include "thumbs.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>

enum : UINT {
    WM_APP_MPV = WM_APP + 1,   // libmpv has events
    WM_APP_MPV_READY,          // init thread finished (wParam = 1 on success)
    WM_APP_INDEX,              // header index ready (lParam = MediaIndex*, wParam = load serial)
    WM_APP_THUMB,              // a thumbnail finished decoding
    WM_APP_TRAY,               // tray icon callback
    WM_APP_DISABLE_CHILD,      // mpv created its video window: make it mouse-transparent
};

constexpr ULONG_PTR kCopyDataOpen = 0x464C5059;    // 'FPLY': WM_COPYDATA payload is a UTF-16 path
constexpr ULONG_PTR kCopyDataCommand = 0x46504143; // 'FPAC': "action <id>", "hover <sec>", "seek <sec>" (scripting/tests)

struct BarState {
    bool hasFile = false, paused = true, mute = false, remaining = false;
    double pos = 0, dur = 0, speed = 1, abA = -1, abB = -1;
    int volume = 100;
    std::vector<double> chapters;
};

class ControlBar;
class ThumbPopup;

class App {
public:
    int Run(HINSTANCE inst, const std::vector<std::wstring>& args);

    // Used by the control bar.
    void SeekTo(double t, bool exact);
    void DoAction(Act a);
    void SetVolume(int v);
    void ShowContextMenu(POINT screen);
    void HoverSeek(double t, POINT anchorScreen);  // t < 0 hides the preview
    const BarState& Bar() const { return bar_; }
    void ToggleRemaining();
    int Dpi() const { return dpi_; }
    HWND Hwnd() const { return hwnd_; }
    void NoteMouseActivity();
    bool Fullscreen() const { return fullscreen_; }

private:
    static LRESULT CALLBACK MainProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK VideoProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnMain(UINT m, WPARAM w, LPARAM l);
    LRESULT OnVideo(HWND h, UINT m, WPARAM w, LPARAM l);

    // Start-up
    bool HandOffToRunningInstance(const std::wstring& file);
    void CreateWindows(bool startHidden);
    void InitThread();
    void SetMpvOptions(mpv_handle* h);
    void OnMpvReady(bool ok);

    // Files
    void Open(const std::wstring& path);
    double PrepareOpen(const std::wstring& path);  // UI-thread bookkeeping; returns the start position
    static void IssueLoad(mpv_handle* h, const std::wstring& path, double start);  // loadfile (any thread)
    void RetireThumbnails();
    void SaveResume();
    void CloseFile();
    void StepFile(int dir);
    void StartPrefetch(const std::wstring& path, double start, unsigned serial);
    void OnIndex(void* msg, unsigned serial);
    void StartThumbnails();
    std::wstring OpenDialog();

    // mpv events
    void DrainEvents();
    void OnProperty(mpv_event_property* p);
    void UpdateTitle();
    void RefreshBar();

    // Window
    void Layout();
    void SetFullscreen(bool on);
    void SetCaption(bool on);
    void SetControls(bool on);
    void SetOnTop(bool on);
    void ResizeToVideo(double scale, bool fitOnly);
    int BarHeight() const;
    void ShowBarOverlay(bool show);
    void UpdateCursor();
    void HandleChord(uint32_t chord);
    void RunCommand(const std::wstring& cmd);
    void BuildMenu(HMENU m);
    void OnMenuCommand(UINT id);
    void AddTray();
    void RemoveTray();
    void HideToTray();
    void ShowFromTray();
    void Quit();
    void ReloadSettingsIfChanged();
    void ApplyLiveSettings();

    // Actions helpers
    void SeekRelative(double secs, const char* flags);
    void CycleTrack(const char* type, int dir);
    void ToggleStats();
    void ShowProperties();
    void ScreenshotSaveAs();
    void GoToDialog();
    void PanZoom(double dx, double dy, double dz);
    void ChangeSpeed(int dir);

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr, video_ = nullptr, mpvChild_ = nullptr;
    ControlBar* barWnd_ = nullptr;
    ThumbPopup* thumbWnd_ = nullptr;
    int dpi_ = 96;

    Ini ini_;
    std::wstring iniPath_;
    FILETIME iniTime_ = {};
    Settings s_;
    KeyMap keys_;
    ResumeStore resume_;

    mpv_handle* mpv_ = nullptr;
    std::thread initThread_;
    HANDLE hwndReady_ = nullptr;
    std::mutex pendingMu_;
    std::wstring pendingFile_;  // file to load once mpv is ready
    bool loadIssuedByInit_ = false;

    // Current file
    std::wstring path_;
    uint64_t fileSize_ = 0, fileMtime_ = 0;
    unsigned loadSerial_ = 0;
    bool firstFrameLogged_ = false;
    bool fitPending_ = false;
    MediaIndex index_;
    bool indexReady_ = false;
    std::unique_ptr<Thumbnailer> thumbs_;
    std::vector<std::thread> retiring_;  // thumbnailers shutting down off the UI thread; joined at exit
    bool spinning_ = false;
    double hoverT_ = -1;
    POINT hoverAnchor_ = {};
    double startOverride_ = -1;          // --start=SECONDS
    double pendingStart_ = 0;
    HANDLE instanceMutex_ = nullptr;

    BarState bar_;
    int64_t vidW_ = 0, vidH_ = 0;
    bool idle_ = true;
    bool statsOn_ = false;
    double panX_ = 0, panY_ = 0, zoom_ = 0;

    // Window state
    bool fullscreen_ = false, onTop_ = false, captionOn_ = true, controlsOn_ = true;
    WINDOWPLACEMENT savedPlacement_ = {sizeof(WINDOWPLACEMENT)};
    bool barOverlayShown_ = false;
    DWORD lastMouseMove_ = 0;
    POINT lastMousePt_ = {-1, -1};
    bool cursorHidden_ = false;
    bool clickPending_ = false, dragging_ = false;
    POINT downPt_ = {};
    bool trayIcon_ = false, inTray_ = false;
    bool benchExit_ = false;
    bool quitting_ = false;
    UINT taskbarCreatedMsg_ = 0;
};

extern App* g_app;
