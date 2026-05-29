#include "vstcompare/harmonic_test_manager.hpp"

#include "vstcompare/analysis_signal.hpp"
#include "vstcompare/latency_alignment.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <vector>

namespace vstcompare {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEps = 1e-15;

struct SpectrumBin {
  double frequencyHz = 0.0;
  double amplitudeDbfs = 0.0;
  double amplitudeLinear = 0.0;
};

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

std::vector<SpectrumBin> computeSpectrumBinsDbfs(const std::vector<float>& signal, int sampleRate, int fftSize,
                                                 int skipHeadSamples) {
  if (signal.empty() || sampleRate <= 0 || fftSize < 2) {
    return {};
  }

  const std::size_t n = static_cast<std::size_t>(fftSize);
  const std::size_t bins = n / 2 + 1;
  const std::size_t skip = static_cast<std::size_t>(std::max(0, skipHeadSamples));

  std::vector<double> window(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    const double x = (2.0 * kPi * static_cast<double>(i)) / static_cast<double>(n - 1);
    window[i] = 0.35875 - (0.48829 * std::cos(x)) + (0.14128 * std::cos(2.0 * x)) - (0.01168 * std::cos(3.0 * x));
  }
  const double coherentGain = std::max(kEps, std::accumulate(window.begin(), window.end(), 0.0));

  std::vector<std::complex<double>> frame(n, {0.0, 0.0});
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t idx = skip + i;
    const double x = (idx < signal.size()) ? static_cast<double>(signal[idx]) : 0.0;
    frame[i] = {x * window[i], 0.0};
  }
  fft(frame);

  std::vector<SpectrumBin> out;
  out.reserve(bins);
  for (std::size_t k = 0; k < bins; ++k) {
    const double freq = (static_cast<double>(sampleRate) * static_cast<double>(k)) / static_cast<double>(fftSize);
    double amp = (2.0 * std::abs(frame[k])) / coherentGain;
    if (k == 0 || (k + 1 == bins)) {
      amp = std::abs(frame[k]) / coherentGain;
    }
    const double dbfs = 20.0 * std::log10(std::max(kEps, amp));
    out.push_back({freq, dbfs, amp});
  }
  return out;
}

double findLocalPeakDbfs(const std::vector<SpectrumBin>& bins, double targetHz, double maxHz) {
  if (bins.empty() || targetHz <= 0.0 || targetHz > maxHz) {
    return -160.0;
  }

  double binWidthHz = 1.0;
  if (bins.size() >= 2) {
    binWidthHz = std::max(1e-9, bins[1].frequencyHz - bins[0].frequencyHz);
  }
  const int center = static_cast<int>(std::llround(targetHz / binWidthHz));
  const int radius = std::max(2, static_cast<int>(std::llround(20.0 / binWidthHz)));
  const int minIdx = std::max(0, center - radius);
  const int maxIdx = std::min(static_cast<int>(bins.size() - 1), center + radius);

  double best = -160.0;
  for (int i = minIdx; i <= maxIdx; ++i) {
    const auto& b = bins[static_cast<std::size_t>(i)];
    if (b.frequencyHz > maxHz) break;
    best = std::max(best, b.amplitudeDbfs);
  }
  return best;
}

double computeNoiseFloorDbfs(const std::vector<SpectrumBin>& bins, double fundamentalHz, int orderMax, double minHz,
                             double maxHz) {
  if (bins.empty() || fundamentalHz <= 0.0 || minHz <= 0.0 || maxHz <= minHz) {
    return -160.0;
  }

  double binWidthHz = 1.0;
  if (bins.size() >= 2) {
    binWidthHz = std::max(1e-9, bins[1].frequencyHz - bins[0].frequencyHz);
  }
  const int exclusionRadius = std::max(2, static_cast<int>(std::llround(20.0 / binWidthHz)));

  std::vector<int> harmonicCenters;
  harmonicCenters.reserve(static_cast<std::size_t>(orderMax));
  for (int order = 1; order <= orderMax; ++order) {
    const double hz = fundamentalHz * static_cast<double>(order);
    if (hz > maxHz) break;
    harmonicCenters.push_back(static_cast<int>(std::llround(hz / binWidthHz)));
  }

  double powerSum = 0.0;
  std::size_t count = 0;
  for (std::size_t i = 0; i < bins.size(); ++i) {
    const double hz = bins[i].frequencyHz;
    if (hz < minHz || hz > maxHz) continue;

    bool excluded = false;
    for (int center : harmonicCenters) {
      if (std::abs(static_cast<int>(i) - center) <= exclusionRadius) {
        excluded = true;
        break;
      }
    }
    if (excluded) continue;

    const double amp = std::max(kEps, bins[i].amplitudeLinear);
    powerSum += amp * amp;
    ++count;
  }

  if (count == 0) return -160.0;
  const double rmsAmp = std::sqrt(powerSum / static_cast<double>(count));
  return 20.0 * std::log10(std::max(kEps, rmsAmp));
}

bool looksPinnedToFloor(const std::vector<SpectrumBin>& bins, double thresholdDbfs) {
  if (bins.empty()) return true;
  double maxDb = -std::numeric_limits<double>::infinity();
  for (const auto& b : bins) {
    maxDb = std::max(maxDb, b.amplitudeDbfs);
  }
  return maxDb <= thresholdDbfs;
}

}  // namespace

namespace harmonic_detail {

AudioBuffer generateSineInput(const TestConfig& config, double frequencyHz) {
  const std::size_t sampleCount =
      static_cast<std::size_t>((static_cast<int64_t>(config.sampleRate) * config.harmonicDurationMs) / 1000);
  AudioBuffer input = AudioBuffer::zeros(1, sampleCount, config.sampleRate);

  const double amplitude = std::pow(10.0, static_cast<double>(config.harmonicInputLevelDbfs) / 20.0);
  for (std::size_t i = 0; i < sampleCount; ++i) {
    const double phase = (2.0 * kPi * frequencyHz * static_cast<double>(i)) / static_cast<double>(config.sampleRate);
    input.channels[0][i] = static_cast<float>(amplitude * std::sin(phase));
  }
  return input;
}

std::vector<HarmonicSpectrumPoint> computeSpectrumDbfs(const std::vector<float>& signal, int sampleRate, int fftSize,
                                                       int skipHeadSamples) {
  const std::vector<SpectrumBin> bins = computeSpectrumBinsDbfs(signal, sampleRate, fftSize, skipHeadSamples);
  std::vector<HarmonicSpectrumPoint> out;
  out.reserve(bins.size());
  for (const auto& b : bins) {
    out.push_back({b.frequencyHz, b.amplitudeDbfs});
  }
  return out;
}

}  // namespace harmonic_detail

HarmonicTestResult HarmonicDistortionTestManager::run(IPluginHost& pluginA, IPluginHost& pluginB,
                                                       const TestConfig& config, std::string& error) const {
  HarmonicTestResult result;
  error.clear();

  if (config.harmonicFrequenciesHz.empty()) {
    error = "Harmonic test requires at least one input frequency.";
    return {};
  }

  const int skipHeadSamples =
      std::max(0, static_cast<int>((static_cast<int64_t>(config.sampleRate) * config.harmonicSkipHeadMs) / 1000));
  const double nyquistHz = static_cast<double>(config.sampleRate) * 0.5;

  double peakAbs = 0.0;
  double sumAbs = 0.0;
  std::size_t absCount = 0;

  for (double frequencyHz : config.harmonicFrequenciesHz) {
    if (frequencyHz <= 0.0 || frequencyHz >= nyquistHz) {
      continue;
    }

    HarmonicPitchResult pitch;
    pitch.fundamentalHz = frequencyHz;
    pitch.input = harmonic_detail::generateSineInput(config, frequencyHz);

    pitch.outputA = pluginA.process(pitch.input, error);
    if (!error.empty()) return {};

    pitch.outputB = pluginB.process(pitch.input, error);
    if (!error.empty()) return {};

    if (pitch.outputA.numSamples() == 0 || pitch.outputB.numSamples() == 0) {
      error = "Harmonic test received empty audio output from one of the plugins.";
      return {};
    }

    std::string modeA;
    std::string modeB;
    pitch.analysisA = toMonoForAnalysis(pitch.outputA, modeA);
    pitch.analysisB = toMonoForAnalysis(pitch.outputB, modeB);
    pitch.analysisSignalMode =
        (modeA == "mono_lr_average" || modeB == "mono_lr_average") ? "mono_lr_average" : "single_channel";

    pitch.pluginLatencySamplesA = pluginA.getLatencySamples();
    pitch.pluginLatencySamplesB = pluginB.getLatencySamples();
    const LatencyAlignedBuffers aligned =
        alignForAnalysis(pitch.analysisA, pitch.analysisB, pitch.pluginLatencySamplesA, pitch.pluginLatencySamplesB);
    pitch.alignedA = aligned.alignedA;
    pitch.alignedB = aligned.alignedB;
    pitch.latencyAlignment = aligned.info;

    if (pitch.alignedA.channels.empty() || pitch.alignedB.channels.empty()) {
      error = "Harmonic alignment returned empty analysis buffers.";
      return {};
    }

    const std::size_t alignedSamples = std::min(pitch.alignedA.numSamples(), pitch.alignedB.numSamples());
    if (alignedSamples == 0) {
      error = "Harmonic alignment produced zero-length output.";
      return {};
    }

    std::vector<float> alignedA(alignedSamples, 0.0f);
    std::vector<float> alignedB(alignedSamples, 0.0f);
    std::copy_n(pitch.alignedA.channels[0].begin(), alignedSamples, alignedA.begin());
    std::copy_n(pitch.alignedB.channels[0].begin(), alignedSamples, alignedB.begin());

    int effectiveSkipSamples = skipHeadSamples;
    if (static_cast<std::size_t>(skipHeadSamples) >= alignedSamples) {
      effectiveSkipSamples = std::max<int>(0, static_cast<int>(alignedSamples) - 1);
      pitch.latencyAlignment.warnings.push_back(
          "Harmonic skipHead exceeded aligned sample length; adjusted analysis start sample.");
    }

    const std::vector<SpectrumBin> binsA =
        computeSpectrumBinsDbfs(alignedA, config.sampleRate, config.harmonicFftSize, effectiveSkipSamples);
    const std::vector<SpectrumBin> binsB =
        computeSpectrumBinsDbfs(alignedB, config.sampleRate, config.harmonicFftSize, effectiveSkipSamples);
    if (binsA.empty() || binsB.empty()) {
      error = "Harmonic spectrum analysis failed.";
      return {};
    }
    if (looksPinnedToFloor(binsA, -250.0) || looksPinnedToFloor(binsB, -250.0)) {
      pitch.latencyAlignment.warnings.push_back(
          "Harmonic spectrum appears pinned near the floor; verify plugin output and analysis window settings.");
    }

    const std::size_t n = std::min(binsA.size(), binsB.size());
    pitch.spectrumA.reserve(n);
    pitch.spectrumB.reserve(n);
    pitch.delta.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      const double hz = binsA[i].frequencyHz;
      const double aDb = binsA[i].amplitudeDbfs;
      const double bDb = binsB[i].amplitudeDbfs;
      const double dDb = aDb - bDb;
      pitch.spectrumA.push_back({hz, aDb});
      pitch.spectrumB.push_back({hz, bDb});
      pitch.delta.push_back({hz, dDb});
      const double absDb = std::fabs(dDb);
      peakAbs = std::max(peakAbs, absDb);
      sumAbs += absDb;
      ++absCount;
    }

    for (int order = 1; order <= 10; ++order) {
      const double orderHz = frequencyHz * static_cast<double>(order);
      if (orderHz > nyquistHz) break;
      const double aDb = findLocalPeakDbfs(binsA, orderHz, nyquistHz);
      const double bDb = findLocalPeakDbfs(binsB, orderHz, nyquistHz);
      pitch.orders.push_back({order, orderHz, aDb, bDb, aDb - bDb});
    }

    pitch.noiseFloorDbfsA = computeNoiseFloorDbfs(binsA, frequencyHz, 10, 20.0, nyquistHz);
    pitch.noiseFloorDbfsB = computeNoiseFloorDbfs(binsB, frequencyHz, 10, 20.0, nyquistHz);
    pitch.noiseFloorDeltaDb = pitch.noiseFloorDbfsA - pitch.noiseFloorDbfsB;

    result.pitches.push_back(std::move(pitch));
  }

  if (result.pitches.empty()) {
    error = "Harmonic test did not run any valid frequency points.";
    return {};
  }

  result.metrics.peakAbsDeltaDb = peakAbs;
  result.metrics.meanAbsDeltaDb = (absCount > 0) ? (sumAbs / static_cast<double>(absCount)) : 0.0;
  return result;
}

}  // namespace vstcompare
