#include "vstcompare/mono_to_stereo_width_test_manager.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>

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
      out.channels[0][i] = input.channels[0][src];
      out.channels[1][i] = input.channels[1][src];
    }
    return out;
  }

  uint32_t getLatencySamples() const override { return static_cast<uint32_t>(delaySamples_); }

private:
  int delaySamples_ = 0;
};

class DummySideInjectHost final : public IPluginHost {
public:
  explicit DummySideInjectHost(float sideGain) : sideGain_(sideGain) {}

  bool load(const std::string&, int, int, std::string&) override { return true; }
  PluginMetadata getMetadata() const override { return {"DummySide", "Test", "dummy_side", "1.0", ""}; }
  std::vector<PluginParameter> listParameters() const override { return {}; }
  bool setParameter(uint32_t, double, std::string&) override { return true; }

  AudioBuffer process(const AudioBuffer& input, std::string& error) override {
    error.clear();
    AudioBuffer out = AudioBuffer::zeros(2, input.numSamples(), input.sampleRate);
    for (std::size_t i = 0; i < input.numSamples(); ++i) {
      const float x = input.channels[0][i];
      out.channels[0][i] = x + (x * sideGain_);
      out.channels[1][i] = x - (x * sideGain_);
    }
    return out;
  }

  uint32_t getLatencySamples() const override { return 0; }

private:
  float sideGain_ = 0.0f;
};

bool approx(double a, double b, double eps = 1e-6) {
  return std::fabs(a - b) <= eps;
}

}  // namespace

int main() {
  TestConfig config;
  config.sampleRate = 48000;
  config.blockSize = 512;
  config.monoStereoWidthDurationMs = 500;
  config.monoStereoWidthNoiseLevelDbfs = -12.0f;
  config.monoStereoWidthFftSize = 65536;
  config.monoStereoWidthOverlapPercent = 50;
  config.monoStereoWidthTimeWindowMs = 20;
  config.monoStereoWidthTimeHopMs = 10;

  {
    const AudioBuffer inputA = mono_stereo_width_detail::generateMonoPinkNoiseStereoInput(config, 123u);
    const AudioBuffer inputB = mono_stereo_width_detail::generateMonoPinkNoiseStereoInput(config, 123u);
    if (inputA.numChannels() != 2 || inputA.numSamples() == 0) {
      std::cerr << "Mono pink-noise generator should return non-empty stereo buffer.\n";
      return 1;
    }
    for (std::size_t i = 0; i < std::min<std::size_t>(256, inputA.numSamples()); ++i) {
      if (!approx(inputA.channels[0][i], inputA.channels[1][i], 1e-7)) {
        std::cerr << "Expected identical L/R for mono pink-noise input.\n";
        return 1;
      }
      if (!approx(inputA.channels[0][i], inputB.channels[0][i], 1e-7)) {
        std::cerr << "Expected deterministic pink-noise sequence for fixed seed.\n";
        return 1;
      }
    }
  }

  {
    DummyBypassHost a(0);
    DummyBypassHost b(24);
    MonoToStereoWidthTestManager test;
    std::string error;
    const MonoToStereoWidthTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected mono-to-stereo bypass error: " << error << "\n";
      return 1;
    }
    if (result.timeSeries.empty() || result.bands.empty()) {
      std::cerr << "Expected non-empty mono-to-stereo width outputs.\n";
      return 1;
    }
    if (result.latencyAlignment.appliedDelayA != 24 || result.latencyAlignment.appliedDelayB != 0) {
      std::cerr << "Mono-to-stereo width latency alignment mismatch.\n";
      return 1;
    }
    if (result.alignedA.numSamples() != result.alignedB.numSamples()) {
      std::cerr << "Aligned stereo sample counts should match.\n";
      return 1;
    }
    if (result.analysisSignalMode != "stereo_identical_lr_pink_noise") {
      std::cerr << "Unexpected analysis signal mode.\n";
      return 1;
    }
    if (result.metrics.meanAbsTimeDeltaWidthPercent > 0.5 || result.metrics.meanAbsBandDeltaWidthPercent > 0.5) {
      std::cerr << "Bypass width delta should remain near zero.\n";
      return 1;
    }
    if (result.timeSeries.front().widthPercentA > 1.0 || result.timeSeries.front().widthPercentB > 1.0) {
      std::cerr << "Bypass mono output should remain near 0% width.\n";
      return 1;
    }
    const auto& firstTime = result.timeSeries.front();
    if (!approx(firstTime.deltaDb, firstTime.ratioDbA - firstTime.ratioDbB, 1e-6)) {
      std::cerr << "Time-series delta definition must be A-B.\n";
      return 1;
    }
    if (!approx(firstTime.deltaWidthPercent, firstTime.widthPercentA - firstTime.widthPercentB, 1e-6)) {
      std::cerr << "Time-series width delta definition must be A-B.\n";
      return 1;
    }
    const auto& firstBin = result.spectrum.front();
    if (!approx(firstBin.deltaDb, firstBin.ratioDbA - firstBin.ratioDbB, 1e-6)) {
      std::cerr << "Band-spectrum delta definition must be A-B.\n";
      return 1;
    }
    if (!approx(firstBin.deltaWidthPercent, firstBin.widthPercentA - firstBin.widthPercentB, 1e-6)) {
      std::cerr << "Band-spectrum width delta definition must be A-B.\n";
      return 1;
    }
  }

  {
    DummyBypassHost a(0);
    DummySideInjectHost b(0.25f);
    MonoToStereoWidthTestManager test;
    std::string error;
    const MonoToStereoWidthTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected mono-to-stereo side-injection error: " << error << "\n";
      return 1;
    }
    if (result.metrics.meanAbsBandDeltaWidthPercent < 5.0) {
      std::cerr << "Expected non-trivial M/S band delta when side is injected.\n";
      return 1;
    }
  }

  return 0;
}
