#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vstcompare {

struct PluginMetadata {
  std::string name;
  std::string vendor;
  std::string uniqueId;
  std::string version;
  std::string path;
};

struct PluginParameter {
  uint32_t id = 0;
  std::string title;
  std::string units;
  std::string display;
  double defaultNormalized = 0.0;
  int32_t stepCount = 0;
  std::vector<std::string> enumDisplayOptions;
};

struct AudioBuffer {
  int sampleRate = 48000;
  std::vector<std::vector<float>> channels;

  std::size_t numChannels() const { return channels.size(); }
  std::size_t numSamples() const { return channels.empty() ? 0 : channels.front().size(); }

  static AudioBuffer zeros(std::size_t channelCount, std::size_t sampleCount, int sr) {
    AudioBuffer b;
    b.sampleRate = sr;
    b.channels.assign(channelCount, std::vector<float>(sampleCount, 0.0f));
    return b;
  }
};

struct TestConfig {
  int sampleRate = 48000;
  int blockSize = 512;
  int irDurationMs = 500;
  float impulseAmplitude = 1.0f;
  int frequencyDurationMs = 2000;
  int frequencyFftSize = 65536;
  int frequencyOverlapPercent = 50;
  float frequencyNoiseLevelDbfs = -12.0f;
  int phaseDurationMs = 2000;
  int phaseFftSize = 65536;
  int phaseOverlapPercent = 50;
  float phaseNoiseLevelDbfs = -12.0f;
  int harmonicDurationMs = 1000;
  int harmonicSkipHeadMs = 200;
  int harmonicFftSize = 65536;
  float harmonicInputLevelDbfs = -6.0f;
  std::vector<double> harmonicFrequenciesHz = {100.0, 1000.0, 5000.0, 10000.0};
  double thdToneFrequencyHz = 1000.0;
  float thdStartLevelDbfs = -60.0f;
  float thdEndLevelDbfs = 6.0f;
  float thdStepDb = 2.0f;
  int thdDurationMs = 1000;
  int thdSkipHeadMs = 200;
  int thdFftSize = 65536;
  int monoStereoWidthDurationMs = 500;
  float monoStereoWidthNoiseLevelDbfs = -12.0f;
  int monoStereoWidthFftSize = 65536;
  int monoStereoWidthOverlapPercent = 50;
  int monoStereoWidthTimeWindowMs = 20;
  int monoStereoWidthTimeHopMs = 10;
  int pitchDurationMs = 5000;
  float pitchStartHz = 110.0f;
  float pitchEndHz = 880.0f;
  float pitchLevelDbfs = -6.0f;
  int pitchFadeMs = 5;
  int pitchFrameMs = 100;
  int pitchHopMs = 10;
  double dynamicsToneFrequencyHz = 1000.0;
  float dynamicsStartLevelDbfs = -90.0f;
  float dynamicsEndLevelDbfs = 0.0f;
  float dynamicsStepDb = 1.0f;
  int dynamicsStepMs = 50;
  int dynamicsRmsWindowMs = 20;
  double timeBurstFrequencyHz = 1000.0;
  float timeBurstLevelDbfs = -6.0f;
  int timePreSilenceMs = 200;
  int timeBurstOnMs = 100;
  int timePostSilenceMs = 200;
  int timeEnvelopeWindowMs = 20;
  int timeCorrelationMaxLagMs = 50;
};

struct IndexedDifference {
  std::size_t sampleIndex = 0;
  float absoluteDelta = 0.0f;
};

struct IrMetrics {
  float peakAbsDelta = 0.0f;
  double energyDelta = 0.0;
  double estimatedLatencyMs = 0.0;
  uint32_t pluginLatencySamplesA = 0;
  uint32_t pluginLatencySamplesB = 0;
};

struct LatencyAlignmentInfo {
  uint32_t reportedA = 0;
  uint32_t reportedB = 0;
  uint32_t clampedA = 0;
  uint32_t clampedB = 0;
  uint32_t appliedDelayA = 0;
  uint32_t appliedDelayB = 0;
  bool clampedOccurred = false;
  std::vector<std::string> warnings;
};

struct IrTestResult {
  AudioBuffer input;
  AudioBuffer outputA;
  AudioBuffer outputB;
  AudioBuffer analysisA;
  AudioBuffer analysisB;
  AudioBuffer alignedA;
  AudioBuffer alignedB;
  AudioBuffer delta;
  AudioBuffer displayAlignedA;
  AudioBuffer displayAlignedB;
  AudioBuffer displayDelta;
  IrMetrics metrics;
  LatencyAlignmentInfo latencyAlignment;
  std::string analysisSignalMode = "single_channel";
  std::vector<IndexedDifference> topDifferences;
};

struct SpectrumPoint {
  double frequencyHz = 0.0;
  double valueDb = 0.0;
};

struct OctaveBandSummary {
  double lowerHz = 0.0;
  double centerHz = 0.0;
  double upperHz = 0.0;
  double avgDeltaDb = 0.0;
  double maxDeltaDb = 0.0;
};

struct FrequencyMetrics {
  double peakAbsDeltaDb = 0.0;
  double meanAbsDeltaDb = 0.0;
  double estimatedLatencyMs = 0.0;
  uint32_t pluginLatencySamplesA = 0;
  uint32_t pluginLatencySamplesB = 0;
};

struct FrequencyTestResult {
  AudioBuffer input;
  AudioBuffer outputA;
  AudioBuffer outputB;
  AudioBuffer analysisA;
  AudioBuffer analysisB;
  AudioBuffer alignedA;
  AudioBuffer alignedB;
  FrequencyMetrics metrics;
  LatencyAlignmentInfo latencyAlignment;
  std::string analysisSignalMode = "single_channel";
  std::vector<SpectrumPoint> inputSpectrum;
  std::vector<SpectrumPoint> spectrumA;
  std::vector<SpectrumPoint> spectrumB;
  std::vector<SpectrumPoint> normalizedSpectrumA;
  std::vector<SpectrumPoint> normalizedSpectrumB;
  std::vector<SpectrumPoint> delta;
  std::vector<OctaveBandSummary> octaveBands;
};

struct PhasePoint {
  double frequencyHz = 0.0;
  double valueRad = 0.0;
};

struct PhaseBandSummary {
  double lowerHz = 0.0;
  double centerHz = 0.0;
  double upperHz = 0.0;
  double avgDeltaRad = 0.0;
  double maxDeltaRad = 0.0;
};

struct PhaseMetrics {
  double peakAbsDeltaRad = 0.0;
  double meanAbsDeltaRad = 0.0;
  double estimatedLatencyMs = 0.0;
  uint32_t pluginLatencySamplesA = 0;
  uint32_t pluginLatencySamplesB = 0;
};

struct PhaseTestResult {
  AudioBuffer input;
  AudioBuffer outputA;
  AudioBuffer outputB;
  AudioBuffer analysisA;
  AudioBuffer analysisB;
  AudioBuffer alignedA;
  AudioBuffer alignedB;
  PhaseMetrics metrics;
  LatencyAlignmentInfo latencyAlignment;
  std::string analysisSignalMode = "single_channel";
  std::vector<PhasePoint> phaseA;
  std::vector<PhasePoint> phaseB;
  std::vector<PhasePoint> delta;
  std::vector<PhaseBandSummary> bands;
};

struct HarmonicSpectrumPoint {
  double frequencyHz = 0.0;
  double valueDbfs = 0.0;
};

struct HarmonicOrderSummary {
  int order = 0;
  double frequencyHz = 0.0;
  double amplitudeDbfsA = 0.0;
  double amplitudeDbfsB = 0.0;
  double deltaDb = 0.0;
};

struct HarmonicPitchResult {
  double fundamentalHz = 0.0;
  AudioBuffer input;
  AudioBuffer outputA;
  AudioBuffer outputB;
  AudioBuffer analysisA;
  AudioBuffer analysisB;
  AudioBuffer alignedA;
  AudioBuffer alignedB;
  uint32_t pluginLatencySamplesA = 0;
  uint32_t pluginLatencySamplesB = 0;
  LatencyAlignmentInfo latencyAlignment;
  std::string analysisSignalMode = "single_channel";
  std::vector<HarmonicSpectrumPoint> spectrumA;
  std::vector<HarmonicSpectrumPoint> spectrumB;
  std::vector<HarmonicSpectrumPoint> delta;
  std::vector<HarmonicOrderSummary> orders;
  double noiseFloorDbfsA = 0.0;
  double noiseFloorDbfsB = 0.0;
  double noiseFloorDeltaDb = 0.0;
};

struct HarmonicMetrics {
  double peakAbsDeltaDb = 0.0;
  double meanAbsDeltaDb = 0.0;
};

struct HarmonicTestResult {
  HarmonicMetrics metrics;
  std::vector<HarmonicPitchResult> pitches;
};

struct ThdSweepPoint {
  double inputLevelDbfs = 0.0;
  uint32_t pluginLatencySamplesA = 0;
  uint32_t pluginLatencySamplesB = 0;
  LatencyAlignmentInfo latencyAlignment;
  std::string analysisSignalMode = "single_channel";
  double thdPercentA = 0.0;
  double thdPercentB = 0.0;
  double thdDeltaPercent = 0.0;
  double thdnPercentA = 0.0;
  double thdnPercentB = 0.0;
  double thdnDeltaPercent = 0.0;
  std::vector<std::string> warnings;
};

struct ThdSegmentSummary {
  std::string name;
  double startLevelDbfs = 0.0;
  double endLevelDbfs = 0.0;
  int pointCount = 0;
  double avgThdDeltaPercent = 0.0;
  double maxAbsThdDeltaPercent = 0.0;
  double avgThdnDeltaPercent = 0.0;
  double maxAbsThdnDeltaPercent = 0.0;
};

struct ThdMetrics {
  double peakAbsThdDeltaPercent = 0.0;
  double meanAbsThdDeltaPercent = 0.0;
  double peakAbsThdnDeltaPercent = 0.0;
  double meanAbsThdnDeltaPercent = 0.0;
};

struct ThdTestResult {
  ThdMetrics metrics;
  std::vector<ThdSweepPoint> sweep;
  std::vector<ThdSegmentSummary> segments;
};

struct WidthTimePoint {
  double timeMs = 0.0;
  double ratioDbA = 0.0;
  double ratioDbB = 0.0;
  double deltaDb = 0.0;
  double widthPercentA = 0.0;
  double widthPercentB = 0.0;
  double deltaWidthPercent = 0.0;
};

struct WidthBandSummary {
  double lowerHz = 0.0;
  double centerHz = 0.0;
  double upperHz = 0.0;
  double ratioDbA = 0.0;
  double ratioDbB = 0.0;
  double deltaDb = 0.0;
  double widthPercentA = 0.0;
  double widthPercentB = 0.0;
  double deltaWidthPercent = 0.0;
};

struct WidthSpectrumPoint {
  double frequencyHz = 0.0;
  double ratioDbA = 0.0;
  double ratioDbB = 0.0;
  double deltaDb = 0.0;
  double widthPercentA = 0.0;
  double widthPercentB = 0.0;
  double deltaWidthPercent = 0.0;
};

struct MonoToStereoWidthMetrics {
  double peakAbsTimeDeltaDb = 0.0;
  double meanAbsTimeDeltaDb = 0.0;
  double peakAbsBandDeltaDb = 0.0;
  double meanAbsBandDeltaDb = 0.0;
  double peakAbsTimeDeltaWidthPercent = 0.0;
  double meanAbsTimeDeltaWidthPercent = 0.0;
  double peakAbsBandDeltaWidthPercent = 0.0;
  double meanAbsBandDeltaWidthPercent = 0.0;
  uint32_t pluginLatencySamplesA = 0;
  uint32_t pluginLatencySamplesB = 0;
};

struct MonoToStereoWidthTestResult {
  AudioBuffer input;
  AudioBuffer outputA;
  AudioBuffer outputB;
  AudioBuffer alignedA;
  AudioBuffer alignedB;
  AudioBuffer analysisMidA;
  AudioBuffer analysisSideA;
  AudioBuffer analysisMidB;
  AudioBuffer analysisSideB;
  std::string analysisSignalMode = "stereo_identical_lr_pink_noise";
  std::string analysisReference = "mono_pink_noise_width_index";
  MonoToStereoWidthMetrics metrics;
  LatencyAlignmentInfo latencyAlignment;
  std::vector<WidthTimePoint> timeSeries;
  std::vector<WidthSpectrumPoint> spectrum;
  std::vector<WidthBandSummary> bands;
  std::vector<std::string> warnings;
};

struct PitchCurvePoint {
  double timeMs = 0.0;
  double inputHz = 0.0;
  double pitchHzA = 0.0;
  double pitchHzB = 0.0;
};

struct PitchMetrics {
  double meanAbsErrorHzA = 0.0;
  double meanAbsErrorHzB = 0.0;
  double meanAbsDeltaHz = 0.0;
  double peakAbsDeltaHz = 0.0;
  double validFrameRateA = 0.0;
  double validFrameRateB = 0.0;
  double estimatedLatencyMs = 0.0;
  uint32_t pluginLatencySamplesA = 0;
  uint32_t pluginLatencySamplesB = 0;
};

struct PitchTestResult {
  AudioBuffer input;
  AudioBuffer outputA;
  AudioBuffer outputB;
  AudioBuffer analysisA;
  AudioBuffer analysisB;
  AudioBuffer alignedA;
  AudioBuffer alignedB;
  PitchMetrics metrics;
  LatencyAlignmentInfo latencyAlignment;
  std::string analysisSignalMode = "single_channel";
  std::vector<PitchCurvePoint> curve;
  std::vector<std::string> warnings;
};

struct DynamicsPoint {
  double inputLevelDbfs = 0.0;
  double outputLevelDbfsA = 0.0;
  double outputLevelDbfsB = 0.0;
  double outputDeltaDb = 0.0;
  double gainReductionDbA = 0.0;
  double gainReductionDbB = 0.0;
  double gainReductionDeltaDb = 0.0;
};

struct DynamicsEstimatedParams {
  double thresholdDbfs = 0.0;
  double ratio = 1.0;
  double kneeWidthDb = 0.0;
  bool valid = false;
};

struct DynamicsMetrics {
  double peakAbsOutputDeltaDb = 0.0;
  double meanAbsOutputDeltaDb = 0.0;
  double peakAbsGainReductionDeltaDb = 0.0;
  double meanAbsGainReductionDeltaDb = 0.0;
  DynamicsEstimatedParams estimatedA;
  DynamicsEstimatedParams estimatedB;
  double thresholdDeltaDb = 0.0;
  double ratioDelta = 0.0;
  double kneeWidthDeltaDb = 0.0;
  uint32_t pluginLatencySamplesA = 0;
  uint32_t pluginLatencySamplesB = 0;
};

struct DynamicsTestResult {
  AudioBuffer input;
  AudioBuffer outputA;
  AudioBuffer outputB;
  AudioBuffer analysisA;
  AudioBuffer analysisB;
  AudioBuffer alignedA;
  AudioBuffer alignedB;
  DynamicsMetrics metrics;
  LatencyAlignmentInfo latencyAlignment;
  std::string analysisSignalMode = "single_channel";
  std::vector<DynamicsPoint> ioCurve;
  std::vector<std::string> warnings;
};

struct TimeResponsePoint {
  double timeMs = 0.0;
  double envelopeDbA = -120.0;
  double envelopeDbB = -120.0;
  double deltaDb = 0.0;
};

struct TimeResponseMetrics {
  double residualLatencyMs = 0.0;
  double attackMsA = 0.0;
  double attackMsB = 0.0;
  double attackDeltaMs = 0.0;
  double releaseMsA = 0.0;
  double releaseMsB = 0.0;
  double releaseDeltaMs = 0.0;
  double postReleaseResidualDbA = 0.0;
  double postReleaseResidualDbB = 0.0;
  double postReleaseResidualDeltaDb = 0.0;
  uint32_t pluginLatencySamplesA = 0;
  uint32_t pluginLatencySamplesB = 0;
};

struct TimeResponseTestResult {
  AudioBuffer input;
  AudioBuffer outputA;
  AudioBuffer outputB;
  AudioBuffer analysisA;
  AudioBuffer analysisB;
  AudioBuffer alignedA;
  AudioBuffer alignedB;
  TimeResponseMetrics metrics;
  LatencyAlignmentInfo latencyAlignment;
  std::string analysisSignalMode = "single_channel";
  std::vector<TimeResponsePoint> curve;
  std::vector<std::string> warnings;
};

struct ParameterSetting {
  std::string plugin;
  std::string name;
  uint32_t parameterId = 0;
  double normalized = 0.0;
  std::string displayInput;
};

struct RunSummary {
  PluginMetadata pluginA;
  PluginMetadata pluginB;
  TestConfig config;
  std::vector<ParameterSetting> appliedParameters;
  std::vector<std::string> warnings;
  IrTestResult irResult;
  FrequencyTestResult frequencyResult;
  PhaseTestResult phaseResult;
  HarmonicTestResult harmonicResult;
  ThdTestResult thdResult;
  MonoToStereoWidthTestResult monoStereoWidthResult;
  PitchTestResult pitchResult;
  DynamicsTestResult dynamicsResult;
  TimeResponseTestResult timeResponseResult;
};

}  // namespace vstcompare
