// Probing, keyframes and waveform (FF.cs Probe/Keyframes/Waveform, Models.cs MediaInfo.Parse).
#pragma once
#include "common.h"

namespace edit {

// Fast input options for a source file: {"-use_mfra_for", "pts"} for fragmented MP4/MOV that carry an mfra (OBS),
// so ffmpeg/ffprobe skip reading every fragment header at open; empty otherwise. Cached per path; cheap (two small
// header reads the first time). Put these right before "-i <path>".
std::vector<std::wstring> FastInputOptions(const std::wstring& path);

// ffprobe -show_format -show_streams -show_chapters (with FastInputOptions). For fragmented MP4 the container
// duration ffprobe reports in mfra mode is incomplete, so duration comes from FastPlay's MP4 header index instead.
MediaInfo Probe(const std::wstring& path, Cancel* ct);
MediaInfo ParseProbe(const std::wstring& path, const std::string& json);  // Models.cs MediaInfo.Parse
double ProbeDuration(const std::wstring& path, Cancel* ct);

// MP4/MOV header first (regular sample tables or fragmented mfra; milliseconds), else ffprobe packet flags.
KeyframeIndex Keyframes(const MediaInfo& m, int stream, Cancel* ct, const std::function<void(double)>& progress = {});

constexpr int kWaveRate = 100;  // peaks per second
// Peak amplitude (0..1) per 1/kWaveRate s of one audio stream, written into target[0..count) while decoding (so it
// can be drawn as it arrives); long files are decoded in parallel time ranges. 'filled' counts written peaks.
void Waveform(const MediaInfo& m, int stream, float* target, size_t count, std::atomic<size_t>* filled, Cancel* ct);

}  // namespace edit
