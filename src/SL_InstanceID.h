#pragma once
#include <cstdint>

// A pointer-free identity travels with AE's copies of an effect's sequence data.
// It only partitions the optional checkpoint cache, never the image/disk keys.
struct SL_InstanceID {
    std::uint64_t lo, hi;
    bool valid() const { return lo != 0 || hi != 0; }
    bool operator==(const SL_InstanceID& other) const { return lo == other.lo && hi == other.hi; }
};

inline bool SL_SameCheckpointOwner(PF_ProgPtr a, const SL_InstanceID& ai,
                                  PF_ProgPtr b, const SL_InstanceID& bi) {
    // Never alias a legacy/unknown owner with a known sequence identity.
    if (ai.valid() || bi.valid()) return ai.valid() && bi.valid() && ai == bi;
    return a == b;
}
