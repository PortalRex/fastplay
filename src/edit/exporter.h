// Export.cs: lossless segment export (separate or merged; keyframe / exact ("normal") / smart cut), Discord-ready
// re-encodes fitted to a size limit, output verification, and the other ffmpeg tools.
#pragma once
#include "common.h"

namespace edit {

class Exporter {
public:
    // streams: kept stream indexes. rotation: -1 = keep, else 0/90/180/270 override. speed: FPS change factor (1 = none).
    Exporter(const MediaInfo& m, const EditSettings& s, std::vector<int> streams, int rotation, double speed,
             const KeyframeIndex& kfi, ProgressFn progress);

    static bool NeedsReencode(const MediaInfo& m, const EditSettings& s, const std::vector<int>& streams, double seconds);
    // Blocking; returns the written files. Throws EditError / Cancelled. Temp files are always removed.
    std::vector<std::wstring> Run(const std::vector<Segment>& segs, Cancel* ct);
    // One line per problem; empty when every output looks right.
    std::vector<std::wstring> Verify(const std::vector<Segment>& segs, const std::vector<std::wstring>& outs, Cancel* ct);
    bool Transcoded() const { return transcode_; }

    static std::vector<std::wstring> FastStartArgs(bool enabled, const std::wstring& ext, double dur,
                                                   const std::vector<StreamInfo>& streams);
    static bool NvencAvailable();  // checked once (ffmpeg h264_nvenc test encode), cached
    static std::wstring ConcatPath(const std::wstring& p);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    bool transcode_ = false;
};

namespace tools {
std::wstring Snapshot(const MediaInfo& m, double t, const std::wstring& outDir, const std::wstring& fmt, Cancel* ct);
void SnapshotsEvery(const MediaInfo& m, double interval, double start, double end, const std::wstring& outDir,
                    const std::wstring& fmt, ProgressFn progress, Cancel* ct);
std::vector<std::wstring> ExtractTracks(const MediaInfo& m, const std::vector<StreamInfo>& streams,
                                        const std::wstring& outDir, ProgressFn progress, Cancel* ct);
// Why the files can't be joined losslessly ("" when they can).
std::wstring ConcatMismatch(const std::vector<std::wstring>& files, ProgressFn progress, Cancel* ct);
void ConcatFiles(const std::vector<std::wstring>& files, const std::wstring& outPath, bool reencode, int sizeMB,
                 ProgressFn progress, Cancel* ct);
void Download(const std::wstring& url, const std::wstring& outPath, ProgressFn progress, Cancel* ct);
std::vector<std::pair<double, double>> DetectBlack(const MediaInfo& m, double minDur, double pixTh, ProgressFn progress, Cancel* ct);
std::vector<std::pair<double, double>> DetectSilence(const MediaInfo& m, double noiseDb, double minDur, ProgressFn progress, Cancel* ct);
std::vector<double> DetectScenes(const MediaInfo& m, double threshold, ProgressFn progress, Cancel* ct);
}  // namespace tools

}  // namespace edit
