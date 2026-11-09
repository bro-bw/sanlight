#pragma once
#include "SL_Track.h"

// Surface layout: depth[w*h], followed by encoded RGB normals[w*h*3].
bool SL_TrackSurfaceMotion(const float* previous, const float* current, int w, int h,
    SL_TrackResult& flow, SL_TrackAbort abort, void* abortData);
bool SL_TemporalSurface(const float* oldImage, const float* image,
    const float* oldSurface, float* surface, int w, int h, float strength,
    float response, bool align, float seconds,
    SL_TrackAbort abort = nullptr, void* abortData = nullptr, bool rotateNormals = false,
    SL_TrackResult* preparedFlow = nullptr);
