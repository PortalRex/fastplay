// Actions and their key/mouse bindings. Defaults follow MPC-HC; every binding can be changed in fastplay.ini [keys].
#pragma once
#include "util.h"
#include <unordered_map>

class Ini;

enum class Act : int {
    None = 0,
    PlayPause, Play, Pause, Stop, FrameStep, FrameBack,
    SeekFwdSmall, SeekBackSmall, SeekFwdMed, SeekBackMed, SeekFwdLarge, SeekBackLarge,
    KeyframeFwd, KeyframeBack, SeekStart, GoTo,
    SpeedUp, SpeedDown, SpeedReset,
    AudioDelayUp, AudioDelayDown, SubDelayUp, SubDelayDown,
    NextChapter, PrevChapter, NextFile, PrevFile,
    Open, Close, Exit,
    Fullscreen, ExitFullscreen, OnTop, ToggleCaption, ToggleControls, ViewMinimal, ViewNormal,
    Zoom50, Zoom100, Zoom200, ZoomAutofit,
    PanLeft, PanRight, PanUp, PanDown, PanReset, ZoomIn, ZoomOut,
    VolUp, VolDown, Mute,
    NextAudio, PrevAudio, NextSub, PrevSub, SubToggle,
    ScreenshotSave, ScreenshotAuto,
    AbStart, AbEnd, AbClear,
    Stats, Properties, EditSettings, Menu,
    Playlist, OpenUrl, AspectNext, RotateCW, RotateCCW, FlipH, FlipV, Deinterlace,
    BrightnessUp, BrightnessDown, ContrastUp, ContrastDown, SaturationUp, SaturationDown, ColorReset,
    Normalize, SubSizeUp, SubSizeDown, SubPosUp, SubPosDown, RepeatFile, SaveClip, DiscordClip, MixAudio, Help,
    EditMode,
    // MPC-HC defaults added 2026-11-23
    Reopen, Recycle, LoadSubtitles, OpenLocation, BossKey, OsdTime, OsdFilename, SubLinePrev, SubLineNext,
    SubShiftUp, SubShiftDown, Zoom25, ZoomAutofitLarger, WidthUp, WidthDown, HeightUp, HeightDown, PanUpLeft,
    PanUpRight, PanDownLeft, PanDownRight, PanCenter, ViewCompact,
    // Timeline markers
    AddMarker, DeleteMarker, RenameMarker, NextMarker, PrevMarker, ClearMarkers,
    Count
};

struct ActionInfo {
    Act act;
    const wchar_t* id;        // ini name
    const wchar_t* label;     // menu / help text
    const wchar_t* defaults;  // MPC-HC default bindings, comma separated
};

const ActionInfo* Actions(int* count);
const ActionInfo* FindAction(Act a);

// Pseudo virtual-key codes for mouse input so it can be bound like keys.
enum : uint32_t { kWheelUp = 0x200, kWheelDown, kWheelLeft, kWheelRight, kMouseMiddle, kMouseX1, kMouseX2, kMouseLeftDbl };
enum : uint32_t { kModCtrl = 1u << 16, kModShift = 1u << 17, kModAlt = 1u << 18 };

uint32_t CurrentMods();
std::wstring ChordName(uint32_t chord);
bool ParseChord(const std::wstring& s, uint32_t* chord);

class KeyMap {
public:
    void Load(const Ini& ini);       // defaults, then [keys] overrides
    void WriteDefaults(Ini& ini) const;  // adds missing actions to [keys] so the file documents everything
    Act Lookup(uint32_t chord) const;
    std::wstring ShortcutText(Act a) const;  // first binding, for menus
    std::wstring AllShortcuts(Act a) const;  // every binding, comma separated

private:
    std::unordered_map<uint32_t, Act> map_;
    std::unordered_map<int, std::wstring> text_, all_;
};
