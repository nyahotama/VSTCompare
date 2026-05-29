#pragma once

#include "vstcompare/interfaces.hpp"

#include <cstdint>
#include <string>

namespace vstcompare {

namespace mono_stereo_width_detail {

AudioBuffer generateMonoPinkNoiseStereoInput(const TestConfig& config, uint32_t seed = 1337);

}  // namespace mono_stereo_width_detail

class MonoToStereoWidthTestManager final {
public:
  MonoToStereoWidthTestResult run(IPluginHost& pluginA, IPluginHost& pluginB, const TestConfig& config,
                                  std::string& error) const;
};

}  // namespace vstcompare

