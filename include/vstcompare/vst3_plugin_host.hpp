#pragma once

#include "vstcompare/interfaces.hpp"

#include <memory>

namespace vstcompare {

class Vst3PluginHost final : public IPluginHost {
public:
  Vst3PluginHost();
  ~Vst3PluginHost() override;

  bool load(const std::string& path, int sampleRate, int blockSize, std::string& error) override;
  PluginMetadata getMetadata() const override;
  std::vector<PluginParameter> listParameters() const override;
  bool setParameter(uint32_t id, double normalized, std::string& warningOrError) override;
  bool setParameterDisplay(uint32_t id, const std::string& displayInput, double& resolvedNormalized,
                           std::string& warningOrError) override;
  AudioBuffer process(const AudioBuffer& input, std::string& error) override;
  uint32_t getLatencySamples() const override;
  void setRequestedClassId(std::string classId);
  bool wasReloadRequestedDuringLoad() const;
  bool shutdown(std::string& warningOrError, int timeoutMs = 3000);
  std::vector<std::string> takeDiagnostics();
  std::string getParameterScanSummary() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vstcompare
