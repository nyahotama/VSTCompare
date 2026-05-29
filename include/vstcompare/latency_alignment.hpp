#pragma once

#include "vstcompare/types.hpp"

namespace vstcompare {

struct LatencyAlignedBuffers {
  AudioBuffer alignedA;
  AudioBuffer alignedB;
  LatencyAlignmentInfo info;
};

LatencyAlignedBuffers alignForAnalysis(const AudioBuffer& outputA, const AudioBuffer& outputB,
                                       uint32_t latencyA, uint32_t latencyB);

}  // namespace vstcompare

