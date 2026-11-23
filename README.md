# FastPlay

A small, fast Windows video player built to replace MPC-HC for everyday playback, especially for huge OBS
recordings (30+ GB, 1 h+, 1440p60). Native Win32 C++ around libmpv: no .NET, no Qt, no scripts. The exe is
~450 KB; libmpv-2.dll sits next to it.

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

## FastCut

- **Edit in FastCut** (Ctrl+E): opens the file in [FastCut](https://github.com/PortalRex/fastcut) at the current
  position, with the A-B loop (if set) as a selected segment (`FastCut.exe <file> --seek=S --segment=A,B`). FastCut is
  found next to fastplay.exe, in `..\FastCut\`, in a side-by-side `fastcut\dist` checkout, or at `fastcut_path`.
- **Save clip (lossless)** (Ctrl+Shift+S): writes the A-B range with `ffmpeg -c copy` in the background (ffmpeg from
  PATH or `ffmpeg_path`). The clip starts on the keyframe at or before A. It uses FastPlay's own keyframe index plus
  `-copyts` and an output-side cut, so fragmented OBS files skip the slow fragment scan and the clip doesn't start a
  fragment early (checked: a 10.0 s A-B range gave a 10.1 s clip starting on the keyframe; a partial file is removed
  on failure or exit). For frame-exact cuts use FastCut.

FastCut stays its own program: it's a C# editor (timeline, segments, smart cut, export queue), and merging it into
this C++ player would mean rewriting it. The two hand over to each other instead.

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
| Ctrl+E | Edit in FastCut | Ctrl+Shift+S | Save A-B clip (lossless) |
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

## Testing and scripting

- `FASTPLAY_TEST=1`: the window opens off every screen, muted, never activated, with its own instance and no
  writes to settings or history. `FASTPLAY_TIMING=<file>` appends start-up milestones (ms since process creation).
  `FASTPLAY_MPV_OPTS="name=value;..."` passes extra mpv options. `--start=SECONDS`, `--bench-exit`.
- `tools\fpctl.ps1 -ProcessId <pid> -Command "action <id>" | "seek <s>" | "hover <s>" | "open <path>" |
  "menu <id>" | "clip <out>" | "get <property>" | "state"` drives a running player over `WM_COPYDATA` (no simulated
  input; `get`/`state` write to the `FASTPLAY_TIMING` log). `-Shot <png> [-Class <window class>]` saves a PrintWindow
  image. `tools\run-test.ps1` and `tools\seektest.ps1` use it. `FASTPLAY_INSTANCE=<name>` gives a test run its own
  single-instance name (used to benchmark the warm tray instance).
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
