# FastPlay

A small, fast Windows video player and lossless editor, built to replace MPC-HC for everyday playback and FastCut for
cutting, especially for huge OBS recordings (30+ GB, 1 h+, 1440p60). Native Win32 C++ around libmpv: no .NET, no Qt,
no scripts. One ~1.6 MB exe; libmpv-2.dll sits next to it; ffmpeg/ffprobe (on PATH or next to the exe) are only used
by Edit mode.

Two modes:

- **View mode** (default): a fast MPC-HC-style player. Nothing of Edit mode is created or loaded.
- **Edit mode** (**E** or **Ctrl+E**, or the *Edit mode* menu item): FastCut's whole editor in the same window and the
  same libmpv instance: timeline with thumbnails / waveform / keyframes, segments, lossless keyframe / normal / smart
  cuts, merge, Discord-ready exports, tracks, batch export, merge files, snapshots, detection and segment import/export.
  **E** again (or *View mode* at the top right) goes back. See [Edit mode](#edit-mode-fastcut-built-in).

## Why it opens big recordings faster

These are measured, not assumed (see [Benchmarks](#benchmarks) and the notes per item):

- **Fragmented MP4 (OBS "fragmented_mp4" / hybrid MP4) opens without reading every fragment.** By default FFmpeg's
  MP4 demuxer, which both mpv and MPC-HC's LAV Splitter use, reads the header of *every* fragment when it opens the
  file: 1 254 seeks for an 87-minute 32 GB recording, about 10 s on a cold hard disk. FastPlay tells it to use the
  trailing `mfra` index instead (`use_mfra_for=pts`): 3 seeks. Timestamps were checked packet by packet against the
  default path (video identical, audio within 1.3 ms). mpv then learns the length only as it reads, so the duration
  comes from FastPlay's own header index (exact: last fragment's `tfdt` + sample durations).
- **Window first, libmpv second.** The window is up ~30 ms after launch; the 120 MB libmpv loads on a background
  thread (loading it in parallel with window creation measured slower for the window and no faster to first frame).
- **Header prefetch in parallel.** While libmpv starts, a thread reads the MP4 index (moov / mfra; ~1 ms) and the
  first 2 MB at the resume point, so the demuxer's first reads hit the OS cache.
- **Early video output** (`force-window`): the D3D11 device and swap chain are created while the file opens.
- **Resume on a keyframe.** An exact resume seek decodes up to a whole GOP (4 s / 250 frames in OBS files) before
  showing anything; FastPlay resumes at the keyframe before the saved position, like MPC-HC.
- **d3d11 gpu-next with zero-copy d3d11va**, shader cache in `%LOCALAPPDATA%\FastPlay\shaders`, no config files,
  no Lua, no OSC, no youtube-dl; smaller demuxer buffers than mpv's defaults (64 MiB ahead, 16 MiB back).
- **Single instance** (default): a second launch hands the file to the running player over `WM_COPYDATA` and exits;
  the handoff process never loads libmpv.
- **Optional warm tray instance** (`tray=1`): closing hides the window and keeps libmpv and the GPU device alive, so
  the next file skips the ~170-250 ms of process + D3D11 start-up.

Tried and dropped (measured no gain): creating a throwaway D3D11 device on a background thread to "pre-warm" the GPU
driver. A process's first D3D11 device costs ~160-200 ms on this machine's NVIDIA driver and later ones ~25 ms, but
the pre-warm itself takes as long as the device libmpv would have created.

## Features (MPC-HC parity)

- MPC-HC default keys (below), all remappable in `fastplay.ini` `[keys]` (two or more bindings per action, mouse
  buttons and wheel included).
- Frame step / step back, speed up/down/reset with pitch correction, A-B loop, Go To (Ctrl+G).
- Audio and subtitle track menus, libass subtitles, drag a subtitle file onto the window to load it, sub/audio delay.
- Zoom 50/100/200 % / auto-fit window, video zoom and pan (numpad), always on top, borderless (Ctrl+0), minimal view.
- Fullscreen on double-click / Alt+Enter, wheel volume, mute, next/previous file in the folder (natural sort),
  chapters, screenshot (Alt+I save as, F5 auto), drag and drop, right-click menu, recent files, statistics overlay,
  properties, auto-hiding controls and cursor in fullscreen, hidden cursor over the video after 2 s.
- Seekbar thumbnails: keyframe times come straight from the MP4 header (regular `stts`/`stss` tables, or the `mfra`
  of fragmented files), and a second hidden libmpv instance decodes keyframes only, without audio, through the
  software render API on a below-normal-priority thread. Hover requests coalesce (only the latest waits). On SSDs it
  also pre-fills 64 evenly spread thumbnails; on spinning disks it only decodes what you hover, so playback isn't
  starved of seeks. At most `thumbnail_cache` (150) thumbnails stay in memory.
- Resume position per file, keyed by path + size + modification time (never a content hash), in
  `%APPDATA%\FastPlay\history.txt`; it doubles as the Recent list.
- Media keys (play/pause, stop, next/previous).
- **Playlist** (Ctrl+7): open or drop several files (Shift+drop appends, dropping onto the playlist window appends),
  double-click / Enter plays, Delete removes; Next/Previous go chapters → playlist → folder, as in MPC-HC.
  **After playback**: do nothing, play next file, repeat file, close, exit.
- **Video**: aspect ratio override (4:3, 16:9, 1.85, 2.35, 21:9, 1:1), rotate (Alt+Num4/6), mirror, flip,
  deinterlace (Ctrl+Shift+D), brightness / contrast / saturation. Mirror and flip are software filters, so while one
  is on the decoder switches to copy-back mode by itself and goes back to zero-copy when both are off.
- **Audio**: output device menu, volume boost up to 150 / 200 %, normalize (dynaudnorm).
- **Subtitles**: bigger/smaller, up/down, load external file.
- **Taskbar**: progress on the taskbar button and Previous / Play-Pause / Next buttons in its thumbnail (set up after
  the first frame, so COM never touches start-up).
- **Open URL** (Ctrl+U) for direct media and stream URLs (no youtube-dl).
- **Register as video player** (Tools menu): per-user registration so FastPlay shows under *Open with* and in
  Windows' Default apps, where you can make it the default; *Remove file type registration* undoes it.
- **Help → Keyboard shortcuts** (Ctrl+F1) lists every action with its keys; control-bar buttons have tooltips.

## Edit mode (FastCut built in)

A C++ port of [FastCut](https://github.com/PortalRex/fastcut) (C# / WinForms) that lives in `src/edit` and is created
the first time you press **E**: View mode's start-up, memory and code paths don't change (first frame measured the same
before and after: 464 / 476 ms median). Entering Edit mode on the 32 GB recording: probe 284 ms, all 1254 keyframes
17 ms later (from the MP4 header, no packet scan).

Layout as in FastCut: menu strip (File / Edit / Playback / View / Tools), video, side panel with **Segments** (cards with
include checkbox, cut warnings, drag to reorder), **Tracks** (which streams to export, rotation override, stream
details, extract all) and **Files** (FastPlay's playlist: open, reorder, merge files, export all), the timeline
(ruler, thumbnail strip, waveform, keyframe marks, segment bars: drag to create, move, resize with Shift snapping to
keyframes, wheel zoom, middle/Alt drag pan), the transport row (`[` start box, transport, time box, volume, end box
`]`) and the export bar (tracks, loading progress, status, progress + cancel, Discord toggle and size limit, format,
cut mode, separate/merge, Export).

Everything FastCut does, with FastCut's default keys (F1 in Edit mode lists them; keys that Edit mode doesn't bind fall
back to View mode's MPC-HC keys; remap in `fastplay.ini` `[edit_keys]`):

- **Segments**: I / O set start / end, + add at playhead, S split, Ctrl+D duplicate, Delete, nudge start/end
  (Alt+Left/Right, Alt+Shift+Left/Right), PageUp/PageDown select, F2 label, Enter edit, X include/exclude, Ctrl+A /
  Ctrl+Shift+A include/exclude all, Y invert, Shift+S snap starts to keyframes, Ctrl+Up/Down reorder, sort, divide
  the timeline (N parts or a length), shift all, segments from chapters, Ctrl+Z / Ctrl+Y undo/redo (200 steps).
- **Playback/navigation**: Space, K, J / L slower/faster, 1-4 speeds, P play segment, Ctrl+L loop segment, , / . frame
  step (mouse back/forward too), Shift+, / Shift+. N frames, Left/Right and Shift+Left/Right seek, Ctrl+Left/Right
  keyframes, Up/Down cut points, Home/End, Ctrl+G time box.
- **Export** (Ctrl+E, Ctrl+Shift+E selected only): separate files or merged (with chapters from segments), keyframe
  cut / normal cut / smart cut (re-encodes only up to the first keyframe, NVENC with CPU fallback), formats, name
  templates, output folder, speed (FPS) change, rotation override, metadata/chapters, fast start, **Discord-ready**
  (H.264 stays lossless; HEVC/AV1/10-bit re-encoded to H.264, fitted under 10 / 50 / 500 MB or a custom limit),
  output verification, and the one-time "export to an SSD instead" tip when source and output share a hard disk.
- **Tools**: full-resolution snapshot (C), snapshots at segment starts or every N s, extract all tracks, merge files
  (lossless when they match, re-encoded otherwise), download URL / HLS, detect black / silence / scene changes, stream
  details, FFmpeg log (F12).
- **Projects**: segments auto-save per file in the **same place and format as FastCut** (`%APPDATA%\FastCut\projects`,
  same file names), so both programs see each other's segments; Ctrl+S saves `<name>-proj.llc` next to the file;
  load `.llc` (also by dropping it); import / export segments as CSV, YouTube chapters, CUE, SRT, MPlayer EDL,
  FFmpeg metadata, LosslessCut projects.
- **Batch export** (Files tab): every file with the current segments, each file's saved segments, or a start/end trim.

Faster than FastCut where it matters for big recordings:

- Every ffmpeg / ffprobe call on an OBS fragmented MP4 gets `-use_mfra_for pts` (and ffconcat lists get the same per
  file), so nothing ever reads all 1254 fragment headers; durations and keyframes come from FastPlay's header index.
- Timeline thumbnails come from FastPlay's keyframe decoder (hidden libmpv, keyframes only) instead of one ffmpeg per
  thumbnail.
- The waveform reads the whole file, so it loads automatically only for files under 2 GB or on an SSD; for a big file
  on a hard disk the status bar says so and *View > Show waveform* loads it on request.
- Leaving Edit mode releases the thumbnail decoder (working set 309 → 268 MB on the 32 GB file; View mode alone ~185 MB).

Also kept from the previous version: **Save clip (lossless)** (Ctrl+Shift+S) in View mode writes the A-B loop range
with `ffmpeg -c copy`, starting on the keyframe at or before A; and *Open in FastCut* (Tools menu) still hands the file
to the separate FastCut app if you prefer it.

Tested in the running app (off-screen, driven by `fpctl` commands) on the 32 GB recording: I/O marking, undo/redo,
include toggling, keyframe export (2 files), smart-cut merge, Discord ≤ 10 MB merge (8.6 MB, 720p H.264), all
"checked OK" by the output verification. The ported modules have their own tests in `tools\edit-tests` (core 80/80
checks, exporter all modes incl. the 32 GB file, timeline/cards render checks, dialogs OK-handler checks).

## Default keys (MPC-HC)

| Key | Action | Key | Action |
|---|---|---|---|
| Space / click video | Play/Pause | . | Stop |
| Ctrl+Right / Ctrl+Left | Frame step / back | Right / Left (Mouse X2 / X1) | Jump 5 s |
| Alt+Right / Alt+Left | Jump 20 s | Shift+Right / Shift+Left | Next / previous keyframe |
| Home | Go to start | Ctrl+G | Go to... |
| Ctrl+Up / Ctrl+Down / Ctrl+R | Faster / slower / normal speed | Num+ / Num- | Audio delay ±10 ms |
| Ctrl+7 | Playlist | Ctrl+U | Open URL |
| Alt+Num6 / Alt+Num4 | Rotate | Ctrl+Shift+D | Deinterlace |
| E, Ctrl+E | Edit mode | Ctrl+Shift+S | Save A-B clip (lossless) |
| Ctrl+F1 | Shortcut list | | |
| F2 / F1 | Subtitle delay ± | PageDown / PageUp | Next / previous chapter (or file) |
| Ctrl+PageDown / Ctrl+PageUp | Next / previous file | Ctrl+O | Open |
| Ctrl+C | Close | Alt+X | Exit |
| Alt+Enter, F11, double-click | Fullscreen | Escape | Exit fullscreen |
| Ctrl+A | Always on top | Ctrl+0 | Hide caption |
| Ctrl+1 / Ctrl+2 | Show/hide controls | 1 / 3 | Minimal / normal view |
| Alt+1 / Alt+2 / Alt+3 / Alt+4 | Zoom 50 / 100 / 200 % / auto-fit | Num4/6/8/2, Num5 | Pan, reset |
| Num9 / Num1, Ctrl+wheel | Video zoom in / out | Up / Down, wheel | Volume |
| Ctrl+M | Mute | A / Shift+A | Next / previous audio track |
| S / Shift+S | Next / previous subtitle | W | Subtitles on/off |
| Alt+I | Save image... | F5 | Save image (auto) |
| [ / ] / \ | A-B loop start / end / clear | Ctrl+4, Tab | Statistics |
| Shift+F10 | Properties | O | Options (opens fastplay.ini) |

## Settings

`%APPDATA%\FastPlay\fastplay.ini` (or `fastplay.ini` next to the exe for a portable install), written on first exit
with every setting and key binding listed. **O** opens it in Notepad; changes apply when you switch back to FastPlay.

| Setting | Default | |
|---|---|---|
| `single_instance` | 1 | Hand new files to the running player |
| `tray` | 0 | Closing hides to the tray and keeps libmpv warm: ~100 ms to first frame, ~125 MB working set while idle |
| `remember_position` | 1 | Resume where you left off |
| `autofit` / `autofit_percent` | 1 / 75 | Size the window to the video at 75 % of the screen (MPC-HC auto-fit) |
| `exact_seek` | 0 | Keyboard jumps land on exact frames instead of keyframes |
| `thumbnails` / `thumbnail_cache` | 1 / 150 | Seekbar previews |
| `early_video_output` | 1 | Create the GPU output while the file opens |
| `prefetch` | 1 | Read the start region in parallel with libmpv start-up |
| `seek_small` / `seek_medium` / `seek_large` | 1 / 5 / 20 | Jump sizes in seconds |
| `speed_step` | 0.25 | 0 = MPC-HC style doubling |
| `volume`, `volume_step`, `mute` | 100, 5, 0 | |
| `hwdec`, `vo` | d3d11va, gpu-next | |
| `screenshot_dir`, `screenshot_format` | Pictures\FastPlay, png | |
| `mpv_options` | | Extra mpv options: `name=value;name=value` |
| `after_playback` | 0 | 0 nothing, 1 next file, 2 repeat, 3 close, 4 exit |
| `volume_max` | 100 | 150 or 200 for boost |
| `taskbar_progress` | 1 | Taskbar progress and thumbnail buttons |
| `fastcut_path`, `ffmpeg_path` | | Override where FastCut / ffmpeg are found |

## Build

Needs Visual Studio 2019 or newer (C++ workload) and the Windows 10/11 SDK.

1. Put the mpv dev package (shinchiro `mpv-dev-x86_64-*.7z`) in `third_party\mpv` (`include\mpv\*.h` and
   `libmpv-2.dll`).
2. Run `build.cmd`. Output: `dist\fastplay.exe` + `dist\libmpv-2.dll` (hard link), and `tools\bench\fpbench.exe`.

Source map: `app.cpp` start-up, libmpv set-up, loading, events · `window.cpp` window, input, actions, menus,
fullscreen, tray · `controls.cpp` owner-drawn control bar and thumbnail popup · `mp4index.cpp` MP4 header index
(ported from FastCut's `Mp4Index.cs`, extended for fragmented MP4) · `thumbs.cpp` thumbnail decoder · `keys.cpp`
actions and MPC-HC default bindings · `settings.cpp` ini, settings, resume store · `features.cpp` playlist,
after playback, video/audio extras, taskbar, file types, FastCut and clip export, help · `dialogs.cpp` small dialogs.
`src/edit/` (Edit mode): `editor.cpp` layout, loading, jobs · `editactions.cpp` actions, keys, menus, segments, export
 and tools · `panel.cpp` flat buttons/strips · `timeline.cpp`, `segcards.cpp` (+ `uicanvas.h`) the two big controls ·
`exporter.cpp` all cut modes, Discord, merge, verify, tools · `media.cpp` probe, keyframes, waveform · `ffrun.cpp`
process runner (job objects, cancellation) · `segio.cpp` projects, import/export, JSON5 · `eddialogs.cpp` dialogs ·
`common.cpp` settings and helpers.

## Testing and scripting

- `FASTPLAY_TEST=1`: the window opens off every screen, muted, never activated, with its own instance and no
  writes to settings or history. `FASTPLAY_TIMING=<file>` appends start-up milestones (ms since process creation).
  `FASTPLAY_MPV_OPTS="name=value;..."` passes extra mpv options. `--start=SECONDS`, `--bench-exit`.
- `tools\fpctl.ps1 -ProcessId <pid> -Command "action <id>" | "seek <s>" | "hover <s>" | "open <path>" |
  "menu <id>" | "clip <out>" | "get <property>" | "state"` drives a running player over `WM_COPYDATA` (no simulated
  input; `get`/`state` write to the `FASTPLAY_TIMING` log). `-Shot <png> [-Class <window class>]` saves a PrintWindow
  image. `tools\run-test.ps1` and `tools\seektest.ps1` use it. `FASTPLAY_INSTANCE=<name>` gives a test run its own
  single-instance name (used to benchmark the warm tray instance).
- Edit mode in tests: `edit <action id>` runs an Edit-mode action, `editset cut_mode|merge|size_mb|discord|format
  <value>` changes export options, `editstate` logs the segments and status. In test mode jobs write only to
  `FASTPLAY_EDIT_OUT` (else FastPlay's cache folder), confirmation dialogs are skipped and message boxes are logged.
- `tools\edit-tests\build-*.cmd`: console/off-screen tests for the Edit-mode modules (core, export, ui, dialogs).
- `tools\bench\fpbench.exe`: player-agnostic time-to-first-frame (below). `--grab <pid> <class> <out.bmp>` saves a
  Windows.Graphics.Capture still of a window, off-screen ones included.

## Benchmarks

Measured 2026-11-22 with `tools\bench\run-bench.ps1` on the target machine (RTX 5090, 240 Hz 1440p monitor, 16
threads, 31 GB RAM). The files are OBS fragmented MP4s (H.264 2560x1440 60 fps, 49 Mbit/s, AAC) on a 12 TB 7200 rpm
hard disk (D:). "First frame" is from just before `CreateProcess` to the DWM composition time of the first captured
frame showing video in the sample region (Windows.Graphics.Capture), so both players are judged on what actually
reached the screen. MPC-HC is clsid2 MPC-HC x64 with the user's settings (MPC Video Renderer, internal LAV filters),
run as a portable copy. Both players start off-screen, muted, under the same no-focus launcher (adds ~10 ms).

| Scenario | FastPlay | MPC-HC | |
|---|---|---|---|
| **Warm**: 32 GB / 87 min file, cached, opened at 10:00 (5 runs each, interleaved, first run dropped) | **412 ms** median (374-448) | **1 489 ms** median (1 473-1 580) | 3.6x |
| **Cold**: never-opened ~5 GB parts of one recording, opened at 2:00 (one fresh file per run) | **405 ms** (404, 405, 456) | **8 802 / 8 994 ms**, third run no frame within 60 s | ~22x |
| **Warm tray instance** (`tray=1`): double-click → handoff → first frame, 32 GB file at 10:00 | **102 ms** median (96-116) | - | |
| Warm tray instance, never-opened parts at 2:00 | **98, 111 ms** | - | |
| Window on screen | 30-50 ms | ~500 ms | |
| Playing (10 s after first frame): CPU, % of one core | 7.0 % | 2.2 % | MPC-HC lower |
| Playing: private / working set | 289 / 180 MB | 310 / 197 MB | |
| Idle tray instance (`tray=1`, nothing open) | 268 / 125 MB | - | |

Honest notes:

- MPC-HC's cold time is the fragment scan described above (its LAV Splitter uses FFmpeg's default). Twice more
  during set-up MPC-HC sat at "Opening..." for 15-40 s on the 32 GB file; that didn't repeat in the measured runs,
  so it is not in the table, and the cause wasn't investigated.
- FastPlay's cold and warm times are about the same because its open reads only three small regions; what's left
  is per-process start-up: ~25 ms to `main`, libmpv load ~20 ms, and the first D3D11 device ~170-250 ms (NVIDIA
  driver initialisation), then decoder set-up and the first frame. Only the warm tray instance avoids it.
- **Playback CPU is higher than MPC-HC's** (7-10 % vs 2.2 % of one core, i.e. ~0.5 % vs ~0.15 % of this 16-thread
  CPU). Measured with each of these and none lowered it: thumbnails off, no audio, `vo=gpu`, `profile=fast`, no
  deband/dither, smaller or bigger demuxer buffers, a 30 s steady-state window, `d3d11-sync-interval=0`, Vulkan (11 %,
  and a 2.2 s first frame), NVDEC copy-back (14 %); d3d11va copy-back doubles it (16-20 %). So it's mpv's own
  playback loop on zero-copy d3d11va, which is already the cheapest path found.
- MPC-HC test copies took the foreground at start-up even with `/nofocus` and without inherited foreground rights
  (activating a hidden modeless dialog). fpbench therefore launches every player under a debugger and patches the
  user32/win32u focus and input exports before any player code runs (see `PatchFocusAndDetach`), with
  `_NO_DEBUG_HEAP=1` (otherwise the debug heap made start-up ~900 ms slower), and kills a player that still takes
  the foreground.

Re-run: `powershell -NoProfile -File tools\bench\run-bench.ps1 -WarmFile <big.mp4> -ColdFiles <a>,<b>,...`
(cold files must be ones nothing has read since boot; each is used once). Raw CSVs and the sample-region frames
of every detection go to `bench\results`.
