#include "vstcompare/report_builder.hpp"

#include <iostream>
#include <string>

using namespace vstcompare;

int main() {
  RunSummary summary;
  summary.pluginA = {"A", "VendorA", "uidA", "1.0", ""};
  summary.pluginB = {"B", "VendorB", "uidB", "1.0", ""};

  summary.irResult.metrics.peakAbsDelta = 0.02f;
  summary.irResult.metrics.energyDelta = 0.0002;
  summary.irResult.metrics.estimatedLatencyMs = 0.12;
  summary.irResult.topDifferences.push_back({480, 0.02f});

  summary.frequencyResult.analysisSignalMode = "mono_lr_average";
  summary.frequencyResult.metrics.peakAbsDeltaDb = 4.0;
  summary.frequencyResult.metrics.meanAbsDeltaDb = 1.2;
  summary.frequencyResult.octaveBands.push_back({800.0, 1000.0, 1250.0, 3.0, -4.0});
  summary.phaseResult.analysisSignalMode = "mono_lr_average";
  summary.phaseResult.metrics.peakAbsDeltaRad = 0.8;
  summary.phaseResult.metrics.meanAbsDeltaRad = 0.2;
  summary.phaseResult.bands.push_back({800.0, 1000.0, 1250.0, 0.25, 0.8});
  summary.harmonicResult.metrics.peakAbsDeltaDb = 1.0;
  summary.harmonicResult.metrics.meanAbsDeltaDb = 0.5;
  summary.thdResult.metrics.peakAbsThdDeltaPercent = 0.2;
  summary.thdResult.metrics.meanAbsThdDeltaPercent = 0.1;
  summary.thdResult.metrics.peakAbsThdnDeltaPercent = 0.3;
  summary.thdResult.metrics.meanAbsThdnDeltaPercent = 0.15;
  summary.thdResult.segments.push_back({"high", -8.0, 0.0, 5, 0.05, 0.2, 0.08, 0.3});
  summary.monoStereoWidthResult.metrics.peakAbsBandDeltaDb = 1.5;
  summary.monoStereoWidthResult.metrics.peakAbsBandDeltaWidthPercent = 12.5;
  summary.monoStereoWidthResult.metrics.meanAbsBandDeltaWidthPercent = 5.0;
  summary.monoStereoWidthResult.metrics.meanAbsTimeDeltaWidthPercent = 3.5;
  summary.monoStereoWidthResult.analysisSignalMode = "stereo_identical_lr_pink_noise";
  summary.monoStereoWidthResult.analysisReference = "mono_pink_noise_width_index";
  summary.pitchResult.analysisSignalMode = "mono_lr_average";
  summary.pitchResult.metrics.meanAbsErrorHzA = 2.0;
  summary.pitchResult.metrics.meanAbsErrorHzB = 3.0;
  summary.pitchResult.metrics.meanAbsDeltaHz = 1.25;
  summary.pitchResult.metrics.peakAbsDeltaHz = 4.0;
  summary.pitchResult.metrics.validFrameRateA = 0.4;
  summary.pitchResult.metrics.validFrameRateB = 0.8;
  summary.dynamicsResult.analysisSignalMode = "mono_lr_average";
  summary.dynamicsResult.metrics.peakAbsOutputDeltaDb = 0.4;
  summary.dynamicsResult.metrics.meanAbsOutputDeltaDb = 0.3;
  summary.dynamicsResult.metrics.peakAbsGainReductionDeltaDb = 0.5;
  summary.dynamicsResult.metrics.meanAbsGainReductionDeltaDb = 0.2;
  summary.dynamicsResult.metrics.estimatedA.thresholdDbfs = -18.0;
  summary.dynamicsResult.metrics.estimatedB.thresholdDbfs = -22.0;
  summary.dynamicsResult.metrics.estimatedA.ratio = 2.0;
  summary.dynamicsResult.metrics.estimatedB.ratio = 4.0;
  summary.dynamicsResult.metrics.estimatedA.kneeWidthDb = 3.0;
  summary.dynamicsResult.metrics.estimatedB.kneeWidthDb = 6.0;
  summary.dynamicsResult.metrics.estimatedA.valid = true;
  summary.dynamicsResult.metrics.estimatedB.valid = true;
  summary.dynamicsResult.metrics.thresholdDeltaDb = 4.0;
  summary.dynamicsResult.metrics.ratioDelta = -2.0;
  summary.dynamicsResult.metrics.kneeWidthDeltaDb = -3.0;
  summary.timeResponseResult.analysisSignalMode = "mono_lr_average";
  summary.timeResponseResult.metrics.attackMsA = 12.0;
  summary.timeResponseResult.metrics.attackMsB = 10.5;
  summary.timeResponseResult.metrics.attackDeltaMs = 1.5;
  summary.timeResponseResult.metrics.releaseMsA = 80.0;
  summary.timeResponseResult.metrics.releaseMsB = 70.0;
  summary.timeResponseResult.metrics.releaseDeltaMs = 10.0;
  summary.timeResponseResult.metrics.residualLatencyMs = 0.5;

  HarmonicPitchResult pitch;
  pitch.fundamentalHz = 1000.0;
  pitch.analysisSignalMode = "mono_lr_average";
  pitch.noiseFloorDeltaDb = 1.0;
  pitch.orders.push_back({2, 2000.0, -60.0, -63.0, 3.0});
  pitch.orders.push_back({3, 3000.0, -70.0, -68.0, -2.0});
  summary.harmonicResult.pitches.push_back(pitch);

  WidthTimePoint timePoint;
  timePoint.timeMs = 10.0;
  timePoint.ratioDbA = 12.0;
  timePoint.ratioDbB = 11.0;
  timePoint.deltaDb = 1.0;
  timePoint.widthPercentA = 22.0;
  timePoint.widthPercentB = 18.5;
  timePoint.deltaWidthPercent = 3.5;
  summary.monoStereoWidthResult.timeSeries.push_back(timePoint);

  WidthBandSummary band;
  band.lowerHz = 20.0;
  band.centerHz = 31.5;
  band.upperHz = 40.0;
  band.ratioDbA = 6.0;
  band.ratioDbB = 5.0;
  band.deltaDb = 1.0;
  band.widthPercentA = 24.0;
  band.widthPercentB = 19.0;
  band.deltaWidthPercent = 5.0;
  summary.monoStereoWidthResult.bands.push_back(band);

  PitchCurvePoint pitchPoint;
  pitchPoint.timeMs = 100.0;
  pitchPoint.inputHz = 440.0;
  pitchPoint.pitchHzA = 441.0;
  pitchPoint.pitchHzB = 439.5;
  summary.pitchResult.curve.push_back(pitchPoint);

  DynamicsPoint dynamicsPoint;
  dynamicsPoint.inputLevelDbfs = -12.0;
  dynamicsPoint.outputLevelDbfsA = -14.0;
  dynamicsPoint.outputLevelDbfsB = -15.0;
  dynamicsPoint.outputDeltaDb = 1.0;
  dynamicsPoint.gainReductionDbA = -2.0;
  dynamicsPoint.gainReductionDbB = -3.0;
  dynamicsPoint.gainReductionDeltaDb = 1.0;
  summary.dynamicsResult.ioCurve.push_back(dynamicsPoint);

  TimeResponsePoint timePointResponse;
  timePointResponse.timeMs = 210.0;
  timePointResponse.envelopeDbA = -6.0;
  timePointResponse.envelopeDbB = -8.0;
  timePointResponse.deltaDb = 2.0;
  summary.timeResponseResult.curve.push_back(timePointResponse);

  FinalReportBuilder report;
  const std::string json = report.toJson(summary);

  if (json.find("\"analysisReference\":\"input_white_noise_psd\",\"latencyAlignment\"") == std::string::npos) {
    std::cerr << "Frequency analysisReference JSON sequence is broken.\n";
    return 1;
  }
  if (json.find("\"analysisReference\":\"input_white_noise_transfer_phase\",\"latencyAlignment\"") == std::string::npos) {
    std::cerr << "Phase analysisReference JSON sequence is broken.\n";
    return 1;
  }
  if (json.find("\"harmonic\":{") == std::string::npos) {
    std::cerr << "Harmonic JSON section is missing.\n";
    return 1;
  }
  if (json.find("\"thd\":{\"analysisReference\":\"fft_fundamental_bin_rejection\"") == std::string::npos) {
    std::cerr << "THD JSON section is missing.\n";
    return 1;
  }
  if (json.find("\"monoStereoWidth\":{\"analysisReference\":\"mono_pink_noise_width_index\"") ==
      std::string::npos) {
    std::cerr << "Mono-to-Stereo Width JSON section is missing.\n";
    return 1;
  }
  if (json.find("\"peakAbsBandDeltaWidthPercent\":12.5") == std::string::npos) {
    std::cerr << "Mono-to-Stereo width-percent metrics are missing.\n";
    return 1;
  }
  if (json.find("\"pitch\":{\"analysisReference\":\"log_sine_sweep_time_pitch_tracking\"") == std::string::npos) {
    std::cerr << "Pitch JSON section is missing.\n";
    return 1;
  }
  if (json.find("\"meanAbsDeltaHz\":1.25") == std::string::npos) {
    std::cerr << "Pitch metrics are missing.\n";
    return 1;
  }
  if (json.find("\"dynamics\":{\"analysisReference\":\"step_sine_rms_io_curve\"") == std::string::npos) {
    std::cerr << "Dynamics JSON section is missing.\n";
    return 1;
  }
  if (json.find("\"timeResponse\":{\"analysisReference\":\"tone_burst_envelope_time_response\"") ==
      std::string::npos) {
    std::cerr << "Time response JSON section is missing.\n";
    return 1;
  }
  if (json.find("\"ir\":{\"peakAbsDelta\":") == std::string::npos ||
      json.find("\"display\":{\"analysisReference\":\"post_latency_alignment_stereo_output\"") == std::string::npos) {
    std::cerr << "IR display JSON section is missing.\n";
    return 1;
  }
  if (json.find("\"llmSummary\":{") == std::string::npos ||
      json.find("\"schemaVersion\":1") == std::string::npos ||
      json.find("\"overall\":{") == std::string::npos ||
      json.find("\"tests\":{\"ir\":{") == std::string::npos) {
    std::cerr << "LLM summary JSON root is missing.\n";
    return 1;
  }
  const std::string testKeys[] = {"ir",      "frequency", "phase",   "harmonic", "thd",
                                  "monoStereoWidth", "pitch", "dynamics", "timeResponse"};
  for (const auto& key : testKeys) {
    if (json.find("\"" + key + "\":{\"status\":") == std::string::npos) {
      std::cerr << "LLM summary test key is missing: " << key << "\n";
      return 1;
    }
  }
  auto countOccurrences = [](const std::string& haystack, const std::string& needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
      ++count;
      pos += needle.size();
    }
    return count;
  };
  if (countOccurrences(json, "\"headline\"") < 10 ||
      countOccurrences(json, "\"differenceLevel\"") < 9 ||
      countOccurrences(json, "\"notableRegions\"") < 9) {
    std::cerr << "LLM summary common fields are missing.\n";
    return 1;
  }
  if (json.find("\"label\":\"1 kHz band\"") == std::string::npos) {
    std::cerr << "Frequency notable region is missing.\n";
    return 1;
  }
  if (json.find("\"label\":\"high input segment\"") == std::string::npos) {
    std::cerr << "THD notable segment is missing.\n";
    return 1;
  }
  if (json.find("\"label\":\"31.5 Hz band\"") == std::string::npos) {
    std::cerr << "Width notable band is missing.\n";
    return 1;
  }
  if (json.find("\"pitch\":{\"status\":\"warning\"") == std::string::npos) {
    std::cerr << "Pitch warning status is missing.\n";
    return 1;
  }
  if (json.find("\"name\":\"thresholdDeltaDb\"") == std::string::npos) {
    std::cerr << "Dynamics estimated parameter metrics are missing.\n";
    return 1;
  }
  if (json.find("\"name\":\"attackDeltaMs\"") == std::string::npos) {
    std::cerr << "Time response delta metric is missing.\n";
    return 1;
  }
  if (json.find("Plugin A is higher than Plugin B") == std::string::npos) {
    std::cerr << "Directional frequency or dynamics wording is missing.\n";
    return 1;
  }
  if (json.find("Plugin A is wider than Plugin B") == std::string::npos) {
    std::cerr << "Directional width wording is missing.\n";
    return 1;
  }
  if (json.find("Plugin A is sharper than Plugin B") == std::string::npos) {
    std::cerr << "Directional pitch wording is missing.\n";
    return 1;
  }
  if (json.find("Plugin A is stronger in gain reduction than Plugin B") == std::string::npos) {
    std::cerr << "Directional dynamics wording is missing.\n";
    return 1;
  }
  if (json.find("Plugin A is slower than Plugin B") == std::string::npos ||
      json.find("Plugin A is longer than Plugin B") == std::string::npos) {
    std::cerr << "Directional time-response wording is missing.\n";
    return 1;
  }
  const std::size_t llmTestsPos = json.find("\"tests\":{");
  const std::size_t llmPitchPos =
      llmTestsPos == std::string::npos ? std::string::npos : json.find("\"pitch\":{\"status\":", llmTestsPos);
  const std::size_t llmDynamicsPos =
      llmPitchPos == std::string::npos ? std::string::npos : json.find("\"dynamics\":{\"status\":", llmPitchPos);
  if (llmPitchPos == std::string::npos || llmDynamicsPos == std::string::npos) {
    std::cerr << "LLM pitch summary section could not be located.\n";
    return 1;
  }
  const std::string pitchSummary = json.substr(llmPitchPos, llmDynamicsPos - llmPitchPos);
  if (pitchSummary.find("dBFS") != std::string::npos || pitchSummary.find("gain") != std::string::npos ||
      pitchSummary.find("volume") != std::string::npos || pitchSummary.find("output level") != std::string::npos) {
    std::cerr << "Pitch LLM summary contains non-pitch level wording.\n";
    return 1;
  }

  const std::string html = report.renderHtml(summary);
  if (html.find("Final Summary") == std::string::npos || html.find("Summary For LLM") == std::string::npos ||
      html.find("LLM Summary") != std::string::npos) {
    std::cerr << "LLM summary HTML cards are missing.\n";
    return 1;
  }

  return 0;
}
