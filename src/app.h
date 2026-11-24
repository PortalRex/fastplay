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
    WM_APP_CLIP_DONE,          // lossless clip export finished (wParam = ok, lParam = std::wstring* message)
    WM_APP_RECYCLED,           // a file went to the Recycle Bin (wParam = ok, lParam = std::wstring* name)
    WM_APP_DCLIP_PROGRESS,     // Discord clip progress (lParam = std::wstring* text)
    WM_APP_DCLIP_DONE,         // Discord clip finished (lParam = DiscordClipResult*)
};

// Menu / taskbar command ids shared by window.cpp and features.cpp.
enum : UINT {
    kCmdAction = 1000,  // + Act
    kCmdAudio = 2000,   // + track id
    kCmdSub = 3000,     // + track id (3000 = off)
    kCmdRecent = 4000,  // + index
    kCmdChapter = 5000, // + index
    kCmdTrayOpen = 6000, kCmdTrayShow, kCmdTrayExit, kCmdLoadSub, kCmdOpenFolder,
    kCmdSpeed = 6100,      // + index
    kCmdThumbPrev = 7001, kCmdThumbPlay, kCmdThumbNext,
    kCmdAudioDev = 7100,   // + index into the device list
    kCmdAspect = 7200,     // + index
    kCmdAfter = 7300,      // + after-playback mode
    kCmdAssocOn = 7400, kCmdAssocOff, kCmdAddFiles, kCmdClearPlaylist, kCmdAbout,
    kCmdVolMax = 7500,     // + index
};

constexpr ULONG_PTR kCopyDataOpen = 0x464C5059;    // 'FPLY': WM_COPYDATA payload: UTF-16 paths separated by '\n'
constexpr ULONG_PTR kCopyDataCommand = 0x46504143; // 'FPAC': "action <id>", "hover <sec>", "seek <sec>" (scripting/tests)

struct BarState {
    bool hasFile = false, paused = true, mute = false, remaining = false;
    double pos = 0, dur = 0, speed = 1, abA = -1, abB = -1;
    int volume = 100, volumeMax = 100;
    std::vector<double> chapters;
    bool operator==(const BarState& o) const {
        return hasFile == o.hasFile && paused == o.paused && mute == o.mute && remaining == o.remaining && pos == o.pos &&
               dur == o.dur && speed == o.speed && abA == o.abA && abB == o.abB && volume == o.volume &&
               volumeMax == o.volumeMax && chapters == o.chapters;
    }
    bool operator!=(const BarState& o) const { return !(*this == o); }
};

class ControlBar;
class ThumbPopup;
namespace edit { class Editor; struct Cancel; }
struct DiscordClipResult;

class App {
    friend class edit::Editor;  // Edit mode drives the player directly

public:
    int Run(HINSTANCE inst, const std::vector<std::wstring>& args);

    // Used by the control bar.
    void SeekTo(double t, bool exact);
    void DoAction(Act a);
    void SetVolume(int v);
    void ShowContextMenu(POINT screen);
    void HoverSeek(double t, POINT anchorScreen);  // t < 0 hides the preview
    const BarState& Bar() const { return bar_; }
    std::wstring ShortcutText(Act a) const { return keys_.ShortcutText(a); }
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
    void RecycleCurrent();
    void ZoomAutofitLarger();
    void StartPrefetch(const std::wstring& path, double start, unsigned serial);
    void OnIndex(void* msg, unsigned serial);
    void StartThumbnails();
    std::wstring OpenDialog();

    // mpv events
    void DrainEvents();
    void OnProperty(mpv_event_property* p);
    void UpdateTitle();
    void RefreshBar();
    void UpdateDurationObserver();

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

    // features.cpp: playlist, after playback, video/audio extras, taskbar, associations, clips, help
    void OpenFiles(const std::vector<std::wstring>& files, bool append);
    bool StepPlaylist(int dir);
    void OnEndOfFile();
    void SetAfterPlayback(int mode);
    void TogglePlaylistWindow();
    void RefreshPlaylistWindow();
    static LRESULT CALLBACK PlaylistProc(HWND, UINT, WPARAM, LPARAM);
    void PlaylistActivate(int index);
    void PlaylistRemove(int index);
    void OnTaskbarButtonCreated();
    void InitTaskbar();
    void UpdateTaskbar(bool force);
    void ToggleVideoFilter(const char* label, const char* filter, const wchar_t* name);
    void AdjustColor(const char* prop, int delta);
    std::vector<std::pair<std::string, std::wstring>> AudioDevices();
    void SetAssociations(bool on);
    void SaveClip();                                                  // asks for the output name
    void StartClip(const std::wstring& out, const std::wstring& ffmpeg); // runs ffmpeg -c copy on the A-B range
    std::wstring FindFfmpeg();
    void OnClipDone(bool ok, std::wstring* msg);
    void DiscordClip();                                               // A-B range -> small MP4, copied (Ctrl+Shift+C)
    void OnDiscordClipDone(DiscordClipResult* r);
    void ApplyAudioMix(bool announce);                                // mpv lavfi-complex amix of all audio tracks
    void ShowHelp();
    void OpenUrlDialog();
    bool ExtraAction(Act a);           // handles the actions added in features.cpp
    void BuildExtraMenus(HMENU m, HMENU play, HMENU video, HMENU audio, HMENU subs);
    bool ExtraMenuCommand(UINT id);

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
    bool durationObserved_ = true;  // see UpdateDurationObserver
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
    RECT savedRect_ = {};  // window rect before fullscreen (test copies go back off-screen exactly)
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

    // Playlist (several files opened/dropped at once); single files navigate their folder instead.
    std::vector<std::wstring> plist_;
    int plPos_ = -1;
    HWND plWnd_ = nullptr, plList_ = nullptr;
    HFONT plFont_ = nullptr;
    HBRUSH plBrush_ = nullptr;
    bool eofHandled_ = false;

    // Taskbar progress and thumbnail buttons
    UINT taskbarButtonMsg_ = 0;
    struct ITaskbarList3* taskbar_ = nullptr;
    bool taskbarWanted_ = false, thumbButtons_ = false, comInit_ = false;
    HICON tbIcons_[4] = {};  // prev, play, pause, next
    DWORD lastTaskbarTick_ = 0;
    int lastTaskbarState_ = -1, lastTaskbarStep_ = -1;

    // Video/audio extras
    int aspectIdx_ = 0, rotate_ = 0;
    double scaleX_ = 1, scaleY_ = 1;  // MPC-HC's "stretch" (Num4/6/8/2): mpv video-scale-x / -y
    bool osdTime_ = false;
    double exactSeekAfterOpen_ = -1;  // Reopen: exact seek once the file shows its first frame
    // The seekbar shows a seek's target until mpv gets there, so late time-pos values from earlier (keyframe) seeks
    // don't pull the playhead back while scrubbing.
    double seekHoldPos_ = -1;
    bool seekHoldExact_ = false;
    DWORD seekHoldUntil_ = 0;
    bool SeekHeld();
    void ReleaseSeekHold(double pos);
    std::vector<std::string> audioDevNames_;

    // Lossless clip export (ffmpeg -c copy)
    std::thread clipThread_;
    HANDLE clipProcess_ = nullptr;
    std::wstring clipOut_;
    bool clipBusy_ = false;

    // Discord clip (Edit mode's exporter on a worker thread, no Edit-mode UI)
    std::thread dclipThread_;
    std::shared_ptr<edit::Cancel> dclipCancel_;
    bool dclipBusy_ = false;

    bool audioMixed_ = false;  // lavfi-complex is mixing this file's audio tracks

    // Edit mode (src/edit), created the first time it is used
    edit::Editor* editor_ = nullptr;
    bool EditActive() const;
    void ToggleEditMode();

public:
    bool PreTranslateKey(MSG& msg);  // keys typed in edit-mode panels go to the actions
};

extern App* g_app;
