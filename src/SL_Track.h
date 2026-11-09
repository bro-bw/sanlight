#pragma once
#include <vector>

// Backward coordinates: each current pixel maps to a reference pixel.
// No previous-frame state: seeking and concurrent renders are deterministic.
struct SL_TrackResult {
    std::vector<float> depth;
    std::vector<float> confidence;
    std::vector<float> flowX, flowY;
    // Optional source-support rejection for temporal surfaces; owned by this result.
    // Empty means no additional rejection (legacy/small-layer/static paths).
    std::vector<float> historyReject;
};
typedef bool (*SL_TrackAbort)(void*);
// Motion-only callers can omit depth output and pass a null referenceDepth.
// In that mode, minimumAlpha zeroes final flow/confidence below the given coverage.
// Both directions of the motion search still use the complete image.
bool SL_TrackDepth(const float* referenceRGBA, const float* currentRGBA,
    const float* referenceDepth, int w, int h, int resolution, float smoothing,
    SL_TrackResult& result, SL_TrackAbort abort = nullptr, void* abortData = nullptr,
    bool illuminationRobust = false, bool keepQuietPatches = true,
    bool symmetricMotion = false, bool produceDepth = true, float minimumAlpha = 0.f);
