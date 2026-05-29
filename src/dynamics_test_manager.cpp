#include "vstcompare/dynamics_test_manager.hpp"

#include "vstcompare/analysis_signal.hpp"
#include "vstcompare/latency_alignment.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace vstcompare {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEps = 1e-12;

double rmsDbfs(const std::vector<float>& signal, std::size_t begin, std::size_t count) {
  if (signal.empty() || begin >= signal.size() || count == 0) return -120.0;
  const std::size_t end = std::min(signal.size(), begin + count);
  if (end <= begin) return -120.0;

  double sum = 0.0;
  for (std::size_t i = begin; i < end; ++i) {
    const double v = static_cast<double>(signal[i]);
    sum += v * v;
  }
  const double rms = std::sqrt(sum / static_cast<double>(end - begin));
  return 20.0 * std::log10(std::max(rms, kEps));
}

struct EstimatedFit {
  DynamicsEstimatedParams params;
  std::string warning;
};

EstimatedFit estimateParams(const std::vector<DynamicsPoint>& ioCurve, bool pluginB) {
  EstimatedFit fit;
  fit.params.ratio = 1.0;
  if (ioCurve.size() < 8) {
    fit.warning = "Dynamics estimate skipped: insufficient points for stable threshold/ratio/knee fitting.";
    return fit;
  }

  std::vector<double> x;
  std::vector<double> y;
  x.reserve(ioCurve.size());
  y.reserve(ioCurve.size());
  for (const auto& p : ioCurve) {
    x.push_back(p.inputLevelDbfs);
    y.push_back(pluginB ? p.outputLevelDbfsB : p.outputLevelDbfsA);
  }

  std::vector<double> slopes;
  slopes.reserve(x.size() - 1);
  for (std::size_t i = 0; i + 1 < x.size(); ++i) {
    const double dx = x[i + 1] - x[i];
    slopes.push_back((std::fabs(dx) <= kEps) ? 1.0 : (y[i + 1] - y[i]) / dx);
  }

  std::vector<double> smooth(slopes.size(), 1.0);
  for (std::size_t i = 0; i < slopes.size(); ++i) {
    const std::size_t from = (i >= 2) ? (i - 2) : 0;
    const std::size_t to = std::min(slopes.size() - 1, i + 2);
    double sum = 0.0;
    int n = 0;
    for (std::size_t j = from; j <= to; ++j) {
      sum += slopes[j];
      ++n;
    }
    smooth[i] = sum / static_cast<double>(std::max(1, n));
  }

  int thresholdIndex = -1;
  for (std::size_t i = 0; i < smooth.size(); ++i) {
    if (smooth[i] < 0.9) {
      thresholdIndex = static_cast<int>(i);
      break;
    }
  }
  if (thresholdIndex < 0) {
    fit.warning = "Dynamics estimate: no clear compression transition found; threshold/ratio/knee set to defaults.";
    return fit;
  }

  fit.params.thresholdDbfs = x[static_cast<std::size_t>(thresholdIndex)];

  const double ratioFitStart = fit.params.thresholdDbfs + 6.0;
  double sumSlope = 0.0;
  int slopeCount = 0;
  for (std::size_t i = 0; i < smooth.size(); ++i) {
    if (x[i] < ratioFitStart) continue;
    const double s = std::max(0.01, smooth[i]);
    sumSlope += s;
    ++slopeCount;
  }
  if (slopeCount > 0) {
    const double avgSlope = sumSlope / static_cast<double>(slopeCount);
    fit.params.ratio = std::clamp(1.0 / std::max(0.01, avgSlope), 1.0, 100.0);
  } else {
    fit.warning = "Dynamics estimate: insufficient compressed-region points for ratio fitting; ratio defaulted to 1.";
    return fit;
  }

  const double targetSlope = 1.0 / std::max(1.0, fit.params.ratio);
  int kneeStart = -1;
  int kneeEnd = -1;
  for (std::size_t i = 0; i < smooth.size(); ++i) {
    if (kneeStart < 0 && smooth[i] <= 0.98) {
      kneeStart = static_cast<int>(i);
    }
    if (kneeStart >= 0 && smooth[i] <= (targetSlope + 0.03)) {
      kneeEnd = static_cast<int>(i);
      break;
    }
  }
  if (kneeStart >= 0 && kneeEnd >= kneeStart) {
    fit.params.kneeWidthDb = std::max(0.0, x[static_cast<std::size_t>(kneeEnd)] - x[static_cast<std::size_t>(kneeStart)]);
  } else {
    fit.params.kneeWidthDb = 0.0;
  }

  fit.params.valid = true;
  return fit;
}

}  // namespace

namespace dynamics_detail {

std::vector<double> buildSweepLevels(const TestConfig& config) {
  std::vector<double> levels;
  const double start = static_cast<double>(config.dynamicsStartLevelDbfs);
  const double end = static_cast<double>(config.dynamicsEndLevelDbfs);
  const double step = static_cast<double>(config.dynamicsStepDb);
  if (step <= 0.0 || end < start) return levels;

  for (double level = start; level <= end + 1e-9; level += step) {
    levels.push_back(level);
  }
  return levels;
}

AudioBuffer generateStepSineInput(const TestConfig& config) {
  const std::vector<double> levels = buildSweepLevels(config);
  const std::size_t stepSamples =
      static_cast<std::size_t>(std::max<int64_t>(1, (static_cast<int64_t>(config.sampleRate) * config.dynamicsStepMs) / 1000));
  AudioBuffer input = AudioBuffer::zeros(1, levels.size() * stepSamples, config.sampleRate);
  if (levels.empty() || input.numSamples() == 0 || config.sampleRate <= 0) return input;

  for (std::size_t i = 0; i < levels.size(); ++i) {
    const double amp = std::pow(10.0, levels[i] / 20.0);
    const std::size_t base = i * stepSamples;
    for (std::size_t s = 0; s < stepSamples; ++s) {
      const std::size_t idx = base + s;
      const double phase = (2.0 * kPi * config.dynamicsToneFrequencyHz * static_cast<double>(idx)) /
                           static_cast<double>(config.sampleRate);
      input.channels[0][idx] = static_cast<float>(amp * std::sin(phase));
    }
  }
  return input;
}

}  // namespace dynamics_detail

DynamicsTestResult DynamicsTestManager::run(IPluginHost& pluginA, IPluginHost& pluginB, const TestConfig& config,
                                             std::string& error) const {
  DynamicsTestResult result;
  error.clear();

  const std::vector<double> levels = dynamics_detail::buildSweepLevels(config);
  if (levels.empty()) {
    error = "Dynamics test level sweep settings are invalid.";
    return {};
  }

  result.input = dynamics_detail::generateStepSineInput(config);
  result.outputA = pluginA.process(result.input, error);
  if (!error.empty()) return {};
  result.outputB = pluginB.process(result.input, error);
  if (!error.empty()) return {};

  if (result.outputA.numSamples() == 0 || result.outputB.numSamples() == 0) {
    error = "Dynamics test received empty audio output from one of the plugins.";
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
  const LatencyAlignedBuffers aligned = alignForAnalysis(result.analysisA, result.analysisB,
                                                         result.metrics.pluginLatencySamplesA,
                                                         result.metrics.pluginLatencySamplesB);
  result.alignedA = aligned.alignedA;
  result.alignedB = aligned.alignedB;
  result.latencyAlignment = aligned.info;

  if (result.alignedA.channels.empty() || result.alignedB.channels.empty()) {
    error = "Dynamics alignment returned empty analysis buffers.";
    return {};
  }

  const std::size_t sampleCount = std::min(result.alignedA.numSamples(), result.alignedB.numSamples());
  if (sampleCount == 0) {
    error = "Dynamics alignment produced zero-length output.";
    return {};
  }

  const std::size_t stepSamples =
      static_cast<std::size_t>(std::max<int64_t>(1, (static_cast<int64_t>(config.sampleRate) * config.dynamicsStepMs) / 1000));
  const std::size_t rmsSamples =
      static_cast<std::size_t>(std::max<int64_t>(1, (static_cast<int64_t>(config.sampleRate) * config.dynamicsRmsWindowMs) / 1000));

  double sumAbsIo = 0.0;
  double sumAbsGr = 0.0;
  std::size_t measured = 0;

  for (std::size_t i = 0; i < levels.size(); ++i) {
    const std::size_t stepStart = i * stepSamples;
    if (stepStart >= sampleCount) break;

    const std::size_t center = stepStart + (stepSamples / 2);
    const std::size_t half = rmsSamples / 2;
    const std::size_t windowStart = (center > half) ? (center - half) : 0;
    const std::size_t count = std::min(rmsSamples, sampleCount - windowStart);
    if (count == 0) break;

    DynamicsPoint p;
    p.inputLevelDbfs = levels[i];
    p.outputLevelDbfsA = rmsDbfs(result.alignedA.channels[0], windowStart, count);
    p.outputLevelDbfsB = rmsDbfs(result.alignedB.channels[0], windowStart, count);
    p.outputDeltaDb = p.outputLevelDbfsA - p.outputLevelDbfsB;
    p.gainReductionDbA = p.inputLevelDbfs - p.outputLevelDbfsA;
    p.gainReductionDbB = p.inputLevelDbfs - p.outputLevelDbfsB;
    p.gainReductionDeltaDb = p.gainReductionDbA - p.gainReductionDbB;

    sumAbsIo += std::fabs(p.outputDeltaDb);
    sumAbsGr += std::fabs(p.gainReductionDeltaDb);
    result.metrics.peakAbsOutputDeltaDb = std::max(result.metrics.peakAbsOutputDeltaDb, std::fabs(p.outputDeltaDb));
    result.metrics.peakAbsGainReductionDeltaDb =
        std::max(result.metrics.peakAbsGainReductionDeltaDb, std::fabs(p.gainReductionDeltaDb));
    ++measured;
    result.ioCurve.push_back(p);
  }

  if (result.ioCurve.empty()) {
    error = "Dynamics analysis produced no valid sweep points.";
    return {};
  }

  if (measured > 0) {
    result.metrics.meanAbsOutputDeltaDb = sumAbsIo / static_cast<double>(measured);
    result.metrics.meanAbsGainReductionDeltaDb = sumAbsGr / static_cast<double>(measured);
  }

  const EstimatedFit fitA = estimateParams(result.ioCurve, false);
  const EstimatedFit fitB = estimateParams(result.ioCurve, true);
  result.metrics.estimatedA = fitA.params;
  result.metrics.estimatedB = fitB.params;
  result.metrics.thresholdDeltaDb = result.metrics.estimatedA.thresholdDbfs - result.metrics.estimatedB.thresholdDbfs;
  result.metrics.ratioDelta = result.metrics.estimatedA.ratio - result.metrics.estimatedB.ratio;
  result.metrics.kneeWidthDeltaDb = result.metrics.estimatedA.kneeWidthDb - result.metrics.estimatedB.kneeWidthDb;

  if (!fitA.warning.empty()) {
    result.warnings.push_back("Dynamics estimate A: " + fitA.warning);
  }
  if (!fitB.warning.empty()) {
    result.warnings.push_back("Dynamics estimate B: " + fitB.warning);
  }

  return result;
}

}  // namespace vstcompare

