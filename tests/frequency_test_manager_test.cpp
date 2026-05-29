#include "vstcompare/frequency_test_manager.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace vstcompare;

namespace {

class DummyGainDelayHost final : public IPluginHost {
public:
  DummyGainDelayHost(float gainL, float gainR, int delaySamples)
      : gainL_(gainL), gainR_(gainR), delaySamples_(delaySamples) {}

  bool load(const std::string&, int, int, std::string&) override { return true; }

  PluginMetadata getMetadata() const override { return {"Dummy", "Test", "dummy", "1.0", ""}; }

  std::vector<PluginParameter> listParameters() const override { return {}; }

  bool setParameter(uint32_t, double, std::string&) override { return true; }

  AudioBuffer process(const AudioBuffer& input, std::string& error) override {
    error.clear();
    AudioBuffer out = AudioBuffer::zeros(2, input.numSamples(), input.sampleRate);
    for (std::size_t i = 0; i < input.numSamples(); ++i) {
      const std::size_t src = (i >= static_cast<std::size_t>(delaySamples_))
                                  ? (i - static_cast<std::size_t>(delaySamples_))
                                  : std::numeric_limits<std::size_t>::max();
      if (src < input.numSamples()) {
        out.channels[0][i] = input.channels[0][src] * gainL_;
        out.channels[1][i] = input.channels[0][src] * gainR_;
      }
    }
    return out;
  }

  uint32_t getLatencySamples() const override { return static_cast<uint32_t>(delaySamples_); }

private:
  float gainL_ = 1.0f;
  float gainR_ = 1.0f;
  int delaySamples_ = 0;
};

double rms(const std::vector<float>& x) {
  if (x.empty()) return 0.0;
  double sum = 0.0;
  for (float v : x) {
    sum += static_cast<double>(v) * static_cast<double>(v);
  }
  return std::sqrt(sum / static_cast<double>(x.size()));
}

double meanInBandDb(const std::vector<SpectrumPoint>& spectrum, double minHz, double maxHz) {
  double sum = 0.0;
  int count = 0;
  for (const auto& p : spectrum) {
    if (p.frequencyHz < minHz || p.frequencyHz > maxHz) continue;
    sum += p.valueDb;
    ++count;
  }
  return (count > 0) ? (sum / static_cast<double>(count)) : 0.0;
}

}  // namespace

int main() {
  {
    TestConfig config;
    config.sampleRate = 48000;
    config.frequencyDurationMs = 2000;
    config.frequencyNoiseLevelDbfs = -12.0f;
    const auto input = frequency_detail::generateWhiteNoiseInput(config, 42);
    if (input.numChannels() != 1 || input.numSamples() != 96000) {
      std::cerr << "Noise generation length/channel mismatch.\n";
      return 1;
    }
    const double measuredRms = rms(input.channels[0]);
    const double targetRms = std::pow(10.0, -12.0 / 20.0);
    if (std::fabs(measuredRms - targetRms) > 0.02) {
      std::cerr << "Noise RMS mismatch.\n";
      return 1;
    }
  }

  {
    const int sampleRate = 48000;
    const int fftSize = 65536;
    std::vector<float> sine(static_cast<std::size_t>(fftSize), 0.0f);
    for (int i = 0; i < fftSize; ++i) {
      sine[static_cast<std::size_t>(i)] =
          static_cast<float>(std::sin((2.0 * 3.14159265358979323846 * 1000.0 * static_cast<double>(i)) / sampleRate));
    }
    const auto spectrum = frequency_detail::computeAveragedPsdDb(sine, sampleRate, fftSize, 50);
    if (spectrum.empty()) {
      std::cerr << "Spectrum computation returned empty output.\n";
      return 1;
    }
    std::size_t peakIdx = 1;
    for (std::size_t i = 2; i < spectrum.size(); ++i) {
      if (spectrum[i].valueDb > spectrum[peakIdx].valueDb) peakIdx = i;
    }
    const double peakFreq = spectrum[peakIdx].frequencyHz;
    if (std::fabs(peakFreq - 1000.0) > 50.0) {
      std::cerr << "Peak frequency mismatch.\n";
      return 1;
    }
  }

  {
    std::vector<SpectrumPoint> delta;
    for (int i = 0; i < 300; ++i) {
      const double f = 20.0 * std::pow(2.0, static_cast<double>(i) / 30.0);
      delta.push_back({f, static_cast<double>(i) * 0.01});
    }
    const auto bands = frequency_detail::summarizeOneThirdOctave(delta, 20.0, 20000.0, 30);
    if (bands.size() != 30) {
      std::cerr << "1/3 octave band count mismatch.\n";
      return 1;
    }
    if (!(bands.front().centerHz >= 19.0 && bands.front().centerHz <= 21.0)) {
      std::cerr << "First octave band center mismatch.\n";
      return 1;
    }
    if (!(bands.back().centerHz > 15000.0 && bands.back().centerHz < 17000.0)) {
      std::cerr << "Last octave band center mismatch.\n";
      return 1;
    }
  }

  {
    DummyGainDelayHost a(1.0f, 1.0f, 0);
    DummyGainDelayHost b(1.0f, 1.0f, 0);

    FrequencyResponseTestManager test;
    TestConfig config;
    config.sampleRate = 48000;
    config.blockSize = 512;
    config.frequencyDurationMs = 2000;
    config.frequencyFftSize = 65536;
    config.frequencyOverlapPercent = 50;

    std::string error;
    FrequencyTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected bypass test error: " << error << "\n";
      return 1;
    }
    const double meanA = meanInBandDb(result.normalizedSpectrumA, 20.0, 20000.0);
    const double meanB = meanInBandDb(result.normalizedSpectrumB, 20.0, 20000.0);
    if (std::fabs(meanA) > 0.2 || std::fabs(meanB) > 0.2) {
      std::cerr << "Bypass normalization should stay near 0 dB.\n";
      return 1;
    }
  }

  {
    const float minus6 = static_cast<float>(std::pow(10.0, -6.0 / 20.0));
    DummyGainDelayHost a(minus6, minus6, 0);
    DummyGainDelayHost b(minus6, minus6, 0);

    FrequencyResponseTestManager test;
    TestConfig config;
    config.sampleRate = 48000;
    config.blockSize = 512;
    config.frequencyDurationMs = 2000;
    config.frequencyFftSize = 65536;
    config.frequencyOverlapPercent = 50;

    std::string error;
    FrequencyTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected gain test error: " << error << "\n";
      return 1;
    }
    const double meanA = meanInBandDb(result.normalizedSpectrumA, 20.0, 20000.0);
    if (std::fabs(meanA + 6.0) > 0.6) {
      std::cerr << "Expected normalized spectrum near -6 dB.\n";
      return 1;
    }
  }

  {
    DummyGainDelayHost a(1.0f, 1.0f, 0);
    DummyGainDelayHost b(0.5f, 0.5f, 5);

    FrequencyResponseTestManager test;
    TestConfig config;
    config.sampleRate = 48000;
    config.blockSize = 512;
    config.frequencyDurationMs = 2000;
    config.frequencyFftSize = 65536;
    config.frequencyOverlapPercent = 50;

    std::string error;
    FrequencyTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected frequency test error: " << error << "\n";
      return 1;
    }
    if (result.analysisSignalMode != "mono_lr_average") {
      std::cerr << "Expected mono_lr_average mode.\n";
      return 1;
    }
    if (result.latencyAlignment.appliedDelayA != 5 || result.latencyAlignment.appliedDelayB != 0) {
      std::cerr << "Latency alignment mismatch.\n";
      return 1;
    }
    if (result.alignedA.numSamples() != result.alignedB.numSamples()) {
      std::cerr << "Aligned sample length mismatch.\n";
      return 1;
    }
    if (result.metrics.meanAbsDeltaDb < 5.0 || result.metrics.meanAbsDeltaDb > 7.5) {
      std::cerr << "Unexpected mean delta dB.\n";
      return 1;
    }
    const std::size_t n = std::min(result.normalizedSpectrumA.size(), result.normalizedSpectrumB.size());
    if (n == 0 || result.delta.size() != n) {
      std::cerr << "Normalized spectrum size mismatch.\n";
      return 1;
    }
    for (std::size_t i = 0; i < std::min<std::size_t>(n, 64); ++i) {
      const double expected = result.normalizedSpectrumA[i].valueDb - result.normalizedSpectrumB[i].valueDb;
      if (std::fabs(result.delta[i].valueDb - expected) > 1e-6) {
        std::cerr << "Delta mismatch against normalized spectra.\n";
        return 1;
      }
    }
    if (result.octaveBands.size() != 30) {
      std::cerr << "Expected 30 octave bands.\n";
      return 1;
    }
  }

  return 0;
}
