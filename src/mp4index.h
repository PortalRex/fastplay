// Keyframe times straight from an MP4/MOV header, so a 30+ GB recording is indexed in milliseconds.
// Ported from FastCut's Mp4Index.cs and extended for fragmented MP4 (OBS "fragmented_mp4"/hybrid MP4):
// those keep their random-access index in the trailing mfra/tfra box, read with two small reads.
#pragma once
#include "util.h"

struct MediaIndex {
    int width = 0, height = 0;       // video track size from tkhd (0 when unknown)
    double duration = 0;             // seconds (0 when unknown)
    bool fragmented = false;
    std::vector<double> keyTimes;    // keyframe presentation times in seconds, ascending
    std::vector<uint64_t> keyOffsets;// file offset of each keyframe's fragment (fragmented only)
};

// Reads header boxes only. allowFragmentScan: when a fragmented file has no mfra, walk every moof
// (one small read per fragment; slow on spinning disks, so only from a background thread).
bool ReadMediaIndex(const std::wstring& path, MediaIndex* out, bool allowFragmentScan);
