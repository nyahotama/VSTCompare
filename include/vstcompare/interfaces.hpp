#pragma once

#include "vstcompare/types.hpp"

#include <string>
#include <vector>

namespace vstcompare {

class IPluginHost {
public:
  virtual ~IPluginHost() = default;

  virtual bool load(const std::string& path, int sampleRate, int blockSize, std::string& error) = 0;
  virtual PluginMetadata getMetadata() const = 0;
  virtual std::vector<PluginParameter> listParameters() const = 0;
  virtual bool setParameter(uint32_t id, double normalized, std::string& warningOrError) = 0;
  virtual bool setParameterDisplay(uint32_t id, const std::string& displayInput, double& resolvedNormalized,
                                   std::string& warningOrError) {
    resolvedNormalized = 0.0;
    warningOrError = "Display-value parameter setting is not supported by this host.";
    return false;
  }
  virtual AudioBuffer process(const AudioBuffer& input, std::string& error) = 0;
  virtual uint32_t getLatencySamples() const = 0;
};

class ITestManager {
public:
  virtual ~ITestManager() = default;
  virtual IrTestResult run(IPluginHost& pluginA, IPluginHost& pluginB, const TestConfig& config,
                           std::string& error) = 0;
};

class IReportSection {
public:
  virtual ~IReportSection() = default;
  virtual std::string renderHtml(const RunSummary& summary) const = 0;
  virtual std::string toJson(const RunSummary& summary) const = 0;
};

}  // namespace vstcompare
