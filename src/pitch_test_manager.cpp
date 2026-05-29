#include "vstcompare/pitch_test_manager.hpp"

#include "vstcompare/analysis_signal.hpp"
#include "vstcompare/latency_alignment.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace vstcompare {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEps = 1e-12;

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

double safeClampHz(double hz, double minHz, double maxHz) {
  if (!std::isfinite(hz)) return 0.0;
  if (hz < minHz || hz > maxHz) return 0.0;
  return hz;
}

}  // namespace

namespace pitch_detail {

AudioBuffer generateLogSweepInput(const TestConfig& config) {
  const std::size_t sampleCount =
      static_cast<std::size_t>((static_cast<int64_t>(config.sampleRate) * config.pitchDurationMs) / 1000);
  AudioBuffer input = AudioBuffer::zeros(1, sampleCount, config.sampleRate);
  if (sampleCount == 0 || config.sampleRate <= 0) return input;

  const double f1 = std::max(1.0, static_cast<double>(config.pitchStartHz));
  const double f2 = std::max(f1 + 1.0, static_cast<double>(config.pitchEndHz));
  const double durationSec = static_cast<double>(sampleCount) / static_cast<double>(config.sampleRate);
  const double amplitude = std::pow(10.0, static_cast<double>(config.pitchLevelDbfs) / 20.0);
  const double ratio = f2 / f1;
  const double logRatio = std::log(ratio);

  for (std::size_t i = 0; i < sampleCount; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(config.sampleRate);
    const double phase = (2.0 * kPi * f1 * (std::exp((t / durationSec) * logRatio) - 1.0) * durationSec) / logRatio;
    input.channels[0][i] = static_cast<float>(amplitude * std::sin(phase));
  }

  const std::size_t fadeSamples =
      std::min<std::size_t>(sampleCount / 2,
                            static_cast<std::size_t>(std::max<int64_t>(
                                0, (static_cast<int64_t>(config.sampleRate) * config.pitchFadeMs) / 1000)));
  if (fadeSamples > 0) {
    const double denom = static_cast<double>(std::max<std::size_t>(1, fadeSamples - 1));
    for (std::size_t i = 0; i < fadeSamples; ++i) {
      const float inGain = static_cast<float>(static_cast<double>(i) / denom);
      const float outGain = static_cast<float>(static_cast<double>(i) / denom);
      input.channels[0][i] *= inGain;
      input.channels[0][sampleCount - 1 - i] *= outGain;
    }
  }
  return input;
}

double estimatePitchHzFrame(const std::vector<float>& signal, std::size_t begin, std::size_t count, int sampleRate,
                            double minHz, double maxHz) {
  if (signal.empty() || sampleRate <= 0 || count < 8 || begin >= signal.size()) return 0.0;
  const std::size_t clampedCount = std::min(count, signal.size() - begin);
  if (clampedCount < 8) return 0.0;
  double peak = 0.0;
  for (std::size_t i = 0; i < clampedCount; ++i) {
    peak = std::max(peak, std::fabs(static_cast<double>(signal[begin + i])));
  }
  if (peak < 1e-5) return 0.0;

  std::vector<double> risingCrossings;
  risingCrossings.reserve(clampedCount / 4);
  for (std::size_t i = 1; i < clampedCount; ++i) {
    const double prev = static_cast<double>(signal[begin + i - 1]);
    const double cur = static_cast<double>(signal[begin + i]);
    if (prev <= 0.0 && cur > 0.0) {
      const double denom = (cur - prev);
      const double frac = (std::fabs(denom) > kEps) ? (-prev / denom) : 0.0;
      const double crossingSample = static_cast<double>(i - 1) + std::clamp(frac, 0.0, 1.0);
      risingCrossings.push_back(crossingSample);
    }
  }
  if (risingCrossings.size() < 2) return 0.0;

  const double spanSamples = risingCrossings.back() - risingCrossings.front();
  if (spanSamples <= 1.0) return 0.0;
  const double cycles = static_cast<double>(risingCrossings.size() - 1);
  const double hz = cycles * static_cast<double>(sampleRate) / spanSamples;
  return safeClampHz(hz, minHz, maxHz);
}

}  // namespace pitch_detail

PitchTestResult PitchCorrectionTrackingTestManager::run(IPluginHost& pluginA, IPluginHost& pluginB,
                                                         const TestConfig& config, std::string& error) const {
  PitchTestResult result;
  error.clear();

  result.input = pitch_detail::generateLogSweepInput(config);
  result.outputA = pluginA.process(result.input, error);
  if (!error.empty()) return {};
  result.outputB = pluginB.process(result.input, error);
  if (!error.empty()) return {};

  const std::size_t sampleCount = std::min(result.outputA.numSamples(), result.outputB.numSamples());
  if (sampleCount == 0) {
    error = "Pitch test received empty audio output from one of the plugins.";
    return {};
  }

  std::string modeA;
  std::string modeB;
  result.analysisA = toMonoForAnalysis(result.outputA, modeA);
  result.analysisB = toMonoForAnalysis(result.outputB, modeB);
  result.analysisSignalMode =
      (modeA == "mono_lr_average" || modeB == "mono_lr_average") ? "mono_lr_average" : "single_channel";

  result.metrics.pluginLatencySamplesA = pluginA.getLatencySamples();
  result.metrics.pluginLatencySamplesB = pluginB.getLatencySamples();
  const LatencyAlignedBuffers aligned =
      alignForAnalysis(result.analysisA, result.analysisB, result.metrics.pluginLatencySamplesA,
                       result.metrics.pluginLatencySamplesB);
  result.alignedA = aligned.alignedA;
  result.alignedB = aligned.alignedB;
  result.latencyAlignment = aligned.info;

  if (result.alignedA.channels.empty() || result.alignedB.channels.empty()) {
    error = "Pitch alignment returned empty analysis buffers.";
    return {};
  }

  const std::size_t alignedSamples = std::min(result.alignedA.numSamples(), result.alignedB.numSamples());
  if (alignedSamples == 0) {
    error = "Pitch alignment produced zero-length output.";
    return {};
  }

  std::vector<float> inputForReference(alignedSamples, 0.0f);
  if (!result.input.channels.empty()) {
    const std::size_t copyCount = std::min(alignedSamples, result.input.channels[0].size());
    std::copy_n(result.input.channels[0].begin(), copyCount, inputForReference.begin());
  }

  const std::size_t frameSamples = static_cast<std::size_t>(
      std::max<int64_t>(64, (static_cast<int64_t>(config.sampleRate) * config.pitchFrameMs) / 1000));
  const std::size_t hopSamples =
      static_cast<std::size_t>(std::max<int64_t>(1, (static_cast<int64_t>(config.sampleRate) * config.pitchHopMs) / 1000));
  if (frameSamples >= alignedSamples) {
    result.warnings.push_back(
        "Pitch frame window is larger than available samples after alignment; running single-frame analysis.");
  }

  const std::size_t maxStart = (alignedSamples > frameSamples) ? (alignedSamples - frameSamples) : 0;
  double sumAbsErrA = 0.0;
  double sumAbsErrB = 0.0;
  double sumAbsDelta = 0.0;
  std::size_t validA = 0;
  std::size_t validB = 0;
  std::size_t validDelta = 0;
  std::size_t frameCount = 0;

  for (std::size_t start = 0; start <= maxStart; start += hopSamples) {
    const std::size_t count = std::min(frameSamples, alignedSamples - start);
    if (count < 64) break;

    PitchCurvePoint p;
    p.timeMs = 1000.0 * static_cast<double>(start + (count / 2)) / static_cast<double>(config.sampleRate);
    p.inputHz = pitch_detail::estimatePitchHzFrame(inputForReference, start, count, config.sampleRate,
                                                   config.pitchStartHz, config.pitchEndHz);
    p.pitchHzA = pitch_detail::estimatePitchHzFrame(result.alignedA.channels[0], start, count, config.sampleRate,
                                                    config.pitchStartHz, config.pitchEndHz);
    p.pitchHzB = pitch_detail::estimatePitchHzFrame(result.alignedB.channels[0], start, count, config.sampleRate,
                                                    config.pitchStartHz, config.pitchEndHz);

    if (p.inputHz > 0.0 && p.pitchHzA > 0.0) {
      sumAbsErrA += std::fabs(p.pitchHzA - p.inputHz);
      ++validA;
    }
    if (p.inputHz > 0.0 && p.pitchHzB > 0.0) {
      sumAbsErrB += std::fabs(p.pitchHzB - p.inputHz);
      ++validB;
    }
    if (p.pitchHzA > 0.0 && p.pitchHzB > 0.0) {
      const double deltaHz = std::fabs(p.pitchHzA - p.pitchHzB);
      sumAbsDelta += deltaHz;
      result.metrics.peakAbsDeltaHz = std::max(result.metrics.peakAbsDeltaHz, deltaHz);
      ++validDelta;
    }

    result.curve.push_back(p);
    ++frameCount;

    if (frameSamples >= alignedSamples) break;
  }

  if (frameCount == 0) {
    error = "Pitch analysis produced no valid frames.";
    return {};
  }

  result.metrics.meanAbsErrorHzA = (validA > 0) ? (sumAbsErrA / static_cast<double>(validA)) : 0.0;
  result.metrics.meanAbsErrorHzB = (validB > 0) ? (sumAbsErrB / static_cast<double>(validB)) : 0.0;
  result.metrics.meanAbsDeltaHz = (validDelta > 0) ? (sumAbsDelta / static_cast<double>(validDelta)) : 0.0;
  result.metrics.validFrameRateA = static_cast<double>(validA) / static_cast<double>(frameCount);
  result.metrics.validFrameRateB = static_cast<double>(validB) / static_cast<double>(frameCount);

  const std::size_t peakA = findAbsPeakIndex(result.alignedA.channels[0]);
  const std::size_t peakB = findAbsPeakIndex(result.alignedB.channels[0]);
  const int64_t lagSamples = static_cast<int64_t>(peakA) - static_cast<int64_t>(peakB);
  result.metrics.estimatedLatencyMs =
      (1000.0 * static_cast<double>(lagSamples)) / static_cast<double>(config.sampleRate);

  if (result.metrics.validFrameRateA < 0.5 || result.metrics.validFrameRateB < 0.5) {
    result.warnings.push_back("Pitch tracking valid frame rate is low; output may be noise-like or weakly tonal.");
  }

  return result;
}

}  // namespace vstcompare
