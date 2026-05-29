#include "vstcompare/time_response_test_manager.hpp"

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

std::vector<double> computeEnvelopeRms(const std::vector<float>& signal, std::size_t windowSamples) {
  std::vector<double> envelope(signal.size(), 0.0);
  if (signal.empty()) return envelope;

  const std::size_t half = windowSamples / 2;
  for (std::size_t i = 0; i < signal.size(); ++i) {
    const std::size_t begin = (i > half) ? (i - half) : 0;
    const std::size_t end = std::min(signal.size(), i + half + 1);
    if (end <= begin) continue;
    double sum = 0.0;
    for (std::size_t j = begin; j < end; ++j) {
      const double v = static_cast<double>(signal[j]);
      sum += v * v;
    }
    envelope[i] = std::sqrt(sum / static_cast<double>(end - begin));
  }
  return envelope;
}

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

double residualLagMs(const std::vector<float>& a, const std::vector<float>& b, int sampleRate, int maxLagSamples) {
  if (a.empty() || b.empty() || sampleRate <= 0) return 0.0;
  const int n = static_cast<int>(std::min(a.size(), b.size()));
  if (n <= 4) return 0.0;
  maxLagSamples = std::max(0, std::min(maxLagSamples, n - 2));

  double bestCorr = -std::numeric_limits<double>::infinity();
  int bestLag = 0;
  for (int lag = -maxLagSamples; lag <= maxLagSamples; ++lag) {
    double sum = 0.0;
    double ea = 0.0;
    double eb = 0.0;
    int count = 0;
    for (int i = 0; i < n; ++i) {
      const int j = i + lag;
      if (j < 0 || j >= n) continue;
      const double va = static_cast<double>(a[static_cast<std::size_t>(i)]);
      const double vb = static_cast<double>(b[static_cast<std::size_t>(j)]);
      sum += va * vb;
      ea += va * va;
      eb += vb * vb;
      ++count;
    }
    if (count < 8 || ea <= kEps || eb <= kEps) continue;
    const double corr = sum / std::sqrt(ea * eb);
    if (corr > bestCorr) {
      bestCorr = corr;
      bestLag = lag;
    }
  }

  return (1000.0 * static_cast<double>(bestLag)) / static_cast<double>(sampleRate);
}

struct EnvelopeTiming {
  double attackMs = 0.0;
  double releaseMs = 0.0;
  double postReleaseResidualDb = 0.0;
  std::string warning;
};

EnvelopeTiming measureEnvelopeTiming(const std::vector<double>& envelope, int sampleRate) {
  EnvelopeTiming timing;
  if (envelope.size() < 16 || sampleRate <= 0) {
    timing.warning = "Time response analysis skipped: insufficient samples for envelope timing estimation.";
    return timing;
  }

  const auto peakIt = std::max_element(envelope.begin(), envelope.end());
  const std::size_t peakIdx = static_cast<std::size_t>(std::distance(envelope.begin(), peakIt));
  const double peak = *peakIt;
  if (peak <= kEps) {
    timing.warning = "Time response analysis found near-silence output; attack/release defaults were used.";
    return timing;
  }

  const double t10 = peak * 0.1;
  const double t90 = peak * 0.9;

  std::size_t rise10 = 0;
  bool foundRise10 = false;
  for (std::size_t i = 0; i <= peakIdx; ++i) {
    if (envelope[i] >= t10) {
      rise10 = i;
      foundRise10 = true;
      break;
    }
  }
  if (!foundRise10) {
    timing.warning = "Time response attack estimation failed: could not find 10% rise crossing.";
    return timing;
  }

  std::size_t rise90 = rise10;
  bool foundRise90 = false;
  for (std::size_t i = rise10; i <= peakIdx; ++i) {
    if (envelope[i] >= t90) {
      rise90 = i;
      foundRise90 = true;
      break;
    }
  }
  if (!foundRise90 || rise90 < rise10) {
    timing.warning = "Time response attack estimation failed: could not find 90% rise crossing.";
    return timing;
  }
  timing.attackMs = (1000.0 * static_cast<double>(rise90 - rise10)) / static_cast<double>(sampleRate);

  std::size_t fall90 = peakIdx;
  bool foundFall90 = false;
  for (std::size_t i = peakIdx; i < envelope.size(); ++i) {
    if (envelope[i] <= t90) {
      fall90 = i;
      foundFall90 = true;
      break;
    }
  }
  if (!foundFall90) {
    timing.warning = "Time response release estimation failed: could not find 90% fall crossing.";
    return timing;
  }

  std::size_t fall10 = fall90;
  bool foundFall10 = false;
  for (std::size_t i = fall90; i < envelope.size(); ++i) {
    if (envelope[i] <= t10) {
      fall10 = i;
      foundFall10 = true;
      break;
    }
  }
  if (!foundFall10 || fall10 < fall90) {
    timing.warning = "Time response release estimation failed: could not find 10% fall crossing.";
    return timing;
  }
  timing.releaseMs = (1000.0 * static_cast<double>(fall10 - fall90)) / static_cast<double>(sampleRate);

  const std::size_t tailBegin = std::min(fall10, envelope.size() - 1);
  double tailPower = 0.0;
  std::size_t tailCount = 0;
  for (std::size_t i = tailBegin; i < envelope.size(); ++i) {
    const double v = envelope[i];
    tailPower += v * v;
    ++tailCount;
  }
  const double tailRms = (tailCount > 0) ? std::sqrt(tailPower / static_cast<double>(tailCount)) : 0.0;
  timing.postReleaseResidualDb = 20.0 * std::log10((tailRms + kEps) / (peak + kEps));
  return timing;
}

}  // namespace

namespace time_response_detail {

AudioBuffer generateToneBurstInput(const TestConfig& config) {
  const std::size_t preSamples =
      static_cast<std::size_t>(std::max<int64_t>(0, (static_cast<int64_t>(config.sampleRate) * config.timePreSilenceMs) / 1000));
  const std::size_t onSamples =
      static_cast<std::size_t>(std::max<int64_t>(0, (static_cast<int64_t>(config.sampleRate) * config.timeBurstOnMs) / 1000));
  const std::size_t postSamples =
      static_cast<std::size_t>(std::max<int64_t>(0, (static_cast<int64_t>(config.sampleRate) * config.timePostSilenceMs) / 1000));
  const std::size_t totalSamples = preSamples + onSamples + postSamples;
  AudioBuffer input = AudioBuffer::zeros(1, totalSamples, config.sampleRate);
  if (totalSamples == 0 || onSamples == 0 || config.sampleRate <= 0) return input;

  const double amp = std::pow(10.0, static_cast<double>(config.timeBurstLevelDbfs) / 20.0);
  for (std::size_t i = 0; i < onSamples; ++i) {
    const std::size_t idx = preSamples + i;
    const double phase = (2.0 * kPi * config.timeBurstFrequencyHz * static_cast<double>(i)) /
                         static_cast<double>(config.sampleRate);
    input.channels[0][idx] = static_cast<float>(amp * std::sin(phase));
  }
  return input;
}

}  // namespace time_response_detail

TimeResponseTestResult TimeResponseTestManager::run(IPluginHost& pluginA, IPluginHost& pluginB, const TestConfig& config,
                                                     std::string& error) const {
  TimeResponseTestResult result;
  error.clear();

  result.input = time_response_detail::generateToneBurstInput(config);
  result.outputA = pluginA.process(result.input, error);
  if (!error.empty()) return {};
  result.outputB = pluginB.process(result.input, error);
  if (!error.empty()) return {};

  if (result.outputA.numSamples() == 0 || result.outputB.numSamples() == 0) {
    error = "Time response test received empty audio output from one of the plugins.";
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
    error = "Time response alignment returned empty analysis buffers.";
    return {};
  }

  const std::size_t sampleCount = std::min(result.alignedA.numSamples(), result.alignedB.numSamples());
  if (sampleCount == 0) {
    error = "Time response alignment produced zero-length output.";
    return {};
  }

  const std::size_t envelopeWindow =
      static_cast<std::size_t>(std::max<int64_t>(1, (static_cast<int64_t>(config.sampleRate) * config.timeEnvelopeWindowMs) / 1000));
  const std::vector<double> envA = computeEnvelopeRms(result.alignedA.channels[0], envelopeWindow);
  const std::vector<double> envB = computeEnvelopeRms(result.alignedB.channels[0], envelopeWindow);

  const EnvelopeTiming timingA = measureEnvelopeTiming(envA, config.sampleRate);
  const EnvelopeTiming timingB = measureEnvelopeTiming(envB, config.sampleRate);
  result.metrics.attackMsA = timingA.attackMs;
  result.metrics.attackMsB = timingB.attackMs;
  result.metrics.attackDeltaMs = timingA.attackMs - timingB.attackMs;
  result.metrics.releaseMsA = timingA.releaseMs;
  result.metrics.releaseMsB = timingB.releaseMs;
  result.metrics.releaseDeltaMs = timingA.releaseMs - timingB.releaseMs;
  result.metrics.postReleaseResidualDbA = timingA.postReleaseResidualDb;
  result.metrics.postReleaseResidualDbB = timingB.postReleaseResidualDb;
  result.metrics.postReleaseResidualDeltaDb = timingA.postReleaseResidualDb - timingB.postReleaseResidualDb;
  if (!timingA.warning.empty()) result.warnings.push_back("Time response estimate A: " + timingA.warning);
  if (!timingB.warning.empty()) result.warnings.push_back("Time response estimate B: " + timingB.warning);

  const int maxLagSamples =
      std::max(1, (config.sampleRate * std::max(1, config.timeCorrelationMaxLagMs)) / 1000);
  result.metrics.residualLatencyMs = residualLagMs(result.alignedA.channels[0], result.alignedB.channels[0],
                                                   config.sampleRate, maxLagSamples);

  const std::size_t hopSamples = static_cast<std::size_t>(std::max(1, config.sampleRate / 1000));
  result.curve.reserve((sampleCount / hopSamples) + 1);
  for (std::size_t i = 0; i < sampleCount; i += hopSamples) {
    TimeResponsePoint p;
    p.timeMs = (1000.0 * static_cast<double>(i)) / static_cast<double>(config.sampleRate);
    p.envelopeDbA = 20.0 * std::log10(std::max(envA[i], kEps));
    p.envelopeDbB = 20.0 * std::log10(std::max(envB[i], kEps));
    p.deltaDb = p.envelopeDbA - p.envelopeDbB;
    if (!std::isfinite(p.envelopeDbA)) p.envelopeDbA = -120.0;
    if (!std::isfinite(p.envelopeDbB)) p.envelopeDbB = -120.0;
    if (!std::isfinite(p.deltaDb)) p.deltaDb = 0.0;
    result.curve.push_back(p);
  }

  const std::size_t postStart = std::min(sampleCount - 1, static_cast<std::size_t>(
      std::max<int64_t>(0, (static_cast<int64_t>(config.sampleRate) * (config.timePreSilenceMs + config.timeBurstOnMs)) / 1000)));
  const std::size_t postLen = sampleCount - postStart;
  if (postLen < static_cast<std::size_t>(std::max(8, config.sampleRate / 500))) {
    result.warnings.push_back("Time response post-release window is short; residual ringing metric may be unstable.");
  } else {
    const double postDbA = rmsDbfs(result.alignedA.channels[0], postStart, postLen);
    const double postDbB = rmsDbfs(result.alignedB.channels[0], postStart, postLen);
    if (!std::isfinite(postDbA) || !std::isfinite(postDbB)) {
      result.warnings.push_back("Time response residual ringing metric encountered non-finite values.");
    }
  }

  return result;
}

}  // namespace vstcompare

