#include "vstcompare/phase_test_manager.hpp"

#include "vstcompare/analysis_signal.hpp"
#include "vstcompare/frequency_test_manager.hpp"
#include "vstcompare/latency_alignment.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <vector>

namespace vstcompare {
namespace {

constexpr double kPi = 3.14159265358979323846;

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

void fft(std::vector<std::complex<double>>& a) {
  const std::size_t n = a.size();
  std::size_t j = 0;
  for (std::size_t i = 1; i < n; ++i) {
    std::size_t bit = n >> 1;
    while (j & bit) {
      j ^= bit;
      bit >>= 1;
    }
    j ^= bit;
    if (i < j) {
      std::swap(a[i], a[j]);
    }
  }

  for (std::size_t len = 2; len <= n; len <<= 1) {
    const double ang = -2.0 * kPi / static_cast<double>(len);
    const std::complex<double> wlen(std::cos(ang), std::sin(ang));
    for (std::size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (std::size_t k = 0; k < len / 2; ++k) {
        const std::complex<double> u = a[i + k];
        const std::complex<double> v = a[i + k + (len / 2)] * w;
        a[i + k] = u + v;
        a[i + k + (len / 2)] = u - v;
        w *= wlen;
      }
    }
  }
}

}  // namespace

namespace phase_detail {

double wrapToPi(double valueRad) {
  while (valueRad > kPi) valueRad -= 2.0 * kPi;
  while (valueRad < -kPi) valueRad += 2.0 * kPi;
  return valueRad;
}

std::vector<PhasePoint> computeTransferPhaseRad(const std::vector<float>& inputSignal,
                                                const std::vector<float>& outputSignal,
                                                int sampleRate, int fftSize, int overlapPercent) {
  if (inputSignal.empty() || outputSignal.empty() || sampleRate <= 0 || fftSize < 2) {
    return {};
  }

  const std::size_t sampleCount = std::min(inputSignal.size(), outputSignal.size());
  if (sampleCount == 0) return {};

  const int clampedOverlap = std::max(0, std::min(99, overlapPercent));
  const int hop = std::max(1, fftSize * (100 - clampedOverlap) / 100);
  const std::size_t n = static_cast<std::size_t>(fftSize);
  const std::size_t bins = n / 2 + 1;

  std::vector<double> window(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    window[i] = 0.5 * (1.0 - std::cos((2.0 * kPi * static_cast<double>(i)) / static_cast<double>(n - 1)));
  }

  std::vector<std::complex<double>> sxyAcc(bins, {0.0, 0.0});
  std::vector<double> sxxAcc(bins, 0.0);
  std::size_t frameCount = 0;

  auto processFrame = [&](std::size_t start) {
    std::vector<std::complex<double>> xin(n, {0.0, 0.0});
    std::vector<std::complex<double>> yout(n, {0.0, 0.0});
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t idx = start + i;
      const double x = (idx < sampleCount) ? static_cast<double>(inputSignal[idx]) : 0.0;
      const double y = (idx < sampleCount) ? static_cast<double>(outputSignal[idx]) : 0.0;
      xin[i] = {x * window[i], 0.0};
      yout[i] = {y * window[i], 0.0};
    }

    fft(xin);
    fft(yout);

    for (std::size_t k = 0; k < bins; ++k) {
      const std::complex<double> sxy = yout[k] * std::conj(xin[k]);
      const double sxx = std::norm(xin[k]);
      sxyAcc[k] += sxy;
      sxxAcc[k] += sxx;
    }
    ++frameCount;
  };

  if (sampleCount < n) {
    processFrame(0);
  } else {
    for (std::size_t start = 0; start + n <= sampleCount; start += static_cast<std::size_t>(hop)) {
      processFrame(start);
    }
    const std::size_t tailStart = sampleCount - n;
    if (((sampleCount - n) % static_cast<std::size_t>(hop)) != 0) {
      processFrame(tailStart);
    }
  }

  if (frameCount == 0) return {};

  constexpr double eps = 1e-18;
  std::vector<PhasePoint> out;
  out.reserve(bins);
  for (std::size_t k = 0; k < bins; ++k) {
    const double freq = (static_cast<double>(sampleRate) * static_cast<double>(k)) / static_cast<double>(fftSize);
    const std::complex<double> h = sxyAcc[k] / std::complex<double>(sxxAcc[k] + eps, 0.0);
    const double phase = std::atan2(h.imag(), h.real());
    out.push_back({freq, wrapToPi(phase)});
  }
  return out;
}

std::vector<PhaseBandSummary> summarizeLogBands(const std::vector<PhasePoint>& deltaPhase, double minHz, double maxHz,
                                                int bandCount) {
  std::vector<PhaseBandSummary> out;
  if (deltaPhase.empty() || bandCount <= 0 || minHz <= 0.0 || maxHz <= minHz) {
    return out;
  }

  const double ratio = std::pow(maxHz / minHz, 1.0 / static_cast<double>(bandCount));
  for (int i = 0; i < bandCount; ++i) {
    const double lower = minHz * std::pow(ratio, static_cast<double>(i));
    const double upper = minHz * std::pow(ratio, static_cast<double>(i + 1));
    const double center = std::sqrt(lower * upper);

    double sum = 0.0;
    int count = 0;
    double maxDelta = 0.0;
    bool maxInitialized = false;
    for (const auto& p : deltaPhase) {
      if (p.frequencyHz < lower || p.frequencyHz > upper) continue;
      sum += p.valueRad;
      ++count;
      if (!maxInitialized || std::fabs(p.valueRad) > std::fabs(maxDelta)) {
        maxDelta = p.valueRad;
        maxInitialized = true;
      }
    }

    PhaseBandSummary band;
    band.lowerHz = lower;
    band.centerHz = center;
    band.upperHz = upper;
    band.avgDeltaRad = (count > 0) ? (sum / static_cast<double>(count)) : 0.0;
    band.maxDeltaRad = maxInitialized ? maxDelta : 0.0;
    out.push_back(band);
  }
  return out;
}

}  // namespace phase_detail

PhaseTestResult PhaseTestManager::run(IPluginHost& pluginA, IPluginHost& pluginB, const TestConfig& config,
                                      std::string& error) const {
  PhaseTestResult result;
  error.clear();

  TestConfig signalConfig = config;
  signalConfig.frequencyDurationMs = config.phaseDurationMs;
  signalConfig.frequencyNoiseLevelDbfs = config.phaseNoiseLevelDbfs;
  result.input = frequency_detail::generateWhiteNoiseInput(signalConfig, 2027);

  result.outputA = pluginA.process(result.input, error);
  if (!error.empty()) return {};

  result.outputB = pluginB.process(result.input, error);
  if (!error.empty()) return {};

  const std::size_t sampleCount = std::min(result.outputA.numSamples(), result.outputB.numSamples());
  if (sampleCount == 0) {
    error = "Phase test received empty audio output from one of the plugins.";
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
    error = "Phase alignment returned empty analysis buffers.";
    return {};
  }

  const std::size_t alignedSamples = std::min(result.alignedA.numSamples(), result.alignedB.numSamples());
  if (alignedSamples == 0) {
    error = "Phase alignment produced zero-length output.";
    return {};
  }

  std::vector<float> inputForReference(alignedSamples, 0.0f);
  if (!result.input.channels.empty()) {
    const std::size_t copyCount = std::min(alignedSamples, result.input.channels[0].size());
    std::copy_n(result.input.channels[0].begin(), copyCount, inputForReference.begin());
  }

  result.phaseA = phase_detail::computeTransferPhaseRad(inputForReference, result.alignedA.channels[0], config.sampleRate,
                                                        config.phaseFftSize, config.phaseOverlapPercent);
  result.phaseB = phase_detail::computeTransferPhaseRad(inputForReference, result.alignedB.channels[0], config.sampleRate,
                                                        config.phaseFftSize, config.phaseOverlapPercent);
  if (result.phaseA.empty() || result.phaseB.empty()) {
    error = "Phase response analysis failed.";
    return {};
  }

  const std::size_t phaseCount = std::min(result.phaseA.size(), result.phaseB.size());
  result.delta.reserve(phaseCount);

  double peakAbs = 0.0;
  double sumAbs = 0.0;
  for (std::size_t i = 0; i < phaseCount; ++i) {
    const double delta = phase_detail::wrapToPi(result.phaseA[i].valueRad - result.phaseB[i].valueRad);
    result.delta.push_back({result.phaseA[i].frequencyHz, delta});
    const double absDelta = std::fabs(delta);
    peakAbs = std::max(peakAbs, absDelta);
    sumAbs += absDelta;
  }
  result.metrics.peakAbsDeltaRad = peakAbs;
  result.metrics.meanAbsDeltaRad = (phaseCount > 0) ? (sumAbs / static_cast<double>(phaseCount)) : 0.0;

  const std::size_t aPeak = findAbsPeakIndex(result.alignedA.channels[0]);
  const std::size_t bPeak = findAbsPeakIndex(result.alignedB.channels[0]);
  const int64_t lagSamples = static_cast<int64_t>(aPeak) - static_cast<int64_t>(bPeak);
  result.metrics.estimatedLatencyMs =
      (1000.0 * static_cast<double>(lagSamples)) / static_cast<double>(config.sampleRate);

  result.bands = phase_detail::summarizeLogBands(result.delta, 20.0, 20000.0, 24);
  return result;
}

}  // namespace vstcompare
