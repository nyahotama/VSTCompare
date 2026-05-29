#include "vstcompare/thd_test_manager.hpp"

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
constexpr int kMinAnalysisFftSize = 2048;

struct SpectrumBin {
  double frequencyHz = 0.0;
  double amplitudeLinear = 0.0;
  double amplitudeDbfs = -300.0;
};

struct DistortionResult {
  double thdRatio = 0.0;
  double thdnRatio = 0.0;
  bool fundamentalMissing = false;
  bool floorPinned = false;
};

struct SpectralFrame {
  std::vector<SpectrumBin> bins;
  std::vector<double> oneSidedPower;
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

SpectralFrame computeSpectralFrame(const std::vector<float>& signal, int sampleRate, int fftSize, int skipHeadSamples) {
  SpectralFrame spectral;
  if (signal.empty() || sampleRate <= 0 || fftSize < 2) {
    return spectral;
  }

  const std::size_t n = static_cast<std::size_t>(fftSize);
  const std::size_t bins = (n / 2) + 1;
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
    const double sample = (idx < signal.size()) ? static_cast<double>(signal[idx]) : 0.0;
    frame[i] = {sample * window[i], 0.0};
  }
  fft(frame);

  spectral.bins.reserve(bins);
  spectral.oneSidedPower.reserve(bins);
  for (std::size_t k = 0; k < bins; ++k) {
    const double mag = std::abs(frame[k]);
    double amp = (2.0 * std::abs(frame[k])) / coherentGain;
    if (k == 0 || (k + 1 == bins)) {
      amp = mag / coherentGain;
    }
    double power = mag * mag;
    if (!(k == 0 || (k + 1 == bins))) {
      power *= 2.0;
    }
    const double dbfs = 20.0 * std::log10(std::max(kEps, amp));
    const double freq = (static_cast<double>(sampleRate) * static_cast<double>(k)) / static_cast<double>(fftSize);
    spectral.bins.push_back({freq, amp, dbfs});
    spectral.oneSidedPower.push_back(power);
  }
  return spectral;
}

double findLocalPeakLinear(const std::vector<SpectrumBin>& bins, double targetHz, double maxHz) {
  if (bins.empty() || targetHz <= 0.0 || targetHz > maxHz) {
    return 0.0;
  }

  double binWidthHz = 1.0;
  if (bins.size() >= 2) {
    binWidthHz = std::max(1e-9, bins[1].frequencyHz - bins[0].frequencyHz);
  }
  const int center = static_cast<int>(std::llround(targetHz / binWidthHz));
  const int radius = std::max(2, static_cast<int>(std::llround(20.0 / binWidthHz)));
  const int minIdx = std::max(0, center - radius);
  const int maxIdx = std::min(static_cast<int>(bins.size() - 1), center + radius);

  double best = 0.0;
  for (int i = minIdx; i <= maxIdx; ++i) {
    const auto& b = bins[static_cast<std::size_t>(i)];
    if (b.frequencyHz > maxHz) break;
    best = std::max(best, b.amplitudeLinear);
  }
  return best;
}

bool looksPinnedToFloor(const std::vector<SpectrumBin>& bins, double thresholdDbfs) {
  if (bins.empty()) return true;
  double maxDb = -std::numeric_limits<double>::infinity();
  for (const auto& b : bins) {
    maxDb = std::max(maxDb, b.amplitudeDbfs);
  }
  return maxDb <= thresholdDbfs;
}

int findLocalPeakIndex(const std::vector<SpectrumBin>& bins, double targetHz, double maxHz) {
  if (bins.empty() || targetHz <= 0.0 || targetHz > maxHz) {
    return -1;
  }

  double binWidthHz = 1.0;
  if (bins.size() >= 2) {
    binWidthHz = std::max(1e-9, bins[1].frequencyHz - bins[0].frequencyHz);
  }
  const int center = static_cast<int>(std::llround(targetHz / binWidthHz));
  const int radius = std::max(2, static_cast<int>(std::llround(20.0 / binWidthHz)));
  const int minIdx = std::max(0, center - radius);
  const int maxIdx = std::min(static_cast<int>(bins.size() - 1), center + radius);

  int bestIndex = -1;
  double best = -std::numeric_limits<double>::infinity();
  for (int i = minIdx; i <= maxIdx; ++i) {
    const auto& b = bins[static_cast<std::size_t>(i)];
    if (b.frequencyHz > maxHz) break;
    if (b.amplitudeLinear > best) {
      best = b.amplitudeLinear;
      bestIndex = i;
    }
  }
  return bestIndex;
}

DistortionResult computeDistortion(const std::vector<float>& signal, int sampleRate, int fftSize, int skipHeadSamples,
                                   double fundamentalHz) {
  DistortionResult out;
  const auto spectral = computeSpectralFrame(signal, sampleRate, fftSize, skipHeadSamples);
  if (spectral.bins.empty() || spectral.oneSidedPower.empty()) {
    out.fundamentalMissing = true;
    return out;
  }

  const double maxHz = 24000.0;
  const double v1 = findLocalPeakLinear(spectral.bins, fundamentalHz, maxHz);
  const int fundamentalIndex = findLocalPeakIndex(spectral.bins, fundamentalHz, maxHz);
  if (v1 <= 1e-12 || fundamentalIndex < 0) {
    out.fundamentalMissing = true;
    out.floorPinned = looksPinnedToFloor(spectral.bins, -250.0);
    return out;
  }

  double harmonicPower = 0.0;
  for (int order = 2; order <= 10; ++order) {
    const double hz = fundamentalHz * static_cast<double>(order);
    if (hz > maxHz) break;
    const double vn = findLocalPeakLinear(spectral.bins, hz, maxHz);
    harmonicPower += vn * vn;
  }
  out.thdRatio = std::sqrt(std::max(0.0, harmonicPower)) / std::max(v1, kEps);

  constexpr int kFundamentalGuardBins = 4;
  const int startIndex = std::max(0, fundamentalIndex - kFundamentalGuardBins);
  const int endIndex = std::min(static_cast<int>(spectral.oneSidedPower.size()) - 1, fundamentalIndex + kFundamentalGuardBins);

  double totalPower = 0.0;
  double fundamentalBandPower = 0.0;
  for (std::size_t i = 0; i < spectral.bins.size(); ++i) {
    const auto& b = spectral.bins[i];
    if (b.frequencyHz < 20.0 || b.frequencyHz > maxHz) continue;
    const double p = spectral.oneSidedPower[i];
    totalPower += p;
    if (static_cast<int>(i) >= startIndex && static_cast<int>(i) <= endIndex) {
      fundamentalBandPower += p;
    }
  }
  if (fundamentalBandPower <= kEps) {
    out.fundamentalMissing = true;
    out.floorPinned = looksPinnedToFloor(spectral.bins, -250.0);
    return out;
  }
  const double residualPower = std::max(0.0, totalPower - fundamentalBandPower);
  out.thdnRatio = std::sqrt(residualPower) / std::sqrt(std::max(fundamentalBandPower, kEps));
  out.floorPinned = looksPinnedToFloor(spectral.bins, -250.0);
  return out;
}

int largestPowerOfTwoLE(std::size_t value) {
  if (value < 2) return 0;
  std::size_t p = 1;
  while ((p << 1) <= value) {
    p <<= 1;
  }
  if (p > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(p);
}

ThdSegmentSummary buildSegmentSummary(const std::vector<ThdSweepPoint>& sweep, const std::string& name, double startDb,
                                      double endDb) {
  ThdSegmentSummary s;
  s.name = name;
  s.startLevelDbfs = startDb;
  s.endLevelDbfs = endDb;

  double sumThd = 0.0;
  double sumThdn = 0.0;
  for (const auto& p : sweep) {
    if (p.inputLevelDbfs < startDb || p.inputLevelDbfs > endDb) continue;
    ++s.pointCount;
    sumThd += p.thdDeltaPercent;
    sumThdn += p.thdnDeltaPercent;
    s.maxAbsThdDeltaPercent = std::max(s.maxAbsThdDeltaPercent, std::fabs(p.thdDeltaPercent));
    s.maxAbsThdnDeltaPercent = std::max(s.maxAbsThdnDeltaPercent, std::fabs(p.thdnDeltaPercent));
  }
  if (s.pointCount > 0) {
    s.avgThdDeltaPercent = sumThd / static_cast<double>(s.pointCount);
    s.avgThdnDeltaPercent = sumThdn / static_cast<double>(s.pointCount);
  }
  return s;
}

}  // namespace

namespace thd_detail {

std::vector<double> buildSweepLevels(const TestConfig& config) {
  std::vector<double> levels;
  const double start = static_cast<double>(config.thdStartLevelDbfs);
  const double end = static_cast<double>(config.thdEndLevelDbfs);
  const double step = static_cast<double>(config.thdStepDb);
  if (step <= 0.0 || end < start) return levels;

  for (double level = start; level <= end + 1e-9; level += step) {
    levels.push_back(level);
  }
  return levels;
}

AudioBuffer generateSineInput(const TestConfig& config, double levelDbfs) {
  const std::size_t sampleCount =
      static_cast<std::size_t>((static_cast<int64_t>(config.sampleRate) * config.thdDurationMs) / 1000);
  AudioBuffer input = AudioBuffer::zeros(1, sampleCount, config.sampleRate);

  const double amplitude = std::pow(10.0, levelDbfs / 20.0);
  const double frequencyHz = config.thdToneFrequencyHz;
  for (std::size_t i = 0; i < sampleCount; ++i) {
    const double phase = (2.0 * kPi * frequencyHz * static_cast<double>(i)) / static_cast<double>(config.sampleRate);
    input.channels[0][i] = static_cast<float>(amplitude * std::sin(phase));
  }
  return input;
}

}  // namespace thd_detail

ThdTestResult ThdThdnTestManager::run(IPluginHost& pluginA, IPluginHost& pluginB, const TestConfig& config,
                                       std::string& error) const {
  ThdTestResult result;
  error.clear();

  const auto levels = thd_detail::buildSweepLevels(config);
  if (levels.empty()) {
    error = "THD test sweep settings are invalid.";
    return {};
  }

  const int skipHeadSamples =
      std::max(0, static_cast<int>((static_cast<int64_t>(config.sampleRate) * config.thdSkipHeadMs) / 1000));

  double sumAbsThd = 0.0;
  double sumAbsThdn = 0.0;
  std::size_t count = 0;

  for (double levelDbfs : levels) {
    ThdSweepPoint point;
    point.inputLevelDbfs = levelDbfs;

    const AudioBuffer input = thd_detail::generateSineInput(config, levelDbfs);
    const auto outputA = pluginA.process(input, error);
    if (!error.empty()) return {};
    const auto outputB = pluginB.process(input, error);
    if (!error.empty()) return {};

    if (outputA.numSamples() == 0 || outputB.numSamples() == 0) {
      error = "THD test received empty audio output from one of the plugins.";
      return {};
    }

    std::string modeA;
    std::string modeB;
    const auto analysisA = toMonoForAnalysis(outputA, modeA);
    const auto analysisB = toMonoForAnalysis(outputB, modeB);
    point.analysisSignalMode = (modeA == "mono_lr_average" || modeB == "mono_lr_average")
                                   ? "mono_lr_average"
                                   : "single_channel";

    point.pluginLatencySamplesA = pluginA.getLatencySamples();
    point.pluginLatencySamplesB = pluginB.getLatencySamples();
    const LatencyAlignedBuffers aligned =
        alignForAnalysis(analysisA, analysisB, point.pluginLatencySamplesA, point.pluginLatencySamplesB);
    point.latencyAlignment = aligned.info;

    if (aligned.alignedA.channels.empty() || aligned.alignedB.channels.empty()) {
      error = "THD alignment returned empty analysis buffers.";
      return {};
    }

    const std::size_t alignedSamples = std::min(aligned.alignedA.numSamples(), aligned.alignedB.numSamples());
    if (alignedSamples == 0) {
      error = "THD alignment produced zero-length output.";
      return {};
    }

    std::vector<float> signalA(alignedSamples, 0.0f);
    std::vector<float> signalB(alignedSamples, 0.0f);
    std::copy_n(aligned.alignedA.channels[0].begin(), alignedSamples, signalA.begin());
    std::copy_n(aligned.alignedB.channels[0].begin(), alignedSamples, signalB.begin());

    int effectiveSkipSamples = skipHeadSamples;
    if (static_cast<std::size_t>(skipHeadSamples) >= alignedSamples) {
      effectiveSkipSamples = std::max<int>(0, static_cast<int>(alignedSamples) - 1);
      point.warnings.push_back("THD skipHead exceeded aligned sample length; adjusted analysis start sample.");
    }

    const std::size_t availableSamples = alignedSamples - static_cast<std::size_t>(effectiveSkipSamples);
    const int availablePowerOfTwo = largestPowerOfTwoLE(availableSamples);
    const int requestedFft = std::max(2, config.thdFftSize);
    const int effectiveFftSize = std::min(requestedFft, availablePowerOfTwo);

    if (effectiveFftSize < kMinAnalysisFftSize) {
      point.warnings.push_back(
          "THD analysis skipped for this sweep point: insufficient post-skip samples for stable FFT analysis.");
    } else {
      if (effectiveFftSize < requestedFft) {
        point.warnings.push_back("THD FFT size reduced to fit available post-skip samples.");
      }

      const DistortionResult a = computeDistortion(signalA, config.sampleRate, effectiveFftSize, effectiveSkipSamples,
                                                   config.thdToneFrequencyHz);
      const DistortionResult b = computeDistortion(signalB, config.sampleRate, effectiveFftSize, effectiveSkipSamples,
                                                   config.thdToneFrequencyHz);

      point.thdPercentA = a.thdRatio * 100.0;
      point.thdPercentB = b.thdRatio * 100.0;
      point.thdnPercentA = a.thdnRatio * 100.0;
      point.thdnPercentB = b.thdnRatio * 100.0;
      point.thdDeltaPercent = point.thdPercentA - point.thdPercentB;
      point.thdnDeltaPercent = point.thdnPercentA - point.thdnPercentB;

      if (a.fundamentalMissing || b.fundamentalMissing) {
        point.warnings.push_back("THD fundamental detection failed near 1kHz at one or both plugins.");
      }
      if (a.floorPinned || b.floorPinned) {
        point.warnings.push_back(
            "THD spectrum appears pinned near the floor; verify plugin output and analysis window settings.");
      }
      if (point.thdnPercentA > 100.0 || point.thdnPercentB > 100.0) {
        point.warnings.push_back("THD+N exceeded 100% at this sweep point; chart clips at the configured 100% upper bound.");
      }
    }

    sumAbsThd += std::fabs(point.thdDeltaPercent);
    sumAbsThdn += std::fabs(point.thdnDeltaPercent);
    result.metrics.peakAbsThdDeltaPercent =
        std::max(result.metrics.peakAbsThdDeltaPercent, std::fabs(point.thdDeltaPercent));
    result.metrics.peakAbsThdnDeltaPercent =
        std::max(result.metrics.peakAbsThdnDeltaPercent, std::fabs(point.thdnDeltaPercent));
    ++count;

    result.sweep.push_back(std::move(point));
  }

  if (result.sweep.empty()) {
    error = "THD test generated no sweep points.";
    return {};
  }

  if (count > 0) {
    result.metrics.meanAbsThdDeltaPercent = sumAbsThd / static_cast<double>(count);
    result.metrics.meanAbsThdnDeltaPercent = sumAbsThdn / static_cast<double>(count);
  }

  result.segments.push_back(buildSegmentSummary(result.sweep, "low", -60.0, -30.0));
  result.segments.push_back(buildSegmentSummary(result.sweep, "mid", -28.0, -10.0));
  result.segments.push_back(buildSegmentSummary(result.sweep, "high", -8.0, 0.0));
  result.segments.push_back(buildSegmentSummary(result.sweep, "overdrive", 2.0, 6.0));
  return result;
}

}  // namespace vstcompare
