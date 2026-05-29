#include "vstcompare/thd_test_manager.hpp"

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
  explicit DummyBypassHost(int delaySamples) : delaySamples_(delaySamples) {}

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
        out.channels[0][i] = x;
        out.channels[1][i] = x;
      }
    }
    return out;
  }

  uint32_t getLatencySamples() const override { return static_cast<uint32_t>(delaySamples_); }

private:
  int delaySamples_ = 0;
};

class DummyNonlinearHost final : public IPluginHost {
public:
  bool load(const std::string&, int, int, std::string&) override { return true; }
  PluginMetadata getMetadata() const override { return {"DummyNL", "Test", "dummy_nl", "1.0", ""}; }
  std::vector<PluginParameter> listParameters() const override { return {}; }
  bool setParameter(uint32_t, double, std::string&) override { return true; }

  AudioBuffer process(const AudioBuffer& input, std::string& error) override {
    error.clear();
    AudioBuffer out = AudioBuffer::zeros(2, input.numSamples(), input.sampleRate);
    for (std::size_t i = 0; i < input.numSamples(); ++i) {
      const double x = static_cast<double>(input.channels[0][i]);
      const double y = x + (0.4 * x * x * x);
      const float clamped = static_cast<float>(std::max(-1.0, std::min(1.0, y)));
      out.channels[0][i] = clamped;
      out.channels[1][i] = clamped;
    }
    return out;
  }

  uint32_t getLatencySamples() const override { return 0; }
};

class DummySilentHost final : public IPluginHost {
public:
  bool load(const std::string&, int, int, std::string&) override { return true; }
  PluginMetadata getMetadata() const override { return {"DummySilent", "Test", "dummy_silent", "1.0", ""}; }
  std::vector<PluginParameter> listParameters() const override { return {}; }
  bool setParameter(uint32_t, double, std::string&) override { return true; }

  AudioBuffer process(const AudioBuffer& input, std::string& error) override {
    error.clear();
    return AudioBuffer::zeros(2, input.numSamples(), input.sampleRate);
  }

  uint32_t getLatencySamples() const override { return 0; }
};

bool almostEqual(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps;
}

}  // namespace

int main() {
  TestConfig config;
  config.sampleRate = 48000;
  config.blockSize = 512;
  config.thdToneFrequencyHz = 1000.0;
  config.thdStartLevelDbfs = -60.0f;
  config.thdEndLevelDbfs = 6.0f;
  config.thdStepDb = 2.0f;
  config.thdDurationMs = 1000;
  config.thdSkipHeadMs = 200;
  config.thdFftSize = 65536;

  {
    const auto levels = thd_detail::buildSweepLevels(config);
    if (levels.size() != 34 || !almostEqual(levels.front(), -60.0) || !almostEqual(levels.back(), 6.0)) {
      std::cerr << "THD sweep level generation mismatch.\n";
      return 1;
    }
  }

  {
    DummyBypassHost a(0);
    DummyBypassHost b(16);
    ThdThdnTestManager test;
    std::string error;
    const ThdTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected THD bypass error: " << error << "\n";
      return 1;
    }
    if (result.sweep.empty()) {
      std::cerr << "THD sweep should not be empty.\n";
      return 1;
    }
    if (result.sweep.front().analysisSignalMode != "mono_lr_average") {
      std::cerr << "Expected mono_lr_average analysis mode.\n";
      return 1;
    }
    if (result.sweep.front().latencyAlignment.appliedDelayA != 16 ||
        result.sweep.front().latencyAlignment.appliedDelayB != 0) {
      std::cerr << "THD latency alignment mismatch.\n";
      return 1;
    }
    if (result.metrics.meanAbsThdDeltaPercent > 0.2 || result.metrics.meanAbsThdnDeltaPercent > 0.2) {
      std::cerr << "Bypass A/B should remain near zero THD deltas.\n";
      return 1;
    }
    double maxThdnPercent = 0.0;
    bool foundReducedFftWarning = false;
    for (const auto& p : result.sweep) {
      if (!almostEqual(p.thdDeltaPercent, p.thdPercentA - p.thdPercentB, 1e-6)) {
        std::cerr << "THD delta mismatch.\n";
        return 1;
      }
      if (!almostEqual(p.thdnDeltaPercent, p.thdnPercentA - p.thdnPercentB, 1e-6)) {
        std::cerr << "THD+N delta mismatch.\n";
        return 1;
      }
      for (const auto& w : p.warnings) {
        if (w.find("FFT size reduced") != std::string::npos) {
          foundReducedFftWarning = true;
          break;
        }
      }
      maxThdnPercent = std::max(maxThdnPercent, std::max(p.thdnPercentA, p.thdnPercentB));
    }
    if (!foundReducedFftWarning) {
      std::cerr << "Expected THD FFT reduction warning when available samples are below configured FFT size.\n";
      return 1;
    }
    if (maxThdnPercent >= 100.0) {
      std::cerr << "THD+N unexpectedly pinned near 100% in bypass case.\n";
      return 1;
    }
  }

  {
    DummyBypassHost a(0);
    DummyNonlinearHost b;
    ThdThdnTestManager test;
    std::string error;
    const ThdTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected THD nonlinear error: " << error << "\n";
      return 1;
    }
    if (result.sweep.size() < 2) {
      std::cerr << "Expected multi-point THD sweep for nonlinear test.\n";
      return 1;
    }
    if (result.sweep.back().thdPercentB <= result.sweep.front().thdPercentB) {
      std::cerr << "Expected THD to increase toward hotter input levels for nonlinear host.\n";
      return 1;
    }
    if (result.metrics.peakAbsThdDeltaPercent < 0.1) {
      std::cerr << "Expected non-trivial THD delta for nonlinear host.\n";
      return 1;
    }
  }

  {
    DummyBypassHost a(0);
    DummySilentHost b;
    ThdThdnTestManager test;
    std::string error;
    const ThdTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected THD silent-host error: " << error << "\n";
      return 1;
    }
    bool foundFundamentalWarning = false;
    for (const auto& p : result.sweep) {
      for (const auto& w : p.warnings) {
        if (w.find("fundamental detection failed") != std::string::npos) {
          foundFundamentalWarning = true;
          break;
        }
      }
      if (foundFundamentalWarning) break;
    }
    if (!foundFundamentalWarning) {
      std::cerr << "Expected fundamental detection warning for silent host.\n";
      return 1;
    }
  }

  {
    TestConfig shortConfig = config;
    shortConfig.thdDurationMs = 20;
    shortConfig.thdSkipHeadMs = 200;

    DummyBypassHost a(0);
    DummyBypassHost b(0);
    ThdThdnTestManager test;
    std::string error;
    const ThdTestResult result = test.run(a, b, shortConfig, error);
    if (!error.empty()) {
      std::cerr << "Unexpected THD short-window error: " << error << "\n";
      return 1;
    }
    if (result.sweep.empty()) {
      std::cerr << "THD short-window sweep should not be empty.\n";
      return 1;
    }
    bool foundInsufficientSampleWarning = false;
    for (const auto& p : result.sweep) {
      if (p.thdPercentA != 0.0 || p.thdPercentB != 0.0 || p.thdnPercentA != 0.0 || p.thdnPercentB != 0.0) {
        std::cerr << "Expected zeroed THD metrics for insufficient analysis samples.\n";
        return 1;
      }
      for (const auto& w : p.warnings) {
        if (w.find("insufficient post-skip samples") != std::string::npos) {
          foundInsufficientSampleWarning = true;
          break;
        }
      }
      if (foundInsufficientSampleWarning) break;
    }
    if (!foundInsufficientSampleWarning) {
      std::cerr << "Expected insufficient-sample warning for short-window THD analysis.\n";
      return 1;
    }
  }

  return 0;
}
