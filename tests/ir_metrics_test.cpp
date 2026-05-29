#include "vstcompare/ir_test_manager.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace vstcompare;

class DummyHost final : public IPluginHost {
public:
  explicit DummyHost(float gainL, float gainR, int delaySamples)
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

class MonoDummyHost final : public IPluginHost {
public:
  explicit MonoDummyHost(float gain, int delaySamples) : gain_(gain), delaySamples_(delaySamples) {}

  bool load(const std::string&, int, int, std::string&) override { return true; }
  PluginMetadata getMetadata() const override { return {"MonoDummy", "Test", "mono-dummy", "1.0", ""}; }
  std::vector<PluginParameter> listParameters() const override { return {}; }
  bool setParameter(uint32_t, double, std::string&) override { return true; }

  AudioBuffer process(const AudioBuffer& input, std::string& error) override {
    error.clear();
    AudioBuffer out = AudioBuffer::zeros(1, input.numSamples(), input.sampleRate);
    for (std::size_t i = 0; i < input.numSamples(); ++i) {
      const std::size_t src = (i >= static_cast<std::size_t>(delaySamples_))
                                  ? (i - static_cast<std::size_t>(delaySamples_))
                                  : std::numeric_limits<std::size_t>::max();
      if (src < input.numSamples()) {
        out.channels[0][i] = input.channels[0][src] * gain_;
      }
    }
    return out;
  }

  uint32_t getLatencySamples() const override { return static_cast<uint32_t>(delaySamples_); }

private:
  float gain_ = 1.0f;
  int delaySamples_ = 0;
};

int main() {
  DummyHost a(1.0f, 0.0f, 0);
  DummyHost b(0.0f, 1.0f, 5);

  IrTestManager test;
  TestConfig config;
  config.sampleRate = 48000;
  config.blockSize = 512;
  config.irDurationMs = 50;

  std::string error;
  IrTestResult result = test.run(a, b, config, error);
  if (!error.empty()) {
    std::cerr << "Unexpected error: " << error << "\n";
    return 1;
  }

  if (result.metrics.peakAbsDelta <= 0.0f) {
    std::cerr << "Expected non-zero delta.\n";
    return 1;
  }

  if (std::fabs(result.metrics.estimatedLatencyMs) > 0.05) {
    std::cerr << "Expected latency difference to be aligned close to zero.\n";
    return 1;
  }

  if (result.latencyAlignment.appliedDelayA != 5 || result.latencyAlignment.appliedDelayB != 0) {
    std::cerr << "Unexpected applied delay values.\n";
    return 1;
  }

  if (result.analysisSignalMode != "mono_lr_average") {
    std::cerr << "Expected mono_lr_average analysis mode.\n";
    return 1;
  }

  if (result.displayAlignedA.numChannels() != 2 || result.displayAlignedB.numChannels() != 2 ||
      result.displayDelta.numChannels() != 2) {
    std::cerr << "Expected stereo display-aligned IR buffers.\n";
    return 1;
  }

  for (std::size_t i = 0; i < result.displayDelta.numSamples(); ++i) {
    const float expectedLeft = result.displayAlignedA.channels[0][i] - result.displayAlignedB.channels[0][i];
    const float expectedRight = result.displayAlignedA.channels[1][i] - result.displayAlignedB.channels[1][i];
    if (std::fabs(result.displayDelta.channels[0][i] - expectedLeft) > 1e-6f ||
        std::fabs(result.displayDelta.channels[1][i] - expectedRight) > 1e-6f) {
      std::cerr << "Display delta must match A-B per channel.\n";
      return 1;
    }
  }

  MonoDummyHost monoA(1.0f, 0);
  MonoDummyHost monoB(0.7f, 3);
  error.clear();
  IrTestResult monoResult = test.run(monoA, monoB, config, error);
  if (!error.empty()) {
    std::cerr << "Unexpected mono-host error: " << error << "\n";
    return 1;
  }
  if (monoResult.displayAlignedA.numChannels() != 1 || monoResult.displayAlignedB.numChannels() != 1 ||
      monoResult.displayDelta.numChannels() != 1) {
    std::cerr << "Mono hosts should keep single-channel display series.\n";
    return 1;
  }

  return 0;
}
