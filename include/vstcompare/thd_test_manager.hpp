#pragma once

#include "vstcompare/interfaces.hpp"

#include <vector>

namespace vstcompare {

namespace thd_detail {

std::vector<double> buildSweepLevels(const TestConfig& config);

AudioBuffer generateSineInput(const TestConfig& config, double levelDbfs);

}  // namespace thd_detail

class ThdThdnTestManager final {
public:
  ThdTestResult run(IPluginHost& pluginA, IPluginHost& pluginB, const TestConfig& config, std::string& error) const;
};

}  // namespace vstcompare
