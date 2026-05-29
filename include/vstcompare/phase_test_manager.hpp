#pragma once

#include "vstcompare/interfaces.hpp"

#include <cstdint>
#include <vector>

namespace vstcompare {

namespace phase_detail {

std::vector<PhasePoint> computeTransferPhaseRad(const std::vector<float>& inputSignal,
                                                const std::vector<float>& outputSignal,
                                                int sampleRate, int fftSize, int overlapPercent);

std::vector<PhaseBandSummary> summarizeLogBands(const std::vector<PhasePoint>& deltaPhase, double minHz = 20.0,
                                                double maxHz = 20000.0, int bandCount = 24);

double wrapToPi(double valueRad);

}  // namespace phase_detail

class PhaseTestManager final {
public:
  PhaseTestResult run(IPluginHost& pluginA, IPluginHost& pluginB, const TestConfig& config, std::string& error) const;
};

}  // namespace vstcompare
