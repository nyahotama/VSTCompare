#include "vstcompare/dynamics_test_manager.hpp"

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

class DummyCompressorHost final : public IPluginHost {
public:
  bool load(const std::string&, int, int, std::string&) override { return true; }
  PluginMetadata getMetadata() const override { return {"DummyComp", "Test", "dummy_comp", "1.0", ""}; }
  std::vector<PluginParameter> listParameters() const override { return {}; }
  bool setParameter(uint32_t, double, std::string&) override { return true; }

  AudioBuffer process(const AudioBuffer& input, std::string& error) override {
    error.clear();
    AudioBuffer out = AudioBuffer::zeros(2, input.numSamples(), input.sampleRate);
    const double thresholdAmp = std::pow(10.0, -24.0 / 20.0);
    const double ratio = 4.0;
    for (std::size_t i = 0; i < input.numSamples(); ++i) {
      const double x = static_cast<double>(input.channels[0][i]);
      const double ax = std::fabs(x);
      double y = ax;
      if (ax > thresholdAmp) {
        y = thresholdAmp + ((ax - thresholdAmp) / ratio);
      }
      const float outSample = static_cast<float>(std::copysign(y, x));
      out.channels[0][i] = outSample;
      out.channels[1][i] = outSample;
    }
    return out;
  }

  uint32_t getLatencySamples() const override { return 0; }
};

bool approx(double a, double b, double eps = 1e-9) {
  return std::fabs(a - b) <= eps;
}

}  // namespace

int main() {
  TestConfig config;
  config.sampleRate = 48000;
  config.blockSize = 512;
  config.dynamicsToneFrequencyHz = 1000.0;
  config.dynamicsStartLevelDbfs = -90.0f;
  config.dynamicsEndLevelDbfs = 0.0f;
  config.dynamicsStepDb = 1.0f;
  config.dynamicsStepMs = 50;
  config.dynamicsRmsWindowMs = 20;

  {
    const auto levels = dynamics_detail::buildSweepLevels(config);
    if (levels.size() != 91 || !approx(levels.front(), -90.0) || !approx(levels.back(), 0.0)) {
      std::cerr << "Dynamics sweep level generation mismatch.\n";
      return 1;
    }

    const auto input = dynamics_detail::generateStepSineInput(config);
    const std::size_t stepSamples = static_cast<std::size_t>((config.sampleRate * config.dynamicsStepMs) / 1000);
    if (input.numChannels() != 1 || input.numSamples() != (levels.size() * stepSamples)) {
      std::cerr << "Dynamics step signal generation length/channel mismatch.\n";
      return 1;
    }
  }

  {
    DummyBypassHost a(0);
    DummyBypassHost b(32);
    DynamicsTestManager test;
    std::string error;
    const DynamicsTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected dynamics bypass error: " << error << "\n";
      return 1;
    }
    if (result.ioCurve.empty()) {
      std::cerr << "Expected non-empty dynamics IO curve.\n";
      return 1;
    }
    if (result.analysisSignalMode != "mono_lr_average") {
      std::cerr << "Expected mono_lr_average dynamics analysis mode.\n";
      return 1;
    }
    if (result.latencyAlignment.appliedDelayA != 32 || result.latencyAlignment.appliedDelayB != 0) {
      std::cerr << "Dynamics latency alignment mismatch.\n";
      return 1;
    }
    if (result.metrics.meanAbsOutputDeltaDb > 0.5 || result.metrics.meanAbsGainReductionDeltaDb > 0.5) {
      std::cerr << "Bypass dynamics deltas should remain near zero.\n";
      return 1;
    }
    for (const auto& p : result.ioCurve) {
      if (!approx(p.outputDeltaDb, p.outputLevelDbfsA - p.outputLevelDbfsB, 1e-6)) {
        std::cerr << "Dynamics IO delta mismatch.\n";
        return 1;
      }
      if (!approx(p.gainReductionDeltaDb, p.gainReductionDbA - p.gainReductionDbB, 1e-6)) {
        std::cerr << "Dynamics GR delta mismatch.\n";
        return 1;
      }
    }
  }

  {
    DummyBypassHost a(0);
    DummyCompressorHost b;
    DynamicsTestManager test;
    std::string error;
    const DynamicsTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected dynamics compressor error: " << error << "\n";
      return 1;
    }
    if (result.metrics.estimatedB.valid == false) {
      std::cerr << "Expected valid estimated dynamics parameters for compressor host.\n";
      return 1;
    }
    if (result.metrics.estimatedB.ratio < 1.2) {
      std::cerr << "Expected compressor ratio estimate > 1.2.\n";
      return 1;
    }
    if (result.metrics.estimatedB.thresholdDbfs > -10.0) {
      std::cerr << "Expected compressor threshold below -10dBFS.\n";
      return 1;
    }
  }

  return 0;
}

