#include "vstcompare/time_response_test_manager.hpp"

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
  PluginMetadata getMetadata() const override { return {"DummyBypass", "Test", "dummy_bypass", "1.0", ""}; }
  std::vector<PluginParameter> listParameters() const override { return {}; }
  bool setParameter(uint32_t, double, std::string&) override { return true; }

  AudioBuffer process(const AudioBuffer& input, std::string& error) override {
    error.clear();
    AudioBuffer out = AudioBuffer::zeros(2, input.numSamples(), input.sampleRate);
    for (std::size_t i = 0; i < input.numSamples(); ++i) {
      const std::size_t src = (i >= static_cast<std::size_t>(delaySamples_))
                                  ? (i - static_cast<std::size_t>(delaySamples_))
                                  : std::numeric_limits<std::size_t>::max();
      if (src >= input.numSamples()) continue;
      const float x = input.channels[0][src];
      out.channels[0][i] = x;
      out.channels[1][i] = x;
    }
    return out;
  }

  uint32_t getLatencySamples() const override { return static_cast<uint32_t>(delaySamples_); }

private:
  int delaySamples_ = 0;
};

class DummySlowEnvelopeHost final : public IPluginHost {
public:
  bool load(const std::string&, int, int, std::string&) override { return true; }
  PluginMetadata getMetadata() const override { return {"DummySlowEnv", "Test", "dummy_slow_env", "1.0", ""}; }
  std::vector<PluginParameter> listParameters() const override { return {}; }
  bool setParameter(uint32_t, double, std::string&) override { return true; }

  AudioBuffer process(const AudioBuffer& input, std::string& error) override {
    error.clear();
    AudioBuffer out = AudioBuffer::zeros(2, input.numSamples(), input.sampleRate);
    double gain = 0.0;
    const double attack = 0.02;
    const double release = 0.004;
    for (std::size_t i = 0; i < input.numSamples(); ++i) {
      const double x = static_cast<double>(input.channels[0][i]);
      const double target = std::fabs(x);
      if (target > gain) {
        gain += attack * (target - gain);
      } else {
        gain += release * (target - gain);
      }
      const float y = static_cast<float>(std::copysign(gain, x));
      out.channels[0][i] = y;
      out.channels[1][i] = y;
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

}  // namespace

int main() {
  TestConfig config;
  config.sampleRate = 48000;
  config.blockSize = 512;
  config.timeBurstFrequencyHz = 1000.0;
  config.timeBurstLevelDbfs = -6.0f;
  config.timePreSilenceMs = 200;
  config.timeBurstOnMs = 100;
  config.timePostSilenceMs = 200;
  config.timeEnvelopeWindowMs = 20;
  config.timeCorrelationMaxLagMs = 50;

  {
    const AudioBuffer input = time_response_detail::generateToneBurstInput(config);
    const std::size_t expectedSamples = static_cast<std::size_t>(config.sampleRate / 2);
    if (input.numChannels() != 1 || input.numSamples() != expectedSamples) {
      std::cerr << "Time burst generation length/channel mismatch.\n";
      return 1;
    }
    const std::size_t preSamples = static_cast<std::size_t>((config.sampleRate * config.timePreSilenceMs) / 1000);
    if (preSamples > 0 && std::fabs(input.channels[0][preSamples - 1]) > 1e-8f) {
      std::cerr << "Expected silence before burst onset.\n";
      return 1;
    }
  }

  {
    DummyBypassHost a(0);
    DummyBypassHost b(24);
    TimeResponseTestManager test;
    std::string error;
    const TimeResponseTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected time-response bypass error: " << error << "\n";
      return 1;
    }
    if (result.curve.empty()) {
      std::cerr << "Expected non-empty time-response curve.\n";
      return 1;
    }
    if (result.analysisSignalMode != "mono_lr_average") {
      std::cerr << "Expected mono_lr_average time-response analysis mode.\n";
      return 1;
    }
    if (result.latencyAlignment.appliedDelayA != 24 || result.latencyAlignment.appliedDelayB != 0) {
      std::cerr << "Time-response latency alignment mismatch.\n";
      return 1;
    }
    if (std::fabs(result.metrics.residualLatencyMs) > 1.0) {
      std::cerr << "Expected near-zero residual latency after alignment.\n";
      return 1;
    }
    if (std::fabs(result.metrics.attackDeltaMs) > 4.0 || std::fabs(result.metrics.releaseDeltaMs) > 4.0) {
      std::cerr << "Bypass hosts should have small attack/release deltas.\n";
      return 1;
    }
  }

  {
    DummyBypassHost a(0);
    DummySlowEnvelopeHost b;
    TimeResponseTestManager test;
    std::string error;
    const TimeResponseTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected time-response slow-envelope error: " << error << "\n";
      return 1;
    }
    if (result.metrics.attackMsB <= result.metrics.attackMsA) {
      std::cerr << "Expected slower host to have longer attack time.\n";
      return 1;
    }
  }

  {
    DummyBypassHost a(0);
    DummySilentHost b;
    TimeResponseTestManager test;
    std::string error;
    const TimeResponseTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected time-response silent-host error: " << error << "\n";
      return 1;
    }
    if (result.warnings.empty()) {
      std::cerr << "Expected warnings for silent-host timing estimation.\n";
      return 1;
    }
  }

  return 0;
}

