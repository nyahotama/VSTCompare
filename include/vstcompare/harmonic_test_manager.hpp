#pragma once

#include "vstcompare/interfaces.hpp"

#include <vector>

namespace vstcompare {

namespace harmonic_detail {

AudioBuffer generateSineInput(const TestConfig& config, double frequencyHz);

std::vector<HarmonicSpectrumPoint> computeSpectrumDbfs(const std::vector<float>& signal, int sampleRate, int fftSize,
                                                       int skipHeadSamples);

}  // namespace harmonic_detail

class HarmonicDistortionTestManager final {
public:
  HarmonicTestResult run(IPluginHost& pluginA, IPluginHost& pluginB, const TestConfig& config,
                         std::string& error) const;
};

}  // namespace vstcompare
