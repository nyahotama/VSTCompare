#include "vstcompare/latency_alignment.hpp"

#include <algorithm>

namespace vstcompare {
namespace {

AudioBuffer applyDelayKeepLength(const AudioBuffer& in, std::size_t delaySamples, std::size_t targetSamples) {
  AudioBuffer out = AudioBuffer::zeros(in.numChannels(), targetSamples, in.sampleRate);
  for (std::size_t ch = 0; ch < in.numChannels(); ++ch) {
    for (std::size_t i = delaySamples; i < targetSamples; ++i) {
      const std::size_t src = i - delaySamples;
      if (src < in.channels[ch].size()) {
        out.channels[ch][i] = in.channels[ch][src];
      }
    }
  }
  return out;
}

}  // namespace

LatencyAlignedBuffers alignForAnalysis(const AudioBuffer& outputA, const AudioBuffer& outputB,
                                       uint32_t latencyA, uint32_t latencyB) {
  LatencyAlignedBuffers aligned;
  const std::size_t channelCount = std::min(outputA.numChannels(), outputB.numChannels());
  const std::size_t sampleCount = std::min(outputA.numSamples(), outputB.numSamples());

  aligned.alignedA = AudioBuffer::zeros(channelCount, sampleCount, outputA.sampleRate);
  aligned.alignedB = AudioBuffer::zeros(channelCount, sampleCount, outputB.sampleRate);
  aligned.info.reportedA = latencyA;
  aligned.info.reportedB = latencyB;

  if (sampleCount == 0 || channelCount == 0) {
    return aligned;
  }

  const uint32_t maxValid = static_cast<uint32_t>(sampleCount - 1);
  const uint32_t clampedA = std::min(latencyA, maxValid);
  const uint32_t clampedB = std::min(latencyB, maxValid);

  aligned.info.clampedA = clampedA;
  aligned.info.clampedB = clampedB;
  if (clampedA != latencyA) {
    aligned.info.clampedOccurred = true;
    aligned.info.warnings.push_back("Plugin A reported latency exceeded sample window and was clamped.");
  }
  if (clampedB != latencyB) {
    aligned.info.clampedOccurred = true;
    aligned.info.warnings.push_back("Plugin B reported latency exceeded sample window and was clamped.");
  }

  const uint32_t target = std::max(clampedA, clampedB);
  const uint32_t delayA = target - clampedA;
  const uint32_t delayB = target - clampedB;
  aligned.info.appliedDelayA = delayA;
  aligned.info.appliedDelayB = delayB;

  AudioBuffer trimmedA = AudioBuffer::zeros(channelCount, sampleCount, outputA.sampleRate);
  AudioBuffer trimmedB = AudioBuffer::zeros(channelCount, sampleCount, outputB.sampleRate);
  for (std::size_t ch = 0; ch < channelCount; ++ch) {
    std::copy_n(outputA.channels[ch].begin(), sampleCount, trimmedA.channels[ch].begin());
    std::copy_n(outputB.channels[ch].begin(), sampleCount, trimmedB.channels[ch].begin());
  }

  aligned.alignedA = applyDelayKeepLength(trimmedA, delayA, sampleCount);
  aligned.alignedB = applyDelayKeepLength(trimmedB, delayB, sampleCount);
  return aligned;
}

}  // namespace vstcompare

