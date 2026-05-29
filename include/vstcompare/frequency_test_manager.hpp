#pragma once

#include "vstcompare/interfaces.hpp"

#include <cstdint>
#include <vector>

namespace vstcompare {

namespace frequency_detail {

AudioBuffer generateWhiteNoiseInput(const TestConfig& config, uint32_t seed = 1337);

std::vector<SpectrumPoint> computeAveragedPsdDb(const std::vector<float>& signal, int sampleRate, int fftSize,
                                                int overlapPercent);

std::vector<OctaveBandSummary> summarizeOneThirdOctave(const std::vector<SpectrumPoint>& deltaSpectrum,
                                                       double minHz = 20.0, double maxHz = 20000.0,
                                                       int bandCount = 30);

}  // namespace frequency_detail

class FrequencyResponseTestManager final {
public:
  FrequencyTestResult run(IPluginHost& pluginA, IPluginHost& pluginB, const TestConfig& config,
                          std::string& error) const;
};

}  // namespace vstcompare

