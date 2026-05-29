#include "vstcompare/ir_test_manager.hpp"
#include "vstcompare/analysis_signal.hpp"
#include "vstcompare/latency_alignment.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace vstcompare {
namespace {

std::size_t findAbsPeakIndex(const std::vector<float>& x) {
  if (x.empty()) return 0;
  float best = -1.0f;
  std::size_t idx = 0;
  for (std::size_t i = 0; i < x.size(); ++i) {
    const float v = std::fabs(x[i]);
    if (v > best) {
      best = v;
      idx = i;
    }
  }
  return idx;
}

}  // namespace

IrTestResult IrTestManager::run(IPluginHost& pluginA, IPluginHost& pluginB, const TestConfig& config,
                                std::string& error) {
  IrTestResult result;
  const std::size_t irSamples =
      static_cast<std::size_t>((static_cast<int64_t>(config.sampleRate) * config.irDurationMs) / 1000);

  result.input = AudioBuffer::zeros(1, irSamples, config.sampleRate);
  if (irSamples > 0) {
    result.input.channels[0][0] = config.impulseAmplitude;
  }

  result.outputA = pluginA.process(result.input, error);
  if (!error.empty()) return {};

  result.outputB = pluginB.process(result.input, error);
  if (!error.empty()) return {};

  const std::size_t sampleCount = std::min(result.outputA.numSamples(), result.outputB.numSamples());
  if (sampleCount == 0) {
    error = "IR test received empty audio output from one of the plugins.";
    return {};
  }

  std::string modeA;
  std::string modeB;
  result.analysisA = toMonoForAnalysis(result.outputA, modeA);
  result.analysisB = toMonoForAnalysis(result.outputB, modeB);
  result.analysisSignalMode = (modeA == "mono_lr_average" || modeB == "mono_lr_average")
                                  ? "mono_lr_average"
                                  : "single_channel";

  result.metrics.pluginLatencySamplesA = pluginA.getLatencySamples();
  result.metrics.pluginLatencySamplesB = pluginB.getLatencySamples();
  const LatencyAlignedBuffers aligned =
      alignForAnalysis(result.analysisA, result.analysisB, result.metrics.pluginLatencySamplesA,
                       result.metrics.pluginLatencySamplesB);
  result.alignedA = aligned.alignedA;
  result.alignedB = aligned.alignedB;
  result.latencyAlignment = aligned.info;

  const LatencyAlignedBuffers displayAligned =
      alignForAnalysis(result.outputA, result.outputB, result.metrics.pluginLatencySamplesA,
                       result.metrics.pluginLatencySamplesB);
  result.displayAlignedA = displayAligned.alignedA;
  result.displayAlignedB = displayAligned.alignedB;
  const std::size_t displayChannels =
      std::min(result.displayAlignedA.numChannels(), result.displayAlignedB.numChannels());
  const std::size_t displaySamples =
      std::min(result.displayAlignedA.numSamples(), result.displayAlignedB.numSamples());
  result.displayDelta = AudioBuffer::zeros(displayChannels, displaySamples, config.sampleRate);
  for (std::size_t ch = 0; ch < displayChannels; ++ch) {
    for (std::size_t i = 0; i < displaySamples; ++i) {
      result.displayDelta.channels[ch][i] =
          result.displayAlignedA.channels[ch][i] - result.displayAlignedB.channels[ch][i];
    }
  }

  const std::size_t alignedChannels = std::min(result.alignedA.numChannels(), result.alignedB.numChannels());
  result.delta = AudioBuffer::zeros(alignedChannels, sampleCount, config.sampleRate);
  for (std::size_t ch = 0; ch < alignedChannels; ++ch) {
    for (std::size_t i = 0; i < sampleCount; ++i) {
      result.delta.channels[ch][i] = result.alignedA.channels[ch][i] - result.alignedB.channels[ch][i];
    }
  }

  const auto& delta = result.delta.channels[0];
  float peak = 0.0f;
  double energy = 0.0;
  std::vector<IndexedDifference> ranked;
  ranked.reserve(sampleCount);
  for (std::size_t i = 0; i < sampleCount; ++i) {
    const float absd = std::fabs(delta[i]);
    peak = std::max(peak, absd);
    energy += static_cast<double>(delta[i]) * static_cast<double>(delta[i]);
    ranked.push_back({i, absd});
  }
  std::sort(ranked.begin(), ranked.end(),
            [](const IndexedDifference& a, const IndexedDifference& b) { return a.absoluteDelta > b.absoluteDelta; });

  const std::size_t aPeak = findAbsPeakIndex(result.alignedA.channels[0]);
  const std::size_t bPeak = findAbsPeakIndex(result.alignedB.channels[0]);
  const int64_t lagSamples = static_cast<int64_t>(aPeak) - static_cast<int64_t>(bPeak);

  result.metrics.peakAbsDelta = peak;
  result.metrics.energyDelta = energy;
  result.metrics.estimatedLatencyMs =
      (1000.0 * static_cast<double>(lagSamples)) / static_cast<double>(config.sampleRate);

  constexpr std::size_t kTopN = 5;
  const std::size_t n = std::min(kTopN, ranked.size());
  result.topDifferences.assign(ranked.begin(), ranked.begin() + n);

  return result;
}

}  // namespace vstcompare
