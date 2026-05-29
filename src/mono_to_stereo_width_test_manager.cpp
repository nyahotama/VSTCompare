#include "vstcompare/mono_to_stereo_width_test_manager.hpp"

#include "vstcompare/latency_alignment.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

namespace vstcompare {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEps = 1e-12;

void dedupeWarnings(std::vector<std::string>& warnings) {
  std::unordered_set<std::string> seen;
  std::vector<std::string> unique;
  unique.reserve(warnings.size());
  for (const auto& warning : warnings) {
    if (seen.insert(warning).second) {
      unique.push_back(warning);
    }
  }
  warnings = std::move(unique);
}

double sanitizeFinite(double value, const char* metricName, std::vector<std::string>& warnings) {
  if (std::isfinite(value)) return value;
  warnings.push_back(std::string("Mono-to-Stereo width: non-finite value detected for ") + metricName +
                     "; substituted with 0.");
  return 0.0;
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
    if (i < j) std::swap(a[i], a[j]);
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

double rms(const std::vector<float>& x, std::size_t begin, std::size_t count) {
  if (count == 0 || begin >= x.size()) return 0.0;
  const std::size_t end = std::min(x.size(), begin + count);
  if (end <= begin) return 0.0;

  double sum = 0.0;
  for (std::size_t i = begin; i < end; ++i) {
    const double v = static_cast<double>(x[i]);
    sum += v * v;
  }
  return std::sqrt(sum / static_cast<double>(end - begin));
}

double midSideRatioDb(const std::vector<float>& left, const std::vector<float>& right, std::size_t begin,
                      std::size_t count) {
  if (left.empty() || right.empty() || count == 0) return 0.0;
  const std::size_t end = std::min({left.size(), right.size(), begin + count});
  if (begin >= end) return 0.0;

  double sumMid = 0.0;
  double sumSide = 0.0;
  for (std::size_t i = begin; i < end; ++i) {
    const double l = static_cast<double>(left[i]);
    const double r = static_cast<double>(right[i]);
    const double m = 0.5 * (l + r);
    const double s = 0.5 * (l - r);
    sumMid += m * m;
    sumSide += s * s;
  }
  const double n = static_cast<double>(end - begin);
  const double rmsMid = std::sqrt(sumMid / n);
  const double rmsSide = std::sqrt(sumSide / n);
  return 20.0 * std::log10((rmsMid + kEps) / (rmsSide + kEps));
}

double widthPercentFromRms(double rmsMid, double rmsSide) {
  return 100.0 * (rmsSide / (rmsMid + rmsSide + kEps));
}

void midSideRmsForRange(const std::vector<float>& left, const std::vector<float>& right, std::size_t begin,
                        std::size_t count, double& rmsMid, double& rmsSide) {
  rmsMid = 0.0;
  rmsSide = 0.0;
  if (left.empty() || right.empty() || count == 0) return;

  const std::size_t end = std::min({left.size(), right.size(), begin + count});
  if (begin >= end) return;

  double sumMid = 0.0;
  double sumSide = 0.0;
  for (std::size_t i = begin; i < end; ++i) {
    const double l = static_cast<double>(left[i]);
    const double r = static_cast<double>(right[i]);
    const double mid = 0.5 * (l + r);
    const double side = 0.5 * (l - r);
    sumMid += mid * mid;
    sumSide += side * side;
  }
  const double n = static_cast<double>(end - begin);
  rmsMid = std::sqrt(sumMid / n);
  rmsSide = std::sqrt(sumSide / n);
}

AudioBuffer prepareStereoForAnalysis(const AudioBuffer& in, const char* pluginLabel, std::vector<std::string>& warnings) {
  const std::size_t sampleCount = in.numSamples();
  if (in.numChannels() == 0 || sampleCount == 0) {
    warnings.push_back(std::string("Mono-to-Stereo width test: ") + pluginLabel + " produced empty output.");
    return AudioBuffer::zeros(2, 0, in.sampleRate);
  }

  AudioBuffer out = AudioBuffer::zeros(2, sampleCount, in.sampleRate);
  if (in.numChannels() == 1) {
    warnings.push_back(std::string("Mono-to-Stereo width test: ") + pluginLabel +
                       " output is mono; duplicated to stereo for analysis.");
    std::copy(in.channels[0].begin(), in.channels[0].end(), out.channels[0].begin());
    std::copy(in.channels[0].begin(), in.channels[0].end(), out.channels[1].begin());
    return out;
  }

  std::copy_n(in.channels[0].begin(), sampleCount, out.channels[0].begin());
  std::copy_n(in.channels[1].begin(), sampleCount, out.channels[1].begin());
  if (in.numChannels() > 2) {
    warnings.push_back(std::string("Mono-to-Stereo width test: ") + pluginLabel +
                       " has more than 2 channels; only L/R channels are used.");
  }
  return out;
}

void buildMidSideSignals(const AudioBuffer& stereo, AudioBuffer& outMid, AudioBuffer& outSide) {
  outMid = AudioBuffer::zeros(1, stereo.numSamples(), stereo.sampleRate);
  outSide = AudioBuffer::zeros(1, stereo.numSamples(), stereo.sampleRate);
  if (stereo.numChannels() < 2) return;
  for (std::size_t i = 0; i < stereo.numSamples(); ++i) {
    const float l = stereo.channels[0][i];
    const float r = stereo.channels[1][i];
    outMid.channels[0][i] = 0.5f * (l + r);
    outSide.channels[0][i] = 0.5f * (l - r);
  }
}

std::vector<WidthTimePoint> computeTimeSeries(const AudioBuffer& stereoA, const AudioBuffer& stereoB, int windowMs,
                                              int hopMs, std::vector<std::string>& warnings) {
  std::vector<WidthTimePoint> out;
  if (stereoA.numChannels() < 2 || stereoB.numChannels() < 2 || stereoA.numSamples() == 0 || stereoB.numSamples() == 0) {
    return out;
  }

  const std::size_t sampleCount = std::min(stereoA.numSamples(), stereoB.numSamples());
  const int sampleRate = std::max(1, stereoA.sampleRate);
  std::size_t windowSamples =
      static_cast<std::size_t>(std::max(1, (sampleRate * std::max(1, windowMs)) / 1000));
  std::size_t hopSamples = static_cast<std::size_t>(std::max(1, (sampleRate * std::max(1, hopMs)) / 1000));
  windowSamples = std::min(windowSamples, sampleCount);

  if (windowSamples < 16) {
    warnings.push_back("Mono-to-Stereo width test: time-window is very short; results may be unstable.");
  }

  const bool singleFrame = sampleCount <= windowSamples;
  for (std::size_t start = 0; start < sampleCount; start += hopSamples) {
    const std::size_t count = singleFrame ? sampleCount : std::min(windowSamples, sampleCount - start);
    if (count == 0) break;

    WidthTimePoint point;
    point.timeMs = 1000.0 * static_cast<double>(start) / static_cast<double>(sampleRate);
    point.ratioDbA = sanitizeFinite(
        midSideRatioDb(stereoA.channels[0], stereoA.channels[1], start, count), "time ratio A", warnings);
    point.ratioDbB = sanitizeFinite(
        midSideRatioDb(stereoB.channels[0], stereoB.channels[1], start, count), "time ratio B", warnings);
    point.deltaDb = sanitizeFinite(point.ratioDbA - point.ratioDbB, "time ratio delta", warnings);
    double rmsMidA = 0.0;
    double rmsSideA = 0.0;
    double rmsMidB = 0.0;
    double rmsSideB = 0.0;
    midSideRmsForRange(stereoA.channels[0], stereoA.channels[1], start, count, rmsMidA, rmsSideA);
    midSideRmsForRange(stereoB.channels[0], stereoB.channels[1], start, count, rmsMidB, rmsSideB);
    point.widthPercentA = sanitizeFinite(widthPercentFromRms(rmsMidA, rmsSideA), "time width A", warnings);
    point.widthPercentB = sanitizeFinite(widthPercentFromRms(rmsMidB, rmsSideB), "time width B", warnings);
    point.deltaWidthPercent =
        sanitizeFinite(point.widthPercentA - point.widthPercentB, "time width delta", warnings);
    out.push_back(point);

    if (singleFrame) break;
    if (start + count >= sampleCount) break;
  }
  return out;
}

struct RatioSpectrum {
  std::vector<double> frequenciesHz;
  std::vector<double> ratioDb;
  std::vector<double> widthPercent;
};

RatioSpectrum computeMidSideRatioSpectrum(const AudioBuffer& stereo, int fftSize, int overlapPercent,
                                          std::vector<std::string>& warnings) {
  RatioSpectrum out;
  if (stereo.numChannels() < 2 || stereo.numSamples() == 0 || fftSize < 64) return out;

  const std::size_t n = static_cast<std::size_t>(fftSize);
  const std::size_t bins = n / 2 + 1;
  const int clampedOverlap = std::clamp(overlapPercent, 0, 95);
  const int hop = std::max(1, fftSize * (100 - clampedOverlap) / 100);

  std::vector<double> window(n, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    window[i] = 0.5 * (1.0 - std::cos((2.0 * kPi * static_cast<double>(i)) / static_cast<double>(n - 1)));
  }

  std::vector<double> powerMid(bins, 0.0);
  std::vector<double> powerSide(bins, 0.0);
  std::size_t frameCount = 0;

  auto processFrame = [&](std::size_t start) {
    std::vector<std::complex<double>> frameMid(n, {0.0, 0.0});
    std::vector<std::complex<double>> frameSide(n, {0.0, 0.0});

    for (std::size_t i = 0; i < n; ++i) {
      const std::size_t idx = start + i;
      const double l = idx < stereo.numSamples() ? static_cast<double>(stereo.channels[0][idx]) : 0.0;
      const double r = idx < stereo.numSamples() ? static_cast<double>(stereo.channels[1][idx]) : 0.0;
      const double mid = 0.5 * (l + r);
      const double side = 0.5 * (l - r);
      frameMid[i] = {mid * window[i], 0.0};
      frameSide[i] = {side * window[i], 0.0};
    }

    fft(frameMid);
    fft(frameSide);
    for (std::size_t k = 0; k < bins; ++k) {
      powerMid[k] += std::norm(frameMid[k]);
      powerSide[k] += std::norm(frameSide[k]);
    }
    ++frameCount;
  };

  if (stereo.numSamples() < n) {
    processFrame(0);
    warnings.push_back("Mono-to-Stereo width test: FFT input shorter than FFT size; zero-padding was applied.");
  } else {
    for (std::size_t start = 0; start + n <= stereo.numSamples(); start += static_cast<std::size_t>(hop)) {
      processFrame(start);
    }
    const std::size_t tailStart = stereo.numSamples() - n;
    if (((stereo.numSamples() - n) % static_cast<std::size_t>(hop)) != 0) {
      processFrame(tailStart);
    }
  }

  if (frameCount == 0) return out;

  out.frequenciesHz.reserve(bins);
  out.ratioDb.reserve(bins);
  out.widthPercent.reserve(bins);
  for (std::size_t k = 0; k < bins; ++k) {
    const double hz = (static_cast<double>(stereo.sampleRate) * static_cast<double>(k)) / static_cast<double>(fftSize);
    const double ratioDb = 10.0 * std::log10((powerMid[k] + kEps) / (powerSide[k] + kEps));
    const double ampMid = std::sqrt(std::max(0.0, powerMid[k]));
    const double ampSide = std::sqrt(std::max(0.0, powerSide[k]));
    const double widthPercent = widthPercentFromRms(ampMid, ampSide);
    out.frequenciesHz.push_back(hz);
    out.ratioDb.push_back(sanitizeFinite(ratioDb, "band ratio", warnings));
    out.widthPercent.push_back(sanitizeFinite(widthPercent, "band width", warnings));
  }
  return out;
}

std::vector<WidthBandSummary> summarizeLogBands24(const std::vector<WidthSpectrumPoint>& spectrum) {
  std::vector<WidthBandSummary> out;
  if (spectrum.empty()) return out;

  constexpr double minHz = 20.0;
  constexpr double maxHz = 20000.0;
  constexpr int bandCount = 24;
  const double ratio = std::pow(maxHz / minHz, 1.0 / static_cast<double>(bandCount));

  out.reserve(bandCount);
  for (int b = 0; b < bandCount; ++b) {
    const double lower = minHz * std::pow(ratio, static_cast<double>(b));
    const double upper = minHz * std::pow(ratio, static_cast<double>(b + 1));
    WidthBandSummary band;
    band.lowerHz = lower;
    band.upperHz = upper;
    band.centerHz = std::sqrt(lower * upper);

    int count = 0;
    for (const auto& point : spectrum) {
      if (point.frequencyHz < lower || point.frequencyHz > upper) continue;
      ++count;
      band.ratioDbA += point.ratioDbA;
      band.ratioDbB += point.ratioDbB;
      band.deltaDb += point.deltaDb;
      band.widthPercentA += point.widthPercentA;
      band.widthPercentB += point.widthPercentB;
      band.deltaWidthPercent += point.deltaWidthPercent;
    }

    if (count > 0) {
      const double inv = 1.0 / static_cast<double>(count);
      band.ratioDbA *= inv;
      band.ratioDbB *= inv;
      band.deltaDb *= inv;
      band.widthPercentA *= inv;
      band.widthPercentB *= inv;
      band.deltaWidthPercent *= inv;
    }
    out.push_back(std::move(band));
  }
  return out;
}

std::vector<float> generatePinkNoise(std::size_t sampleCount, uint32_t seed) {
  std::vector<float> out(sampleCount, 0.0f);
  std::mt19937 rng(seed);
  std::uniform_real_distribution<double> dist(-1.0, 1.0);

  double b0 = 0.0;
  double b1 = 0.0;
  double b2 = 0.0;
  double b3 = 0.0;
  double b4 = 0.0;
  double b5 = 0.0;
  double b6 = 0.0;

  for (std::size_t i = 0; i < sampleCount; ++i) {
    const double white = dist(rng);
    b0 = (0.99886 * b0) + (white * 0.0555179);
    b1 = (0.99332 * b1) + (white * 0.0750759);
    b2 = (0.96900 * b2) + (white * 0.1538520);
    b3 = (0.86650 * b3) + (white * 0.3104856);
    b4 = (0.55000 * b4) + (white * 0.5329522);
    b5 = (-0.7616 * b5) - (white * 0.0168980);
    const double pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + (white * 0.5362);
    b6 = white * 0.115926;
    out[i] = static_cast<float>(pink * 0.11);
  }
  return out;
}

void scaleToTargetRms(std::vector<float>& x, float targetDbfs) {
  if (x.empty()) return;
  const double target = std::pow(10.0, static_cast<double>(targetDbfs) / 20.0);
  const double current = rms(x, 0, x.size());
  if (current <= kEps) return;
  const double gain = target / current;
  for (float& v : x) {
    v = static_cast<float>(v * gain);
  }
}

}  // namespace

namespace mono_stereo_width_detail {

AudioBuffer generateMonoPinkNoiseStereoInput(const TestConfig& config, uint32_t seed) {
  const std::size_t sampleCount =
      static_cast<std::size_t>((static_cast<int64_t>(config.sampleRate) * config.monoStereoWidthDurationMs) / 1000);
  AudioBuffer input = AudioBuffer::zeros(2, sampleCount, config.sampleRate);
  if (sampleCount == 0) return input;

  std::vector<float> mono = generatePinkNoise(sampleCount, seed);
  scaleToTargetRms(mono, config.monoStereoWidthNoiseLevelDbfs);
  std::copy(mono.begin(), mono.end(), input.channels[0].begin());
  std::copy(mono.begin(), mono.end(), input.channels[1].begin());
  return input;
}

}  // namespace mono_stereo_width_detail

MonoToStereoWidthTestResult MonoToStereoWidthTestManager::run(IPluginHost& pluginA, IPluginHost& pluginB,
                                                               const TestConfig& config, std::string& error) const {
  MonoToStereoWidthTestResult result;
  error.clear();

  result.input = mono_stereo_width_detail::generateMonoPinkNoiseStereoInput(config, 1337u);
  result.outputA = pluginA.process(result.input, error);
  if (!error.empty()) return {};
  result.outputB = pluginB.process(result.input, error);
  if (!error.empty()) return {};

  AudioBuffer stereoA = prepareStereoForAnalysis(result.outputA, "Plugin A", result.warnings);
  AudioBuffer stereoB = prepareStereoForAnalysis(result.outputB, "Plugin B", result.warnings);

  result.metrics.pluginLatencySamplesA = pluginA.getLatencySamples();
  result.metrics.pluginLatencySamplesB = pluginB.getLatencySamples();
  const LatencyAlignedBuffers aligned =
      alignForAnalysis(stereoA, stereoB, result.metrics.pluginLatencySamplesA, result.metrics.pluginLatencySamplesB);
  result.alignedA = aligned.alignedA;
  result.alignedB = aligned.alignedB;
  result.latencyAlignment = aligned.info;

  if (result.alignedA.numChannels() < 2 || result.alignedB.numChannels() < 2 || result.alignedA.numSamples() == 0 ||
      result.alignedB.numSamples() == 0) {
    result.warnings.push_back("Mono-to-Stereo width test: aligned output is invalid for stereo analysis.");
    dedupeWarnings(result.warnings);
    return result;
  }

  buildMidSideSignals(result.alignedA, result.analysisMidA, result.analysisSideA);
  buildMidSideSignals(result.alignedB, result.analysisMidB, result.analysisSideB);

  result.timeSeries = computeTimeSeries(result.alignedA, result.alignedB, config.monoStereoWidthTimeWindowMs,
                                        config.monoStereoWidthTimeHopMs, result.warnings);

  const RatioSpectrum spectrumA = computeMidSideRatioSpectrum(result.alignedA, config.monoStereoWidthFftSize,
                                                              config.monoStereoWidthOverlapPercent, result.warnings);
  const RatioSpectrum spectrumB = computeMidSideRatioSpectrum(result.alignedB, config.monoStereoWidthFftSize,
                                                              config.monoStereoWidthOverlapPercent, result.warnings);

  const std::size_t n = std::min(spectrumA.frequenciesHz.size(), spectrumB.frequenciesHz.size());
  result.spectrum.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double hz = spectrumA.frequenciesHz[i];
    if (hz < 20.0 || hz > 20000.0) continue;
    const double a = sanitizeFinite(spectrumA.ratioDb[i], "spectrum ratio A", result.warnings);
    const double b = sanitizeFinite(spectrumB.ratioDb[i], "spectrum ratio B", result.warnings);
    const double widthA = sanitizeFinite(spectrumA.widthPercent[i], "spectrum width A", result.warnings);
    const double widthB = sanitizeFinite(spectrumB.widthPercent[i], "spectrum width B", result.warnings);
    const double deltaWidth = sanitizeFinite(widthA - widthB, "spectrum width delta", result.warnings);
    result.spectrum.push_back({hz, a, b, sanitizeFinite(a - b, "spectrum ratio delta", result.warnings), widthA,
                               widthB, deltaWidth});
  }

  result.bands = summarizeLogBands24(result.spectrum);

  double sumAbsTime = 0.0;
  double sumAbsTimeWidth = 0.0;
  for (const auto& p : result.timeSeries) {
    const double absDelta = std::fabs(p.deltaDb);
    result.metrics.peakAbsTimeDeltaDb = std::max(result.metrics.peakAbsTimeDeltaDb, absDelta);
    sumAbsTime += absDelta;
    const double absWidthDelta = std::fabs(p.deltaWidthPercent);
    result.metrics.peakAbsTimeDeltaWidthPercent =
        std::max(result.metrics.peakAbsTimeDeltaWidthPercent, absWidthDelta);
    sumAbsTimeWidth += absWidthDelta;
  }
  if (!result.timeSeries.empty()) {
    result.metrics.meanAbsTimeDeltaDb = sumAbsTime / static_cast<double>(result.timeSeries.size());
    result.metrics.meanAbsTimeDeltaWidthPercent =
        sumAbsTimeWidth / static_cast<double>(result.timeSeries.size());
  }

  double sumAbsBand = 0.0;
  double sumAbsBandWidth = 0.0;
  int bandCount = 0;
  int bandWidthCount = 0;
  for (const auto& b : result.bands) {
    const double absDelta = std::fabs(b.deltaDb);
    result.metrics.peakAbsBandDeltaDb = std::max(result.metrics.peakAbsBandDeltaDb, absDelta);
    if (std::isfinite(absDelta)) {
      sumAbsBand += absDelta;
      ++bandCount;
    }
    const double absWidthDelta = std::fabs(b.deltaWidthPercent);
    result.metrics.peakAbsBandDeltaWidthPercent =
        std::max(result.metrics.peakAbsBandDeltaWidthPercent, absWidthDelta);
    if (std::isfinite(absWidthDelta)) {
      sumAbsBandWidth += absWidthDelta;
      ++bandWidthCount;
    }
  }
  if (bandCount > 0) {
    result.metrics.meanAbsBandDeltaDb = sumAbsBand / static_cast<double>(bandCount);
  }
  if (bandWidthCount > 0) {
    result.metrics.meanAbsBandDeltaWidthPercent = sumAbsBandWidth / static_cast<double>(bandWidthCount);
  }

  dedupeWarnings(result.warnings);
  return result;
}

}  // namespace vstcompare
