#pragma once
#include "SL_Track.h"

// Adjacent-pair, bounded-resolution motion. No persistent frame or initial-flow state.
// Failure/cancellation is propagated; it never silently substitutes cached geometry.
bool SL_DenseMotion(const float* previous,const float* current,int w,int h,
    SL_TrackResult& result,SL_TrackAbort abort=nullptr,void* data=nullptr);
