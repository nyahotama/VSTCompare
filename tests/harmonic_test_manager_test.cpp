#include "vstcompare/harmonic_test_manager.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace vstcompare;

namespace {

class DummyBypassHost final : public IPluginHost {
public:
  DummyBypassHost(float gainL, float gainR, int delaySamples) : gainL_(gainL), gainR_(gainR), delaySamples_(delaySamples) {}

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
        const float x = input.channels[0][src];
        out.channels[0][i] = x * gainL_;
        out.channels[1][i] = x * gainR_;
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

class DummyNonlinearHost final : public IPluginHost {
public:
  explicit DummyNonlinearHost(int delaySamples) : delaySamples_(delaySamples) {}

  bool load(const std::string&, int, int, std::string&) override { return true; }
  PluginMetadata getMetadata() const override { return {"DummyNonlinear", "Test", "dummy_nl", "1.0", ""}; }
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
        const double x = static_cast<double>(input.channels[0][src]);
        const double y = x + (0.35 * x * x) + (0.2 * x * x * x);
        const float clamped = static_cast<float>(std::max(-1.0, std::min(1.0, y)));
        out.channels[0][i] = clamped;
        out.channels[1][i] = clamped;
      }
    }
    return out;
  }

  uint32_t getLatencySamples() const override { return static_cast<uint32_t>(delaySamples_); }

private:
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

}  // namespace

int main() {
  TestConfig config;
  config.sampleRate = 48000;
  config.blockSize = 512;
  config.harmonicDurationMs = 1000;
  config.harmonicSkipHeadMs = 200;
  config.harmonicFftSize = 65536;
  config.harmonicInputLevelDbfs = -6.0f;
  config.harmonicFrequenciesHz = {1000.0};

  {
    const auto input = harmonic_detail::generateSineInput(config, 1000.0);
    if (input.numChannels() != 1 || input.numSamples() != 48000) {
      std::cerr << "Sine generation length/channel mismatch.\n";
      return 1;
    }
    const double measuredRms = rms(input.channels[0]);
    const double amp = std::pow(10.0, -6.0 / 20.0);
    const double targetRms = amp / std::sqrt(2.0);
    if (std::fabs(measuredRms - targetRms) > 0.02) {
      std::cerr << "Sine RMS mismatch.\n";
      return 1;
    }
  }

  {
    const auto input = harmonic_detail::generateSineInput(config, 1000.0);
    const auto spectrum =
        harmonic_detail::computeSpectrumDbfs(input.channels[0], config.sampleRate, config.harmonicFftSize,
                                             static_cast<int>((config.sampleRate * config.harmonicSkipHeadMs) / 1000));
    if (spectrum.empty()) {
      std::cerr << "Harmonic spectrum computation returned empty output.\n";
      return 1;
    }
    std::size_t peakIdx = 1;
    for (std::size_t i = 2; i < spectrum.size(); ++i) {
      if (spectrum[i].valueDbfs > spectrum[peakIdx].valueDbfs) peakIdx = i;
    }
    const double peakFreq = spectrum[peakIdx].frequencyHz;
    if (std::fabs(peakFreq - 1000.0) > 40.0) {
      std::cerr << "Harmonic spectrum peak frequency mismatch.\n";
      return 1;
    }
  }

  {
    DummyBypassHost a(1.0f, 1.0f, 16);
    DummyBypassHost b(1.0f, 1.0f, 0);
    HarmonicDistortionTestManager test;
    std::string error;
    HarmonicTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected bypass harmonic test error: " << error << "\n";
      return 1;
    }
    if (result.pitches.size() != 1) {
      std::cerr << "Expected exactly one harmonic pitch result.\n";
      return 1;
    }
    const auto& p = result.pitches.front();
    if (p.analysisSignalMode != "mono_lr_average") {
      std::cerr << "Expected mono_lr_average mode.\n";
      return 1;
    }
    if (p.latencyAlignment.appliedDelayA != 0 || p.latencyAlignment.appliedDelayB != 16) {
      std::cerr << "Harmonic latency alignment mismatch.\n";
      return 1;
    }
    if (p.orders.empty()) {
      std::cerr << "Expected harmonic order summaries.\n";
      return 1;
    }
    if (std::fabs(p.orders.front().deltaDb) > 0.8) {
      std::cerr << "Bypass hosts should have near-zero fundamental delta.\n";
      return 1;
    }
    if (result.metrics.meanAbsDeltaDb > 2.0) {
      std::cerr << "Bypass hosts should have small average delta.\n";
      return 1;
    }
  }

  {
    TestConfig shortConfig = config;
    shortConfig.harmonicDurationMs = 10;
    shortConfig.harmonicSkipHeadMs = 200;

    DummyBypassHost a(1.0f, 1.0f, 0);
    DummyBypassHost b(1.0f, 1.0f, 0);
    HarmonicDistortionTestManager test;
    std::string error;
    HarmonicTestResult result = test.run(a, b, shortConfig, error);
    if (!error.empty()) {
      std::cerr << "Unexpected short-window harmonic test error: " << error << "\n";
      return 1;
    }
    if (result.pitches.empty()) {
      std::cerr << "Expected short-window harmonic pitch result.\n";
      return 1;
    }
    const auto& warnings = result.pitches.front().latencyAlignment.warnings;
    bool foundSkipWarning = false;
    for (const auto& w : warnings) {
      if (w.find("skipHead exceeded") != std::string::npos) {
        foundSkipWarning = true;
        break;
      }
    }
    if (!foundSkipWarning) {
      std::cerr << "Expected skipHead adjustment warning for short-window case.\n";
      return 1;
    }
  }

  {
    DummyBypassHost a(1.0f, 1.0f, 0);
    DummyNonlinearHost b(0);
    HarmonicDistortionTestManager test;
    std::string error;
    HarmonicTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected nonlinear harmonic test error: " << error << "\n";
      return 1;
    }
    if (result.pitches.empty()) {
      std::cerr << "Expected harmonic pitch results.\n";
      return 1;
    }
    const auto& p = result.pitches.front();
    bool foundLargeOrderDelta = false;
    for (const auto& order : p.orders) {
      if (order.order >= 2 && std::fabs(order.deltaDb) > 6.0) {
        foundLargeOrderDelta = true;
        break;
      }
    }
    if (!foundLargeOrderDelta) {
      std::cerr << "Expected strong higher-order harmonic deltas for nonlinear host.\n";
      return 1;
    }
  }

  return 0;
}
