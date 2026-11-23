// Edit mode: FastCut's editor (MainForm.cs) inside FastPlay's window. Created the first time the user switches to
// Edit mode and only then; View mode never touches it. Shares FastPlay's libmpv instance, window, thumbnails and
// playlist (the Files tab).
#pragma once
#include "common.h"
#include "timeline.h"
#include "segcards.h"
#include "panel.h"
#include <map>
#include <mutex>
#include <set>
#include <thread>

class App;

namespace edit {

enum : UINT {
    WM_APP_EDIT_LOADED = WM_APP + 40,  // probe finished (lParam = LoadResult*, wParam = load serial)
    WM_APP_EDIT_KEYFRAMES,             // keyframe index ready (lParam = KeyframeIndex*, wParam = serial)
    WM_APP_EDIT_JOBDONE,               // a job finished (lParam = JobOutcome*)
    WM_APP_EDIT_UI,                    // run a queued UI-thread callback (lParam = std::function<void()>*)
};

class Editor : public TimelineThumbs {
public:
    explicit Editor(App* app);
    ~Editor() override;

    bool Active() const { return active_; }
    void Enter();
    void Leave();
    void Layout(const RECT& client);                 // places the video host and all editor windows
    bool HandleChord(uint32_t chord, bool repeat);   // edit-mode key map; false = not bound here
    void OnFileOpened(const std::wstring& path);     // App::PrepareOpen (also while hidden: keeps state per file)
    void OnFileClosed();
    void OnTimer();                                  // ~30 Hz while active
    void OnDpi(int dpi);
    bool OnAppMessage(UINT m, WPARAM w, LPARAM l);
    bool ConfirmQuit();                              // false = user kept a running job
    void Shutdown();                                 // cancel jobs and loads, autosave, join threads
    bool LoadProjectFile(const std::wstring& llc);   // .llc dropped / opened
    void AppendMenuItems(HMENU m);                   // FastPlay's context menu while editing
    std::wstring ActionsHelp() const;                // cheat sheet text
    bool RunMenuId(UINT id);                         // ids from AppendMenuItems / ShowMenu
    bool RunAction(const std::wstring& id);          // scripting / tests (FastPlay's "edit <id>" command)
    std::wstring DebugState() const;                 // one line for test logs
    void SetOption(const std::wstring& key, const std::wstring& value);  // tests: cut_mode, merge, size_mb, format
    bool JobRunning() const { return jobRunning_; }

    // TimelineThumbs
    std::shared_ptr<const Thumb> Get(double t, double tol) override;
    void Want(const std::vector<double>& times, double tol, double center) override;

private:
    struct Action {
        std::wstring id, label, category;
        std::wstring defaults;    // chords, comma separated (FastPlay key names)
        bool repeat = false;
        std::function<void()> run;
    };
    struct LoadResult;
    struct JobOutcome;

    // editor.cpp: windows, layout, media loading, ticks, jobs
    void CreateWindows();
    void BuildMenuStrip();
    void BuildControlRow();
    void BuildExportBar();
    void BuildSidePanel();
    void ShowTab(int i);
    void LayoutSide(const RECT& rc);
    void StartLoad(const std::wstring& path);
    void OnLoaded(LoadResult* r, unsigned serial);
    void LoadWaveIfNeeded(bool force);
    void UpdateUiForMedia();
    void UpdateLoadIndicators();
    void RebuildTrackList();
    void RefreshFileList();
    void SetStatus(const std::wstring& text, const std::wstring& output = L"", int kind = -1);
    void RunJob(const std::wstring& name, std::function<std::pair<std::wstring, std::wstring>(ProgressFn, Cancel*)> work,
                bool onExportButton = false, std::function<void(bool ok)> after = {});
    void OnJobDone(JobOutcome* o);
    void Post(std::function<void()> fn);             // run on the UI thread
    void ShowIssues(const std::vector<std::wstring>& issues);
    std::wstring OutDir() const;
    EditSettings JobSettings() const;  // settings for a job (test runs: output forced into FASTPLAY_EDIT_OUT)
    int Box(const std::wstring& text, const std::wstring& title, UINT flags);  // MessageBox; logged instead in test mode

    // editactions.cpp: actions, keys, menus, segments, export, tools
    void RegisterActions();
    void LoadKeymap();
    void Run(const std::wstring& id);
    std::wstring KeyText(const std::wstring& id) const;
    void ShowMenu(const std::wstring& title, POINT screen);
    void BuildMenu(HMENU m, const std::vector<const wchar_t*>& ids);
    void UpdateChecks();
    bool WillReencode() const;
    void UpdateExportBar();
    std::wstring CutWarning(int i) const;
    void Seek(double t);
    double Pos() const;
    std::wstring FmtT(double t) const;
    bool ParseT(const std::wstring& s, double* t) const;
    Segment NewSeg(double a, double b);
    std::vector<Segment> Colorize(std::vector<Segment> list);
    const Segment* Sel() const;
    Segment* SelMut();
    std::vector<int> Targets() const;
    void PushUndo();
    void Undo();
    void Redo();
    void SegmentsChanged(bool rebuildList = true);
    void RebuildSegList();
    void UpdateSegHeader();
    void UpdateCutBoxes();
    void SetCutTime(bool isStart, const std::wstring& text);
    void SelectIndex(int i, uint32_t mods, bool seek);
    void SyncListSelection();
    void ReorderSegment(int from, int to);
    void InsertAndSelect(Segment s);
    void SetStart();
    void SetEnd();
    void AddSegment();
    void SplitSegment();
    void DeleteSegment();
    void DuplicateSegment();
    void SelectSegment(int dir);
    void LabelSegment();
    void EditSegment();
    void ToggleSegment();
    void SetAllSegments(bool on);
    void MoveSegment(int dir);
    void SortSegments();
    void InvertSegments();
    void DivideTimeline();
    void ShiftSegments();
    void SegmentsFromChapters();
    void SnapToKeyframes();
    void ChangeSpeed(double factor);
    void SetSpeed(double speed);
    void FrameStep(bool forward);
    void FrameJump(int frames);
    void Nudge(bool start, int dir);
    void JumpKeyframe(bool forward);
    void JumpCut(bool forward);
    void ToggleLoop();
    void PlaySegment();
    void SetTimecodeOffset();
    void ToggleSidebar();
    void SetAllTracks(bool on);
    void ShowStreamInfo();
    std::vector<Segment> LoadAutosave(const std::wstring& path);
    void SaveAutosave();
    void SaveProject();
    void LoadProjectDialog();
    void ImportSegments();
    void ExportSegmentsAs();
    void Export(bool onlySelected);
    void OfferFasterDrive(double outputSeconds);
    void Snapshot();
    void SnapshotSegments();
    void SnapshotEvery();
    void ExtractTracks();
    void ConcatFiles();
    void DownloadUrl();
    void DetectBlack();
    void DetectSilence();
    void DetectScenes();
    void ApplyDetected(const std::vector<std::pair<double, double>>& found);
    void ShowSettings();
    void BatchExport();
    void ApplyTimelineHeight();
    void SaveSettings();

    App* app_;
    bool active_ = false, created_ = false, sidebar_ = true;
    int dpi_ = 96;
    EditSettings s_;
    WINDOWPLACEMENT viewPlacement_ = {sizeof(WINDOWPLACEMENT)};
    bool restoreViewPlacement_ = false;

    // Windows
    Panel menuStrip_, controlRow_, exportBar_, tabStrip_, segBar_, trackBar_, fileBar_, segSummary_;
    Timeline tl_;
    SegmentCards cards_;
    HWND side_ = nullptr, trackList_ = nullptr, fileList_ = nullptr, rotationBox_ = nullptr;
    HWND timeBox_ = nullptr, cutStartBox_ = nullptr, cutEndBox_ = nullptr;
    int tab_ = 0;
    bool rebuildingTracks_ = false;  // ignore the list's own check notifications while filling it
    bool splitterDrag_ = false;
    HBRUSH panelBrush_ = nullptr;

    // Media and segments
    std::wstring file_;
    std::shared_ptr<MediaInfo> media_;
    KeyframeIndex kf_;
    std::vector<Segment> segs_;
    std::vector<std::vector<Segment>> undo_, redo_;
    std::set<int> marked_;
    int nextColor_ = 0;
    double timeOffset_ = 0, lastPos_ = -1, playUntil_ = -1, holdPos_ = 0;
    DWORD holdPosUntil_ = 0, lastNudge_ = 0;
    bool looping_ = false, lastPaused_ = true;
    std::wstring lastOutput_, loadText_;
    std::wstring pendingProject_;  // .llc to apply once its media has been probed
    unsigned loadSerial_ = 0;
    std::shared_ptr<Cancel> loadCancel_;
    std::thread loadThread_, waveThread_;
    std::vector<float> wave_;
    std::atomic<size_t> waveFilled_{0};
    std::atomic<double> keyframeProgress_{-1}, waveProgress_{-1};
    bool waveLoading_ = false;

    // Jobs
    std::thread jobThread_;
    std::shared_ptr<Cancel> jobCancel_;
    bool jobRunning_ = false, jobOnExport_ = false;
    std::wstring jobName_;
    std::mutex progMu_;
    double progFrac_ = -1;
    std::wstring progMsg_;
    DWORD jobStart_ = 0;
    std::function<void(bool)> jobAfter_;

    // Actions and keys
    std::vector<Action> actions_;
    std::map<std::wstring, size_t> actionIndex_;
    std::map<uint32_t, std::wstring> keyToAction_;
    std::map<std::wstring, std::wstring> keyText_;
};

}  // namespace edit
