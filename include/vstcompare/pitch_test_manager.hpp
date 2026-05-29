#pragma once

#include "vstcompare/interfaces.hpp"

#include <cstddef>
#include <vector>

namespace vstcompare {

namespace pitch_detail {

AudioBuffer generateLogSweepInput(const TestConfig& config);

double estimatePitchHzFrame(const std::vector<float>& signal, std::size_t begin, std::size_t count, int sampleRate,
                            double minHz = 20.0, double maxHz = 20000.0);

}  // namespace pitch_detail

class PitchCorrectionTrackingTestManager final {
public:
  PitchTestResult run(IPluginHost& pluginA, IPluginHost& pluginB, const TestConfig& config, std::string& error) const;
};

}  // namespace vstcompare
