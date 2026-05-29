#pragma once

#include "vstcompare/interfaces.hpp"

namespace vstcompare {

namespace time_response_detail {

AudioBuffer generateToneBurstInput(const TestConfig& config);

}  // namespace time_response_detail

class TimeResponseTestManager final {
public:
  TimeResponseTestResult run(IPluginHost& pluginA, IPluginHost& pluginB, const TestConfig& config,
                             std::string& error) const;
};

}  // namespace vstcompare

