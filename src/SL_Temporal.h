#pragma once
#include "SL_Track.h"

// The previous map is a stabilized state, not a fresh independent AI estimate.
bool SL_TemporalDepth(const float* previousRGBA,const float* currentRGBA,
    const float* previousDepth,float* currentDepth,int width,int height,
    float strength,float response,bool alignMotion,float frameSeconds,
    SL_TrackAbort abort=nullptr,void* abortData=nullptr);

// All history is supplied by the caller; no playback-order state is retained.
// Refines depth only; the host rebuilds normals from that refined depth.
bool SL_TemporalRefine(const float* previousRGBA, const float* currentRGBA,
    const float* previousDepth, const float* previousNormal,
    float* currentDepth, float* currentNormal, int width, int height,
    float strength, float response, bool alignMotion,
    SL_TrackAbort abort = nullptr, void* abortData = nullptr);
