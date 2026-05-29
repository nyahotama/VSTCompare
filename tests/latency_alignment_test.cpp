#include "vstcompare/latency_alignment.hpp"

#include <iostream>

using namespace vstcompare;

namespace {

AudioBuffer makeRamp(std::size_t channels, std::size_t samples) {
  AudioBuffer b = AudioBuffer::zeros(channels, samples, 48000);
  for (std::size_t ch = 0; ch < channels; ++ch) {
    for (std::size_t i = 0; i < samples; ++i) {
      b.channels[ch][i] = static_cast<float>(i + 1);
    }
  }
  return b;
}

bool approxEq(float a, float b) {
  const float d = a - b;
  return d < 1e-6f && d > -1e-6f;
}

}  // namespace

int main() {
  {
    auto a = makeRamp(2, 8);
    auto b = makeRamp(2, 8);
    auto r = alignForAnalysis(a, b, 2, 5);
    if (r.info.appliedDelayA != 3 || r.info.appliedDelayB != 0) {
      std::cerr << "Case1 applied delay mismatch\n";
      return 1;
    }
    if (!approxEq(r.alignedA.channels[0][0], 0.0f) || !approxEq(r.alignedA.channels[0][3], 1.0f)) {
      std::cerr << "Case1 aligned data mismatch\n";
      return 1;
    }
  }

  {
    auto a = makeRamp(1, 8);
    auto b = makeRamp(1, 8);
    auto r = alignForAnalysis(a, b, 4, 4);
    if (r.info.appliedDelayA != 0 || r.info.appliedDelayB != 0) {
      std::cerr << "Case2 no-shift mismatch\n";
      return 1;
    }
  }

  {
    auto a = makeRamp(1, 8);
    auto b = makeRamp(1, 8);
    auto r = alignForAnalysis(a, b, 99, 1);
    if (!r.info.clampedOccurred || r.info.clampedA != 7) {
      std::cerr << "Case3 clamp mismatch\n";
      return 1;
    }
    if (r.info.appliedDelayB != 6) {
      std::cerr << "Case3 applied delay mismatch\n";
      return 1;
    }
  }

  return 0;
}

