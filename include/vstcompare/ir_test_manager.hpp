#pragma once

#include "vstcompare/interfaces.hpp"

namespace vstcompare {

class IrTestManager final : public ITestManager {
public:
  IrTestResult run(IPluginHost& pluginA, IPluginHost& pluginB, const TestConfig& config,
                   std::string& error) override;
};

}  // namespace vstcompare

