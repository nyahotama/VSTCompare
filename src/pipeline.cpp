#include "vstcompare/pipeline.hpp"

#include "vstcompare/frequency_test_manager.hpp"
#include "vstcompare/harmonic_test_manager.hpp"
#include "vstcompare/ir_test_manager.hpp"
#include "vstcompare/mono_to_stereo_width_test_manager.hpp"
#include "vstcompare/dynamics_test_manager.hpp"
#include "vstcompare/phase_test_manager.hpp"
#include "vstcompare/pitch_test_manager.hpp"
#include "vstcompare/report_builder.hpp"
#include "vstcompare/subprocess_plugin_host.hpp"
#include "vstcompare/thd_test_manager.hpp"
#include "vstcompare/time_response_test_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <unordered_set>

#if defined(_WIN32)
#include <io.h>
#endif

namespace vstcompare {
namespace {

struct PendingParameterInput {
  std::string plugin;
  std::string name;
  uint32_t parameterId = 0;
  std::string inputText;
};

std::string trimCopy(const std::string& text) {
  std::size_t start = 0;
  while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
    ++start;
  }
  std::size_t end = text.size();
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
    --end;
  }
  return text.substr(start, end - start);
}

std::vector<PendingParameterInput> promptPluginParameters(const std::string& pluginLabel,
                                                          const std::vector<PluginParameter>& params,
                                                          bool forceNonInteractive) {
  if (forceNonInteractive) {
    return {};
  }
#if defined(_WIN32)
  const bool interactiveStdin = (_isatty(_fileno(stdin)) != 0);
  if (!interactiveStdin) {
    return {};
  }
#endif

  std::vector<PendingParameterInput> picked;
  std::cout << "\nSet parameters for Plugin " << pluginLabel << " (all published parameters, blank = skip each).\n";
  for (const auto& p : params) {
    std::cout << "[" << pluginLabel << "] Parameter [" << p.title << "] default " << p.defaultNormalized;
    if (!p.display.empty()) {
      std::cout << " | display: " << p.display;
    }
    if (!p.units.empty()) {
      std::cout << " | units: " << p.units;
    }
    if (p.stepCount > 0) {
      std::cout << " | steps: " << (p.stepCount + 1);
    }
    if (!p.enumDisplayOptions.empty()) {
      std::cout << " | options: ";
      for (std::size_t i = 0; i < p.enumDisplayOptions.size(); ++i) {
        if (i > 0) std::cout << " / ";
        std::cout << p.enumDisplayOptions[i];
      }
    }
    std::cout << " | enter display value (or normalized 0.0-1.0): ";

    std::string line;
    std::getline(std::cin, line);
    if (trimCopy(line).empty()) continue;

    picked.push_back({pluginLabel, p.title, p.id, line});
  }
  return picked;
}

void dedupeWarnings(std::vector<std::string>& warnings) {
  std::unordered_set<std::string> seen;
  std::vector<std::string> unique;
  unique.reserve(warnings.size());
  for (const auto& w : warnings) {
    if (seen.insert(w).second) {
      unique.push_back(w);
    }
  }
  warnings = std::move(unique);
}

double elapsedSeconds(std::chrono::steady_clock::time_point start) {
  const auto now = std::chrono::steady_clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
  return static_cast<double>(ms) / 1000.0;
}

void logStageDuration(const std::string& stage, std::chrono::steady_clock::time_point start) {
  std::cout << "[timing] " << stage << ": " << std::fixed << std::setprecision(3) << elapsedSeconds(start) << "s\n";
}

}  // namespace

int Pipeline::run(const CliOptions& options) const {
  constexpr int kShutdownTimeoutMs = 3000;
  const auto runStart = std::chrono::steady_clock::now();
  std::string error;
  SubprocessPluginHost pluginA("A");
  SubprocessPluginHost pluginB("B");
  pluginA.setRequestedClassId(options.pluginAClassId);
  pluginB.setRequestedClassId(options.pluginBClassId);
  RunSummary summary;
  bool summaryInitialized = false;

  auto shutdownHost = [&](const char* name, SubprocessPluginHost& host, bool allowTimeoutAsWarning) -> bool {
    std::string shutdownMessage;
    const auto shutdownStart = std::chrono::steady_clock::now();
    const bool ok = host.shutdown(shutdownMessage, kShutdownTimeoutMs);
    logStageDuration(std::string("shutdown ") + name, shutdownStart);
    if (ok) return true;

    std::string warning = shutdownMessage;
    if (warning.empty()) {
      warning = std::string("shutdown: Host shutdown failed for ") + name + ".";
    }
    const bool timeout = (warning.find("timeout") != std::string::npos || warning.find("Timeout") != std::string::npos);
    if (timeout) {
      warning = "Host shutdown timeout (" + std::string(name) + ", " + std::to_string(kShutdownTimeoutMs) +
                "ms). " + warning;
    } else {
      warning = "Host shutdown failure (" + std::string(name) + "). " + warning;
    }

    if (allowTimeoutAsWarning && timeout) {
      if (summaryInitialized) {
        summary.warnings.push_back(warning);
      } else {
        std::cerr << warning << "\n";
      }
      return true;
    }

    std::cerr << warning << "\n";
    return false;
  };

  auto failWithCleanup = [&](const std::string& message) -> int {
    if (!message.empty()) {
      std::cerr << message << "\n";
    }
    shutdownHost("A", pluginA, false);
    shutdownHost("B", pluginB, false);
    logStageDuration("total", runStart);
    return 1;
  };

  const auto loadAStart = std::chrono::steady_clock::now();
  if (!pluginA.load(options.pluginAPath, options.sampleRate, options.blockSize, error)) {
    return failWithCleanup("Failed to load plugin A: " + error);
  }
  logStageDuration("load A", loadAStart);

  const auto loadBStart = std::chrono::steady_clock::now();
  if (!pluginB.load(options.pluginBPath, options.sampleRate, options.blockSize, error)) {
    return failWithCleanup("Failed to load plugin B: " + error);
  }
  logStageDuration("load B", loadBStart);

  const auto metaA = pluginA.getMetadata();
  const auto metaB = pluginB.getMetadata();

  std::cout << "Loaded:\n";
  std::cout << "  A: " << metaA.name << " (" << metaA.vendor << ")\n";
  std::cout << "  B: " << metaB.name << " (" << metaB.vendor << ")\n";

  const auto paramsA = pluginA.listParameters();
  const auto paramsB = pluginB.listParameters();
  const auto diagnosticsA = pluginA.takeDiagnostics();
  const auto diagnosticsB = pluginB.takeDiagnostics();
  const auto scanSummaryA = pluginA.getParameterScanSummary();
  const auto scanSummaryB = pluginB.getParameterScanSummary();
  std::cout << "Parameter count: A=" << paramsA.size() << " B=" << paramsB.size() << "\n";
  std::cout << "[parameter scan] A: " << (scanSummaryA.empty() ? "n/a" : scanSummaryA) << "\n";
  std::cout << "[parameter scan] B: " << (scanSummaryB.empty() ? "n/a" : scanSummaryB) << "\n";
  for (const auto& d : diagnosticsA) {
    if (d.rfind("parameter scan stage:", 0) == 0) {
      std::cout << "[parameter scan] A/" << d << "\n";
    }
  }
  for (const auto& d : diagnosticsB) {
    if (d.rfind("parameter scan stage:", 0) == 0) {
      std::cout << "[parameter scan] B/" << d << "\n";
    }
  }

  const auto chosenA = promptPluginParameters("A", paramsA, options.nonInteractive);
  const auto chosenB = promptPluginParameters("B", paramsB, options.nonInteractive);

  summaryInitialized = true;
  summary.pluginA = metaA;
  summary.pluginB = metaB;
  summary.config.sampleRate = options.sampleRate;
  summary.config.blockSize = options.blockSize;
  summary.config.irDurationMs = 500;
  summary.config.impulseAmplitude = 1.0f;
  summary.config.frequencyDurationMs = 2000;
  summary.config.frequencyFftSize = 65536;
  summary.config.frequencyOverlapPercent = 50;
  summary.config.frequencyNoiseLevelDbfs = -12.0f;
  summary.config.phaseDurationMs = 2000;
  summary.config.phaseFftSize = 65536;
  summary.config.phaseOverlapPercent = 50;
  summary.config.phaseNoiseLevelDbfs = -12.0f;
  summary.config.harmonicDurationMs = 1000;
  summary.config.harmonicSkipHeadMs = 200;
  summary.config.harmonicFftSize = 65536;
  summary.config.harmonicInputLevelDbfs = -6.0f;
  summary.config.harmonicFrequenciesHz = {100.0, 1000.0, 5000.0, 10000.0};
  summary.config.thdToneFrequencyHz = 1000.0;
  summary.config.thdStartLevelDbfs = -60.0f;
  summary.config.thdEndLevelDbfs = 6.0f;
  summary.config.thdStepDb = 2.0f;
  summary.config.thdDurationMs = 1000;
  summary.config.thdSkipHeadMs = 200;
  summary.config.thdFftSize = 65536;
  summary.config.monoStereoWidthDurationMs = 500;
  summary.config.monoStereoWidthNoiseLevelDbfs = -12.0f;
  summary.config.monoStereoWidthFftSize = 65536;
  summary.config.monoStereoWidthOverlapPercent = 50;
  summary.config.monoStereoWidthTimeWindowMs = 20;
  summary.config.monoStereoWidthTimeHopMs = 10;
  summary.config.pitchDurationMs = 5000;
  summary.config.pitchStartHz = 110.0f;
  summary.config.pitchEndHz = 880.0f;
  summary.config.pitchLevelDbfs = -6.0f;
  summary.config.pitchFadeMs = 5;
  summary.config.pitchFrameMs = 100;
  summary.config.pitchHopMs = 10;
  summary.config.dynamicsToneFrequencyHz = 1000.0;
  summary.config.dynamicsStartLevelDbfs = -90.0f;
  summary.config.dynamicsEndLevelDbfs = 0.0f;
  summary.config.dynamicsStepDb = 1.0f;
  summary.config.dynamicsStepMs = 50;
  summary.config.dynamicsRmsWindowMs = 20;
  summary.config.timeBurstFrequencyHz = 1000.0;
  summary.config.timeBurstLevelDbfs = -6.0f;
  summary.config.timePreSilenceMs = 200;
  summary.config.timeBurstOnMs = 100;
  summary.config.timePostSilenceMs = 200;
  summary.config.timeEnvelopeWindowMs = 20;
  summary.config.timeCorrelationMaxLagMs = 50;
  bool hadZeroAfterRetryA = false;
  bool hadZeroAfterRetryB = false;
  for (const auto& d : diagnosticsA) {
    if (d.rfind("parameter scan stage:", 0) == 0) continue;
    if (d.find("stillZeroAfterRetry") != std::string::npos) hadZeroAfterRetryA = true;
    summary.warnings.push_back("A/" + d);
  }
  for (const auto& d : diagnosticsB) {
    if (d.rfind("parameter scan stage:", 0) == 0) continue;
    if (d.find("stillZeroAfterRetry") != std::string::npos) hadZeroAfterRetryB = true;
    summary.warnings.push_back("B/" + d);
  }
  if (paramsA.empty() && !hadZeroAfterRetryA) {
    summary.warnings.push_back("A/No parameters enumerated after retry. Tests continued.");
  }
  if (paramsB.empty() && !hadZeroAfterRetryB) {
    summary.warnings.push_back("B/No parameters enumerated after retry. Tests continued.");
  }

  for (const auto& param : chosenA) {
    std::string msg;
    double resolvedNormalized = 0.0;
    const bool ok = pluginA.setParameterDisplay(param.parameterId, param.inputText, resolvedNormalized, msg);
    if (!ok) {
      summary.warnings.push_back("A/" + param.name + ": " + (msg.empty() ? "set failed" : msg));
      continue;
    }
    if (!msg.empty()) {
      summary.warnings.push_back("A/" + param.name + ": " + msg);
    }
    summary.appliedParameters.push_back(
        {param.plugin, param.name, param.parameterId, resolvedNormalized, param.inputText});
  }
  for (const auto& param : chosenB) {
    std::string msg;
    double resolvedNormalized = 0.0;
    const bool ok = pluginB.setParameterDisplay(param.parameterId, param.inputText, resolvedNormalized, msg);
    if (!ok) {
      summary.warnings.push_back("B/" + param.name + ": " + (msg.empty() ? "set failed" : msg));
      continue;
    }
    if (!msg.empty()) {
      summary.warnings.push_back("B/" + param.name + ": " + msg);
    }
    summary.appliedParameters.push_back(
        {param.plugin, param.name, param.parameterId, resolvedNormalized, param.inputText});
  }

  IrTestManager ir;
  const auto irStart = std::chrono::steady_clock::now();
  summary.irResult = ir.run(pluginA, pluginB, summary.config, error);
  logStageDuration("test IR", irStart);
  if (!error.empty()) {
    return failWithCleanup("IR test failed: " + error);
  }
  for (const auto& w : summary.irResult.latencyAlignment.warnings) {
    summary.warnings.push_back(w);
  }

  FrequencyResponseTestManager frequency;
  const auto freqStart = std::chrono::steady_clock::now();
  summary.frequencyResult = frequency.run(pluginA, pluginB, summary.config, error);
  logStageDuration("test Frequency", freqStart);
  if (!error.empty()) {
    return failWithCleanup("Frequency test failed: " + error);
  }
  for (const auto& w : summary.frequencyResult.latencyAlignment.warnings) {
    summary.warnings.push_back(w);
  }

  PhaseTestManager phase;
  const auto phaseStart = std::chrono::steady_clock::now();
  summary.phaseResult = phase.run(pluginA, pluginB, summary.config, error);
  logStageDuration("test Phase", phaseStart);
  if (!error.empty()) {
    return failWithCleanup("Phase test failed: " + error);
  }
  for (const auto& w : summary.phaseResult.latencyAlignment.warnings) {
    summary.warnings.push_back(w);
  }

  HarmonicDistortionTestManager harmonic;
  const auto harmonicStart = std::chrono::steady_clock::now();
  summary.harmonicResult = harmonic.run(pluginA, pluginB, summary.config, error);
  logStageDuration("test Harmonic", harmonicStart);
  if (!error.empty()) {
    return failWithCleanup("Harmonic test failed: " + error);
  }
  for (const auto& pitch : summary.harmonicResult.pitches) {
    for (const auto& w : pitch.latencyAlignment.warnings) {
      summary.warnings.push_back(w);
    }
  }

  ThdThdnTestManager thd;
  const auto thdStart = std::chrono::steady_clock::now();
  summary.thdResult = thd.run(pluginA, pluginB, summary.config, error);
  logStageDuration("test THD/THD+N", thdStart);
  if (!error.empty()) {
    return failWithCleanup("THD test failed: " + error);
  }
  for (const auto& point : summary.thdResult.sweep) {
    for (const auto& w : point.latencyAlignment.warnings) {
      summary.warnings.push_back(w);
    }
    for (const auto& w : point.warnings) {
      summary.warnings.push_back(w);
    }
  }

  MonoToStereoWidthTestManager monoStereoWidth;
  const auto widthStart = std::chrono::steady_clock::now();
  summary.monoStereoWidthResult = monoStereoWidth.run(pluginA, pluginB, summary.config, error);
  logStageDuration("test MonoToStereoWidth", widthStart);
  if (!error.empty()) {
    return failWithCleanup("Mono-to-Stereo width test failed: " + error);
  }
  for (const auto& w : summary.monoStereoWidthResult.latencyAlignment.warnings) {
    summary.warnings.push_back(w);
  }
  for (const auto& w : summary.monoStereoWidthResult.warnings) {
    summary.warnings.push_back(w);
  }

  PitchCorrectionTrackingTestManager pitch;
  const auto pitchStart = std::chrono::steady_clock::now();
  summary.pitchResult = pitch.run(pluginA, pluginB, summary.config, error);
  logStageDuration("test Pitch", pitchStart);
  if (!error.empty()) {
    return failWithCleanup("Pitch test failed: " + error);
  }
  for (const auto& w : summary.pitchResult.latencyAlignment.warnings) {
    summary.warnings.push_back(w);
  }
  for (const auto& w : summary.pitchResult.warnings) {
    summary.warnings.push_back(w);
  }

  DynamicsTestManager dynamics;
  const auto dynamicsStart = std::chrono::steady_clock::now();
  summary.dynamicsResult = dynamics.run(pluginA, pluginB, summary.config, error);
  logStageDuration("test Dynamics", dynamicsStart);
  if (!error.empty()) {
    return failWithCleanup("Dynamics test failed: " + error);
  }
  for (const auto& w : summary.dynamicsResult.latencyAlignment.warnings) {
    summary.warnings.push_back(w);
  }
  for (const auto& w : summary.dynamicsResult.warnings) {
    summary.warnings.push_back(w);
  }

  TimeResponseTestManager timeResponse;
  const auto timeResponseStart = std::chrono::steady_clock::now();
  summary.timeResponseResult = timeResponse.run(pluginA, pluginB, summary.config, error);
  logStageDuration("test TimeResponse", timeResponseStart);
  if (!error.empty()) {
    return failWithCleanup("Time response test failed: " + error);
  }
  for (const auto& w : summary.timeResponseResult.latencyAlignment.warnings) {
    summary.warnings.push_back(w);
  }
  for (const auto& w : summary.timeResponseResult.warnings) {
    summary.warnings.push_back(w);
  }
  if (!shutdownHost("A", pluginA, true)) {
    return failWithCleanup("Failed to shutdown plugin A before report output.");
  }
  if (!shutdownHost("B", pluginB, true)) {
    return failWithCleanup("Failed to shutdown plugin B before report output.");
  }
  dedupeWarnings(summary.warnings);

  const auto reportStart = std::chrono::steady_clock::now();
  FinalReportBuilder reportBuilder;
  const std::string html = reportBuilder.renderHtml(summary);
  const std::string reportPath = buildOutputReportPath(options.outDirOrFile, metaA.name, metaB.name);

  std::ofstream ofs(reportPath, std::ios::binary);
  if (!ofs.is_open()) {
    return failWithCleanup("Failed to open report output: " + reportPath);
  }
  ofs << html;
  ofs.close();
  logStageDuration("report", reportStart);

  std::cout << "\nReport generated:\n" << reportPath << "\n";
  logStageDuration("total", runStart);
  return 0;
}

}  // namespace vstcompare
