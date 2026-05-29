#include "vstcompare/pitch_test_manager.hpp"

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
    AudioBuffer out = AudioBuffer::zeros(1, input.numSamples(), input.sampleRate);
    for (std::size_t i = 0; i < input.numSamples(); ++i) {
      const std::size_t src = (i >= static_cast<std::size_t>(delaySamples_))
                                  ? (i - static_cast<std::size_t>(delaySamples_))
                                  : std::numeric_limits<std::size_t>::max();
      if (src >= input.numSamples()) continue;
      out.channels[0][i] = input.channels[0][src];
    }
    return out;
  }

  uint32_t getLatencySamples() const override { return static_cast<uint32_t>(delaySamples_); }

private:
  int delaySamples_ = 0;
};

bool approx(double a, double b, double eps = 1e-6) {
  return std::fabs(a - b) <= eps;
}

}  // namespace

int main() {
  {
    TestConfig config;
    config.sampleRate = 48000;
    config.pitchDurationMs = 5000;
    config.pitchStartHz = 110.0f;
    config.pitchEndHz = 880.0f;
    config.pitchLevelDbfs = -6.0f;
    config.pitchFadeMs = 5;
    const auto input = pitch_detail::generateLogSweepInput(config);
    if (input.numChannels() != 1 || input.numSamples() != 240000) {
      std::cerr << "Pitch sweep generation length/channel mismatch.\n";
      return 1;
    }
    if (std::fabs(input.channels[0].front()) > 1e-5 || std::fabs(input.channels[0].back()) > 1e-5) {
      std::cerr << "Pitch sweep fade-in/fade-out should start and end near zero.\n";
      return 1;
    }
  }

  {
    const int sampleRate = 48000;
    const double targetHz = 440.0;
    const std::size_t count = static_cast<std::size_t>(sampleRate / 2);
    std::vector<float> tone(count, 0.0f);
    for (std::size_t i = 0; i < count; ++i) {
      const double phase = (2.0 * 3.14159265358979323846 * targetHz * static_cast<double>(i)) / sampleRate;
      tone[i] = static_cast<float>(0.5 * std::sin(phase));
    }
    const double estimated = pitch_detail::estimatePitchHzFrame(tone, 0, tone.size(), sampleRate, 110.0, 880.0);
    if (std::fabs(estimated - targetHz) > 2.0) {
      std::cerr << "Pitch frame estimator did not track known 440Hz tone.\n";
      return 1;
    }
  }

  {
    TestConfig config;
    config.sampleRate = 48000;
    config.blockSize = 512;
    config.pitchDurationMs = 1000;
    config.pitchFrameMs = 100;
    config.pitchHopMs = 10;
    config.pitchStartHz = 110.0f;
    config.pitchEndHz = 880.0f;
    config.pitchLevelDbfs = -6.0f;
    config.pitchFadeMs = 5;

    DummyBypassHost a(0);
    DummyBypassHost b(24);
    PitchCorrectionTrackingTestManager test;
    std::string error;
    const PitchTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected pitch manager error: " << error << "\n";
      return 1;
    }
    if (result.curve.empty()) {
      std::cerr << "Expected non-empty pitch tracking curve.\n";
      return 1;
    }
    if (result.latencyAlignment.appliedDelayA != 24 || result.latencyAlignment.appliedDelayB != 0) {
      std::cerr << "Pitch latency alignment mismatch.\n";
      return 1;
    }
    if (result.alignedA.numSamples() != result.alignedB.numSamples()) {
      std::cerr << "Aligned pitch analysis lengths should match.\n";
      return 1;
    }
    if (result.metrics.meanAbsDeltaHz > 5.0) {
      std::cerr << "Bypass pitch delta should remain small.\n";
      return 1;
    }
    if (result.metrics.validFrameRateA <= 0.1 || result.metrics.validFrameRateB <= 0.1) {
      std::cerr << "Valid frame rate should not collapse for bypass hosts.\n";
      return 1;
    }
    if (!approx(result.curve.front().pitchHzA, result.curve.front().pitchHzB, 10.0)) {
      std::cerr << "Bypass hosts should have similar first-frame pitch.\n";
      return 1;
    }
  }

  return 0;
}
