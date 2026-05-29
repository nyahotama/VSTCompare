#include "vstcompare/phase_test_manager.hpp"

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
  DummyGainDelayHost(float gainL, float gainR, int delaySamples) : gainL_(gainL), gainR_(gainR), delaySamples_(delaySamples) {}

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

double meanAbsInBand(const std::vector<PhasePoint>& phase, double minHz, double maxHz) {
  double sum = 0.0;
  int count = 0;
  for (const auto& p : phase) {
    if (p.frequencyHz < minHz || p.frequencyHz > maxHz) continue;
    sum += std::fabs(p.valueRad);
    ++count;
  }
  return (count > 0) ? (sum / static_cast<double>(count)) : 0.0;
}

}  // namespace

int main() {
  TestConfig config;
  config.sampleRate = 48000;
  config.blockSize = 512;
  config.phaseDurationMs = 2000;
  config.phaseFftSize = 65536;
  config.phaseOverlapPercent = 50;
  config.phaseNoiseLevelDbfs = -12.0f;

  {
    DummyGainDelayHost a(1.0f, 1.0f, 0);
    DummyGainDelayHost b(1.0f, 1.0f, 0);
    PhaseTestManager test;
    std::string error;
    PhaseTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected bypass phase test error: " << error << "\n";
      return 1;
    }
    if (result.analysisSignalMode != "mono_lr_average") {
      std::cerr << "Expected mono_lr_average mode.\n";
      return 1;
    }
    const double meanA = meanAbsInBand(result.phaseA, 50.0, 10000.0);
    const double meanB = meanAbsInBand(result.phaseB, 50.0, 10000.0);
    if (meanA > 0.12 || meanB > 0.12) {
      std::cerr << "Bypass transfer phase should remain near 0 rad.\n";
      return 1;
    }
  }

  {
    DummyGainDelayHost a(1.0f, 1.0f, 16);
    DummyGainDelayHost b(1.0f, 1.0f, 16);
    PhaseTestManager test;
    std::string error;
    PhaseTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected common-delay phase test error: " << error << "\n";
      return 1;
    }
    const double meanDelta = meanAbsInBand(result.delta, 50.0, 10000.0);
    if (meanDelta > 0.14) {
      std::cerr << "Common delay should cancel in A-B delta.\n";
      return 1;
    }
  }

  {
    DummyGainDelayHost a(1.0f, 1.0f, 0);
    DummyGainDelayHost b(1.0f, 1.0f, 32);
    PhaseTestManager test;
    std::string error;
    PhaseTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected latency alignment phase test error: " << error << "\n";
      return 1;
    }
    if (result.latencyAlignment.appliedDelayA != 32 || result.latencyAlignment.appliedDelayB != 0) {
      std::cerr << "Phase latency alignment mismatch.\n";
      return 1;
    }
    if (result.alignedA.numSamples() != result.alignedB.numSamples()) {
      std::cerr << "Aligned sample length mismatch.\n";
      return 1;
    }
    const double meanDelta = meanAbsInBand(result.delta, 50.0, 10000.0);
    if (meanDelta > 0.14) {
      std::cerr << "Aligned known delay should produce small A-B phase delta.\n";
      return 1;
    }
  }

  {
    DummyGainDelayHost a(1.0f, 1.0f, 0);
    DummyGainDelayHost b(0.5f, 0.5f, 5);
    PhaseTestManager test;
    std::string error;
    PhaseTestResult result = test.run(a, b, config, error);
    if (!error.empty()) {
      std::cerr << "Unexpected delta verification error: " << error << "\n";
      return 1;
    }
    const std::size_t n = std::min(result.phaseA.size(), result.phaseB.size());
    if (n == 0 || result.delta.size() != n) {
      std::cerr << "Phase vector size mismatch.\n";
      return 1;
    }
    for (std::size_t i = 0; i < std::min<std::size_t>(n, 128); ++i) {
      const double expected = phase_detail::wrapToPi(result.phaseA[i].valueRad - result.phaseB[i].valueRad);
      if (std::fabs(result.delta[i].valueRad - expected) > 1e-6) {
        std::cerr << "Phase delta mismatch.\n";
        return 1;
      }
    }
    if (result.bands.size() != 24) {
      std::cerr << "Expected 24 phase bands.\n";
      return 1;
    }
    if (!(result.bands.front().lowerHz >= 19.9 && result.bands.front().lowerHz <= 20.1)) {
      std::cerr << "Phase band start mismatch.\n";
      return 1;
    }
    if (!(result.bands.back().upperHz >= 19900.0 && result.bands.back().upperHz <= 20100.0)) {
      std::cerr << "Phase band end mismatch.\n";
      return 1;
    }
  }

  return 0;
}
