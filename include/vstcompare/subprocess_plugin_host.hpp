#pragma once

#include "vstcompare/interfaces.hpp"

#include <memory>
#include <string>

namespace vstcompare {

class SubprocessPluginHost final : public IPluginHost {
public:
  explicit SubprocessPluginHost(std::string instanceName);
  ~SubprocessPluginHost() override;

  bool load(const std::string& path, int sampleRate, int blockSize, std::string& error) override;
  PluginMetadata getMetadata() const override;
  std::vector<PluginParameter> listParameters() const override;
  bool setParameter(uint32_t id, double normalized, std::string& warningOrError) override;
  bool setParameterDisplay(uint32_t id, const std::string& displayInput, double& resolvedNormalized,
                           std::string& warningOrError) override;
  AudioBuffer process(const AudioBuffer& input, std::string& error) override;
  uint32_t getLatencySamples() const override;

  void setRequestedClassId(std::string classId);

  bool shutdown(std::string& warningOrError, int timeoutMs = 3000);
  std::vector<std::string> takeDiagnostics();
  std::string getParameterScanSummary() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vstcompare
