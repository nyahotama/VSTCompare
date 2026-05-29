#pragma once

#include "vstcompare/interfaces.hpp"

#include <vector>

namespace vstcompare {

namespace dynamics_detail {

std::vector<double> buildSweepLevels(const TestConfig& config);

AudioBuffer generateStepSineInput(const TestConfig& config);

}  // namespace dynamics_detail

class DynamicsTestManager final {
public:
  DynamicsTestResult run(IPluginHost& pluginA, IPluginHost& pluginB, const TestConfig& config,
                         std::string& error) const;
};

}  // namespace vstcompare

