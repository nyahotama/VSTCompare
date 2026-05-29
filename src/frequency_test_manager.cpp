#include "vstcompare/frequency_test_manager.hpp"

#include "vstcompare/analysis_signal.hpp"
#include "vstcompare/latency_alignment.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <string>
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

namespace frequency_detail {

AudioBuffer generateWhiteNoiseInput(const TestConfig& config, uint32_t seed) {
  const std::size_t sampleCount =
      static_cast<std::size_t>((static_cast<int64_t>(config.sampleRate) * config.frequencyDurationMs) / 1000);
  AudioBuffer input = AudioBuffer::zeros(1, sampleCount, config.sampleRate);

  const double rms = std::pow(10.0, static_cast<double>(config.frequencyNoiseLevelDbfs) / 20.0);
  const double range = rms * std::sqrt(3.0);
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> dist(-range, range);
  for (std::size_t i = 0; i < sampleCount; ++i) {
    input.channels[0][i] = static_cast<float>(dist(rng));
  }
  return input;
}

std::vector<SpectrumPoint> computeAveragedPsdDb(const std::vector<float>& signal, int sampleRate, int fftSize,
                                                int overlapPercent) {
  if (signal.empty() || sampleRate <= 0 || fftSize < 2) {
    return {};
  }

  const int clampedOverlap = std::max(0, std::min(99, overlapPercent));
  const int hop = std::max(1, fftSize * (100 - clampedOverlap) / 100);
  const std::size_t n = static_cast<std::size_t>(fftSize);
  const std::size_t bins = n / 2 + 1;

  std::vector<double> window(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    window[i] = 0.5 * (1.0 - std::cos((2.0 * kPi * static_cast<double>(i)) / static_cast<double>(n - 1)));
  }
  const double windowPower = std::max(1e-12, std::inner_product(window.begin(), window.end(), window.begin(), 0.0));

  std::vector<double> psdAcc(bins, 0.0);
  std::size_t frameCount = 0;

  auto processFrame = [&](std::size_t start) {
    std::vector<std::complex<double>> frame(n, {0.0, 0.0});
    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t idx = start + i;
      const double x = (idx < signal.size()) ? static_cast<double>(signal[idx]) : 0.0;
      frame[i] = {x * window[i], 0.0};
    }
    fft(frame);

    for (std::size_t k = 0; k < bins; ++k) {
      const double mag2 = std::norm(frame[k]) / windowPower;
      psdAcc[k] += mag2;
    }
    ++frameCount;
  };

  if (signal.size() < n) {
    processFrame(0);
  } else {
    for (std::size_t start = 0; start + n <= signal.size(); start += static_cast<std::size_t>(hop)) {
      processFrame(start);
    }
    const std::size_t tailStart = signal.size() - n;
    if (((signal.size() - n) % static_cast<std::size_t>(hop)) != 0) {
      processFrame(tailStart);
    }
  }

  if (frameCount == 0) return {};

  constexpr double eps = 1e-15;
  std::vector<SpectrumPoint> out;
  out.reserve(bins);
  for (std::size_t k = 0; k < bins; ++k) {
    const double freq = (static_cast<double>(sampleRate) * static_cast<double>(k)) / static_cast<double>(fftSize);
    const double p = psdAcc[k] / static_cast<double>(frameCount);
    const double db = 10.0 * std::log10(p + eps);
    out.push_back({freq, db});
  }
  return out;
}

std::vector<OctaveBandSummary> summarizeOneThirdOctave(const std::vector<SpectrumPoint>& deltaSpectrum,
                                                       double minHz, double maxHz, int bandCount) {
  std::vector<OctaveBandSummary> out;
  if (deltaSpectrum.empty() || bandCount <= 0 || minHz <= 0.0 || maxHz <= minHz) {
    return out;
  }

  const double edgeRatio = std::pow(2.0, 1.0 / 6.0);
  for (int i = 0; i < bandCount; ++i) {
    const double center = minHz * std::pow(2.0, static_cast<double>(i) / 3.0);
    const double lower = center / edgeRatio;
    const double upper = center * edgeRatio;

    double sum = 0.0;
    int count = 0;
    double maxDelta = 0.0;
    bool maxInitialized = false;

    for (const auto& p : deltaSpectrum) {
      if (p.frequencyHz < lower || p.frequencyHz > upper) continue;
      sum += p.valueDb;
      ++count;
      if (!maxInitialized || std::fabs(p.valueDb) > std::fabs(maxDelta)) {
        maxDelta = p.valueDb;
        maxInitialized = true;
      }
    }

    if (upper >= minHz && lower <= maxHz) {
      OctaveBandSummary band;
      band.lowerHz = lower;
      band.centerHz = center;
      band.upperHz = upper;
      band.avgDeltaDb = (count > 0) ? (sum / static_cast<double>(count)) : 0.0;
      band.maxDeltaDb = maxInitialized ? maxDelta : 0.0;
      out.push_back(band);
    }
  }
  return out;
}

}  // namespace frequency_detail

FrequencyTestResult FrequencyResponseTestManager::run(IPluginHost& pluginA, IPluginHost& pluginB,
                                                       const TestConfig& config, std::string& error) const {
  FrequencyTestResult result;
  error.clear();

  result.input = frequency_detail::generateWhiteNoiseInput(config, 1337);

  result.outputA = pluginA.process(result.input, error);
  if (!error.empty()) return {};

  result.outputB = pluginB.process(result.input, error);
  if (!error.empty()) return {};

  const std::size_t sampleCount = std::min(result.outputA.numSamples(), result.outputB.numSamples());
  if (sampleCount == 0) {
    error = "Frequency test received empty audio output from one of the plugins.";
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

  if (result.alignedA.channels.empty() || result.alignedB.channels.empty()) {
    error = "Frequency alignment returned empty analysis buffers.";
    return {};
  }

  const std::size_t alignedSamples = std::min(result.alignedA.numSamples(), result.alignedB.numSamples());
  if (alignedSamples == 0) {
    error = "Frequency alignment produced zero-length output.";
    return {};
  }

  std::vector<float> inputForReference(alignedSamples, 0.0f);
  if (!result.input.channels.empty()) {
    const std::size_t copyCount = std::min(alignedSamples, result.input.channels[0].size());
    std::copy_n(result.input.channels[0].begin(), copyCount, inputForReference.begin());
  }

  result.inputSpectrum = frequency_detail::computeAveragedPsdDb(inputForReference, config.sampleRate,
                                                                config.frequencyFftSize,
                                                                config.frequencyOverlapPercent);
  result.spectrumA = frequency_detail::computeAveragedPsdDb(result.alignedA.channels[0], config.sampleRate,
                                                            config.frequencyFftSize, config.frequencyOverlapPercent);
  result.spectrumB = frequency_detail::computeAveragedPsdDb(result.alignedB.channels[0], config.sampleRate,
                                                            config.frequencyFftSize, config.frequencyOverlapPercent);
  if (result.inputSpectrum.empty() || result.spectrumA.empty() || result.spectrumB.empty()) {
    error = "Frequency spectrum analysis failed.";
    return {};
  }

  const std::size_t spectrumSize = std::min(result.inputSpectrum.size(), std::min(result.spectrumA.size(), result.spectrumB.size()));
  result.normalizedSpectrumA.reserve(spectrumSize);
  result.normalizedSpectrumB.reserve(spectrumSize);
  result.delta.reserve(spectrumSize);
  double peakAbs = 0.0;
  double sumAbs = 0.0;
  for (std::size_t i = 0; i < spectrumSize; ++i) {
    const double normalizedA = result.spectrumA[i].valueDb - result.inputSpectrum[i].valueDb;
    const double normalizedB = result.spectrumB[i].valueDb - result.inputSpectrum[i].valueDb;
    result.normalizedSpectrumA.push_back({result.spectrumA[i].frequencyHz, normalizedA});
    result.normalizedSpectrumB.push_back({result.spectrumB[i].frequencyHz, normalizedB});
    const double deltaDb = normalizedA - normalizedB;
    result.delta.push_back({result.spectrumA[i].frequencyHz, deltaDb});
    const double absDelta = std::fabs(deltaDb);
    peakAbs = std::max(peakAbs, absDelta);
    sumAbs += absDelta;
  }
  result.metrics.peakAbsDeltaDb = peakAbs;
  result.metrics.meanAbsDeltaDb = (spectrumSize > 0) ? (sumAbs / static_cast<double>(spectrumSize)) : 0.0;

  const std::size_t aPeak = findAbsPeakIndex(result.alignedA.channels[0]);
  const std::size_t bPeak = findAbsPeakIndex(result.alignedB.channels[0]);
  const int64_t lagSamples = static_cast<int64_t>(aPeak) - static_cast<int64_t>(bPeak);
  result.metrics.estimatedLatencyMs =
      (1000.0 * static_cast<double>(lagSamples)) / static_cast<double>(config.sampleRate);

  result.octaveBands = frequency_detail::summarizeOneThirdOctave(result.delta, 20.0, 20000.0, 30);
  return result;
}

}  // namespace vstcompare
