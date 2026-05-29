#include "vstcompare/report_builder.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace vstcompare {
namespace {

constexpr double kPi = 3.14159265358979323846;

std::string escapeHtml(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::string escapeJson(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::string sanitizeFilename(std::string s) {
  for (char& c : s) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) {
      c = '_';
    }
  }
  if (s.empty()) s = "Unknown";
  return s;
}

std::string polylineFromSignal(const std::vector<float>& signal, int width, int height) {
  if (signal.empty()) return "";

  const std::size_t targetPoints = static_cast<std::size_t>(std::max(256, width));
  const std::size_t stride = std::max<std::size_t>(1, signal.size() / targetPoints);

  std::ostringstream oss;
  const float halfH = static_cast<float>(height) * 0.5f;
  const float xScale = static_cast<float>(width) / static_cast<float>(signal.size() - 1);

  bool first = true;
  for (std::size_t i = 0; i < signal.size(); i += stride) {
    const float x = static_cast<float>(i) * xScale;
    const float y = halfH - (signal[i] * (halfH - 4.0f));
    if (!first) oss << ' ';
    first = false;
    oss << x << ',' << y;
  }
  return oss.str();
}

struct IrAutoWindow {
  std::size_t startSample = 0;
  std::size_t endSample = 0;
  std::size_t totalSamples = 0;
  bool usedAutoZoom = false;
};

IrAutoWindow detectIrAutoWindow(const std::vector<const std::vector<float>*>& series, int sampleRate) {
  IrAutoWindow window;
  for (const auto* s : series) {
    if (s && !s->empty()) {
      window.totalSamples = std::max(window.totalSamples, s->size());
    }
  }

  if (window.totalSamples == 0) {
    return window;
  }

  float peakAbs = 0.0f;
  for (const auto* s : series) {
    if (!s) continue;
    for (float v : *s) {
      peakAbs = std::max(peakAbs, std::fabs(v));
    }
  }

  const float threshold = std::max(peakAbs * 0.01f, 1.0e-6f);
  std::size_t first = window.totalSamples;
  std::size_t last = 0;
  bool found = false;
  for (const auto* s : series) {
    if (!s) continue;
    for (std::size_t i = 0; i < s->size(); ++i) {
      if (std::fabs((*s)[i]) > threshold) {
        first = std::min(first, i);
        last = std::max(last, i);
        found = true;
      }
    }
  }

  if (!found) {
    window.startSample = 0;
    window.endSample = window.totalSamples - 1;
    return window;
  }

  const std::size_t padding =
      std::max<std::size_t>(1, static_cast<std::size_t>((static_cast<int64_t>(sampleRate) * 2) / 1000));
  window.startSample = (first > padding) ? (first - padding) : 0;
  window.endSample = std::min(window.totalSamples - 1, last + padding);
  window.usedAutoZoom = true;
  return window;
}

std::string polylineFromSignalWindow(const std::vector<float>& signal, int width, int height, std::size_t startSample,
                                     std::size_t endSample) {
  if (signal.empty()) return "";
  const std::size_t maxIndex = signal.size() - 1;
  const std::size_t start = std::min(startSample, maxIndex);
  const std::size_t end = std::max(start, std::min(endSample, maxIndex));
  const std::size_t windowSamples = end - start + 1;

  const std::size_t targetPoints = static_cast<std::size_t>(std::max(256, width));
  const std::size_t stride = std::max<std::size_t>(1, windowSamples / targetPoints);
  const float halfH = static_cast<float>(height) * 0.5f;
  const float xScale = (windowSamples > 1) ? (static_cast<float>(width) / static_cast<float>(windowSamples - 1)) : 0.0f;

  std::ostringstream oss;
  bool firstPoint = true;
  std::size_t lastIndex = start;
  std::size_t i = start;
  while (i <= end) {
    const float x = static_cast<float>(i - start) * xScale;
    const float y = halfH - (signal[i] * (halfH - 4.0f));
    if (!firstPoint) oss << ' ';
    firstPoint = false;
    oss << x << ',' << y;
    lastIndex = i;
    if (end - i < stride) break;
    i += stride;
  }

  if (lastIndex != end) {
    const float x = static_cast<float>(end - start) * xScale;
    const float y = halfH - (signal[end] * (halfH - 4.0f));
    if (!firstPoint) oss << ' ';
    oss << x << ',' << y;
  }
  return oss.str();
}

std::string formatMsLabel(double valueMs) {
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(2) << valueMs << " ms";
  return oss.str();
}

std::string buildIrTimeAxisOverlay(int width, int height, double startMs, double endMs) {
  const double clampedEndMs = std::max(startMs, endMs);
  const double midMs = (startMs + clampedEndMs) * 0.5;
  const double ticksX[] = {0.0, static_cast<double>(width) * 0.5, static_cast<double>(width)};
  const double ticksMs[] = {startMs, midMs, clampedEndMs};

  std::ostringstream oss;
  const double centerY = static_cast<double>(height) * 0.5;
  oss << "<line x1=\"0\" y1=\"" << centerY << "\" x2=\"" << width << "\" y2=\"" << centerY
      << "\" stroke=\"#d8e1ef\" stroke-dasharray=\"4 4\"/>";
  for (int i = 0; i < 3; ++i) {
    const double x = ticksX[i];
    oss << "<line x1=\"" << x << "\" y1=\"" << (height - 18) << "\" x2=\"" << x << "\" y2=\"" << height
        << "\" stroke=\"#d6e2f2\"/>";
    oss << "<text x=\"" << x << "\" y=\"" << (height - 2)
        << "\" text-anchor=\"middle\" fill=\"#65758b\" font-size=\"11\">" << formatMsLabel(ticksMs[i]) << "</text>";
  }
  return oss.str();
}

std::string polylineFromSpectrum(const std::vector<SpectrumPoint>& points, int width, int height, double minFreq,
                                 double maxFreq, double minDb, double maxDb) {
  if (points.empty() || minFreq <= 0.0 || maxFreq <= minFreq || maxDb <= minDb) return "";

  const double logMin = std::log10(minFreq);
  const double logMax = std::log10(maxFreq);
  const double logRange = logMax - logMin;
  const double dbRange = maxDb - minDb;

  std::ostringstream oss;
  bool first = true;
  for (const auto& p : points) {
    if (p.frequencyHz < minFreq || p.frequencyHz > maxFreq) continue;
    const double lx = (std::log10(p.frequencyHz) - logMin) / logRange;
    const double ly = (std::clamp(p.valueDb, minDb, maxDb) - minDb) / dbRange;
    const double x = lx * static_cast<double>(width);
    const double y = static_cast<double>(height) - (ly * static_cast<double>(height));
    if (!first) oss << ' ';
    first = false;
    oss << x << ',' << y;
  }
  return oss.str();
}

std::string polylineFromHarmonicSpectrum(const std::vector<HarmonicSpectrumPoint>& points, int width, int height,
                                         double minFreq, double maxFreq, double minDb, double maxDb) {
  if (points.empty() || minFreq <= 0.0 || maxFreq <= minFreq || maxDb <= minDb) return "";

  const double logMin = std::log10(minFreq);
  const double logMax = std::log10(maxFreq);
  const double logRange = logMax - logMin;
  const double dbRange = maxDb - minDb;

  std::ostringstream oss;
  bool first = true;
  for (const auto& p : points) {
    if (p.frequencyHz < minFreq || p.frequencyHz > maxFreq) continue;
    const double lx = (std::log10(p.frequencyHz) - logMin) / logRange;
    const double ly = (std::clamp(p.valueDbfs, minDb, maxDb) - minDb) / dbRange;
    const double x = lx * static_cast<double>(width);
    const double y = static_cast<double>(height) - (ly * static_cast<double>(height));
    if (!first) oss << ' ';
    first = false;
    oss << x << ',' << y;
  }
  return oss.str();
}

double spectrumXFromFreq(double freq, int width, double minFreq, double maxFreq) {
  if (freq <= 0.0) return 0.0;
  const double logMin = std::log10(minFreq);
  const double logMax = std::log10(maxFreq);
  const double ratio = (std::log10(freq) - logMin) / (logMax - logMin);
  return std::clamp(ratio, 0.0, 1.0) * static_cast<double>(width);
}

double spectrumYFromDb(double db, int height, double minDb, double maxDb) {
  const double ratio = (std::clamp(db, minDb, maxDb) - minDb) / (maxDb - minDb);
  return static_cast<double>(height) - (ratio * static_cast<double>(height));
}

std::string buildFrequencyAxisOverlay(int width, int height, double minFreq, double maxFreq, double minDb,
                                      double maxDb) {
  std::ostringstream oss;
  const double dbTicks[] = {24.0, 12.0, 0.0, -12.0, -24.0};
  const double freqTicks[] = {20.0, 100.0, 1000.0, 10000.0, 20000.0};
  const char* freqLabels[] = {"20Hz", "100Hz", "1kHz", "10kHz", "20kHz"};

  for (double db : dbTicks) {
    const double y = spectrumYFromDb(db, height, minDb, maxDb);
    const double labelY = std::clamp(y - 4.0, 12.0, static_cast<double>(height - 24));
    const bool center = std::fabs(db) < 1e-9;
    oss << "<line x1=\"0\" y1=\"" << y << "\" x2=\"" << width << "\" y2=\"" << y
        << "\" stroke=\"" << (center ? "#b8c8df" : "#e4edf8") << "\" stroke-dasharray=\"4 4\"/>";
    oss << "<text x=\"6\" y=\"" << labelY << "\" fill=\"#65758b\" font-size=\"11\">" << (db > 0 ? "+" : "")
        << db << " dB</text>";
  }

  for (int i = 0; i < 5; ++i) {
    const double x = spectrumXFromFreq(freqTicks[i], width, minFreq, maxFreq);
    oss << "<line x1=\"" << x << "\" y1=\"" << (height - 18) << "\" x2=\"" << x << "\" y2=\"" << height
        << "\" stroke=\"#d6e2f2\"/>";
    oss << "<text x=\"" << x << "\" y=\"" << (height - 2) << "\" text-anchor=\"middle\" fill=\"#65758b\" font-size=\"11\">"
        << freqLabels[i] << "</text>";
  }
  return oss.str();
}

std::string buildHarmonicAxisOverlay(int width, int height, double minFreq, double maxFreq, double minDb, double maxDb) {
  std::ostringstream oss;
  const double dbTicks[] = {0.0, -20.0, -40.0, -60.0, -80.0, -100.0, -120.0, -140.0};
  const double freqTicks[] = {20.0, 100.0, 1000.0, 10000.0, 20000.0, 24000.0};
  const char* freqLabels[] = {"20Hz", "100Hz", "1kHz", "10kHz", "20kHz", "24kHz"};

  for (double db : dbTicks) {
    const double y = spectrumYFromDb(db, height, minDb, maxDb);
    const double labelY = std::clamp(y - 4.0, 12.0, static_cast<double>(height - 24));
    oss << "<line x1=\"0\" y1=\"" << y << "\" x2=\"" << width << "\" y2=\"" << y
        << "\" stroke=\"#e4edf8\" stroke-dasharray=\"4 4\"/>";
    oss << "<text x=\"6\" y=\"" << labelY << "\" fill=\"#65758b\" font-size=\"11\">" << db << " dBFS</text>";
  }

  for (int i = 0; i < 6; ++i) {
    const double x = spectrumXFromFreq(freqTicks[i], width, minFreq, maxFreq);
    oss << "<line x1=\"" << x << "\" y1=\"" << (height - 18) << "\" x2=\"" << x << "\" y2=\"" << height
        << "\" stroke=\"#d6e2f2\"/>";
    oss << "<text x=\"" << x << "\" y=\"" << (height - 2) << "\" text-anchor=\"middle\" fill=\"#65758b\" font-size=\"11\">"
        << freqLabels[i] << "</text>";
  }
  return oss.str();
}

std::string buildHarmonicDeltaAxisOverlay(int width, int height, double minFreq, double maxFreq, double minDb,
                                          double maxDb) {
  std::ostringstream oss;
  const double dbTicks[] = {24.0, 12.0, 0.0, -12.0, -24.0};
  const double freqTicks[] = {20.0, 100.0, 1000.0, 10000.0, 20000.0, 24000.0};
  const char* freqLabels[] = {"20Hz", "100Hz", "1kHz", "10kHz", "20kHz", "24kHz"};

  for (double db : dbTicks) {
    const double y = spectrumYFromDb(db, height, minDb, maxDb);
    const double labelY = std::clamp(y - 4.0, 12.0, static_cast<double>(height - 24));
    const bool center = std::fabs(db) < 1e-9;
    oss << "<line x1=\"0\" y1=\"" << y << "\" x2=\"" << width << "\" y2=\"" << y
        << "\" stroke=\"" << (center ? "#b8c8df" : "#e4edf8") << "\" stroke-dasharray=\"4 4\"/>";
    oss << "<text x=\"6\" y=\"" << labelY << "\" fill=\"#65758b\" font-size=\"11\">" << (db > 0 ? "+" : "") << db
        << " dB</text>";
  }

  for (int i = 0; i < 6; ++i) {
    const double x = spectrumXFromFreq(freqTicks[i], width, minFreq, maxFreq);
    oss << "<line x1=\"" << x << "\" y1=\"" << (height - 18) << "\" x2=\"" << x << "\" y2=\"" << height
        << "\" stroke=\"#d6e2f2\"/>";
    oss << "<text x=\"" << x << "\" y=\"" << (height - 2)
        << "\" text-anchor=\"middle\" fill=\"#65758b\" font-size=\"11\">" << freqLabels[i] << "</text>";
  }
  return oss.str();
}

double thdXFromInputLevel(double inputDbfs, int width, double minInputDbfs, double maxInputDbfs) {
  if (maxInputDbfs <= minInputDbfs) return 0.0;
  const double ratio = (inputDbfs - minInputDbfs) / (maxInputDbfs - minInputDbfs);
  return std::clamp(ratio, 0.0, 1.0) * static_cast<double>(width);
}

double thdYFromPercent(double percent, int height, double minPercent, double maxPercent) {
  if (percent <= 0.0 || minPercent <= 0.0 || maxPercent <= minPercent) return static_cast<double>(height);
  const double logMin = std::log10(minPercent);
  const double logMax = std::log10(maxPercent);
  const double logValue = std::log10(std::clamp(percent, minPercent, maxPercent));
  const double ratio = (logValue - logMin) / (logMax - logMin);
  return static_cast<double>(height) - (ratio * static_cast<double>(height));
}

std::string polylineFromThdSweep(const std::vector<ThdSweepPoint>& points, int width, int height, double minInputDbfs,
                                 double maxInputDbfs, double minPercent, double maxPercent, bool useThdn,
                                 bool usePluginB) {
  if (points.empty()) return "";

  std::ostringstream oss;
  bool first = true;
  for (const auto& p : points) {
    const double x = thdXFromInputLevel(p.inputLevelDbfs, width, minInputDbfs, maxInputDbfs);
    const double value = useThdn ? (usePluginB ? p.thdnPercentB : p.thdnPercentA)
                                 : (usePluginB ? p.thdPercentB : p.thdPercentA);
    const double y = thdYFromPercent(value, height, minPercent, maxPercent);
    if (!first) oss << ' ';
    first = false;
    oss << x << ',' << y;
  }
  return oss.str();
}

std::string buildThdAxisOverlay(int width, int height, double minInputDbfs, double maxInputDbfs, double minPercent,
                                double maxPercent) {
  std::ostringstream oss;
  const double yTicks[] = {100.0, 10.0, 1.0, 0.1, 0.01, 0.001, 0.0001};
  const double xTicks[] = {-60.0, -40.0, -20.0, 0.0, 6.0};
  const char* xLabels[] = {"-60", "-40", "-20", "0", "+6"};

  for (double v : yTicks) {
    const double y = thdYFromPercent(v, height, minPercent, maxPercent);
    const bool center = std::fabs(v - 1.0) < 1e-12;
    oss << "<line x1=\"0\" y1=\"" << y << "\" x2=\"" << width << "\" y2=\"" << y
        << "\" stroke=\"" << (center ? "#b8c8df" : "#e4edf8") << "\" stroke-dasharray=\"4 4\"/>";
    oss << "<text x=\"6\" y=\"" << std::clamp(y - 4.0, 12.0, static_cast<double>(height - 24))
        << "\" fill=\"#65758b\" font-size=\"11\">" << v << "%</text>";
  }

  for (int i = 0; i < 5; ++i) {
    const double x = thdXFromInputLevel(xTicks[i], width, minInputDbfs, maxInputDbfs);
    oss << "<line x1=\"" << x << "\" y1=\"" << (height - 18) << "\" x2=\"" << x << "\" y2=\"" << height
        << "\" stroke=\"#d6e2f2\"/>";
    oss << "<text x=\"" << x << "\" y=\"" << (height - 2)
        << "\" text-anchor=\"middle\" fill=\"#65758b\" font-size=\"11\">" << xLabels[i] << " dBFS</text>";
  }
  return oss.str();
}

double dynamicsXFromInputLevel(double inputDbfs, int width, double minInputDbfs, double maxInputDbfs) {
  if (maxInputDbfs <= minInputDbfs) return 0.0;
  const double ratio = (inputDbfs - minInputDbfs) / (maxInputDbfs - minInputDbfs);
  return std::clamp(ratio, 0.0, 1.0) * static_cast<double>(width);
}

double dynamicsYFromDb(double valueDb, int height, double minDb, double maxDb) {
  if (maxDb <= minDb) return static_cast<double>(height);
  const double ratio = (std::clamp(valueDb, minDb, maxDb) - minDb) / (maxDb - minDb);
  return static_cast<double>(height) - (ratio * static_cast<double>(height));
}

std::string polylineFromDynamicsIo(const std::vector<DynamicsPoint>& points, int width, int height, double minInputDbfs,
                                   double maxInputDbfs, double minOutputDbfs, double maxOutputDbfs, bool useB,
                                   bool useDelta) {
  if (points.empty()) return "";
  std::ostringstream oss;
  bool first = true;
  for (const auto& p : points) {
    const double value = useDelta ? p.outputDeltaDb : (useB ? p.outputLevelDbfsB : p.outputLevelDbfsA);
    const double x = dynamicsXFromInputLevel(p.inputLevelDbfs, width, minInputDbfs, maxInputDbfs);
    const double y = dynamicsYFromDb(value, height, minOutputDbfs, maxOutputDbfs);
    if (!first) oss << ' ';
    first = false;
    oss << x << ',' << y;
  }
  return oss.str();
}

std::string polylineFromDynamicsGr(const std::vector<DynamicsPoint>& points, int width, int height, double minInputDbfs,
                                   double maxInputDbfs, double minGrDb, double maxGrDb, bool useB, bool useDelta) {
  if (points.empty()) return "";
  std::ostringstream oss;
  bool first = true;
  for (const auto& p : points) {
    const double value = useDelta ? p.gainReductionDeltaDb : (useB ? p.gainReductionDbB : p.gainReductionDbA);
    const double x = dynamicsXFromInputLevel(p.inputLevelDbfs, width, minInputDbfs, maxInputDbfs);
    const double y = dynamicsYFromDb(value, height, minGrDb, maxGrDb);
    if (!first) oss << ' ';
    first = false;
    oss << x << ',' << y;
  }
  return oss.str();
}

std::string buildDynamicsAxisOverlay(int width, int height, double minInputDbfs, double maxInputDbfs, double minYDb,
                                     double maxYDb, const std::vector<double>& yTicks, const char* yUnit) {
  std::ostringstream oss;
  const double xTicks[] = {-90.0, -60.0, -30.0, 0.0};
  const char* xLabels[] = {"-90", "-60", "-30", "0"};

  for (double v : yTicks) {
    const double y = dynamicsYFromDb(v, height, minYDb, maxYDb);
    const bool center = std::fabs(v) < 1e-9;
    oss << "<line x1=\"0\" y1=\"" << y << "\" x2=\"" << width << "\" y2=\"" << y
        << "\" stroke=\"" << (center ? "#b8c8df" : "#e4edf8") << "\" stroke-dasharray=\"4 4\"/>";
    oss << "<text x=\"6\" y=\"" << std::clamp(y - 4.0, 12.0, static_cast<double>(height - 24))
        << "\" fill=\"#65758b\" font-size=\"11\">" << (v > 0 ? "+" : "") << v << " " << yUnit << "</text>";
  }

  for (int i = 0; i < 4; ++i) {
    const double x = dynamicsXFromInputLevel(xTicks[i], width, minInputDbfs, maxInputDbfs);
    oss << "<line x1=\"" << x << "\" y1=\"" << (height - 18) << "\" x2=\"" << x << "\" y2=\"" << height
        << "\" stroke=\"#d6e2f2\"/>";
    oss << "<text x=\"" << x << "\" y=\"" << (height - 2)
        << "\" text-anchor=\"middle\" fill=\"#65758b\" font-size=\"11\">" << xLabels[i] << " dBFS</text>";
  }
  return oss.str();
}

double phaseYFromRad(double rad, int height) {
  const double minRad = -kPi;
  const double maxRad = kPi;
  const double ratio = (std::clamp(rad, minRad, maxRad) - minRad) / (maxRad - minRad);
  return static_cast<double>(height) - (ratio * static_cast<double>(height));
}

std::string polylineFromPhase(const std::vector<PhasePoint>& points, int width, int height, double minFreq,
                              double maxFreq) {
  if (points.empty() || minFreq <= 0.0 || maxFreq <= minFreq) return "";

  const double logMin = std::log10(minFreq);
  const double logMax = std::log10(maxFreq);
  const double logRange = logMax - logMin;

  std::ostringstream oss;
  bool first = true;
  for (const auto& p : points) {
    if (p.frequencyHz < minFreq || p.frequencyHz > maxFreq) continue;
    const double lx = (std::log10(p.frequencyHz) - logMin) / logRange;
    const double x = lx * static_cast<double>(width);
    const double y = phaseYFromRad(p.valueRad, height);
    if (!first) oss << ' ';
    first = false;
    oss << x << ',' << y;
  }
  return oss.str();
}

std::string buildPhaseAxisOverlay(int width, int height, double minFreq, double maxFreq) {
  std::ostringstream oss;
  const double phaseTicks[] = {kPi, kPi * 0.5, 0.0, -kPi * 0.5, -kPi};
  const char* phaseLabels[] = {"+&pi;", "+&pi;/2", "0", "-&pi;/2", "-&pi;"};
  const double freqTicks[] = {20.0, 100.0, 1000.0, 10000.0, 20000.0};
  const char* freqLabels[] = {"20Hz", "100Hz", "1kHz", "10kHz", "20kHz"};

  for (int i = 0; i < 5; ++i) {
    const double y = phaseYFromRad(phaseTicks[i], height);
    const double labelY = std::clamp(y - 4.0, 12.0, static_cast<double>(height - 24));
    const bool center = i == 2;
    oss << "<line x1=\"0\" y1=\"" << y << "\" x2=\"" << width << "\" y2=\"" << y
        << "\" stroke=\"" << (center ? "#b8c8df" : "#e4edf8") << "\" stroke-dasharray=\"4 4\"/>";
    oss << "<text x=\"6\" y=\"" << labelY << "\" fill=\"#65758b\" font-size=\"11\">" << phaseLabels[i] << " rad</text>";
  }

  for (int i = 0; i < 5; ++i) {
    const double x = spectrumXFromFreq(freqTicks[i], width, minFreq, maxFreq);
    oss << "<line x1=\"" << x << "\" y1=\"" << (height - 18) << "\" x2=\"" << x << "\" y2=\"" << height
        << "\" stroke=\"#d6e2f2\"/>";
    oss << "<text x=\"" << x << "\" y=\"" << (height - 2) << "\" text-anchor=\"middle\" fill=\"#65758b\" font-size=\"11\">"
        << freqLabels[i] << "</text>";
  }
  return oss.str();
}

void appendCompactSpectrumJson(std::ostringstream& oss, const std::vector<SpectrumPoint>& spectrum,
                               std::size_t maxPoints) {
  oss << "[";
  if (!spectrum.empty() && maxPoints > 0) {
    const std::size_t stride = std::max<std::size_t>(1, spectrum.size() / maxPoints);
    bool first = true;
    for (std::size_t i = 0; i < spectrum.size(); i += stride) {
      if (!first) oss << ",";
      first = false;
      oss << "{\"hz\":" << spectrum[i].frequencyHz << ",\"db\":" << spectrum[i].valueDb << "}";
    }
  }
  oss << "]";
}

void appendCompactPhaseJson(std::ostringstream& oss, const std::vector<PhasePoint>& phase, std::size_t maxPoints) {
  oss << "[";
  if (!phase.empty() && maxPoints > 0) {
    const std::size_t stride = std::max<std::size_t>(1, phase.size() / maxPoints);
    bool first = true;
    for (std::size_t i = 0; i < phase.size(); i += stride) {
      if (!first) oss << ",";
      first = false;
      oss << "{\"hz\":" << phase[i].frequencyHz << ",\"rad\":" << phase[i].valueRad << "}";
    }
  }
  oss << "]";
}

void appendCompactHarmonicSpectrumJson(std::ostringstream& oss, const std::vector<HarmonicSpectrumPoint>& spectrum,
                                       std::size_t maxPoints) {
  oss << "[";
  if (!spectrum.empty() && maxPoints > 0) {
    const std::size_t stride = std::max<std::size_t>(1, spectrum.size() / maxPoints);
    bool first = true;
    for (std::size_t i = 0; i < spectrum.size(); i += stride) {
      if (!first) oss << ",";
      first = false;
      oss << "{\"hz\":" << spectrum[i].frequencyHz << ",\"dbfs\":" << spectrum[i].valueDbfs << "}";
    }
  }
  oss << "]";
}

void appendCompactSignalJson(std::ostringstream& oss, const std::vector<float>& signal, std::size_t maxPoints) {
  oss << "[";
  if (!signal.empty() && maxPoints > 0) {
    const std::size_t stride = std::max<std::size_t>(1, signal.size() / maxPoints);
    bool first = true;
    for (std::size_t i = 0; i < signal.size(); i += stride) {
      if (!first) oss << ",";
      first = false;
      oss << signal[i];
    }
  }
  oss << "]";
}

double widthYFromValue(double value, int height, double minValue, double maxValue) {
  const double ratio = (std::clamp(value, minValue, maxValue) - minValue) / (maxValue - minValue);
  return static_cast<double>(height) - (ratio * static_cast<double>(height));
}

std::string polylineFromWidthTime(const std::vector<WidthTimePoint>& points, int width, int height, double minTimeMs,
                                  double maxTimeMs, double minValue, double maxValue, bool useB, bool useDelta) {
  if (points.empty() || maxTimeMs <= minTimeMs || maxValue <= minValue) return "";
  std::ostringstream oss;
  bool first = true;
  for (const auto& p : points) {
    const double value = useDelta ? p.deltaWidthPercent : (useB ? p.widthPercentB : p.widthPercentA);
    const double xRatio = (p.timeMs - minTimeMs) / (maxTimeMs - minTimeMs);
    const double x = std::clamp(xRatio, 0.0, 1.0) * static_cast<double>(width);
    const double y = widthYFromValue(value, height, minValue, maxValue);
    if (!first) oss << ' ';
    first = false;
    oss << x << ',' << y;
  }
  return oss.str();
}

std::string polylineFromWidthSpectrum(const std::vector<WidthSpectrumPoint>& points, int width, int height,
                                      double minFreq, double maxFreq, double minValue, double maxValue, bool useB,
                                      bool useDelta) {
  if (points.empty() || minFreq <= 0.0 || maxFreq <= minFreq || maxValue <= minValue) return "";
  const double logMin = std::log10(minFreq);
  const double logMax = std::log10(maxFreq);
  const double logRange = logMax - logMin;

  std::ostringstream oss;
  bool first = true;
  for (const auto& p : points) {
    if (p.frequencyHz < minFreq || p.frequencyHz > maxFreq) continue;
    const double value = useDelta ? p.deltaWidthPercent : (useB ? p.widthPercentB : p.widthPercentA);
    const double x = ((std::log10(p.frequencyHz) - logMin) / logRange) * static_cast<double>(width);
    const double y = widthYFromValue(value, height, minValue, maxValue);
    if (!first) oss << ' ';
    first = false;
    oss << x << ',' << y;
  }
  return oss.str();
}

std::string buildWidthTimeAxisOverlay(int width, int height, double minTimeMs, double maxTimeMs, double minDb,
                                      double maxDb) {
  std::ostringstream oss;
  const double dbTicks[] = {100.0, 75.0, 50.0, 25.0, 0.0};
  for (double db : dbTicks) {
    const double y = widthYFromValue(db, height, minDb, maxDb);
    const bool center = std::fabs(db - 50.0) < 1e-9;
    oss << "<line x1=\"0\" y1=\"" << y << "\" x2=\"" << width << "\" y2=\"" << y
        << "\" stroke=\"" << (center ? "#b8c8df" : "#e4edf8") << "\" stroke-dasharray=\"4 4\"/>";
    oss << "<text x=\"6\" y=\"" << std::clamp(y - 4.0, 12.0, static_cast<double>(height - 24))
        << "\" fill=\"#65758b\" font-size=\"11\">" << db << "%</text>";
  }

  const double total = std::max(1.0, maxTimeMs - minTimeMs);
  const double tickRatios[] = {0.0, 0.25, 0.5, 0.75, 1.0};
  for (double r : tickRatios) {
    const double x = r * static_cast<double>(width);
    const double t = minTimeMs + (total * r);
    oss << "<line x1=\"" << x << "\" y1=\"" << (height - 18) << "\" x2=\"" << x << "\" y2=\"" << height
        << "\" stroke=\"#d6e2f2\"/>";
    oss << "<text x=\"" << x << "\" y=\"" << (height - 2)
        << "\" text-anchor=\"middle\" fill=\"#65758b\" font-size=\"11\">" << t << " ms</text>";
  }
  return oss.str();
}

std::string buildWidthBandAxisOverlay(int width, int height, double minFreq, double maxFreq, double minDb,
                                      double maxDb) {
  std::ostringstream oss;
  const double dbTicks[] = {100.0, 75.0, 50.0, 25.0, 0.0};
  const double freqTicks[] = {20.0, 100.0, 1000.0, 10000.0, 20000.0};
  const char* freqLabels[] = {"20Hz", "100Hz", "1kHz", "10kHz", "20kHz"};
  for (double db : dbTicks) {
    const double y = widthYFromValue(db, height, minDb, maxDb);
    const bool center = std::fabs(db - 50.0) < 1e-9;
    oss << "<line x1=\"0\" y1=\"" << y << "\" x2=\"" << width << "\" y2=\"" << y
        << "\" stroke=\"" << (center ? "#b8c8df" : "#e4edf8") << "\" stroke-dasharray=\"4 4\"/>";
    oss << "<text x=\"6\" y=\"" << std::clamp(y - 4.0, 12.0, static_cast<double>(height - 24))
        << "\" fill=\"#65758b\" font-size=\"11\">" << db << "%</text>";
  }

  for (int i = 0; i < 5; ++i) {
    const double x = spectrumXFromFreq(freqTicks[i], width, minFreq, maxFreq);
    oss << "<line x1=\"" << x << "\" y1=\"" << (height - 18) << "\" x2=\"" << x << "\" y2=\"" << height
        << "\" stroke=\"#d6e2f2\"/>";
    oss << "<text x=\"" << x << "\" y=\"" << (height - 2) << "\" text-anchor=\"middle\" fill=\"#65758b\" font-size=\"11\">"
        << freqLabels[i] << "</text>";
  }
  return oss.str();
}

std::string buildWidthTimeDeltaAxisOverlay(int width, int height, double minTimeMs, double maxTimeMs, double minValue,
                                           double maxValue) {
  std::ostringstream oss;
  const double ticks[] = {50.0, 25.0, 0.0, -25.0, -50.0};
  for (double tick : ticks) {
    const double y = widthYFromValue(tick, height, minValue, maxValue);
    const bool center = std::fabs(tick) < 1e-9;
    oss << "<line x1=\"0\" y1=\"" << y << "\" x2=\"" << width << "\" y2=\"" << y
        << "\" stroke=\"" << (center ? "#b8c8df" : "#e4edf8") << "\" stroke-dasharray=\"4 4\"/>";
    oss << "<text x=\"6\" y=\"" << std::clamp(y - 4.0, 12.0, static_cast<double>(height - 24))
        << "\" fill=\"#65758b\" font-size=\"11\">" << (tick > 0 ? "+" : "") << tick << " pt</text>";
  }

  const double total = std::max(1.0, maxTimeMs - minTimeMs);
  const double tickRatios[] = {0.0, 0.25, 0.5, 0.75, 1.0};
  for (double r : tickRatios) {
    const double x = r * static_cast<double>(width);
    const double t = minTimeMs + (total * r);
    oss << "<line x1=\"" << x << "\" y1=\"" << (height - 18) << "\" x2=\"" << x << "\" y2=\"" << height
        << "\" stroke=\"#d6e2f2\"/>";
    oss << "<text x=\"" << x << "\" y=\"" << (height - 2)
        << "\" text-anchor=\"middle\" fill=\"#65758b\" font-size=\"11\">" << t << " ms</text>";
  }
  return oss.str();
}

std::string buildWidthBandDeltaAxisOverlay(int width, int height, double minFreq, double maxFreq, double minValue,
                                           double maxValue) {
  std::ostringstream oss;
  const double ticks[] = {50.0, 25.0, 0.0, -25.0, -50.0};
  const double freqTicks[] = {20.0, 100.0, 1000.0, 10000.0, 20000.0};
  const char* freqLabels[] = {"20Hz", "100Hz", "1kHz", "10kHz", "20kHz"};
  for (double tick : ticks) {
    const double y = widthYFromValue(tick, height, minValue, maxValue);
    const bool center = std::fabs(tick) < 1e-9;
    oss << "<line x1=\"0\" y1=\"" << y << "\" x2=\"" << width << "\" y2=\"" << y
        << "\" stroke=\"" << (center ? "#b8c8df" : "#e4edf8") << "\" stroke-dasharray=\"4 4\"/>";
    oss << "<text x=\"6\" y=\"" << std::clamp(y - 4.0, 12.0, static_cast<double>(height - 24))
        << "\" fill=\"#65758b\" font-size=\"11\">" << (tick > 0 ? "+" : "") << tick << " pt</text>";
  }
  for (int i = 0; i < 5; ++i) {
    const double x = spectrumXFromFreq(freqTicks[i], width, minFreq, maxFreq);
    oss << "<line x1=\"" << x << "\" y1=\"" << (height - 18) << "\" x2=\"" << x << "\" y2=\"" << height
        << "\" stroke=\"#d6e2f2\"/>";
    oss << "<text x=\"" << x << "\" y=\"" << (height - 2) << "\" text-anchor=\"middle\" fill=\"#65758b\" font-size=\"11\">"
        << freqLabels[i] << "</text>";
  }
  return oss.str();
}

double pitchXFromTime(double timeMs, int width, double minTimeMs, double maxTimeMs) {
  if (maxTimeMs <= minTimeMs) return 0.0;
  const double ratio = (timeMs - minTimeMs) / (maxTimeMs - minTimeMs);
  return std::clamp(ratio, 0.0, 1.0) * static_cast<double>(width);
}

double pitchYFromHz(double hz, int height, double minHz, double maxHz) {
  if (minHz <= 0.0 || maxHz <= minHz || hz <= 0.0) return static_cast<double>(height);
  const double logMin = std::log10(minHz);
  const double logMax = std::log10(maxHz);
  const double logHz = std::log10(std::clamp(hz, minHz, maxHz));
  const double ratio = (logHz - logMin) / (logMax - logMin);
  return static_cast<double>(height) - (ratio * static_cast<double>(height));
}

std::string polylineFromPitchCurve(const std::vector<PitchCurvePoint>& points, int width, int height, double minTimeMs,
                                   double maxTimeMs, double minHz, double maxHz, int seriesType) {
  if (points.empty() || maxTimeMs <= minTimeMs || minHz <= 0.0 || maxHz <= minHz) return "";
  std::ostringstream oss;
  bool first = true;
  for (const auto& p : points) {
    double hz = 0.0;
    if (seriesType == 0) {
      hz = p.inputHz;
    } else if (seriesType == 1) {
      hz = p.pitchHzA;
    } else {
      hz = p.pitchHzB;
    }
    if (!std::isfinite(hz) || hz <= 0.0) continue;
    const double x = pitchXFromTime(p.timeMs, width, minTimeMs, maxTimeMs);
    const double y = pitchYFromHz(hz, height, minHz, maxHz);
    if (!first) oss << ' ';
    first = false;
    oss << x << ',' << y;
  }
  return oss.str();
}

std::string buildPitchAxisOverlay(int width, int height, double minTimeMs, double maxTimeMs, double minHz,
                                  double maxHz) {
  std::ostringstream oss;
  const bool looksLikeA2ToA5 = std::fabs(minHz - 110.0) < 1e-6 && std::fabs(maxHz - 880.0) < 1e-6;
  const std::vector<double> hzTicks = looksLikeA2ToA5 ? std::vector<double>{880.0, 440.0, 220.0, 110.0}
                                                       : std::vector<double>{maxHz, 440.0, 220.0, minHz};
  const std::vector<std::string> hzLabels = looksLikeA2ToA5 ? std::vector<std::string>{"A5 880Hz", "A4 440Hz",
                                                                                         "A3 220Hz", "A2 110Hz"}
                                                             : std::vector<std::string>{
                                                                   std::to_string(static_cast<int>(std::round(maxHz))) +
                                                                       "Hz",
                                                                   "A4 440Hz", "A3 220Hz",
                                                                   std::to_string(static_cast<int>(std::round(minHz))) +
                                                                       "Hz"};
  for (std::size_t i = 0; i < hzTicks.size(); ++i) {
    const double y = pitchYFromHz(hzTicks[i], height, minHz, maxHz);
    oss << "<line x1=\"0\" y1=\"" << y << "\" x2=\"" << width << "\" y2=\"" << y
        << "\" stroke=\"#e4edf8\" stroke-dasharray=\"4 4\"/>";
    oss << "<text x=\"6\" y=\"" << std::clamp(y - 4.0, 12.0, static_cast<double>(height - 24))
        << "\" fill=\"#65758b\" font-size=\"11\">" << hzLabels[i] << "</text>";
  }

  const double total = std::max(1.0, maxTimeMs - minTimeMs);
  const double tickRatios[] = {0.0, 0.25, 0.5, 0.75, 1.0};
  for (double r : tickRatios) {
    const double x = r * static_cast<double>(width);
    const double tSec = (minTimeMs + (total * r)) / 1000.0;
    oss << "<line x1=\"" << x << "\" y1=\"" << (height - 18) << "\" x2=\"" << x << "\" y2=\"" << height
        << "\" stroke=\"#d6e2f2\"/>";
    oss << "<text x=\"" << x << "\" y=\"" << (height - 2)
        << "\" text-anchor=\"middle\" fill=\"#65758b\" font-size=\"11\">" << tSec << " s</text>";
  }
  return oss.str();
}

double timeResponseXFromTime(double timeMs, int width, double minTimeMs, double maxTimeMs) {
  if (maxTimeMs <= minTimeMs) return 0.0;
  const double ratio = (timeMs - minTimeMs) / (maxTimeMs - minTimeMs);
  return std::clamp(ratio, 0.0, 1.0) * static_cast<double>(width);
}

double timeResponseYFromDb(double db, int height, double minDb, double maxDb) {
  if (maxDb <= minDb) return static_cast<double>(height);
  const double ratio = (std::clamp(db, minDb, maxDb) - minDb) / (maxDb - minDb);
  return static_cast<double>(height) - (ratio * static_cast<double>(height));
}

std::string polylineFromTimeResponse(const std::vector<TimeResponsePoint>& points, int width, int height, double minTimeMs,
                                     double maxTimeMs, double minDb, double maxDb, int mode) {
  if (points.empty()) return "";
  std::ostringstream oss;
  bool first = true;
  for (const auto& p : points) {
    const double value = (mode == 0) ? p.envelopeDbA : (mode == 1) ? p.envelopeDbB : p.deltaDb;
    if (!std::isfinite(value)) continue;
    const double x = timeResponseXFromTime(p.timeMs, width, minTimeMs, maxTimeMs);
    const double y = timeResponseYFromDb(value, height, minDb, maxDb);
    if (!first) oss << ' ';
    first = false;
    oss << x << ',' << y;
  }
  return oss.str();
}

std::string buildTimeResponseAxisOverlay(int width, int height, double minTimeMs, double maxTimeMs, double minDb,
                                         double maxDb, const std::vector<double>& yTicks, const std::string& yUnit,
                                         bool emphasizeZeroLine) {
  std::ostringstream oss;
  for (double v : yTicks) {
    const double y = timeResponseYFromDb(v, height, minDb, maxDb);
    const bool center = emphasizeZeroLine && std::fabs(v) < 1e-9;
    oss << "<line x1=\"0\" y1=\"" << y << "\" x2=\"" << width << "\" y2=\"" << y << "\" stroke=\""
        << (center ? "#b8c8df" : "#e4edf8") << "\" stroke-dasharray=\"4 4\"/>";
    oss << "<text x=\"6\" y=\"" << std::clamp(y - 4.0, 12.0, static_cast<double>(height - 24))
        << "\" fill=\"#65758b\" font-size=\"11\">" << (v > 0 ? "+" : "") << v << " " << yUnit << "</text>";
  }

  const double total = std::max(1.0, maxTimeMs - minTimeMs);
  const double tickRatios[] = {0.0, 0.25, 0.5, 0.75, 1.0};
  for (double r : tickRatios) {
    const double x = r * static_cast<double>(width);
    const double tMs = minTimeMs + (total * r);
    oss << "<line x1=\"" << x << "\" y1=\"" << (height - 18) << "\" x2=\"" << x << "\" y2=\"" << height
        << "\" stroke=\"#d6e2f2\"/>";
    oss << "<text x=\"" << x << "\" y=\"" << (height - 2)
        << "\" text-anchor=\"middle\" fill=\"#65758b\" font-size=\"11\">" << tMs << " ms</text>";
  }
  return oss.str();
}

void appendCompactWidthTimeJson(std::ostringstream& oss, const std::vector<WidthTimePoint>& points,
                                std::size_t maxPoints) {
  oss << "[";
  if (!points.empty() && maxPoints > 0) {
    const std::size_t stride = std::max<std::size_t>(1, points.size() / maxPoints);
    bool first = true;
    for (std::size_t i = 0; i < points.size(); i += stride) {
      if (!first) oss << ",";
      first = false;
      oss << "{\"ms\":" << points[i].timeMs << ",\"aDb\":" << points[i].ratioDbA << ",\"bDb\":" << points[i].ratioDbB
          << ",\"deltaDb\":" << points[i].deltaDb << ",\"widthPercentA\":" << points[i].widthPercentA
          << ",\"widthPercentB\":" << points[i].widthPercentB << ",\"deltaWidthPercent\":"
          << points[i].deltaWidthPercent << "}";
    }
  }
  oss << "]";
}

void appendCompactPitchCurveJson(std::ostringstream& oss, const std::vector<PitchCurvePoint>& points,
                                 std::size_t maxPoints) {
  oss << "[";
  if (!points.empty() && maxPoints > 0) {
    const std::size_t stride = std::max<std::size_t>(1, points.size() / maxPoints);
    bool first = true;
    for (std::size_t i = 0; i < points.size(); i += stride) {
      if (!first) oss << ",";
      first = false;
      oss << "{\"ms\":" << points[i].timeMs << ",\"inputHz\":" << points[i].inputHz << ",\"aHz\":"
          << points[i].pitchHzA << ",\"bHz\":" << points[i].pitchHzB << "}";
    }
  }
  oss << "]";
}

void appendCompactDynamicsCurveJson(std::ostringstream& oss, const std::vector<DynamicsPoint>& points,
                                    std::size_t maxPoints) {
  oss << "[";
  if (!points.empty() && maxPoints > 0) {
    const std::size_t stride = std::max<std::size_t>(1, points.size() / maxPoints);
    bool first = true;
    for (std::size_t i = 0; i < points.size(); i += stride) {
      if (!first) oss << ",";
      first = false;
      oss << "{\"inputDbfs\":" << points[i].inputLevelDbfs << ",\"outputA\":" << points[i].outputLevelDbfsA
          << ",\"outputB\":" << points[i].outputLevelDbfsB << ",\"outputDelta\":" << points[i].outputDeltaDb
          << ",\"grA\":" << points[i].gainReductionDbA << ",\"grB\":" << points[i].gainReductionDbB
          << ",\"grDelta\":" << points[i].gainReductionDeltaDb << "}";
    }
  }
  oss << "]";
}

void appendCompactTimeResponseCurveJson(std::ostringstream& oss, const std::vector<TimeResponsePoint>& points,
                                        std::size_t maxPoints) {
  oss << "[";
  if (!points.empty() && maxPoints > 0) {
    const std::size_t stride = std::max<std::size_t>(1, points.size() / maxPoints);
    bool first = true;
    for (std::size_t i = 0; i < points.size(); i += stride) {
      if (!first) oss << ",";
      first = false;
      oss << "{\"ms\":" << points[i].timeMs << ",\"aDb\":" << points[i].envelopeDbA << ",\"bDb\":"
          << points[i].envelopeDbB << ",\"deltaDb\":" << points[i].deltaDb << "}";
    }
  }
  oss << "]";
}

void appendCompactWidthSpectrumJson(std::ostringstream& oss, const std::vector<WidthSpectrumPoint>& points,
                                     std::size_t maxPoints) {
  oss << "[";
  if (!points.empty() && maxPoints > 0) {
    const std::size_t stride = std::max<std::size_t>(1, points.size() / maxPoints);
    bool first = true;
    for (std::size_t i = 0; i < points.size(); i += stride) {
      if (!first) oss << ",";
      first = false;
      oss << "{\"hz\":" << points[i].frequencyHz << ",\"aDb\":" << points[i].ratioDbA << ",\"bDb\":"
          << points[i].ratioDbB << ",\"deltaDb\":" << points[i].deltaDb << ",\"widthPercentA\":"
          << points[i].widthPercentA << ",\"widthPercentB\":" << points[i].widthPercentB
          << ",\"deltaWidthPercent\":" << points[i].deltaWidthPercent << "}";
    }
  }
  oss << "]";
}

struct LlmMetric {
  std::string name;
  double value = 0.0;
  std::string unit;
};

struct LlmRegion {
  std::string label;
  std::string reason;
  double value = 0.0;
  std::string unit;
};

struct LlmTestSummary {
  std::string key;
  std::string category;
  std::string whatWasTested;
  std::string status;
  std::string differenceLevel;
  std::string headline;
  std::vector<std::string> observations;
  std::vector<LlmMetric> keyMetrics;
  std::vector<LlmRegion> notableRegions;
  std::vector<std::string> warnings;
};

struct LlmOverallSummary {
  std::string headline;
  std::vector<std::string> dominantDifferences;
  std::string characterSummary;
  std::string dataQuality;
  std::vector<std::string> topFindings;
  std::vector<std::string> warnings;
};

struct LlmSummary {
  LlmOverallSummary overall;
  std::vector<LlmTestSummary> tests;
};

struct RankedRegion {
  double score = 0.0;
  LlmRegion region;
};

bool finiteNumber(double value) {
  return std::isfinite(value);
}

double cleanNumber(double value) {
  return finiteNumber(value) ? value : 0.0;
}

std::string trimNumber(std::string value) {
  const auto dot = value.find('.');
  if (dot == std::string::npos) return value;
  while (!value.empty() && value.back() == '0') value.pop_back();
  if (!value.empty() && value.back() == '.') value.pop_back();
  if (value == "-0") return "0";
  return value;
}

std::string formatNumber(double value) {
  value = cleanNumber(value);
  const double absValue = std::fabs(value);
  int precision = 2;
  if (absValue >= 100.0) {
    precision = 0;
  } else if (absValue >= 10.0) {
    precision = 1;
  } else if (absValue > 0.0 && absValue < 0.01) {
    precision = 4;
  }
  std::ostringstream oss;
  oss << std::fixed << std::setprecision(precision) << value;
  return trimNumber(oss.str());
}

std::string formatValue(double value, const std::string& unit) {
  const std::string number = formatNumber(value);
  return unit.empty() ? number : (number + " " + unit);
}

std::string formatHz(double hz) {
  if (std::fabs(hz) >= 1000.0) {
    return formatValue(hz / 1000.0, "kHz");
  }
  return formatValue(hz, "Hz");
}

std::string ordinal(int value) {
  const int mod100 = value % 100;
  const int mod10 = value % 10;
  std::string suffix = "th";
  if (mod100 < 11 || mod100 > 13) {
    if (mod10 == 1) suffix = "st";
    if (mod10 == 2) suffix = "nd";
    if (mod10 == 3) suffix = "rd";
  }
  return std::to_string(value) + suffix;
}

void appendUnique(std::vector<std::string>& values, const std::string& value) {
  if (value.empty()) return;
  if (std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
}

void appendWarnings(std::vector<std::string>& values, const std::vector<std::string>& warnings) {
  for (const auto& warning : warnings) appendUnique(values, warning);
}

void appendLatencyWarnings(std::vector<std::string>& values, const LatencyAlignmentInfo& info,
                           const std::string& fallback) {
  appendWarnings(values, info.warnings);
  if (info.clampedOccurred) appendUnique(values, fallback);
}

void addMetric(LlmTestSummary& summary, const std::string& name, double value, const std::string& unit) {
  summary.keyMetrics.push_back({name, cleanNumber(value), unit});
}

void addObservation(LlmTestSummary& summary, const std::string& observation) {
  appendUnique(summary.observations, observation);
}

std::string directionPhrase(double value, const std::string& positive, const std::string& negative,
                            const std::string& neutral = "matches") {
  value = cleanNumber(value);
  if (std::fabs(value) < 1.0e-9) return neutral;
  return value > 0.0 ? positive : negative;
}

std::string pluginADirection(double value, const std::string& positive, const std::string& negative) {
  return "Plugin A is " + directionPhrase(value, positive, negative) + " than Plugin B";
}

std::string signedDirectionSentence(double value, const std::string& positive, const std::string& negative,
                                    const std::string& unit) {
  return pluginADirection(value, positive, negative) + " by " + formatValue(std::fabs(cleanNumber(value)), unit);
}

std::string levelForScore(double score, double small, double moderate, double large) {
  score = std::fabs(cleanNumber(score));
  if (score < small) return "none";
  if (score < moderate) return "small";
  if (score < large) return "moderate";
  return "large";
}

std::string levelForIr(double peakAbsDelta, double energyDelta, double latencyMs) {
  const double peak = std::fabs(cleanNumber(peakAbsDelta));
  const double energy = std::fabs(cleanNumber(energyDelta));
  const double latency = std::fabs(cleanNumber(latencyMs));
  if (peak < 0.001 && energy < 0.000001 && latency < 0.05) return "none";
  if (peak < 0.01 && energy < 0.0001 && latency < 0.2) return "small";
  if (peak < 0.1 && energy < 0.01 && latency < 1.0) return "moderate";
  return "large";
}

int scoreForLevel(const std::string& level) {
  if (level == "large") return 3;
  if (level == "moderate") return 2;
  if (level == "small") return 1;
  return 0;
}

std::string statusFrom(bool insufficient, const std::vector<std::string>& warnings) {
  if (insufficient) return "insufficient_data";
  return warnings.empty() ? "ok" : "warning";
}

void keepTopRegions(std::vector<LlmRegion>& target, std::vector<RankedRegion> ranked, std::size_t maxCount) {
  std::sort(ranked.begin(), ranked.end(), [](const RankedRegion& a, const RankedRegion& b) {
    return a.score > b.score;
  });
  for (const auto& item : ranked) {
    if (target.size() >= maxCount) break;
    if (item.score <= 0.0) continue;
    target.push_back(item.region);
  }
}

std::string categoryLabel(const std::string& category) {
  if (category == "tonal") return "tonal balance";
  if (category == "phase-time") return "phase and latency behavior";
  if (category == "transient") return "transient response";
  if (category == "harmonic-distortion") return "harmonic distortion";
  if (category == "stereo-width") return "stereo width";
  if (category == "pitch") return "pitch tracking";
  if (category == "dynamics") return "dynamics behavior";
  if (category == "envelope") return "envelope timing";
  return category;
}

std::string strongestCoarseBand(const std::vector<OctaveBandSummary>& bands, double* signedAverageOut) {
  struct Bucket {
    const char* name;
    double absSum = 0.0;
    double signedSum = 0.0;
    int count = 0;
  };
  Bucket low{"low"}, mid{"mid"}, high{"high"};
  for (const auto& band : bands) {
    Bucket* bucket = nullptr;
    if (band.centerHz >= 20.0 && band.centerHz < 250.0) {
      bucket = &low;
    } else if (band.centerHz >= 250.0 && band.centerHz < 4000.0) {
      bucket = &mid;
    } else if (band.centerHz >= 4000.0 && band.centerHz <= 20000.0) {
      bucket = &high;
    }
    if (!bucket) continue;
    bucket->absSum += std::fabs(cleanNumber(band.avgDeltaDb));
    bucket->signedSum += cleanNumber(band.avgDeltaDb);
    bucket->count += 1;
  }

  Bucket* best = nullptr;
  for (Bucket* bucket : {&low, &mid, &high}) {
    if (bucket->count == 0) continue;
    if (!best || (bucket->absSum / bucket->count) > (best->absSum / best->count)) best = bucket;
  }
  if (!best) {
    if (signedAverageOut) *signedAverageOut = 0.0;
    return "";
  }
  if (signedAverageOut) *signedAverageOut = best->signedSum / best->count;
  return best->name;
}

LlmTestSummary buildIrLlmSummary(const RunSummary& summary) {
  const auto& result = summary.irResult;
  LlmTestSummary out;
  out.key = "ir";
  out.category = "transient";
  out.whatWasTested =
      "A single-sample impulse was passed through both plugins and compared after latency alignment.";
  appendLatencyWarnings(out.warnings, result.latencyAlignment,
                        "IR latency alignment clamped reported latency to the available sample window.");
  out.status = statusFrom(summary.config.sampleRate <= 0, out.warnings);
  out.differenceLevel =
      levelForIr(result.metrics.peakAbsDelta, result.metrics.energyDelta, result.metrics.estimatedLatencyMs);
  out.headline = "IR peak delta is " + formatNumber(result.metrics.peakAbsDelta) + ", energy delta is " +
                 formatNumber(result.metrics.energyDelta) + ", and " +
                 signedDirectionSentence(result.metrics.estimatedLatencyMs, "later", "earlier", "ms") +
                 " after latency estimation.";

  addMetric(out, "peakAbsDelta", result.metrics.peakAbsDelta, "");
  addMetric(out, "energyDelta", result.metrics.energyDelta, "");
  addMetric(out, "estimatedLatencyMs", result.metrics.estimatedLatencyMs, "ms");
  addObservation(out, "Peak |A-B| is " + formatNumber(result.metrics.peakAbsDelta) + ".");
  addObservation(out, "Energy(A-B) is " + formatNumber(result.metrics.energyDelta) + ".");
  addObservation(out, signedDirectionSentence(result.metrics.estimatedLatencyMs, "later", "earlier", "ms") +
                          " in estimated residual latency.");

  std::vector<RankedRegion> ranked;
  for (const auto& diff : result.topDifferences) {
    const double ms = summary.config.sampleRate > 0
                          ? (1000.0 * static_cast<double>(diff.sampleIndex) /
                             static_cast<double>(summary.config.sampleRate))
                          : 0.0;
    ranked.push_back({std::fabs(diff.absoluteDelta),
                      {formatValue(ms, "ms") + " (sample " + std::to_string(diff.sampleIndex) + ")",
                       "largest absolute impulse mismatch at this time index", diff.absoluteDelta, ""}});
  }
  keepTopRegions(out.notableRegions, ranked, 3);
  return out;
}

LlmTestSummary buildFrequencyLlmSummary(const RunSummary& summary) {
  const auto& result = summary.frequencyResult;
  LlmTestSummary out;
  out.key = "frequency";
  out.category = "tonal";
  out.whatWasTested =
      "White-noise frequency response was measured as the normalized output spectrum relative to the input spectrum.";
  appendLatencyWarnings(out.warnings, result.latencyAlignment,
                        "Frequency response latency alignment clamped reported latency to the available sample window.");
  const bool insufficient = result.octaveBands.empty() && result.normalizedSpectrumA.empty() &&
                            result.normalizedSpectrumB.empty() && result.delta.empty();
  out.status = statusFrom(insufficient, out.warnings);
  out.differenceLevel = levelForScore(result.metrics.meanAbsDeltaDb, 0.5, 2.0, 6.0);
  double signedBandAverage = 0.0;
  const std::string coarseBand = strongestCoarseBand(result.octaveBands, &signedBandAverage);
  out.headline = "Frequency response mean |delta| is " +
                 formatValue(result.metrics.meanAbsDeltaDb, "dB") + " with a peak |delta| of " +
                 formatValue(result.metrics.peakAbsDeltaDb, "dB") +
                 (coarseBand.empty() ? "." : "; " + signedDirectionSentence(signedBandAverage, "higher", "lower", "dB") +
                                                " in the " + coarseBand + " range.");

  addMetric(out, "meanAbsDeltaDb", result.metrics.meanAbsDeltaDb, "dB");
  addMetric(out, "peakAbsDeltaDb", result.metrics.peakAbsDeltaDb, "dB");
  addMetric(out, "estimatedLatencyMs", result.metrics.estimatedLatencyMs, "ms");
  addObservation(out, "Average tonal difference is " + formatValue(result.metrics.meanAbsDeltaDb, "dB") + ".");
  if (!coarseBand.empty()) {
    addObservation(out, "The strongest coarse-band tonal difference is in the " + coarseBand + " range, where " +
                            signedDirectionSentence(signedBandAverage, "higher", "lower", "dB") + ".");
  }

  std::vector<RankedRegion> ranked;
  for (const auto& band : result.octaveBands) {
    const double avg = cleanNumber(band.avgDeltaDb);
    const double maxDelta = cleanNumber(band.maxDeltaDb);
    const double value = std::fabs(maxDelta) >= std::fabs(avg) ? maxDelta : avg;
    ranked.push_back({std::max(std::fabs(avg), std::fabs(maxDelta)),
                      {formatHz(band.centerHz) + " band",
                       pluginADirection(value, "higher", "lower") + " in this frequency band", value, "dB"}});
  }
  keepTopRegions(out.notableRegions, ranked, 3);
  return out;
}

LlmTestSummary buildPhaseLlmSummary(const RunSummary& summary) {
  const auto& result = summary.phaseResult;
  LlmTestSummary out;
  out.key = "phase";
  out.category = "phase-time";
  out.whatWasTested = "White-noise transfer phase was compared over 20 Hz to 20 kHz after latency alignment.";
  appendLatencyWarnings(out.warnings, result.latencyAlignment,
                        "Phase response latency alignment clamped reported latency to the available sample window.");
  const bool insufficient = result.bands.empty() && result.phaseA.empty() && result.phaseB.empty() && result.delta.empty();
  out.status = statusFrom(insufficient, out.warnings);
  out.differenceLevel = levelForScore(result.metrics.meanAbsDeltaRad, 0.1, 0.5, 1.0);
  const double meanDeg = result.metrics.meanAbsDeltaRad * 180.0 / kPi;
  const double peakDeg = result.metrics.peakAbsDeltaRad * 180.0 / kPi;
  double signedPhaseSum = 0.0;
  for (const auto& band : result.bands) signedPhaseSum += cleanNumber(band.avgDeltaRad);
  const double signedPhaseAverage =
      result.bands.empty() ? 0.0 : signedPhaseSum / static_cast<double>(result.bands.size());
  out.headline = "Phase mean |delta| is " + formatValue(result.metrics.meanAbsDeltaRad, "rad") + " (" +
                 formatValue(meanDeg, "deg") + "), with a peak of " +
                 formatValue(result.metrics.peakAbsDeltaRad, "rad") + " (" + formatValue(peakDeg, "deg") +
                 "); " + signedDirectionSentence(signedPhaseAverage, "more positive in phase", "more negative in phase", "rad") +
                 " on average across phase bands.";

  addMetric(out, "meanAbsDeltaRad", result.metrics.meanAbsDeltaRad, "rad");
  addMetric(out, "peakAbsDeltaRad", result.metrics.peakAbsDeltaRad, "rad");
  addMetric(out, "estimatedLatencyMs", result.metrics.estimatedLatencyMs, "ms");
  addObservation(out, "Average phase difference is " + formatValue(result.metrics.meanAbsDeltaRad, "rad") + " (" +
                          formatValue(meanDeg, "deg") + ").");
  addObservation(out, "Peak phase difference is " + formatValue(result.metrics.peakAbsDeltaRad, "rad") + " (" +
                          formatValue(peakDeg, "deg") + ").");
  if (!result.bands.empty()) {
    addObservation(out, signedDirectionSentence(signedPhaseAverage, "more positive in phase", "more negative in phase",
                                                "rad") +
                            " on average across phase bands.");
  }

  std::vector<RankedRegion> ranked;
  for (const auto& band : result.bands) {
    const double avg = cleanNumber(band.avgDeltaRad);
    const double maxDelta = cleanNumber(band.maxDeltaRad);
    const double value = std::fabs(maxDelta) >= std::fabs(avg) ? maxDelta : avg;
    ranked.push_back({std::max(std::fabs(avg), std::fabs(maxDelta)),
                      {formatHz(band.centerHz) + " band",
                       pluginADirection(value, "more positive in phase", "more negative in phase") +
                           " in this phase band",
                       value, "rad"}});
  }
  keepTopRegions(out.notableRegions, ranked, 3);
  return out;
}

LlmTestSummary buildHarmonicLlmSummary(const RunSummary& summary) {
  const auto& result = summary.harmonicResult;
  LlmTestSummary out;
  out.key = "harmonic";
  out.category = "harmonic-distortion";
  out.whatWasTested = "Fixed sine tones were analyzed for harmonic order levels and noise-floor differences.";
  out.differenceLevel = levelForScore(result.metrics.meanAbsDeltaDb, 0.5, 2.0, 6.0);
  out.headline = "Harmonic distortion mean |delta| is " + formatValue(result.metrics.meanAbsDeltaDb, "dB") +
                 " with a peak |delta| of " + formatValue(result.metrics.peakAbsDeltaDb, "dB") + ".";

  addMetric(out, "meanAbsDeltaDb", result.metrics.meanAbsDeltaDb, "dB");
  addMetric(out, "peakAbsDeltaDb", result.metrics.peakAbsDeltaDb, "dB");
  addObservation(out, "Across tested fundamentals, mean harmonic delta is " +
                          formatValue(result.metrics.meanAbsDeltaDb, "dB") + ".");

  bool hasOrders = false;
  double evenSum = 0.0;
  double oddSum = 0.0;
  double highSum = 0.0;
  double lowOrderSum = 0.0;
  double signedOrderSum = 0.0;
  int signedOrderCount = 0;
  double largestPitchScore = 0.0;
  double largestPitchHz = 0.0;
  double largestPitchSignedDelta = 0.0;
  double largestNoiseFloorDelta = 0.0;
  std::vector<RankedRegion> ranked;
  for (const auto& pitch : result.pitches) {
    appendLatencyWarnings(out.warnings, pitch.latencyAlignment,
                          "Harmonic latency alignment clamped reported latency to the available sample window.");
    largestNoiseFloorDelta =
        std::max(largestNoiseFloorDelta, std::fabs(cleanNumber(pitch.noiseFloorDeltaDb)));
    double pitchScore = 0.0;
    for (const auto& order : pitch.orders) {
      hasOrders = true;
      const double absDelta = std::fabs(cleanNumber(order.deltaDb));
      signedOrderSum += cleanNumber(order.deltaDb);
      signedOrderCount += 1;
      pitchScore = std::max(pitchScore, absDelta);
      if (order.order >= 2 && order.order <= 10) {
        if (order.order % 2 == 0) {
          evenSum += absDelta;
        } else {
          oddSum += absDelta;
        }
        if (order.order >= 6) {
          highSum += absDelta;
        } else {
          lowOrderSum += absDelta;
        }
      }
      ranked.push_back({absDelta,
                        {formatHz(pitch.fundamentalHz) + " fundamental, " + ordinal(order.order) + " harmonic",
                         pluginADirection(order.deltaDb, "stronger", "weaker") +
                             " for this harmonic order",
                         order.deltaDb, "dB"}});
    }
    if (pitchScore > largestPitchScore) {
      largestPitchScore = pitchScore;
      largestPitchHz = pitch.fundamentalHz;
      for (const auto& order : pitch.orders) {
        if (std::fabs(cleanNumber(order.deltaDb)) == pitchScore) {
          largestPitchSignedDelta = order.deltaDb;
          break;
        }
      }
    }
  }

  if (largestPitchScore > 0.0) {
    addObservation(out, "The largest harmonic-order difference appears around the " + formatHz(largestPitchHz) +
                            " fundamental, where " +
                            signedDirectionSentence(largestPitchSignedDelta, "stronger", "weaker", "dB") + ".");
  }
  if (signedOrderCount > 0) {
    out.headline += " Across harmonic orders, " +
                    signedDirectionSentence(signedOrderSum / static_cast<double>(signedOrderCount), "stronger",
                                            "weaker", "dB") +
                    " on average.";
  }
  if (evenSum > oddSum * 1.15) {
    addObservation(out, "Even-order harmonics carry more aggregate delta than odd-order harmonics.");
  } else if (oddSum > evenSum * 1.15) {
    addObservation(out, "Odd-order harmonics carry more aggregate delta than even-order harmonics.");
  } else if (evenSum + oddSum > 0.0) {
    addObservation(out, "Even- and odd-order harmonic deltas are broadly balanced.");
  }
  if (highSum > lowOrderSum && highSum > 0.0) {
    addObservation(out, "Higher harmonics from the 6th order upward carry more aggregate delta than lower orders.");
  }
  addObservation(out, "Largest measured noise-floor delta is " + formatValue(largestNoiseFloorDelta, "dB") + ".");

  out.status = statusFrom(result.pitches.empty() || !hasOrders, out.warnings);
  keepTopRegions(out.notableRegions, ranked, 3);
  return out;
}

LlmTestSummary buildThdLlmSummary(const RunSummary& summary) {
  const auto& result = summary.thdResult;
  LlmTestSummary out;
  out.key = "thd";
  out.category = "harmonic-distortion";
  out.whatWasTested = "A 1 kHz sine sweep was measured for THD and THD+N across input levels.";
  const double score = std::max(std::fabs(result.metrics.meanAbsThdDeltaPercent),
                                std::fabs(result.metrics.meanAbsThdnDeltaPercent));
  out.differenceLevel = levelForScore(score, 0.01, 0.1, 1.0);
  double signedThdSum = 0.0;
  double signedThdnSum = 0.0;
  int signedSweepCount = 0;
  for (const auto& point : result.sweep) {
    signedThdSum += cleanNumber(point.thdDeltaPercent);
    signedThdnSum += cleanNumber(point.thdnDeltaPercent);
    signedSweepCount += 1;
  }
  double signedThdDirection = 0.0;
  double signedThdnDirection = 0.0;
  if (signedSweepCount > 0) {
    signedThdDirection = signedThdSum / static_cast<double>(signedSweepCount);
    signedThdnDirection = signedThdnSum / static_cast<double>(signedSweepCount);
  } else if (!result.segments.empty()) {
    for (const auto& segment : result.segments) {
      signedThdDirection += cleanNumber(segment.avgThdDeltaPercent);
      signedThdnDirection += cleanNumber(segment.avgThdnDeltaPercent);
    }
    signedThdDirection /= static_cast<double>(result.segments.size());
    signedThdnDirection /= static_cast<double>(result.segments.size());
  }
  out.headline = "THD mean |delta| is " + formatValue(result.metrics.meanAbsThdDeltaPercent, "pt") +
                 " and THD+N mean |delta| is " + formatValue(result.metrics.meanAbsThdnDeltaPercent, "pt") +
                 "; " + signedDirectionSentence(signedThdDirection, "higher in THD", "lower in THD", "pt") +
                 " and " + signedDirectionSentence(signedThdnDirection, "higher in THD+N", "lower in THD+N", "pt") +
                 ".";

  addMetric(out, "meanAbsThdDeltaPercent", result.metrics.meanAbsThdDeltaPercent, "pt");
  addMetric(out, "meanAbsThdnDeltaPercent", result.metrics.meanAbsThdnDeltaPercent, "pt");
  addMetric(out, "peakAbsThdDeltaPercent", result.metrics.peakAbsThdDeltaPercent, "pt");
  addMetric(out, "peakAbsThdnDeltaPercent", result.metrics.peakAbsThdnDeltaPercent, "pt");
  addObservation(out, std::fabs(result.metrics.meanAbsThdnDeltaPercent) >=
                              std::fabs(result.metrics.meanAbsThdDeltaPercent)
                          ? "THD+N delta is the larger average distortion difference."
                          : "THD delta is the larger average distortion difference.");
  addObservation(out, signedDirectionSentence(signedThdDirection, "higher in THD", "lower in THD", "pt") +
                          " on average.");
  addObservation(out, signedDirectionSentence(signedThdnDirection, "higher in THD+N", "lower in THD+N", "pt") +
                          " on average.");

  std::vector<RankedRegion> ranked;
  for (const auto& segment : result.segments) {
    const double thd = std::fabs(cleanNumber(segment.maxAbsThdDeltaPercent));
    const double thdn = std::fabs(cleanNumber(segment.maxAbsThdnDeltaPercent));
    const bool useThdn = std::fabs(cleanNumber(segment.avgThdnDeltaPercent)) >=
                         std::fabs(cleanNumber(segment.avgThdDeltaPercent));
    const double signedValue = useThdn ? segment.avgThdnDeltaPercent : segment.avgThdDeltaPercent;
    ranked.push_back({std::max(thd, thdn),
                      {segment.name + " input segment",
                       pluginADirection(signedValue, useThdn ? "higher in THD+N" : "higher in THD",
                                        useThdn ? "lower in THD+N" : "lower in THD") +
                           " in this input segment",
                       signedValue, "pt"}});
  }
  if (!ranked.empty()) {
    std::sort(ranked.begin(), ranked.end(), [](const RankedRegion& a, const RankedRegion& b) {
      return a.score > b.score;
    });
    addObservation(out, "The largest segment-level distortion delta is in the " + ranked.front().region.label + ".");
  }

  for (const auto& point : result.sweep) {
    appendWarnings(out.warnings, point.warnings);
    appendLatencyWarnings(out.warnings, point.latencyAlignment,
                          "THD sweep latency alignment clamped reported latency to the available sample window.");
    const bool useThdn = std::fabs(cleanNumber(point.thdnDeltaPercent)) >= std::fabs(cleanNumber(point.thdDeltaPercent));
    const double signedValue = useThdn ? point.thdnDeltaPercent : point.thdDeltaPercent;
    ranked.push_back({std::fabs(signedValue),
                      {formatValue(point.inputLevelDbfs, "dBFS") + " sweep point",
                       pluginADirection(signedValue, useThdn ? "higher in THD+N" : "higher in THD",
                                        useThdn ? "lower in THD+N" : "lower in THD") +
                           " at this input level",
                       signedValue, "pt"}});
  }
  out.status = statusFrom(result.sweep.empty() && result.segments.empty(), out.warnings);
  keepTopRegions(out.notableRegions, ranked, 3);
  return out;
}

LlmTestSummary buildWidthLlmSummary(const RunSummary& summary) {
  const auto& result = summary.monoStereoWidthResult;
  LlmTestSummary out;
  out.key = "monoStereoWidth";
  out.category = "stereo-width";
  out.whatWasTested =
      "Identical stereo pink noise was measured as mid/side width over time and frequency bands.";
  appendWarnings(out.warnings, result.warnings);
  appendLatencyWarnings(out.warnings, result.latencyAlignment,
                        "Mono-to-stereo width latency alignment clamped reported latency to the available sample window.");
  const double score = std::max(std::fabs(result.metrics.meanAbsBandDeltaWidthPercent),
                                std::fabs(result.metrics.meanAbsTimeDeltaWidthPercent));
  out.differenceLevel = levelForScore(score, 2.0, 10.0, 25.0);
  double signedTimeSum = 0.0;
  for (const auto& point : result.timeSeries) signedTimeSum += cleanNumber(point.deltaWidthPercent);
  const double signedTimeAverage =
      result.timeSeries.empty() ? 0.0 : signedTimeSum / static_cast<double>(result.timeSeries.size());
  double signedBandSum = 0.0;
  for (const auto& band : result.bands) signedBandSum += cleanNumber(band.deltaWidthPercent);
  const double signedBandAverage =
      result.bands.empty() ? 0.0 : signedBandSum / static_cast<double>(result.bands.size());
  const double signedWidthHeadline = result.bands.empty() ? signedTimeAverage : signedBandAverage;
  out.headline = "Width mean delta is " + formatValue(result.metrics.meanAbsTimeDeltaWidthPercent, "pt") +
                 " over time and " + formatValue(result.metrics.meanAbsBandDeltaWidthPercent, "pt") +
                 " across bands; " + signedDirectionSentence(signedWidthHeadline, "wider", "narrower", "pt") +
                 (result.bands.empty() ? " on average over time." : " on average across bands.");

  addMetric(out, "meanAbsTimeDeltaWidthPercent", result.metrics.meanAbsTimeDeltaWidthPercent, "pt");
  addMetric(out, "peakAbsTimeDeltaWidthPercent", result.metrics.peakAbsTimeDeltaWidthPercent, "pt");
  addMetric(out, "meanAbsBandDeltaWidthPercent", result.metrics.meanAbsBandDeltaWidthPercent, "pt");
  addMetric(out, "peakAbsBandDeltaWidthPercent", result.metrics.peakAbsBandDeltaWidthPercent, "pt");
  addObservation(out, "Time-domain width mean |delta| is " +
                          formatValue(result.metrics.meanAbsTimeDeltaWidthPercent, "pt") + "; " +
                          signedDirectionSentence(signedTimeAverage, "wider", "narrower", "pt") + " over time.");
  addObservation(out, "Band width mean |delta| is " +
                          formatValue(result.metrics.meanAbsBandDeltaWidthPercent, "pt") + "; " +
                          signedDirectionSentence(signedBandAverage, "wider", "narrower", "pt") + " across bands.");

  if (!result.bands.empty()) {
    addObservation(out, "Across bands, " + signedDirectionSentence(signedBandAverage, "wider", "narrower", "pt") +
                            ".");
  }

  std::vector<RankedRegion> ranked;
  for (const auto& band : result.bands) {
    ranked.push_back({std::fabs(cleanNumber(band.deltaWidthPercent)),
                      {formatHz(band.centerHz) + " band", "largest band width delta",
                       band.deltaWidthPercent, "pt"}});
    ranked.back().region.reason =
        pluginADirection(band.deltaWidthPercent, "wider", "narrower") + " in this width band";
  }
  for (const auto& point : result.timeSeries) {
    ranked.push_back({std::fabs(cleanNumber(point.deltaWidthPercent)),
                      {formatValue(point.timeMs, "ms"), "largest time-domain width delta",
                       point.deltaWidthPercent, "pt"}});
    ranked.back().region.reason =
        pluginADirection(point.deltaWidthPercent, "wider", "narrower") + " at this time point";
  }
  out.status = statusFrom(result.bands.empty() && result.timeSeries.empty(), out.warnings);
  keepTopRegions(out.notableRegions, ranked, 3);
  return out;
}

LlmTestSummary buildPitchLlmSummary(const RunSummary& summary) {
  const auto& result = summary.pitchResult;
  LlmTestSummary out;
  out.key = "pitch";
  out.category = "pitch";
  out.whatWasTested = "A logarithmic sine sweep was analyzed for input tracking accuracy and A/B pitch divergence.";
  appendWarnings(out.warnings, result.warnings);
  appendLatencyWarnings(out.warnings, result.latencyAlignment,
                        "Pitch latency alignment clamped reported latency to the available sample window.");
  if (result.metrics.validFrameRateA < 0.5 || result.metrics.validFrameRateB < 0.5) {
    appendUnique(out.warnings, "Pitch tracking valid frame rate is below 0.5 for at least one plugin.");
  }
  out.status = statusFrom(result.curve.empty(), out.warnings);
  out.differenceLevel = levelForScore(result.metrics.meanAbsDeltaHz, 2.0, 10.0, 50.0);
  double signedPitchSum = 0.0;
  for (const auto& point : result.curve) signedPitchSum += cleanNumber(point.pitchHzA - point.pitchHzB);
  const double signedPitchAverage =
      result.curve.empty() ? 0.0 : signedPitchSum / static_cast<double>(result.curve.size());
  const double trackingErrorDelta = cleanNumber(result.metrics.meanAbsErrorHzA - result.metrics.meanAbsErrorHzB);
  out.headline = "Pitch A-B mean |delta| is " + formatValue(result.metrics.meanAbsDeltaHz, "Hz") +
                 " with a peak |delta| of " + formatValue(result.metrics.peakAbsDeltaHz, "Hz") +
                 "; " + signedDirectionSentence(signedPitchAverage, "sharper", "flatter", "Hz") +
                 " on average versus Plugin B" +
                 "; valid frame rates are A " + formatNumber(result.metrics.validFrameRateA) + " and B " +
                 formatNumber(result.metrics.validFrameRateB) + ".";

  addMetric(out, "meanAbsErrorHzA", result.metrics.meanAbsErrorHzA, "Hz");
  addMetric(out, "meanAbsErrorHzB", result.metrics.meanAbsErrorHzB, "Hz");
  addMetric(out, "meanAbsDeltaHz", result.metrics.meanAbsDeltaHz, "Hz");
  addMetric(out, "peakAbsDeltaHz", result.metrics.peakAbsDeltaHz, "Hz");
  addMetric(out, "validFrameRateA", result.metrics.validFrameRateA, "");
  addMetric(out, "validFrameRateB", result.metrics.validFrameRateB, "");
  addObservation(out, "Plugin A mean tracking error is " + formatValue(result.metrics.meanAbsErrorHzA, "Hz") +
                          "; Plugin B mean tracking error is " +
                          formatValue(result.metrics.meanAbsErrorHzB, "Hz") + ".");
  addObservation(out, signedDirectionSentence(trackingErrorDelta, "less accurate relative to the input", "more accurate relative to the input",
                                              "Hz") +
                          " in mean tracking error.");
  addObservation(out, "A/B pitch divergence averages " + formatValue(result.metrics.meanAbsDeltaHz, "Hz") +
                          " and peaks at " + formatValue(result.metrics.peakAbsDeltaHz, "Hz") + ".");
  if (!result.curve.empty()) {
    addObservation(out, signedDirectionSentence(signedPitchAverage, "sharper", "flatter", "Hz") +
                            " on average versus Plugin B.");
  }

  std::vector<RankedRegion> ranked;
  for (const auto& point : result.curve) {
    const double delta = cleanNumber(point.pitchHzA - point.pitchHzB);
    ranked.push_back({std::fabs(delta),
                      {formatValue(point.timeMs, "ms") + ", input " + formatHz(point.inputHz),
                       pluginADirection(delta, "sharper", "flatter") + " at this sweep point", delta, "Hz"}});
  }
  keepTopRegions(out.notableRegions, ranked, 3);
  return out;
}

LlmTestSummary buildDynamicsLlmSummary(const RunSummary& summary) {
  const auto& result = summary.dynamicsResult;
  LlmTestSummary out;
  out.key = "dynamics";
  out.category = "dynamics";
  out.whatWasTested = "A stepped 1 kHz sine was measured as RMS I/O and gain-reduction curves.";
  appendWarnings(out.warnings, result.warnings);
  appendLatencyWarnings(out.warnings, result.latencyAlignment,
                        "Dynamics latency alignment clamped reported latency to the available sample window.");
  if (!result.metrics.estimatedA.valid || !result.metrics.estimatedB.valid) {
    appendUnique(out.warnings, "Dynamics parameter estimate is incomplete for one or both plugins.");
  }
  const double score = std::max(std::fabs(result.metrics.meanAbsOutputDeltaDb),
                                std::fabs(result.metrics.meanAbsGainReductionDeltaDb));
  out.status = statusFrom(result.ioCurve.empty(), out.warnings);
  out.differenceLevel = levelForScore(score, 0.5, 2.0, 6.0);
  double signedOutputSum = 0.0;
  double signedGrSum = 0.0;
  for (const auto& point : result.ioCurve) {
    signedOutputSum += cleanNumber(point.outputDeltaDb);
    signedGrSum += cleanNumber(point.gainReductionDeltaDb);
  }
  const double signedOutputAverage =
      result.ioCurve.empty() ? 0.0 : signedOutputSum / static_cast<double>(result.ioCurve.size());
  const double signedGrAverage =
      result.ioCurve.empty() ? 0.0 : signedGrSum / static_cast<double>(result.ioCurve.size());
  out.headline = "Dynamics mean output delta is " + formatValue(result.metrics.meanAbsOutputDeltaDb, "dB") +
                 " and mean gain-reduction delta is " +
                 formatValue(result.metrics.meanAbsGainReductionDeltaDb, "dB") + "; " +
                 signedDirectionSentence(result.metrics.thresholdDeltaDb, "higher in threshold", "lower in threshold", "dB") +
                 " and " + signedDirectionSentence(signedGrAverage, "stronger in gain reduction", "weaker in gain reduction",
                                                    "dB") +
                 " across sampled levels.";

  addMetric(out, "meanAbsOutputDeltaDb", result.metrics.meanAbsOutputDeltaDb, "dB");
  addMetric(out, "peakAbsOutputDeltaDb", result.metrics.peakAbsOutputDeltaDb, "dB");
  addMetric(out, "meanAbsGainReductionDeltaDb", result.metrics.meanAbsGainReductionDeltaDb, "dB");
  addMetric(out, "peakAbsGainReductionDeltaDb", result.metrics.peakAbsGainReductionDeltaDb, "dB");
  addMetric(out, "thresholdDeltaDb", result.metrics.thresholdDeltaDb, "dB");
  addMetric(out, "ratioDelta", result.metrics.ratioDelta, "");
  addMetric(out, "kneeWidthDeltaDb", result.metrics.kneeWidthDeltaDb, "dB");
  addObservation(out, "Estimated threshold: " +
                          signedDirectionSentence(result.metrics.thresholdDeltaDb, "higher", "lower", "dB") + ".");
  addObservation(out, "Estimated ratio: " +
                          signedDirectionSentence(result.metrics.ratioDelta, "higher", "lower", "") + ".");
  addObservation(out, "Estimated knee width: " +
                          signedDirectionSentence(result.metrics.kneeWidthDeltaDb, "wider", "narrower", "dB") + ".");
  addObservation(out, "Average output curve: " +
                          signedDirectionSentence(signedOutputAverage, "higher", "lower", "dB") + ".");

  if (!result.ioCurve.empty()) {
    addObservation(out, "Across sampled levels, " +
                            signedDirectionSentence(signedGrAverage, "stronger in gain reduction",
                                                    "weaker in gain reduction", "dB") + ".");
  }

  std::vector<RankedRegion> ranked;
  for (const auto& point : result.ioCurve) {
    const double outDelta = cleanNumber(point.outputDeltaDb);
    const double grDelta = cleanNumber(point.gainReductionDeltaDb);
    const bool useGr = std::fabs(grDelta) >= std::fabs(outDelta);
    ranked.push_back({std::max(std::fabs(outDelta), std::fabs(grDelta)),
                      {formatValue(point.inputLevelDbfs, "dBFS"),
                       useGr ? pluginADirection(grDelta, "stronger in gain reduction", "weaker in gain reduction") +
                                   " at this input level"
                             : pluginADirection(outDelta, "higher in output level", "lower in output level") +
                                   " at this input level",
                       useGr ? grDelta : outDelta, "dB"}});
  }
  keepTopRegions(out.notableRegions, ranked, 3);
  return out;
}

LlmTestSummary buildTimeResponseLlmSummary(const RunSummary& summary) {
  const auto& result = summary.timeResponseResult;
  LlmTestSummary out;
  out.key = "timeResponse";
  out.category = "envelope";
  out.whatWasTested = "A tone burst was measured for envelope timing, release behavior, and residual latency.";
  appendWarnings(out.warnings, result.warnings);
  appendLatencyWarnings(out.warnings, result.latencyAlignment,
                        "Time response latency alignment clamped reported latency to the available sample window.");
  const double score = std::max(std::fabs(result.metrics.attackDeltaMs),
                                std::max(std::fabs(result.metrics.releaseDeltaMs),
                                         std::fabs(result.metrics.residualLatencyMs)));
  out.status = statusFrom(result.curve.empty(), out.warnings);
  out.differenceLevel = levelForScore(score, 2.0, 10.0, 50.0);
  out.headline = "Time response attack delta is " + formatValue(result.metrics.attackDeltaMs, "ms") +
                 ", release delta is " + formatValue(result.metrics.releaseDeltaMs, "ms") +
                 ", and residual latency is " + formatValue(result.metrics.residualLatencyMs, "ms") +
                 "; " + signedDirectionSentence(result.metrics.attackDeltaMs, "slower in attack", "faster in attack", "ms") +
                 " and " + signedDirectionSentence(result.metrics.releaseDeltaMs, "longer in release", "shorter in release",
                                                    "ms") +
                 ".";

  addMetric(out, "residualLatencyMs", result.metrics.residualLatencyMs, "ms");
  addMetric(out, "attackDeltaMs", result.metrics.attackDeltaMs, "ms");
  addMetric(out, "releaseDeltaMs", result.metrics.releaseDeltaMs, "ms");
  addMetric(out, "postReleaseResidualDeltaDb", result.metrics.postReleaseResidualDeltaDb, "dB");
  addObservation(out, "Attack times are A " + formatValue(result.metrics.attackMsA, "ms") + " and B " +
                          formatValue(result.metrics.attackMsB, "ms") + "; " +
                          signedDirectionSentence(result.metrics.attackDeltaMs, "slower", "faster", "ms") + ".");
  addObservation(out, "Release times are A " + formatValue(result.metrics.releaseMsA, "ms") + " and B " +
                          formatValue(result.metrics.releaseMsB, "ms") + "; " +
                          signedDirectionSentence(result.metrics.releaseDeltaMs, "longer", "shorter", "ms") + ".");
  addObservation(out, "Residual latency indicates " +
                          signedDirectionSentence(result.metrics.residualLatencyMs, "later", "earlier", "ms") + ".");
  addObservation(out, "Post-release residual indicates " +
                          signedDirectionSentence(result.metrics.postReleaseResidualDeltaDb, "higher in residual level",
                                                  "lower in residual level", "dB") +
                          ".");

  std::vector<RankedRegion> ranked;
  for (const auto& point : result.curve) {
    ranked.push_back({std::fabs(cleanNumber(point.deltaDb)),
                      {formatValue(point.timeMs, "ms"),
                       pluginADirection(point.deltaDb, "higher in envelope level", "lower in envelope level") +
                           " at this time point",
                       point.deltaDb, "dB"}});
  }
  keepTopRegions(out.notableRegions, ranked, 3);
  return out;
}

LlmOverallSummary buildOverallLlmSummary(const RunSummary& summary, const std::vector<LlmTestSummary>& tests) {
  struct RankedTest {
    int score = 0;
    std::size_t index = 0;
  };
  std::vector<RankedTest> ranked;
  ranked.reserve(tests.size());
  for (std::size_t i = 0; i < tests.size(); ++i) ranked.push_back({scoreForLevel(tests[i].differenceLevel), i});
  std::sort(ranked.begin(), ranked.end(), [](const RankedTest& a, const RankedTest& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.index < b.index;
  });

  LlmOverallSummary out;
  appendWarnings(out.warnings, summary.warnings);
  bool hasInsufficient = false;
  bool hasWarning = !summary.warnings.empty();
  for (const auto& test : tests) {
    appendWarnings(out.warnings, test.warnings);
    hasInsufficient = hasInsufficient || test.status == "insufficient_data";
    hasWarning = hasWarning || test.status == "warning" || !test.warnings.empty();
  }
  out.dataQuality = hasInsufficient ? "partial" : (hasWarning ? "warning" : "complete");

  for (const auto& item : ranked) {
    if (out.dominantDifferences.size() >= 3) break;
    if (item.score <= 0) continue;
    appendUnique(out.dominantDifferences, tests[item.index].category);
  }
  for (const auto& item : ranked) {
    if (out.topFindings.size() >= 5) break;
    appendUnique(out.topFindings, tests[item.index].headline);
  }

  if (out.dominantDifferences.empty()) {
    out.headline = "The measured tests show no material A/B difference above the none bucket.";
    out.characterSummary =
        "The plugins are broadly similar across the measured tests, with no dominant measured category.";
  } else {
    const std::string firstCategory = out.dominantDifferences.front();
    const LlmTestSummary* firstTest = nullptr;
    for (const auto& item : ranked) {
      if (tests[item.index].category == firstCategory) {
        firstTest = &tests[item.index];
        break;
      }
    }
    out.headline = "The largest measured difference is " + categoryLabel(firstCategory);
    if (firstTest) out.headline += ": " + firstTest->headline;
    out.characterSummary = "The dominant measured differences are ";
    for (std::size_t i = 0; i < out.dominantDifferences.size(); ++i) {
      if (i) out.characterSummary += (i + 1 == out.dominantDifferences.size()) ? " and " : ", ";
      out.characterSummary += categoryLabel(out.dominantDifferences[i]);
    }
    out.characterSummary += ".";
  }
  return out;
}

LlmSummary buildLlmSummary(const RunSummary& summary) {
  LlmSummary out;
  out.tests.push_back(buildIrLlmSummary(summary));
  out.tests.push_back(buildFrequencyLlmSummary(summary));
  out.tests.push_back(buildPhaseLlmSummary(summary));
  out.tests.push_back(buildHarmonicLlmSummary(summary));
  out.tests.push_back(buildThdLlmSummary(summary));
  out.tests.push_back(buildWidthLlmSummary(summary));
  out.tests.push_back(buildPitchLlmSummary(summary));
  out.tests.push_back(buildDynamicsLlmSummary(summary));
  out.tests.push_back(buildTimeResponseLlmSummary(summary));
  out.overall = buildOverallLlmSummary(summary, out.tests);
  return out;
}

void appendJsonStringArray(std::ostringstream& oss, const std::vector<std::string>& values) {
  oss << "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i) oss << ",";
    oss << "\"" << escapeJson(values[i]) << "\"";
  }
  oss << "]";
}

void appendLlmMetricsJson(std::ostringstream& oss, const std::vector<LlmMetric>& metrics) {
  oss << "[";
  for (std::size_t i = 0; i < metrics.size(); ++i) {
    if (i) oss << ",";
    oss << "{\"name\":\"" << escapeJson(metrics[i].name) << "\",\"value\":" << cleanNumber(metrics[i].value)
        << ",\"unit\":\"" << escapeJson(metrics[i].unit) << "\"}";
  }
  oss << "]";
}

void appendLlmRegionsJson(std::ostringstream& oss, const std::vector<LlmRegion>& regions) {
  oss << "[";
  for (std::size_t i = 0; i < regions.size(); ++i) {
    if (i) oss << ",";
    oss << "{\"label\":\"" << escapeJson(regions[i].label) << "\",\"reason\":\""
        << escapeJson(regions[i].reason) << "\",\"value\":" << cleanNumber(regions[i].value)
        << ",\"unit\":\"" << escapeJson(regions[i].unit) << "\"}";
  }
  oss << "]";
}

void appendLlmTestJson(std::ostringstream& oss, const LlmTestSummary& test) {
  oss << "{\"status\":\"" << escapeJson(test.status) << "\",\"differenceLevel\":\""
      << escapeJson(test.differenceLevel) << "\",\"headline\":\"" << escapeJson(test.headline)
      << "\",\"observations\":";
  appendJsonStringArray(oss, test.observations);
  oss << ",\"keyMetrics\":";
  appendLlmMetricsJson(oss, test.keyMetrics);
  oss << ",\"notableRegions\":";
  appendLlmRegionsJson(oss, test.notableRegions);
  oss << ",\"warnings\":";
  appendJsonStringArray(oss, test.warnings);
  oss << "}";
}

void appendLlmSummaryJson(std::ostringstream& oss, const LlmSummary& summary) {
  oss << "\"llmSummary\":{\"schemaVersion\":1,\"overall\":{\"headline\":\""
      << escapeJson(summary.overall.headline) << "\",\"dominantDifferences\":";
  appendJsonStringArray(oss, summary.overall.dominantDifferences);
  oss << ",\"characterSummary\":\"" << escapeJson(summary.overall.characterSummary)
      << "\",\"dataQuality\":\"" << escapeJson(summary.overall.dataQuality) << "\",\"topFindings\":";
  appendJsonStringArray(oss, summary.overall.topFindings);
  oss << ",\"warnings\":";
  appendJsonStringArray(oss, summary.overall.warnings);
  oss << "},\"tests\":{";
  for (std::size_t i = 0; i < summary.tests.size(); ++i) {
    if (i) oss << ",";
    oss << "\"" << escapeJson(summary.tests[i].key) << "\":";
    appendLlmTestJson(oss, summary.tests[i]);
  }
  oss << "}}";
}

std::string renderMetricListHtml(const std::vector<LlmMetric>& metrics) {
  if (metrics.empty()) return "<small>None</small>";
  std::ostringstream html;
  html << "<ul>";
  for (const auto& metric : metrics) {
    html << "<li><span class=\"mono\">" << escapeHtml(metric.name) << "</span>: "
         << escapeHtml(formatValue(metric.value, metric.unit)) << "</li>";
  }
  html << "</ul>";
  return html.str();
}

std::string renderRegionListHtml(const std::vector<LlmRegion>& regions) {
  if (regions.empty()) return "<small>None highlighted.</small>";
  std::ostringstream html;
  html << "<ul>";
  for (const auto& region : regions) {
    html << "<li>" << escapeHtml(region.label) << ": " << escapeHtml(region.reason) << " ("
         << escapeHtml(formatValue(region.value, region.unit)) << ")</li>";
  }
  html << "</ul>";
  return html.str();
}

std::string renderStringListHtml(const std::vector<std::string>& values, std::size_t maxCount = 0) {
  if (values.empty()) return "<small>None</small>";
  const std::size_t limit = maxCount == 0 ? values.size() : std::min(maxCount, values.size());
  std::ostringstream html;
  html << "<ul>";
  for (std::size_t i = 0; i < limit; ++i) html << "<li>" << escapeHtml(values[i]) << "</li>";
  if (limit < values.size()) html << "<li>" << (values.size() - limit) << " more warning(s).</li>";
  html << "</ul>";
  return html.str();
}

std::string joinEscapedCategories(const std::vector<std::string>& categories) {
  if (categories.empty()) return "None";
  std::string out;
  for (std::size_t i = 0; i < categories.size(); ++i) {
    if (i) out += ", ";
    out += categoryLabel(categories[i]) + " (" + categories[i] + ")";
  }
  return out;
}

std::string renderOverallSummaryHtml(const LlmOverallSummary& overall) {
  std::ostringstream html;
  html << "<div class=\"card full\"><h2>Final Summary</h2><table>"
       << "<tr><th>Overall finding</th><td>" << escapeHtml(overall.headline) << "</td></tr>"
       << "<tr><th>Dominant differences</th><td>" << escapeHtml(joinEscapedCategories(overall.dominantDifferences))
       << "</td></tr>"
       << "<tr><th>Data quality</th><td>" << escapeHtml(overall.dataQuality) << " - "
       << escapeHtml(overall.characterSummary) << "</td></tr>"
       << "</table><h3>Top findings</h3>" << renderStringListHtml(overall.topFindings, 5);
  if (!overall.warnings.empty()) {
    html << "<h3>Warnings</h3>" << renderStringListHtml(overall.warnings, 5);
  }
  html << "</div>";
  return html.str();
}

std::string renderTestSummaryHtml(const LlmTestSummary& test) {
  std::ostringstream html;
  html << "<div class=\"card full\"><h2>Summary For LLM</h2><table>"
       << "<tr><th>What was tested</th><td>" << escapeHtml(test.whatWasTested) << "</td></tr>"
       << "<tr><th>Main result</th><td>" << escapeHtml(test.headline) << "</td></tr>"
       << "<tr><th>Status</th><td>" << escapeHtml(test.status) << " / " << escapeHtml(test.differenceLevel)
       << "</td></tr>"
       << "<tr><th>Key numbers</th><td>" << renderMetricListHtml(test.keyMetrics) << "</td></tr>"
       << "<tr><th>Notable regions or points</th><td>" << renderRegionListHtml(test.notableRegions) << "</td></tr>";
  if (!test.warnings.empty()) {
    html << "<tr><th>Caveats</th><td>" << renderStringListHtml(test.warnings, 5) << "</td></tr>";
  }
  html << "</table></div>";
  return html.str();
}

}  // namespace

std::string buildOutputReportPath(const std::string& outputArg, const std::string& pluginAName,
                                  const std::string& pluginBName) {
  namespace fs = std::filesystem;

  const fs::path outPath(outputArg);
  const bool looksLikeHtml = outPath.has_extension() && outPath.extension().string() == ".html";
  if (looksLikeHtml) {
    return outPath.string();
  }

  fs::path dir = outPath;
  if (!fs::exists(dir)) {
    fs::create_directories(dir);
  }
  const std::string fileName = "test_report_" + sanitizeFilename(pluginAName) + "_vs_" +
                               sanitizeFilename(pluginBName) + ".html";
  return (dir / fileName).string();
}

std::string FinalReportBuilder::toJson(const RunSummary& summary) const {
  const LlmSummary llmSummary = buildLlmSummary(summary);
  std::ostringstream oss;
  oss << "{";
  oss << "\"plugins\":{";
  oss << "\"A\":{\"name\":\"" << escapeJson(summary.pluginA.name) << "\",\"vendor\":\""
      << escapeJson(summary.pluginA.vendor) << "\",\"uid\":\"" << escapeJson(summary.pluginA.uniqueId)
      << "\"},";
  oss << "\"B\":{\"name\":\"" << escapeJson(summary.pluginB.name) << "\",\"vendor\":\""
      << escapeJson(summary.pluginB.vendor) << "\",\"uid\":\"" << escapeJson(summary.pluginB.uniqueId)
      << "\"}";
  oss << "},";
  oss << "\"config\":{\"sampleRate\":" << summary.config.sampleRate << ",\"blockSize\":" << summary.config.blockSize
      << ",\"irDurationMs\":" << summary.config.irDurationMs << ",\"frequencyDurationMs\":"
      << summary.config.frequencyDurationMs << ",\"frequencyFftSize\":" << summary.config.frequencyFftSize
      << ",\"frequencyOverlapPercent\":" << summary.config.frequencyOverlapPercent
      << ",\"frequencyNoiseLevelDbfs\":" << summary.config.frequencyNoiseLevelDbfs
      << ",\"phaseDurationMs\":" << summary.config.phaseDurationMs << ",\"phaseFftSize\":"
      << summary.config.phaseFftSize << ",\"phaseOverlapPercent\":" << summary.config.phaseOverlapPercent
      << ",\"phaseNoiseLevelDbfs\":" << summary.config.phaseNoiseLevelDbfs
      << ",\"harmonicDurationMs\":" << summary.config.harmonicDurationMs
      << ",\"harmonicSkipHeadMs\":" << summary.config.harmonicSkipHeadMs
      << ",\"harmonicFftSize\":" << summary.config.harmonicFftSize
      << ",\"harmonicInputLevelDbfs\":" << summary.config.harmonicInputLevelDbfs
      << ",\"harmonicFrequenciesHz\":[";
  for (std::size_t i = 0; i < summary.config.harmonicFrequenciesHz.size(); ++i) {
    if (i) oss << ",";
    oss << summary.config.harmonicFrequenciesHz[i];
  }
  oss << "],\"thdToneFrequencyHz\":" << summary.config.thdToneFrequencyHz
      << ",\"thdStartLevelDbfs\":" << summary.config.thdStartLevelDbfs
      << ",\"thdEndLevelDbfs\":" << summary.config.thdEndLevelDbfs << ",\"thdStepDb\":"
      << summary.config.thdStepDb << ",\"thdDurationMs\":" << summary.config.thdDurationMs
      << ",\"thdSkipHeadMs\":" << summary.config.thdSkipHeadMs
      << ",\"thdFftSize\":" << summary.config.thdFftSize << ",\"monoStereoWidthDurationMs\":"
      << summary.config.monoStereoWidthDurationMs << ",\"monoStereoWidthNoiseLevelDbfs\":"
      << summary.config.monoStereoWidthNoiseLevelDbfs << ",\"monoStereoWidthFftSize\":"
      << summary.config.monoStereoWidthFftSize << ",\"monoStereoWidthOverlapPercent\":"
      << summary.config.monoStereoWidthOverlapPercent << ",\"monoStereoWidthTimeWindowMs\":"
      << summary.config.monoStereoWidthTimeWindowMs << ",\"monoStereoWidthTimeHopMs\":"
      << summary.config.monoStereoWidthTimeHopMs << ",\"pitchDurationMs\":" << summary.config.pitchDurationMs
      << ",\"pitchStartHz\":" << summary.config.pitchStartHz << ",\"pitchEndHz\":" << summary.config.pitchEndHz
      << ",\"pitchLevelDbfs\":" << summary.config.pitchLevelDbfs << ",\"pitchFadeMs\":"
      << summary.config.pitchFadeMs << ",\"pitchFrameMs\":" << summary.config.pitchFrameMs
      << ",\"pitchHopMs\":" << summary.config.pitchHopMs
      << ",\"dynamicsToneFrequencyHz\":" << summary.config.dynamicsToneFrequencyHz
      << ",\"dynamicsStartLevelDbfs\":" << summary.config.dynamicsStartLevelDbfs
      << ",\"dynamicsEndLevelDbfs\":" << summary.config.dynamicsEndLevelDbfs
      << ",\"dynamicsStepDb\":" << summary.config.dynamicsStepDb
      << ",\"dynamicsStepMs\":" << summary.config.dynamicsStepMs
      << ",\"dynamicsRmsWindowMs\":" << summary.config.dynamicsRmsWindowMs
      << ",\"timeBurstFrequencyHz\":" << summary.config.timeBurstFrequencyHz
      << ",\"timeBurstLevelDbfs\":" << summary.config.timeBurstLevelDbfs
      << ",\"timePreSilenceMs\":" << summary.config.timePreSilenceMs
      << ",\"timeBurstOnMs\":" << summary.config.timeBurstOnMs
      << ",\"timePostSilenceMs\":" << summary.config.timePostSilenceMs
      << ",\"timeEnvelopeWindowMs\":" << summary.config.timeEnvelopeWindowMs
      << ",\"timeCorrelationMaxLagMs\":" << summary.config.timeCorrelationMaxLagMs << "},";
  const std::vector<float> emptySignal;
  const auto& irDisplayLeftA =
      (summary.irResult.displayAlignedA.numChannels() >= 1) ? summary.irResult.displayAlignedA.channels[0] : emptySignal;
  const auto& irDisplayLeftB =
      (summary.irResult.displayAlignedB.numChannels() >= 1) ? summary.irResult.displayAlignedB.channels[0] : emptySignal;
  const auto& irDisplayLeftDelta =
      (summary.irResult.displayDelta.numChannels() >= 1) ? summary.irResult.displayDelta.channels[0] : emptySignal;
  const auto& irDisplayRightA =
      (summary.irResult.displayAlignedA.numChannels() >= 2) ? summary.irResult.displayAlignedA.channels[1] : emptySignal;
  const auto& irDisplayRightB =
      (summary.irResult.displayAlignedB.numChannels() >= 2) ? summary.irResult.displayAlignedB.channels[1] : emptySignal;
  const auto& irDisplayRightDelta =
      (summary.irResult.displayDelta.numChannels() >= 2) ? summary.irResult.displayDelta.channels[1] : emptySignal;
  const bool irDisplayHasRight = !irDisplayRightA.empty() && !irDisplayRightB.empty() && !irDisplayRightDelta.empty();

  oss << "\"ir\":{\"peakAbsDelta\":" << summary.irResult.metrics.peakAbsDelta << ",\"energyDelta\":"
      << summary.irResult.metrics.energyDelta << ",\"estimatedLatencyMs\":"
      << summary.irResult.metrics.estimatedLatencyMs << ",\"pluginLatencySamplesA\":"
      << summary.irResult.metrics.pluginLatencySamplesA << ",\"pluginLatencySamplesB\":"
      << summary.irResult.metrics.pluginLatencySamplesB << ",\"analysisSignal\":\""
      << summary.irResult.analysisSignalMode << "\",\"latencyAlignment\":{"
      << "\"reportedA\":" << summary.irResult.latencyAlignment.reportedA << ",\"reportedB\":"
      << summary.irResult.latencyAlignment.reportedB << ",\"clampedA\":"
      << summary.irResult.latencyAlignment.clampedA << ",\"clampedB\":"
      << summary.irResult.latencyAlignment.clampedB << ",\"appliedDelayA\":"
      << summary.irResult.latencyAlignment.appliedDelayA << ",\"appliedDelayB\":"
      << summary.irResult.latencyAlignment.appliedDelayB << ",\"clampedOccurred\":"
      << (summary.irResult.latencyAlignment.clampedOccurred ? "true" : "false")
      << "},\"display\":{\"analysisReference\":\"post_latency_alignment_stereo_output\",\"hasRightChannel\":"
      << (irDisplayHasRight ? "true" : "false") << ",\"leftA\":";
  appendCompactSignalJson(oss, irDisplayLeftA, 1024);
  oss << ",\"leftB\":";
  appendCompactSignalJson(oss, irDisplayLeftB, 1024);
  oss << ",\"leftDelta\":";
  appendCompactSignalJson(oss, irDisplayLeftDelta, 1024);
  oss << ",\"rightA\":";
  appendCompactSignalJson(oss, irDisplayRightA, 1024);
  oss << ",\"rightB\":";
  appendCompactSignalJson(oss, irDisplayRightB, 1024);
  oss << ",\"rightDelta\":";
  appendCompactSignalJson(oss, irDisplayRightDelta, 1024);
  oss << "},\"topDiff\":[";
  for (std::size_t i = 0; i < summary.irResult.topDifferences.size(); ++i) {
    if (i) oss << ",";
    const auto& d = summary.irResult.topDifferences[i];
    oss << "{\"sampleIndex\":" << d.sampleIndex << ",\"absDelta\":" << d.absoluteDelta << "}";
  }
  oss << "]},";

  oss << "\"frequency\":{\"peakAbsDeltaDb\":" << summary.frequencyResult.metrics.peakAbsDeltaDb
      << ",\"meanAbsDeltaDb\":" << summary.frequencyResult.metrics.meanAbsDeltaDb
      << ",\"estimatedLatencyMs\":" << summary.frequencyResult.metrics.estimatedLatencyMs
      << ",\"pluginLatencySamplesA\":" << summary.frequencyResult.metrics.pluginLatencySamplesA
      << ",\"pluginLatencySamplesB\":" << summary.frequencyResult.metrics.pluginLatencySamplesB
      << ",\"analysisSignal\":\"" << summary.frequencyResult.analysisSignalMode
      << "\",\"analysisReference\":\"input_white_noise_psd\""
      << ",\"latencyAlignment\":{"
      << "\"reportedA\":" << summary.frequencyResult.latencyAlignment.reportedA << ",\"reportedB\":"
      << summary.frequencyResult.latencyAlignment.reportedB << ",\"clampedA\":"
      << summary.frequencyResult.latencyAlignment.clampedA << ",\"clampedB\":"
      << summary.frequencyResult.latencyAlignment.clampedB << ",\"appliedDelayA\":"
      << summary.frequencyResult.latencyAlignment.appliedDelayA << ",\"appliedDelayB\":"
      << summary.frequencyResult.latencyAlignment.appliedDelayB << ",\"clampedOccurred\":"
      << (summary.frequencyResult.latencyAlignment.clampedOccurred ? "true" : "false")
      << "},\"normalizedSpectrumA\":";
  appendCompactSpectrumJson(oss, summary.frequencyResult.normalizedSpectrumA, 1024);
  oss << ",\"normalizedSpectrumB\":";
  appendCompactSpectrumJson(oss, summary.frequencyResult.normalizedSpectrumB, 1024);
  oss << ",\"octaveBands\":[";
  for (std::size_t i = 0; i < summary.frequencyResult.octaveBands.size(); ++i) {
    if (i) oss << ",";
    const auto& b = summary.frequencyResult.octaveBands[i];
    oss << "{\"lowerHz\":" << b.lowerHz << ",\"centerHz\":" << b.centerHz << ",\"upperHz\":" << b.upperHz
        << ",\"avgDeltaDb\":" << b.avgDeltaDb << ",\"maxDeltaDb\":" << b.maxDeltaDb << "}";
  }
  oss << "]},";

  oss << "\"phase\":{\"peakAbsDeltaRad\":" << summary.phaseResult.metrics.peakAbsDeltaRad
      << ",\"meanAbsDeltaRad\":" << summary.phaseResult.metrics.meanAbsDeltaRad
      << ",\"estimatedLatencyMs\":" << summary.phaseResult.metrics.estimatedLatencyMs
      << ",\"pluginLatencySamplesA\":" << summary.phaseResult.metrics.pluginLatencySamplesA
      << ",\"pluginLatencySamplesB\":" << summary.phaseResult.metrics.pluginLatencySamplesB
      << ",\"analysisSignal\":\"" << summary.phaseResult.analysisSignalMode
      << "\",\"analysisReference\":\"input_white_noise_transfer_phase\""
      << ",\"latencyAlignment\":{"
      << "\"reportedA\":" << summary.phaseResult.latencyAlignment.reportedA << ",\"reportedB\":"
      << summary.phaseResult.latencyAlignment.reportedB << ",\"clampedA\":"
      << summary.phaseResult.latencyAlignment.clampedA << ",\"clampedB\":"
      << summary.phaseResult.latencyAlignment.clampedB << ",\"appliedDelayA\":"
      << summary.phaseResult.latencyAlignment.appliedDelayA << ",\"appliedDelayB\":"
      << summary.phaseResult.latencyAlignment.appliedDelayB << ",\"clampedOccurred\":"
      << (summary.phaseResult.latencyAlignment.clampedOccurred ? "true" : "false") << "},\"phaseA\":";
  appendCompactPhaseJson(oss, summary.phaseResult.phaseA, 1024);
  oss << ",\"phaseB\":";
  appendCompactPhaseJson(oss, summary.phaseResult.phaseB, 1024);
  oss << ",\"delta\":";
  appendCompactPhaseJson(oss, summary.phaseResult.delta, 1024);
  oss << ",\"bands\":[";
  for (std::size_t i = 0; i < summary.phaseResult.bands.size(); ++i) {
    if (i) oss << ",";
    const auto& b = summary.phaseResult.bands[i];
    oss << "{\"lowerHz\":" << b.lowerHz << ",\"centerHz\":" << b.centerHz << ",\"upperHz\":" << b.upperHz
        << ",\"avgDeltaRad\":" << b.avgDeltaRad << ",\"maxDeltaRad\":" << b.maxDeltaRad << "}";
  }
  oss << "]},";

  oss << "\"harmonic\":{\"analysisReference\":\"blackman_harris_fft_dbfs\",\"metrics\":{"
      << "\"peakAbsDeltaDb\":" << summary.harmonicResult.metrics.peakAbsDeltaDb
      << ",\"meanAbsDeltaDb\":" << summary.harmonicResult.metrics.meanAbsDeltaDb << "},\"pitches\":[";
  for (std::size_t i = 0; i < summary.harmonicResult.pitches.size(); ++i) {
    if (i) oss << ",";
    const auto& p = summary.harmonicResult.pitches[i];
    oss << "{\"fundamentalHz\":" << p.fundamentalHz << ",\"analysisSignal\":\"" << p.analysisSignalMode
        << "\",\"pluginLatencySamplesA\":" << p.pluginLatencySamplesA
        << ",\"pluginLatencySamplesB\":" << p.pluginLatencySamplesB << ",\"latencyAlignment\":{"
        << "\"reportedA\":" << p.latencyAlignment.reportedA << ",\"reportedB\":" << p.latencyAlignment.reportedB
        << ",\"clampedA\":" << p.latencyAlignment.clampedA << ",\"clampedB\":" << p.latencyAlignment.clampedB
        << ",\"appliedDelayA\":" << p.latencyAlignment.appliedDelayA
        << ",\"appliedDelayB\":" << p.latencyAlignment.appliedDelayB << ",\"clampedOccurred\":"
        << (p.latencyAlignment.clampedOccurred ? "true" : "false")
        << "},\"noiseFloorDbfsA\":" << p.noiseFloorDbfsA << ",\"noiseFloorDbfsB\":" << p.noiseFloorDbfsB
        << ",\"noiseFloorDeltaDb\":" << p.noiseFloorDeltaDb << ",\"spectrumA\":";
    appendCompactHarmonicSpectrumJson(oss, p.spectrumA, 1024);
    oss << ",\"spectrumB\":";
    appendCompactHarmonicSpectrumJson(oss, p.spectrumB, 1024);
    oss << ",\"delta\":";
    appendCompactHarmonicSpectrumJson(oss, p.delta, 1024);
    oss << ",\"orders\":[";
    for (std::size_t j = 0; j < p.orders.size(); ++j) {
      if (j) oss << ",";
      const auto& o = p.orders[j];
      oss << "{\"order\":" << o.order << ",\"hz\":" << o.frequencyHz << ",\"aDbfs\":" << o.amplitudeDbfsA
          << ",\"bDbfs\":" << o.amplitudeDbfsB << ",\"deltaDb\":" << o.deltaDb << "}";
    }
    oss << "]}";
  }
  oss << "]},";

  oss << "\"thd\":{\"analysisReference\":\"fft_fundamental_bin_rejection\",\"metrics\":{"
      << "\"peakAbsThdDeltaPercent\":" << summary.thdResult.metrics.peakAbsThdDeltaPercent
      << ",\"meanAbsThdDeltaPercent\":" << summary.thdResult.metrics.meanAbsThdDeltaPercent
      << ",\"peakAbsThdnDeltaPercent\":" << summary.thdResult.metrics.peakAbsThdnDeltaPercent
      << ",\"meanAbsThdnDeltaPercent\":" << summary.thdResult.metrics.meanAbsThdnDeltaPercent << "},\"sweep\":[";
  for (std::size_t i = 0; i < summary.thdResult.sweep.size(); ++i) {
    if (i) oss << ",";
    const auto& p = summary.thdResult.sweep[i];
    oss << "{\"inputLevelDbfs\":" << p.inputLevelDbfs << ",\"analysisSignal\":\"" << p.analysisSignalMode
        << "\",\"pluginLatencySamplesA\":" << p.pluginLatencySamplesA
        << ",\"pluginLatencySamplesB\":" << p.pluginLatencySamplesB << ",\"latencyAlignment\":{"
        << "\"reportedA\":" << p.latencyAlignment.reportedA << ",\"reportedB\":" << p.latencyAlignment.reportedB
        << ",\"clampedA\":" << p.latencyAlignment.clampedA << ",\"clampedB\":" << p.latencyAlignment.clampedB
        << ",\"appliedDelayA\":" << p.latencyAlignment.appliedDelayA
        << ",\"appliedDelayB\":" << p.latencyAlignment.appliedDelayB << ",\"clampedOccurred\":"
        << (p.latencyAlignment.clampedOccurred ? "true" : "false")
        << "},\"thdPercentA\":" << p.thdPercentA << ",\"thdPercentB\":" << p.thdPercentB
        << ",\"thdDeltaPercent\":" << p.thdDeltaPercent << ",\"thdnPercentA\":" << p.thdnPercentA
        << ",\"thdnPercentB\":" << p.thdnPercentB << ",\"thdnDeltaPercent\":" << p.thdnDeltaPercent
        << ",\"warnings\":[";
    for (std::size_t w = 0; w < p.warnings.size(); ++w) {
      if (w) oss << ",";
      oss << "\"" << escapeJson(p.warnings[w]) << "\"";
    }
    oss << "]}";
  }
  oss << "],\"segments\":[";
  for (std::size_t i = 0; i < summary.thdResult.segments.size(); ++i) {
    if (i) oss << ",";
    const auto& s = summary.thdResult.segments[i];
    oss << "{\"name\":\"" << escapeJson(s.name) << "\",\"startLevelDbfs\":" << s.startLevelDbfs
        << ",\"endLevelDbfs\":" << s.endLevelDbfs << ",\"pointCount\":" << s.pointCount
        << ",\"avgThdDeltaPercent\":" << s.avgThdDeltaPercent
        << ",\"maxAbsThdDeltaPercent\":" << s.maxAbsThdDeltaPercent
        << ",\"avgThdnDeltaPercent\":" << s.avgThdnDeltaPercent
        << ",\"maxAbsThdnDeltaPercent\":" << s.maxAbsThdnDeltaPercent << "}";
  }
  oss << "]},";

  oss << "\"monoStereoWidth\":{\"analysisReference\":\"" << summary.monoStereoWidthResult.analysisReference
      << "\",\"analysisSignal\":\"" << summary.monoStereoWidthResult.analysisSignalMode << "\",\"metrics\":{"
      << "\"peakAbsTimeDeltaDb\":" << summary.monoStereoWidthResult.metrics.peakAbsTimeDeltaDb
      << ",\"meanAbsTimeDeltaDb\":" << summary.monoStereoWidthResult.metrics.meanAbsTimeDeltaDb
      << ",\"peakAbsBandDeltaDb\":" << summary.monoStereoWidthResult.metrics.peakAbsBandDeltaDb
      << ",\"meanAbsBandDeltaDb\":" << summary.monoStereoWidthResult.metrics.meanAbsBandDeltaDb
      << ",\"peakAbsTimeDeltaWidthPercent\":"
      << summary.monoStereoWidthResult.metrics.peakAbsTimeDeltaWidthPercent
      << ",\"meanAbsTimeDeltaWidthPercent\":"
      << summary.monoStereoWidthResult.metrics.meanAbsTimeDeltaWidthPercent
      << ",\"peakAbsBandDeltaWidthPercent\":"
      << summary.monoStereoWidthResult.metrics.peakAbsBandDeltaWidthPercent
      << ",\"meanAbsBandDeltaWidthPercent\":"
      << summary.monoStereoWidthResult.metrics.meanAbsBandDeltaWidthPercent
      << ",\"pluginLatencySamplesA\":" << summary.monoStereoWidthResult.metrics.pluginLatencySamplesA
      << ",\"pluginLatencySamplesB\":" << summary.monoStereoWidthResult.metrics.pluginLatencySamplesB
      << "},\"latencyAlignment\":{"
      << "\"reportedA\":" << summary.monoStereoWidthResult.latencyAlignment.reportedA << ",\"reportedB\":"
      << summary.monoStereoWidthResult.latencyAlignment.reportedB << ",\"clampedA\":"
      << summary.monoStereoWidthResult.latencyAlignment.clampedA << ",\"clampedB\":"
      << summary.monoStereoWidthResult.latencyAlignment.clampedB << ",\"appliedDelayA\":"
      << summary.monoStereoWidthResult.latencyAlignment.appliedDelayA << ",\"appliedDelayB\":"
      << summary.monoStereoWidthResult.latencyAlignment.appliedDelayB << ",\"clampedOccurred\":"
      << (summary.monoStereoWidthResult.latencyAlignment.clampedOccurred ? "true" : "false") << "},\"timeSeries\":";
  appendCompactWidthTimeJson(oss, summary.monoStereoWidthResult.timeSeries, 1024);
  oss << ",\"spectrum\":";
  appendCompactWidthSpectrumJson(oss, summary.monoStereoWidthResult.spectrum, 1024);
  oss << ",\"bands\":[";
  for (std::size_t i = 0; i < summary.monoStereoWidthResult.bands.size(); ++i) {
    if (i) oss << ",";
    const auto& b = summary.monoStereoWidthResult.bands[i];
    oss << "{\"lowerHz\":" << b.lowerHz << ",\"centerHz\":" << b.centerHz << ",\"upperHz\":" << b.upperHz
        << ",\"ratioDbA\":" << b.ratioDbA << ",\"ratioDbB\":" << b.ratioDbB << ",\"deltaDb\":" << b.deltaDb
        << ",\"widthPercentA\":" << b.widthPercentA << ",\"widthPercentB\":" << b.widthPercentB
        << ",\"deltaWidthPercent\":" << b.deltaWidthPercent
        << "}";
  }
  oss << "],\"warnings\":[";
  for (std::size_t i = 0; i < summary.monoStereoWidthResult.warnings.size(); ++i) {
    if (i) oss << ",";
    oss << "\"" << escapeJson(summary.monoStereoWidthResult.warnings[i]) << "\"";
  }
  oss << "]},";

  oss << "\"pitch\":{\"analysisReference\":\"log_sine_sweep_time_pitch_tracking\",\"analysisSignal\":\""
      << summary.pitchResult.analysisSignalMode << "\",\"metrics\":{"
      << "\"meanAbsErrorHzA\":" << summary.pitchResult.metrics.meanAbsErrorHzA
      << ",\"meanAbsErrorHzB\":" << summary.pitchResult.metrics.meanAbsErrorHzB
      << ",\"meanAbsDeltaHz\":" << summary.pitchResult.metrics.meanAbsDeltaHz
      << ",\"peakAbsDeltaHz\":" << summary.pitchResult.metrics.peakAbsDeltaHz
      << ",\"validFrameRateA\":" << summary.pitchResult.metrics.validFrameRateA
      << ",\"validFrameRateB\":" << summary.pitchResult.metrics.validFrameRateB
      << ",\"estimatedLatencyMs\":" << summary.pitchResult.metrics.estimatedLatencyMs
      << ",\"pluginLatencySamplesA\":" << summary.pitchResult.metrics.pluginLatencySamplesA
      << ",\"pluginLatencySamplesB\":" << summary.pitchResult.metrics.pluginLatencySamplesB
      << "},\"latencyAlignment\":{"
      << "\"reportedA\":" << summary.pitchResult.latencyAlignment.reportedA << ",\"reportedB\":"
      << summary.pitchResult.latencyAlignment.reportedB << ",\"clampedA\":"
      << summary.pitchResult.latencyAlignment.clampedA << ",\"clampedB\":"
      << summary.pitchResult.latencyAlignment.clampedB << ",\"appliedDelayA\":"
      << summary.pitchResult.latencyAlignment.appliedDelayA << ",\"appliedDelayB\":"
      << summary.pitchResult.latencyAlignment.appliedDelayB << ",\"clampedOccurred\":"
      << (summary.pitchResult.latencyAlignment.clampedOccurred ? "true" : "false") << "},\"curve\":";
  appendCompactPitchCurveJson(oss, summary.pitchResult.curve, 2048);
  oss << ",\"warnings\":[";
  for (std::size_t i = 0; i < summary.pitchResult.warnings.size(); ++i) {
    if (i) oss << ",";
    oss << "\"" << escapeJson(summary.pitchResult.warnings[i]) << "\"";
  }
  oss << "]},";

  oss << "\"dynamics\":{\"analysisReference\":\"step_sine_rms_io_curve\",\"analysisSignal\":\""
      << summary.dynamicsResult.analysisSignalMode << "\",\"metrics\":{"
      << "\"peakAbsOutputDeltaDb\":" << summary.dynamicsResult.metrics.peakAbsOutputDeltaDb
      << ",\"meanAbsOutputDeltaDb\":" << summary.dynamicsResult.metrics.meanAbsOutputDeltaDb
      << ",\"peakAbsGainReductionDeltaDb\":" << summary.dynamicsResult.metrics.peakAbsGainReductionDeltaDb
      << ",\"meanAbsGainReductionDeltaDb\":" << summary.dynamicsResult.metrics.meanAbsGainReductionDeltaDb
      << ",\"thresholdA\":" << summary.dynamicsResult.metrics.estimatedA.thresholdDbfs
      << ",\"thresholdB\":" << summary.dynamicsResult.metrics.estimatedB.thresholdDbfs
      << ",\"thresholdDelta\":" << summary.dynamicsResult.metrics.thresholdDeltaDb
      << ",\"ratioA\":" << summary.dynamicsResult.metrics.estimatedA.ratio
      << ",\"ratioB\":" << summary.dynamicsResult.metrics.estimatedB.ratio
      << ",\"ratioDelta\":" << summary.dynamicsResult.metrics.ratioDelta
      << ",\"kneeWidthA\":" << summary.dynamicsResult.metrics.estimatedA.kneeWidthDb
      << ",\"kneeWidthB\":" << summary.dynamicsResult.metrics.estimatedB.kneeWidthDb
      << ",\"kneeWidthDelta\":" << summary.dynamicsResult.metrics.kneeWidthDeltaDb
      << ",\"estimatedAValid\":" << (summary.dynamicsResult.metrics.estimatedA.valid ? "true" : "false")
      << ",\"estimatedBValid\":" << (summary.dynamicsResult.metrics.estimatedB.valid ? "true" : "false")
      << ",\"pluginLatencySamplesA\":" << summary.dynamicsResult.metrics.pluginLatencySamplesA
      << ",\"pluginLatencySamplesB\":" << summary.dynamicsResult.metrics.pluginLatencySamplesB
      << "},\"latencyAlignment\":{"
      << "\"reportedA\":" << summary.dynamicsResult.latencyAlignment.reportedA << ",\"reportedB\":"
      << summary.dynamicsResult.latencyAlignment.reportedB << ",\"clampedA\":"
      << summary.dynamicsResult.latencyAlignment.clampedA << ",\"clampedB\":"
      << summary.dynamicsResult.latencyAlignment.clampedB << ",\"appliedDelayA\":"
      << summary.dynamicsResult.latencyAlignment.appliedDelayA << ",\"appliedDelayB\":"
      << summary.dynamicsResult.latencyAlignment.appliedDelayB << ",\"clampedOccurred\":"
      << (summary.dynamicsResult.latencyAlignment.clampedOccurred ? "true" : "false") << "},\"curve\":";
  appendCompactDynamicsCurveJson(oss, summary.dynamicsResult.ioCurve, 1024);
  oss << ",\"warnings\":[";
  for (std::size_t i = 0; i < summary.dynamicsResult.warnings.size(); ++i) {
    if (i) oss << ",";
    oss << "\"" << escapeJson(summary.dynamicsResult.warnings[i]) << "\"";
  }
  oss << "]},";

  oss << "\"timeResponse\":{\"analysisReference\":\"tone_burst_envelope_time_response\",\"analysisSignal\":\""
      << summary.timeResponseResult.analysisSignalMode << "\",\"metrics\":{"
      << "\"residualLatencyMs\":" << summary.timeResponseResult.metrics.residualLatencyMs
      << ",\"attackMsA\":" << summary.timeResponseResult.metrics.attackMsA
      << ",\"attackMsB\":" << summary.timeResponseResult.metrics.attackMsB
      << ",\"attackDeltaMs\":" << summary.timeResponseResult.metrics.attackDeltaMs
      << ",\"releaseMsA\":" << summary.timeResponseResult.metrics.releaseMsA
      << ",\"releaseMsB\":" << summary.timeResponseResult.metrics.releaseMsB
      << ",\"releaseDeltaMs\":" << summary.timeResponseResult.metrics.releaseDeltaMs
      << ",\"postReleaseResidualDbA\":" << summary.timeResponseResult.metrics.postReleaseResidualDbA
      << ",\"postReleaseResidualDbB\":" << summary.timeResponseResult.metrics.postReleaseResidualDbB
      << ",\"postReleaseResidualDeltaDb\":" << summary.timeResponseResult.metrics.postReleaseResidualDeltaDb
      << ",\"pluginLatencySamplesA\":" << summary.timeResponseResult.metrics.pluginLatencySamplesA
      << ",\"pluginLatencySamplesB\":" << summary.timeResponseResult.metrics.pluginLatencySamplesB
      << "},\"latencyAlignment\":{"
      << "\"reportedA\":" << summary.timeResponseResult.latencyAlignment.reportedA << ",\"reportedB\":"
      << summary.timeResponseResult.latencyAlignment.reportedB << ",\"clampedA\":"
      << summary.timeResponseResult.latencyAlignment.clampedA << ",\"clampedB\":"
      << summary.timeResponseResult.latencyAlignment.clampedB << ",\"appliedDelayA\":"
      << summary.timeResponseResult.latencyAlignment.appliedDelayA << ",\"appliedDelayB\":"
      << summary.timeResponseResult.latencyAlignment.appliedDelayB << ",\"clampedOccurred\":"
      << (summary.timeResponseResult.latencyAlignment.clampedOccurred ? "true" : "false") << "},\"curve\":";
  appendCompactTimeResponseCurveJson(oss, summary.timeResponseResult.curve, 1024);
  oss << ",\"warnings\":[";
  for (std::size_t i = 0; i < summary.timeResponseResult.warnings.size(); ++i) {
    if (i) oss << ",";
    oss << "\"" << escapeJson(summary.timeResponseResult.warnings[i]) << "\"";
  }
  oss << "]},";

  appendLlmSummaryJson(oss, llmSummary);
  oss << ",";

  oss << "\"appliedParameters\":[";
  for (std::size_t i = 0; i < summary.appliedParameters.size(); ++i) {
    if (i) oss << ",";
    const auto& p = summary.appliedParameters[i];
    oss << "{\"plugin\":\"" << escapeJson(p.plugin) << "\",\"name\":\"" << escapeJson(p.name)
        << "\",\"parameterId\":" << p.parameterId << ",\"normalized\":" << p.normalized
        << ",\"displayInput\":\"" << escapeJson(p.displayInput) << "\"}";
  }
  oss << "],";
  oss << "\"warnings\":[";
  for (std::size_t i = 0; i < summary.warnings.size(); ++i) {
    if (i) oss << ",";
    oss << "\"" << escapeJson(summary.warnings[i]) << "\"";
  }
  oss << "]";
  oss << "}";
  return oss.str();
}

std::string FinalReportBuilder::renderHtml(const RunSummary& summary) const {
  const int width = 980;
  const int height = 300;

  const std::vector<float> empty;
  const auto& irLeftA =
      (summary.irResult.displayAlignedA.numChannels() >= 1) ? summary.irResult.displayAlignedA.channels[0] : empty;
  const auto& irLeftB =
      (summary.irResult.displayAlignedB.numChannels() >= 1) ? summary.irResult.displayAlignedB.channels[0] : empty;
  const auto& irLeftD = (summary.irResult.displayDelta.numChannels() >= 1) ? summary.irResult.displayDelta.channels[0] : empty;
  const auto& irRightA =
      (summary.irResult.displayAlignedA.numChannels() >= 2) ? summary.irResult.displayAlignedA.channels[1] : empty;
  const auto& irRightB =
      (summary.irResult.displayAlignedB.numChannels() >= 2) ? summary.irResult.displayAlignedB.channels[1] : empty;
  const auto& irRightD =
      (summary.irResult.displayDelta.numChannels() >= 2) ? summary.irResult.displayDelta.channels[1] : empty;
  const bool irRightAvailable = !irRightA.empty() && !irRightB.empty() && !irRightD.empty();

  const IrAutoWindow irLeftWindow = detectIrAutoWindow({&irLeftA, &irLeftB, &irLeftD}, summary.config.sampleRate);
  const IrAutoWindow irRightWindow = detectIrAutoWindow({&irRightA, &irRightB, &irRightD}, summary.config.sampleRate);

  const double irLeftStartMs =
      (irLeftWindow.totalSamples > 0)
          ? (1000.0 * static_cast<double>(irLeftWindow.startSample) / static_cast<double>(summary.config.sampleRate))
          : 0.0;
  const double irLeftEndMs =
      (irLeftWindow.totalSamples > 0)
          ? (1000.0 * static_cast<double>(irLeftWindow.endSample) / static_cast<double>(summary.config.sampleRate))
          : 0.0;
  const double irRightStartMs =
      (irRightWindow.totalSamples > 0)
          ? (1000.0 * static_cast<double>(irRightWindow.startSample) / static_cast<double>(summary.config.sampleRate))
          : 0.0;
  const double irRightEndMs =
      (irRightWindow.totalSamples > 0)
          ? (1000.0 * static_cast<double>(irRightWindow.endSample) / static_cast<double>(summary.config.sampleRate))
          : 0.0;

  const LlmSummary llmSummary = buildLlmSummary(summary);
  const std::string json = toJson(summary);
  const std::string irLeftPolyA =
      polylineFromSignalWindow(irLeftA, width, height, irLeftWindow.startSample, irLeftWindow.endSample);
  const std::string irLeftPolyB =
      polylineFromSignalWindow(irLeftB, width, height, irLeftWindow.startSample, irLeftWindow.endSample);
  const std::string irLeftPolyD =
      polylineFromSignalWindow(irLeftD, width, height, irLeftWindow.startSample, irLeftWindow.endSample);
  const std::string irLeftAxisOverlay = buildIrTimeAxisOverlay(width, height, irLeftStartMs, irLeftEndMs);
  const std::string irRightPolyA =
      polylineFromSignalWindow(irRightA, width, height, irRightWindow.startSample, irRightWindow.endSample);
  const std::string irRightPolyB =
      polylineFromSignalWindow(irRightB, width, height, irRightWindow.startSample, irRightWindow.endSample);
  const std::string irRightPolyD =
      polylineFromSignalWindow(irRightD, width, height, irRightWindow.startSample, irRightWindow.endSample);
  const std::string irRightAxisOverlay = buildIrTimeAxisOverlay(width, height, irRightStartMs, irRightEndMs);

  const std::string freqPolyA =
      polylineFromSpectrum(summary.frequencyResult.normalizedSpectrumA, width, height, 20.0, 20000.0, -24.0, 24.0);
  const std::string freqPolyB =
      polylineFromSpectrum(summary.frequencyResult.normalizedSpectrumB, width, height, 20.0, 20000.0, -24.0, 24.0);
  const std::string freqPolyD =
      polylineFromSpectrum(summary.frequencyResult.delta, width, height, 20.0, 20000.0, -24.0, 24.0);
  const std::string freqAxisOverlay = buildFrequencyAxisOverlay(width, height, 20.0, 20000.0, -24.0, 24.0);
  const std::string phasePolyA = polylineFromPhase(summary.phaseResult.phaseA, width, height, 20.0, 20000.0);
  const std::string phasePolyB = polylineFromPhase(summary.phaseResult.phaseB, width, height, 20.0, 20000.0);
  const std::string phasePolyD = polylineFromPhase(summary.phaseResult.delta, width, height, 20.0, 20000.0);
  const std::string phaseAxisOverlay = buildPhaseAxisOverlay(width, height, 20.0, 20000.0);
  const std::string harmonicAxisOverlay = buildHarmonicAxisOverlay(width, height, 20.0, 24000.0, -140.0, 0.0);
  const std::string harmonicDeltaAxisOverlay = buildHarmonicDeltaAxisOverlay(width, height, 20.0, 24000.0, -24.0, 24.0);
  const std::string thdAxisOverlay = buildThdAxisOverlay(width, height, -60.0, 6.0, 0.0001, 100.0);
  const std::string thdPolyA =
      polylineFromThdSweep(summary.thdResult.sweep, width, height, -60.0, 6.0, 0.0001, 100.0, false, false);
  const std::string thdPolyB =
      polylineFromThdSweep(summary.thdResult.sweep, width, height, -60.0, 6.0, 0.0001, 100.0, false, true);
  const std::string thdnPolyA =
      polylineFromThdSweep(summary.thdResult.sweep, width, height, -60.0, 6.0, 0.0001, 100.0, true, false);
  const std::string thdnPolyB =
      polylineFromThdSweep(summary.thdResult.sweep, width, height, -60.0, 6.0, 0.0001, 100.0, true, true);

  const double widthTimeMinMs = summary.monoStereoWidthResult.timeSeries.empty()
                                    ? 0.0
                                    : summary.monoStereoWidthResult.timeSeries.front().timeMs;
  const double widthTimeMaxMs = summary.monoStereoWidthResult.timeSeries.empty()
                                    ? 1.0
                                    : std::max(widthTimeMinMs + 1.0, summary.monoStereoWidthResult.timeSeries.back().timeMs);
  const std::string widthTimePolyA = polylineFromWidthTime(summary.monoStereoWidthResult.timeSeries, width, height,
                                                           widthTimeMinMs, widthTimeMaxMs, 0.0, 100.0, false, false);
  const std::string widthTimePolyB = polylineFromWidthTime(summary.monoStereoWidthResult.timeSeries, width, height,
                                                           widthTimeMinMs, widthTimeMaxMs, 0.0, 100.0, true, false);
  const std::string widthTimePolyD = polylineFromWidthTime(summary.monoStereoWidthResult.timeSeries, width, height,
                                                           widthTimeMinMs, widthTimeMaxMs, -50.0, 50.0, false, true);
  const std::string widthBandPolyA = polylineFromWidthSpectrum(summary.monoStereoWidthResult.spectrum, width, height,
                                                               20.0, 20000.0, 0.0, 100.0, false, false);
  const std::string widthBandPolyB = polylineFromWidthSpectrum(summary.monoStereoWidthResult.spectrum, width, height,
                                                               20.0, 20000.0, 0.0, 100.0, true, false);
  const std::string widthBandPolyD = polylineFromWidthSpectrum(summary.monoStereoWidthResult.spectrum, width, height,
                                                               20.0, 20000.0, -50.0, 50.0, false, true);
  const std::string widthTimeAxisOverlay =
      buildWidthTimeAxisOverlay(width, height, widthTimeMinMs, widthTimeMaxMs, 0.0, 100.0);
  const std::string widthTimeDeltaAxisOverlay =
      buildWidthTimeDeltaAxisOverlay(width, height, widthTimeMinMs, widthTimeMaxMs, -50.0, 50.0);
  const std::string widthBandAxisOverlay = buildWidthBandAxisOverlay(width, height, 20.0, 20000.0, 0.0, 100.0);
  const std::string widthBandDeltaAxisOverlay =
      buildWidthBandDeltaAxisOverlay(width, height, 20.0, 20000.0, -50.0, 50.0);
  const double pitchTimeMinMs = summary.pitchResult.curve.empty() ? 0.0 : summary.pitchResult.curve.front().timeMs;
  const double pitchTimeMaxMs =
      summary.pitchResult.curve.empty() ? static_cast<double>(summary.config.pitchDurationMs)
                                        : std::max(pitchTimeMinMs + 1.0, summary.pitchResult.curve.back().timeMs);
  const double pitchHzMin = std::max(1.0, static_cast<double>(summary.config.pitchStartHz));
  const double pitchHzMax = std::max(pitchHzMin + 1.0, static_cast<double>(summary.config.pitchEndHz));
  const std::string pitchPolyInput = polylineFromPitchCurve(summary.pitchResult.curve, width, height, pitchTimeMinMs,
                                                             pitchTimeMaxMs, pitchHzMin, pitchHzMax, 0);
  const std::string pitchPolyA = polylineFromPitchCurve(summary.pitchResult.curve, width, height, pitchTimeMinMs,
                                                         pitchTimeMaxMs, pitchHzMin, pitchHzMax, 1);
  const std::string pitchPolyB = polylineFromPitchCurve(summary.pitchResult.curve, width, height, pitchTimeMinMs,
                                                         pitchTimeMaxMs, pitchHzMin, pitchHzMax, 2);
  const std::string pitchAxisOverlay =
      buildPitchAxisOverlay(width, height, pitchTimeMinMs, pitchTimeMaxMs, pitchHzMin, pitchHzMax);
  const std::string dynamicsIoPolyA =
      polylineFromDynamicsIo(summary.dynamicsResult.ioCurve, width, height, -90.0, 0.0, -90.0, 0.0, false, false);
  const std::string dynamicsIoPolyB =
      polylineFromDynamicsIo(summary.dynamicsResult.ioCurve, width, height, -90.0, 0.0, -90.0, 0.0, true, false);
  const std::string dynamicsIoPolyD =
      polylineFromDynamicsIo(summary.dynamicsResult.ioCurve, width, height, -90.0, 0.0, -24.0, 24.0, false, true);
  const std::string dynamicsIoAxis =
      buildDynamicsAxisOverlay(width, height, -90.0, 0.0, -90.0, 0.0, {0.0, -30.0, -60.0, -90.0}, "dBFS");
  const std::string dynamicsDeltaAxis =
      buildDynamicsAxisOverlay(width, height, -90.0, 0.0, -24.0, 24.0, {24.0, 12.0, 0.0, -12.0, -24.0}, "dB");
  const double timeResponseMinMs =
      summary.timeResponseResult.curve.empty() ? 0.0 : summary.timeResponseResult.curve.front().timeMs;
  const double timeResponseMaxMs = summary.timeResponseResult.curve.empty()
                                       ? 500.0
                                       : std::max(timeResponseMinMs + 1.0, summary.timeResponseResult.curve.back().timeMs);
  const std::string timeResponsePolyA =
      polylineFromTimeResponse(summary.timeResponseResult.curve, width, height, timeResponseMinMs, timeResponseMaxMs,
                               -90.0, 0.0, 0);
  const std::string timeResponsePolyB =
      polylineFromTimeResponse(summary.timeResponseResult.curve, width, height, timeResponseMinMs, timeResponseMaxMs,
                               -90.0, 0.0, 1);
  const std::string timeResponsePolyD =
      polylineFromTimeResponse(summary.timeResponseResult.curve, width, height, timeResponseMinMs, timeResponseMaxMs,
                               -24.0, 24.0, 2);
  const std::string timeResponseAxis =
      buildTimeResponseAxisOverlay(width, height, timeResponseMinMs, timeResponseMaxMs, -90.0, 0.0,
                                   {0.0, -20.0, -40.0, -60.0, -80.0}, "dB", false);
  const std::string timeResponseDeltaAxis =
      buildTimeResponseAxisOverlay(width, height, timeResponseMinMs, timeResponseMaxMs, -24.0, 24.0,
                                   {24.0, 12.0, 0.0, -12.0, -24.0}, "dB", true);

  std::ostringstream html;
  html << "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
       << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
       << "<title>VSTCompare Report</title>"
       << "<style>"
       << ":root{--bg:#f6f7fb;--card:#ffffff;--ink:#152238;--muted:#65758b;--a:#0f62fe;--b:#ff6b00;--d:#11a36a;}"
       << "body{font-family:Segoe UI,system-ui,sans-serif;background:linear-gradient(135deg,#eef3ff,#f7fbff);"
       << "color:var(--ink);margin:0;padding:20px;}h1{margin:0 0 6px;}h2{margin:0 0 10px;font-size:1.1rem;}"
       << "h3{margin:4px 0 10px;font-size:0.98rem;color:var(--muted);}"
       << ".grid{display:grid;grid-template-columns:1fr;gap:14px;max-width:1100px;margin:auto;}"
       << ".card{background:var(--card);border-radius:12px;padding:16px;box-shadow:0 6px 28px rgba(0,0,0,.07);}"
       << "table{width:100%;border-collapse:collapse;}td,th{padding:8px 6px;border-bottom:1px solid #e8edf4;"
       << "text-align:left;}th{font-size:.92rem;}small{color:var(--muted);}ul{margin:8px 0 0 20px;}"
       << ".legend{display:flex;gap:12px;font-size:.9rem;margin-bottom:8px;color:var(--muted);}"
       << ".dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:5px;}"
       << ".mono{font-family:Consolas,monospace;}"
       << ".scroll{max-height:260px;overflow:auto;}"
       << "@media(min-width:900px){.grid{grid-template-columns:1fr 1fr;}.full{grid-column:1 / -1;}}"
       << "</style></head><body><div class=\"grid\">"
       << "<div class=\"card full\"><h1>VSTCompare MVP Report</h1><small>IR + Frequency + Phase + Harmonic + THD/THD+N + Mono-to-Stereo Width + Pitch Tracking + Dynamics + Time Response Comparison</small></div>"
       << "<div class=\"card\"><h2>Plugin A</h2><table>"
       << "<tr><th>Name</th><td>" << escapeHtml(summary.pluginA.name) << "</td></tr>"
       << "<tr><th>Vendor</th><td>" << escapeHtml(summary.pluginA.vendor) << "</td></tr>"
       << "<tr><th>Unique ID</th><td class=\"mono\">" << escapeHtml(summary.pluginA.uniqueId) << "</td></tr>"
       << "<tr><th>Version</th><td>" << escapeHtml(summary.pluginA.version) << "</td></tr>"
       << "</table></div>"
       << "<div class=\"card\"><h2>Plugin B</h2><table>"
       << "<tr><th>Name</th><td>" << escapeHtml(summary.pluginB.name) << "</td></tr>"
       << "<tr><th>Vendor</th><td>" << escapeHtml(summary.pluginB.vendor) << "</td></tr>"
       << "<tr><th>Unique ID</th><td class=\"mono\">" << escapeHtml(summary.pluginB.uniqueId) << "</td></tr>"
       << "<tr><th>Version</th><td>" << escapeHtml(summary.pluginB.version) << "</td></tr>"
       << "</table></div>"
       << renderOverallSummaryHtml(llmSummary.overall)
       << "<div class=\"card full\"><h2>Run Config</h2><table>"
       << "<tr><th>Sample Rate</th><td>" << summary.config.sampleRate << " Hz</td></tr>"
       << "<tr><th>Block Size</th><td>" << summary.config.blockSize << "</td></tr>"
       << "<tr><th>IR Duration</th><td>" << summary.config.irDurationMs << " ms</td></tr>"
       << "<tr><th>Frequency Duration</th><td>" << summary.config.frequencyDurationMs << " ms</td></tr>"
       << "<tr><th>Frequency FFT</th><td>" << summary.config.frequencyFftSize << " (" << summary.config.frequencyOverlapPercent
       << "% overlap)</td></tr>"
       << "<tr><th>Frequency Input Level</th><td>" << summary.config.frequencyNoiseLevelDbfs << " dBFS</td></tr>"
       << "<tr><th>Phase Duration</th><td>" << summary.config.phaseDurationMs << " ms</td></tr>"
       << "<tr><th>Phase FFT</th><td>" << summary.config.phaseFftSize << " (" << summary.config.phaseOverlapPercent
       << "% overlap)</td></tr>"
       << "<tr><th>Phase Input Level</th><td>" << summary.config.phaseNoiseLevelDbfs << " dBFS</td></tr>"
       << "<tr><th>Harmonic Duration</th><td>" << summary.config.harmonicDurationMs << " ms</td></tr>"
       << "<tr><th>Harmonic Skip Head</th><td>" << summary.config.harmonicSkipHeadMs << " ms</td></tr>"
       << "<tr><th>Harmonic FFT</th><td>" << summary.config.harmonicFftSize << "</td></tr>"
       << "<tr><th>Harmonic Input Level</th><td>" << summary.config.harmonicInputLevelDbfs << " dBFS</td></tr>"
       << "<tr><th>Harmonic Frequencies</th><td>";
  for (std::size_t i = 0; i < summary.config.harmonicFrequenciesHz.size(); ++i) {
    if (i) html << ", ";
    html << summary.config.harmonicFrequenciesHz[i] << " Hz";
  }
  html << "</td></tr>"
       << "<tr><th>THD Tone Frequency</th><td>" << summary.config.thdToneFrequencyHz << " Hz</td></tr>"
       << "<tr><th>THD Sweep Range</th><td>" << summary.config.thdStartLevelDbfs << " to " << summary.config.thdEndLevelDbfs
       << " dBFS (step " << summary.config.thdStepDb << " dB)</td></tr>"
       << "<tr><th>THD Tone Duration</th><td>" << summary.config.thdDurationMs << " ms</td></tr>"
       << "<tr><th>THD Skip Head</th><td>" << summary.config.thdSkipHeadMs << " ms</td></tr>"
       << "<tr><th>THD FFT</th><td>" << summary.config.thdFftSize << "</td></tr>"
       << "<tr><th>Mono-to-Stereo Width Duration</th><td>" << summary.config.monoStereoWidthDurationMs << " ms</td></tr>"
       << "<tr><th>Mono-to-Stereo Width Noise Level</th><td>" << summary.config.monoStereoWidthNoiseLevelDbfs
       << " dBFS</td></tr>"
       << "<tr><th>Mono-to-Stereo Width FFT</th><td>" << summary.config.monoStereoWidthFftSize << " ("
       << summary.config.monoStereoWidthOverlapPercent << "% overlap)</td></tr>"
       << "<tr><th>Mono-to-Stereo Width Time Window/Hop</th><td>" << summary.config.monoStereoWidthTimeWindowMs
       << " ms / " << summary.config.monoStereoWidthTimeHopMs << " ms</td></tr>"
       << "<tr><th>Pitch Sweep</th><td>" << summary.config.pitchStartHz << " Hz to " << summary.config.pitchEndHz
       << " Hz (" << summary.config.pitchDurationMs << " ms)</td></tr>"
       << "<tr><th>Pitch Level/Fade</th><td>" << summary.config.pitchLevelDbfs << " dBFS, fade "
       << summary.config.pitchFadeMs << " ms</td></tr>"
       << "<tr><th>Pitch Frame/Hop</th><td>" << summary.config.pitchFrameMs << " ms / "
       << summary.config.pitchHopMs << " ms</td></tr>"
       << "<tr><th>Dynamics Tone Frequency</th><td>" << summary.config.dynamicsToneFrequencyHz << " Hz</td></tr>"
       << "<tr><th>Dynamics Sweep Range</th><td>" << summary.config.dynamicsStartLevelDbfs << " to "
       << summary.config.dynamicsEndLevelDbfs << " dBFS (step " << summary.config.dynamicsStepDb
       << " dB, " << summary.config.dynamicsStepMs << " ms/step)</td></tr>"
       << "<tr><th>Dynamics RMS Window</th><td>" << summary.config.dynamicsRmsWindowMs << " ms</td></tr>"
       << "<tr><th>Time Burst</th><td>" << summary.config.timeBurstFrequencyHz << " Hz, "
       << summary.config.timeBurstLevelDbfs << " dBFS, pre/on/post "
       << summary.config.timePreSilenceMs << "/" << summary.config.timeBurstOnMs << "/"
       << summary.config.timePostSilenceMs << " ms</td></tr>"
       << "<tr><th>Time Envelope/Correlation</th><td>window " << summary.config.timeEnvelopeWindowMs
       << " ms, max lag " << summary.config.timeCorrelationMaxLagMs << " ms</td></tr>"
       << "</table></div>"
       << renderTestSummaryHtml(llmSummary.tests[0])
       << "<div class=\"card full\"><h2>IR Metrics</h2><table>"
       << "<tr><th>Analysis Signal</th><td>Mono (L+R)/2</td></tr>"
       << "<tr><th>Estimated Latency (A-B)</th><td>" << summary.irResult.metrics.estimatedLatencyMs << " ms</td></tr>"
       << "<tr><th>Reported Latency</th><td>A: " << summary.irResult.metrics.pluginLatencySamplesA
       << " samples, B: " << summary.irResult.metrics.pluginLatencySamplesB << " samples</td></tr>"
       << "<tr><th>Applied Alignment Delay</th><td>A: " << summary.irResult.latencyAlignment.appliedDelayA
       << " samples, B: " << summary.irResult.latencyAlignment.appliedDelayB << " samples</td></tr>"
       << "<tr><th>Clamped Latency</th><td>A: " << summary.irResult.latencyAlignment.clampedA
       << " samples, B: " << summary.irResult.latencyAlignment.clampedB << " samples (occurred: "
       << (summary.irResult.latencyAlignment.clampedOccurred ? "yes" : "no") << ")</td></tr>"
       << "<tr><th>Peak |A-B|</th><td>" << summary.irResult.metrics.peakAbsDelta << "</td></tr>"
       << "<tr><th>Energy(A-B)</th><td>" << summary.irResult.metrics.energyDelta << "</td></tr>"
       << "</table></div>"
       << "<div class=\"card\"><h2>IR Left (A/B)</h2>"
       << "<small>Auto-scaled time window (" << formatMsLabel(irLeftStartMs) << " to " << formatMsLabel(irLeftEndMs)
       << ")</small>"
       << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--a)\"></span>Plugin A Left</span>"
       << "<span><span class=\"dot\" style=\"background:var(--b)\"></span>Plugin B Left</span></div>"
       << "<svg viewBox=\"0 0 " << width << ' ' << height
       << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"IR left A/B chart\">"
       << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
       << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
       << irLeftAxisOverlay
       << "<polyline fill=\"none\" stroke=\"var(--a)\" stroke-width=\"1.4\" points=\"" << irLeftPolyA << "\"/>"
       << "<polyline fill=\"none\" stroke=\"var(--b)\" stroke-width=\"1.4\" points=\"" << irLeftPolyB << "\"/>"
       << "</svg></div>"
       << "<div class=\"card\"><h2>IR Left Delta (A-B)</h2>"
       << "<small>Auto-scaled time window (" << formatMsLabel(irLeftStartMs) << " to " << formatMsLabel(irLeftEndMs)
       << ")</small>"
       << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--d)\"></span>Left Delta</span></div>"
       << "<svg viewBox=\"0 0 " << width << ' ' << height
       << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"IR left delta chart\">"
       << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
       << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
       << irLeftAxisOverlay
       << "<polyline fill=\"none\" stroke=\"var(--d)\" stroke-width=\"1.1\" points=\"" << irLeftPolyD << "\"/>"
       << "</svg></div>";

  if (irRightAvailable) {
    html << "<div class=\"card\"><h2>IR Right (A/B)</h2>"
         << "<small>Auto-scaled time window (" << formatMsLabel(irRightStartMs) << " to " << formatMsLabel(irRightEndMs)
         << ")</small>"
         << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--a)\"></span>Plugin A Right</span>"
         << "<span><span class=\"dot\" style=\"background:var(--b)\"></span>Plugin B Right</span></div>"
         << "<svg viewBox=\"0 0 " << width << ' ' << height
         << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"IR right A/B chart\">"
         << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
         << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
         << irRightAxisOverlay
         << "<polyline fill=\"none\" stroke=\"var(--a)\" stroke-width=\"1.4\" points=\"" << irRightPolyA << "\"/>"
         << "<polyline fill=\"none\" stroke=\"var(--b)\" stroke-width=\"1.4\" points=\"" << irRightPolyB << "\"/>"
         << "</svg></div>"
         << "<div class=\"card\"><h2>IR Right Delta (A-B)</h2>"
         << "<small>Auto-scaled time window (" << formatMsLabel(irRightStartMs) << " to " << formatMsLabel(irRightEndMs)
         << ")</small>"
         << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--d)\"></span>Right Delta</span></div>"
         << "<svg viewBox=\"0 0 " << width << ' ' << height
         << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"IR right delta chart\">"
         << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
         << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
         << irRightAxisOverlay
         << "<polyline fill=\"none\" stroke=\"var(--d)\" stroke-width=\"1.1\" points=\"" << irRightPolyD << "\"/>"
         << "</svg></div>";
  } else {
    html << "<div class=\"card\"><h2>IR Right (A/B)</h2><small>R unavailable: at least one plugin returned fewer than 2 channels.</small></div>"
         << "<div class=\"card\"><h2>IR Right Delta (A-B)</h2><small>R unavailable: at least one plugin returned fewer than 2 channels.</small></div>";
  }

  if (!summary.irResult.topDifferences.empty()) {
    html << "<div class=\"card full\"><h2>IR Top Differences</h2><ul>";
    for (const auto& d : summary.irResult.topDifferences) {
      html << "<li>sample " << d.sampleIndex << " : |delta| = " << d.absoluteDelta << "</li>";
    }
    html << "</ul></div>";
  }

  html << renderTestSummaryHtml(llmSummary.tests[1])
       << "<div class=\"card full\"><h2>Frequency Response Metrics</h2><table>"
       << "<tr><th>Analysis Signal</th><td>Mono (L+R)/2</td></tr>"
       << "<tr><th>Analysis Reference</th><td>Input White Noise PSD (0 dB center)</td></tr>"
       << "<tr><th>A/B Axis Range</th><td>-24 dB to +24 dB</td></tr>"
       << "<tr><th>Estimated Latency (A-B)</th><td>" << summary.frequencyResult.metrics.estimatedLatencyMs << " ms</td></tr>"
       << "<tr><th>Reported Latency</th><td>A: " << summary.frequencyResult.metrics.pluginLatencySamplesA
       << " samples, B: " << summary.frequencyResult.metrics.pluginLatencySamplesB << " samples</td></tr>"
       << "<tr><th>Applied Alignment Delay</th><td>A: " << summary.frequencyResult.latencyAlignment.appliedDelayA
       << " samples, B: " << summary.frequencyResult.latencyAlignment.appliedDelayB << " samples</td></tr>"
       << "<tr><th>Clamped Latency</th><td>A: " << summary.frequencyResult.latencyAlignment.clampedA
       << " samples, B: " << summary.frequencyResult.latencyAlignment.clampedB << " samples (occurred: "
       << (summary.frequencyResult.latencyAlignment.clampedOccurred ? "yes" : "no") << ")</td></tr>"
       << "<tr><th>Peak |Delta| (dB)</th><td>" << summary.frequencyResult.metrics.peakAbsDeltaDb << "</td></tr>"
       << "<tr><th>Mean |Delta| (dB)</th><td>" << summary.frequencyResult.metrics.meanAbsDeltaDb << "</td></tr>"
       << "</table></div>"
       << "<div class=\"card full\"><h2>Frequency Response (Mono (L+R)/2)</h2>"
       << "<h3>Plugin A vs Plugin B (Reference to Input White Noise, 0 dB Center)</h3>"
       << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--a)\"></span>Plugin A</span>"
       << "<span><span class=\"dot\" style=\"background:var(--b)\"></span>Plugin B</span></div>"
       << "<svg viewBox=\"0 0 " << width << ' ' << height
       << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"Frequency A/B chart\">"
       << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
       << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
       << freqAxisOverlay
       << "<polyline fill=\"none\" stroke=\"var(--a)\" stroke-width=\"1.4\" points=\"" << freqPolyA << "\"/>"
       << "<polyline fill=\"none\" stroke=\"var(--b)\" stroke-width=\"1.4\" points=\"" << freqPolyB << "\"/>"
       << "</svg>"
       << "<h3>Delta (A-B)</h3>"
       << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--d)\"></span>Delta (A-B)</span></div>"
       << "<svg viewBox=\"0 0 " << width << ' ' << height
       << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"Frequency delta chart\">"
       << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
       << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
       << freqAxisOverlay
       << "<polyline fill=\"none\" stroke=\"var(--d)\" stroke-width=\"1.2\" points=\"" << freqPolyD << "\"/>"
       << "</svg></div>";

  if (!summary.frequencyResult.octaveBands.empty()) {
    html << "<div class=\"card full\"><h2>1/3 Octave Summary (20Hz-20kHz)</h2><div class=\"scroll\"><table>"
         << "<tr><th>Band Center (Hz)</th><th>Avg Delta (dB)</th><th>Max Delta (dB)</th></tr>";
    for (const auto& b : summary.frequencyResult.octaveBands) {
      html << "<tr><td>" << b.centerHz << "</td><td>" << b.avgDeltaDb << "</td><td>" << b.maxDeltaDb << "</td></tr>";
    }
    html << "</table></div></div>";
  }

  html << renderTestSummaryHtml(llmSummary.tests[2])
       << "<div class=\"card full\"><h2>Phase Response Metrics</h2><table>"
       << "<tr><th>Analysis Signal</th><td>Mono (L+R)/2</td></tr>"
       << "<tr><th>Analysis Reference</th><td>Input White Noise Transfer Phase</td></tr>"
       << "<tr><th>Phase Axis Range</th><td>-&pi; rad to +&pi; rad</td></tr>"
       << "<tr><th>Estimated Latency (A-B)</th><td>" << summary.phaseResult.metrics.estimatedLatencyMs << " ms</td></tr>"
       << "<tr><th>Reported Latency</th><td>A: " << summary.phaseResult.metrics.pluginLatencySamplesA
       << " samples, B: " << summary.phaseResult.metrics.pluginLatencySamplesB << " samples</td></tr>"
       << "<tr><th>Applied Alignment Delay</th><td>A: " << summary.phaseResult.latencyAlignment.appliedDelayA
       << " samples, B: " << summary.phaseResult.latencyAlignment.appliedDelayB << " samples</td></tr>"
       << "<tr><th>Clamped Latency</th><td>A: " << summary.phaseResult.latencyAlignment.clampedA
       << " samples, B: " << summary.phaseResult.latencyAlignment.clampedB << " samples (occurred: "
       << (summary.phaseResult.latencyAlignment.clampedOccurred ? "yes" : "no") << ")</td></tr>"
       << "<tr><th>Peak |Delta| (rad)</th><td>" << summary.phaseResult.metrics.peakAbsDeltaRad << "</td></tr>"
       << "<tr><th>Mean |Delta| (rad)</th><td>" << summary.phaseResult.metrics.meanAbsDeltaRad << "</td></tr>"
       << "</table></div>"
       << "<div class=\"card full\"><h2>Phase Response (Mono (L+R)/2)</h2>"
       << "<h3>Plugin A vs Plugin B</h3>"
       << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--a)\"></span>Plugin A</span>"
       << "<span><span class=\"dot\" style=\"background:var(--b)\"></span>Plugin B</span></div>"
       << "<svg viewBox=\"0 0 " << width << ' ' << height
       << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"Phase A/B chart\">"
       << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
       << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
       << phaseAxisOverlay
       << "<polyline fill=\"none\" stroke=\"var(--a)\" stroke-width=\"1.4\" points=\"" << phasePolyA << "\"/>"
       << "<polyline fill=\"none\" stroke=\"var(--b)\" stroke-width=\"1.4\" points=\"" << phasePolyB << "\"/>"
       << "</svg>"
       << "<h3>Delta (A-B)</h3>"
       << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--d)\"></span>Delta (A-B)</span></div>"
       << "<svg viewBox=\"0 0 " << width << ' ' << height
       << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"Phase delta chart\">"
       << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
       << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
       << phaseAxisOverlay
       << "<polyline fill=\"none\" stroke=\"var(--d)\" stroke-width=\"1.2\" points=\"" << phasePolyD << "\"/>"
       << "</svg></div>";

  if (!summary.phaseResult.bands.empty()) {
    html << "<div class=\"card full\"><h2>Phase Band Summary (24 log bands, 20Hz-20kHz)</h2><div class=\"scroll\"><table>"
         << "<tr><th>Band Center (Hz)</th><th>Avg Delta (rad)</th><th>Max Delta (rad)</th></tr>";
    for (const auto& b : summary.phaseResult.bands) {
      html << "<tr><td>" << b.centerHz << "</td><td>" << b.avgDeltaRad << "</td><td>" << b.maxDeltaRad << "</td></tr>";
    }
    html << "</table></div></div>";
  }

  html << renderTestSummaryHtml(llmSummary.tests[3])
       << "<div class=\"card full\"><h2>Harmonic Distortion Metrics</h2><table>"
       << "<tr><th>Analysis Signal</th><td>Mono (L+R)/2</td></tr>"
       << "<tr><th>Analysis Reference</th><td>Blackman-Harris FFT (dBFS)</td></tr>"
       << "<tr><th>Frequency Axis Range</th><td>20Hz to 24kHz (log)</td></tr>"
       << "<tr><th>Amplitude Axis Range</th><td>-140 dBFS to 0 dBFS</td></tr>"
       << "<tr><th>Delta Axis Range</th><td>-24 dB to +24 dB (0 dB center)</td></tr>"
       << "<tr><th>Peak |Delta| (dB)</th><td>" << summary.harmonicResult.metrics.peakAbsDeltaDb << "</td></tr>"
       << "<tr><th>Mean |Delta| (dB)</th><td>" << summary.harmonicResult.metrics.meanAbsDeltaDb << "</td></tr>"
       << "</table></div>";

  for (const auto& p : summary.harmonicResult.pitches) {
    const std::string harmonicPolyA = polylineFromHarmonicSpectrum(p.spectrumA, width, height, 20.0, 24000.0, -140.0, 0.0);
    const std::string harmonicPolyB = polylineFromHarmonicSpectrum(p.spectrumB, width, height, 20.0, 24000.0, -140.0, 0.0);
    const std::string harmonicPolyD = polylineFromHarmonicSpectrum(p.delta, width, height, 20.0, 24000.0, -24.0, 24.0);

    html << "<div class=\"card full\"><h2>Harmonic Spectrum (" << p.fundamentalHz << " Hz)</h2><table>"
         << "<tr><th>Analysis Signal</th><td>"
         << (p.analysisSignalMode == "mono_lr_average" ? "Mono (L+R)/2" : "Single Channel") << "</td></tr>"
         << "<tr><th>Reported Latency</th><td>A: " << p.pluginLatencySamplesA << " samples, B: "
         << p.pluginLatencySamplesB << " samples</td></tr>"
         << "<tr><th>Applied Alignment Delay</th><td>A: " << p.latencyAlignment.appliedDelayA
         << " samples, B: " << p.latencyAlignment.appliedDelayB << " samples</td></tr>"
         << "<tr><th>Clamped Latency</th><td>A: " << p.latencyAlignment.clampedA << " samples, B: "
         << p.latencyAlignment.clampedB << " samples (occurred: "
         << (p.latencyAlignment.clampedOccurred ? "yes" : "no") << ")</td></tr>"
         << "<tr><th>Noise Floor</th><td>A: " << p.noiseFloorDbfsA << " dBFS, B: " << p.noiseFloorDbfsB
         << " dBFS, Delta: " << p.noiseFloorDeltaDb << " dB</td></tr>"
         << "</table>"
         << "<h3>Plugin A vs Plugin B</h3>"
         << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--a)\"></span>Plugin A</span>"
         << "<span><span class=\"dot\" style=\"background:var(--b)\"></span>Plugin B</span></div>"
         << "<svg viewBox=\"0 0 " << width << ' ' << height
         << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"Harmonic A/B chart\">"
         << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
         << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
         << harmonicAxisOverlay
         << "<polyline fill=\"none\" stroke=\"var(--a)\" stroke-width=\"1.3\" points=\"" << harmonicPolyA << "\"/>"
         << "<polyline fill=\"none\" stroke=\"var(--b)\" stroke-width=\"1.3\" points=\"" << harmonicPolyB << "\"/>"
         << "</svg>"
         << "<h3>Delta (A-B)</h3>"
         << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--d)\"></span>Delta (A-B)</span></div>"
         << "<svg viewBox=\"0 0 " << width << ' ' << height
         << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"Harmonic delta chart\">"
         << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
         << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
         << harmonicDeltaAxisOverlay
         << "<polyline fill=\"none\" stroke=\"var(--d)\" stroke-width=\"1.2\" points=\"" << harmonicPolyD << "\"/>"
         << "</svg>";

    if (!p.orders.empty()) {
      html << "<h3>Harmonic Orders (1-10)</h3><div class=\"scroll\"><table>"
           << "<tr><th>Order</th><th>Frequency (Hz)</th><th>A (dBFS)</th><th>B (dBFS)</th><th>Delta (dB)</th></tr>";
      for (const auto& o : p.orders) {
        html << "<tr><td>" << o.order << "</td><td>" << o.frequencyHz << "</td><td>" << o.amplitudeDbfsA
             << "</td><td>" << o.amplitudeDbfsB << "</td><td>" << o.deltaDb << "</td></tr>";
      }
      html << "</table></div>";
    }
    html << "</div>";
  }

  html << renderTestSummaryHtml(llmSummary.tests[4])
       << "<div class=\"card full\"><h2>THD / THD+N Metrics</h2><table>"
       << "<tr><th>Analysis Signal</th><td>Mono (L+R)/2</td></tr>"
       << "<tr><th>Analysis Reference</th><td>FFT Fundamental Band Rejection</td></tr>"
       << "<tr><th>X Axis Range</th><td>Input Level -60 dBFS to +6 dBFS</td></tr>"
       << "<tr><th>Y Axis Range</th><td>Distortion % (log, 0.0001 to 100)</td></tr>"
       << "<tr><th>Peak |THD Delta| (%)</th><td>" << summary.thdResult.metrics.peakAbsThdDeltaPercent << "</td></tr>"
       << "<tr><th>Mean |THD Delta| (%)</th><td>" << summary.thdResult.metrics.meanAbsThdDeltaPercent << "</td></tr>"
       << "<tr><th>Peak |THD+N Delta| (%)</th><td>" << summary.thdResult.metrics.peakAbsThdnDeltaPercent << "</td></tr>"
       << "<tr><th>Mean |THD+N Delta| (%)</th><td>" << summary.thdResult.metrics.meanAbsThdnDeltaPercent << "</td></tr>"
       << "</table></div>"
       << "<div class=\"card full\"><h2>THD Sweep (1kHz, Mono (L+R)/2)</h2>"
       << "<h3>THD (A/B)</h3>"
       << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--a)\"></span>Plugin A</span>"
       << "<span><span class=\"dot\" style=\"background:var(--b)\"></span>Plugin B</span></div>"
       << "<svg viewBox=\"0 0 " << width << ' ' << height
       << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"THD A/B chart\">"
       << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
       << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
       << thdAxisOverlay
       << "<polyline fill=\"none\" stroke=\"var(--a)\" stroke-width=\"1.4\" points=\"" << thdPolyA << "\"/>"
       << "<polyline fill=\"none\" stroke=\"var(--b)\" stroke-width=\"1.4\" points=\"" << thdPolyB << "\"/>"
       << "</svg>"
       << "<h3>THD+N (A/B)</h3>"
       << "<small>Fundamental band rejection (FFT)</small>"
       << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--a)\"></span>Plugin A</span>"
       << "<span><span class=\"dot\" style=\"background:var(--b)\"></span>Plugin B</span></div>"
       << "<svg viewBox=\"0 0 " << width << ' ' << height
       << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"THD+N A/B chart\">"
       << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
       << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
       << thdAxisOverlay
       << "<polyline fill=\"none\" stroke=\"var(--a)\" stroke-width=\"1.4\" points=\"" << thdnPolyA << "\"/>"
       << "<polyline fill=\"none\" stroke=\"var(--b)\" stroke-width=\"1.4\" points=\"" << thdnPolyB << "\"/>"
       << "</svg></div>";

  if (!summary.thdResult.segments.empty()) {
    html << "<div class=\"card full\"><h2>THD Segment Summary</h2><div class=\"scroll\"><table>"
         << "<tr><th>Segment</th><th>Input Range (dBFS)</th><th>Points</th><th>Avg THD Delta (%)</th>"
         << "<th>Max |THD Delta| (%)</th><th>Avg THD+N Delta (%)</th><th>Max |THD+N Delta| (%)</th></tr>";
    for (const auto& s : summary.thdResult.segments) {
      html << "<tr><td>" << escapeHtml(s.name) << "</td><td>" << s.startLevelDbfs << " .. " << s.endLevelDbfs
           << "</td><td>" << s.pointCount << "</td><td>" << s.avgThdDeltaPercent << "</td><td>"
           << s.maxAbsThdDeltaPercent << "</td><td>" << s.avgThdnDeltaPercent << "</td><td>"
           << s.maxAbsThdnDeltaPercent << "</td></tr>";
    }
    html << "</table></div></div>";
  }

  html << renderTestSummaryHtml(llmSummary.tests[5])
       << "<div class=\"card full\"><h2>Mono-to-Stereo Width Metrics</h2><table>"
       << "<tr><th>Analysis Signal</th><td>Stereo identical L/R pink noise input</td></tr>"
       << "<tr><th>Analysis Reference</th><td>Width Index (0%=mono, 50%=mid/side equal, 100%=side dominant)</td></tr>"
       << "<tr><th>Reported Latency</th><td>A: " << summary.monoStereoWidthResult.metrics.pluginLatencySamplesA
       << " samples, B: " << summary.monoStereoWidthResult.metrics.pluginLatencySamplesB << " samples</td></tr>"
       << "<tr><th>Applied Alignment Delay</th><td>A: " << summary.monoStereoWidthResult.latencyAlignment.appliedDelayA
       << " samples, B: " << summary.monoStereoWidthResult.latencyAlignment.appliedDelayB << " samples</td></tr>"
       << "<tr><th>Clamped Latency</th><td>A: " << summary.monoStereoWidthResult.latencyAlignment.clampedA
       << " samples, B: " << summary.monoStereoWidthResult.latencyAlignment.clampedB << " samples (occurred: "
       << (summary.monoStereoWidthResult.latencyAlignment.clampedOccurred ? "yes" : "no") << ")</td></tr>"
       << "<tr><th>Peak |Time Delta| (pt)</th><td>"
       << summary.monoStereoWidthResult.metrics.peakAbsTimeDeltaWidthPercent
       << "</td></tr>"
       << "<tr><th>Mean |Time Delta| (pt)</th><td>"
       << summary.monoStereoWidthResult.metrics.meanAbsTimeDeltaWidthPercent
       << "</td></tr>"
       << "<tr><th>Peak |Band Delta| (pt)</th><td>"
       << summary.monoStereoWidthResult.metrics.peakAbsBandDeltaWidthPercent
       << "</td></tr>"
       << "<tr><th>Mean |Band Delta| (pt)</th><td>"
       << summary.monoStereoWidthResult.metrics.meanAbsBandDeltaWidthPercent
       << "</td></tr>"
       << "</table></div>"
       << "<div class=\"card full\"><h2>Mono-to-Stereo Width (Time Domain Width Index)</h2>"
       << "<h3>Plugin A vs Plugin B</h3>"
       << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--a)\"></span>Plugin A</span>"
       << "<span><span class=\"dot\" style=\"background:var(--b)\"></span>Plugin B</span></div>"
       << "<svg viewBox=\"0 0 " << width << ' ' << height
       << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"Mono-to-Stereo width time A/B chart\">"
       << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
       << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
       << widthTimeAxisOverlay
       << "<polyline fill=\"none\" stroke=\"var(--a)\" stroke-width=\"1.3\" points=\"" << widthTimePolyA << "\"/>"
       << "<polyline fill=\"none\" stroke=\"var(--b)\" stroke-width=\"1.3\" points=\"" << widthTimePolyB << "\"/>"
       << "</svg>"
       << "<h3>Delta (A-B)</h3>"
       << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--d)\"></span>Delta (A-B)</span></div>"
       << "<svg viewBox=\"0 0 " << width << ' ' << height
       << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"Mono-to-Stereo width time delta chart\">"
       << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
       << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
       << widthTimeDeltaAxisOverlay
       << "<polyline fill=\"none\" stroke=\"var(--d)\" stroke-width=\"1.2\" points=\"" << widthTimePolyD << "\"/>"
       << "</svg></div>"
       << "<div class=\"card full\"><h2>Mono-to-Stereo Width (Band Width Index)</h2>"
       << "<h3>Plugin A vs Plugin B</h3>"
       << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--a)\"></span>Plugin A</span>"
       << "<span><span class=\"dot\" style=\"background:var(--b)\"></span>Plugin B</span></div>"
       << "<svg viewBox=\"0 0 " << width << ' ' << height
       << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"Mono-to-Stereo width band A/B chart\">"
       << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
       << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
       << widthBandAxisOverlay
       << "<polyline fill=\"none\" stroke=\"var(--a)\" stroke-width=\"1.3\" points=\"" << widthBandPolyA << "\"/>"
       << "<polyline fill=\"none\" stroke=\"var(--b)\" stroke-width=\"1.3\" points=\"" << widthBandPolyB << "\"/>"
       << "</svg>"
       << "<h3>Delta (A-B)</h3>"
       << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--d)\"></span>Delta (A-B)</span></div>"
       << "<svg viewBox=\"0 0 " << width << ' ' << height
       << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"Mono-to-Stereo width band delta chart\">"
       << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
       << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
       << widthBandDeltaAxisOverlay
       << "<polyline fill=\"none\" stroke=\"var(--d)\" stroke-width=\"1.2\" points=\"" << widthBandPolyD << "\"/>"
       << "</svg></div>";

  if (!summary.monoStereoWidthResult.bands.empty()) {
    html << "<div class=\"card full\"><h2>Band Summary (24 log bands, 20Hz-20kHz)</h2><div class=\"scroll\"><table>"
         << "<tr><th>Band Center (Hz)</th><th>Width A (%)</th><th>Width B (%)</th><th>Delta (pt)</th></tr>";
    for (const auto& band : summary.monoStereoWidthResult.bands) {
      html << "<tr><td>" << band.centerHz << "</td><td>" << band.widthPercentA << "</td><td>"
           << band.widthPercentB << "</td><td>" << band.deltaWidthPercent << "</td></tr>";
    }
    html << "</table></div></div>";
  }

  html << renderTestSummaryHtml(llmSummary.tests[6])
       << "<div class=\"card full\"><h2>Pitch Tracking Metrics</h2><table>"
       << "<tr><th>Analysis Signal</th><td>Mono (L+R)/2</td></tr>"
       << "<tr><th>Analysis Reference</th><td>Log Sine Sweep Time-Pitch Tracking (Hz)</td></tr>"
       << "<tr><th>Auto-Tune Access Condition</th><td>Default plugin parameters (no test-specific override)</td></tr>"
       << "<tr><th>Reported Latency</th><td>A: " << summary.pitchResult.metrics.pluginLatencySamplesA
       << " samples, B: " << summary.pitchResult.metrics.pluginLatencySamplesB << " samples</td></tr>"
       << "<tr><th>Applied Alignment Delay</th><td>A: " << summary.pitchResult.latencyAlignment.appliedDelayA
       << " samples, B: " << summary.pitchResult.latencyAlignment.appliedDelayB << " samples</td></tr>"
       << "<tr><th>Clamped Latency</th><td>A: " << summary.pitchResult.latencyAlignment.clampedA
       << " samples, B: " << summary.pitchResult.latencyAlignment.clampedB << " samples (occurred: "
       << (summary.pitchResult.latencyAlignment.clampedOccurred ? "yes" : "no") << ")</td></tr>"
       << "<tr><th>Estimated Latency (A-B)</th><td>" << summary.pitchResult.metrics.estimatedLatencyMs << " ms</td></tr>"
       << "<tr><th>Mean |A-Input| (Hz)</th><td>" << summary.pitchResult.metrics.meanAbsErrorHzA << "</td></tr>"
       << "<tr><th>Mean |B-Input| (Hz)</th><td>" << summary.pitchResult.metrics.meanAbsErrorHzB << "</td></tr>"
       << "<tr><th>Mean |A-B| (Hz)</th><td>" << summary.pitchResult.metrics.meanAbsDeltaHz << "</td></tr>"
       << "<tr><th>Peak |A-B| (Hz)</th><td>" << summary.pitchResult.metrics.peakAbsDeltaHz << "</td></tr>"
       << "<tr><th>Valid Frame Rate</th><td>A: " << summary.pitchResult.metrics.validFrameRateA
       << ", B: " << summary.pitchResult.metrics.validFrameRateB << "</td></tr>"
       << "</table></div>"
       << "<div class=\"card full\"><h2>Pitch Tracking Curve (Hz)</h2>"
       << "<h3>Input vs Plugin A vs Plugin B</h3>"
       << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:#8b95a8\"></span>Input Sweep</span>"
       << "<span><span class=\"dot\" style=\"background:var(--a)\"></span>Plugin A</span>"
       << "<span><span class=\"dot\" style=\"background:var(--b)\"></span>Plugin B</span></div>"
       << "<svg viewBox=\"0 0 " << width << ' ' << height
       << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"Pitch tracking curve chart\">"
       << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
       << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
       << pitchAxisOverlay
       << "<polyline fill=\"none\" stroke=\"#8b95a8\" stroke-width=\"1.1\" points=\"" << pitchPolyInput << "\"/>"
       << "<polyline fill=\"none\" stroke=\"var(--a)\" stroke-width=\"1.3\" points=\"" << pitchPolyA << "\"/>"
       << "<polyline fill=\"none\" stroke=\"var(--b)\" stroke-width=\"1.3\" points=\"" << pitchPolyB << "\"/>"
       << "</svg></div>";

  html << renderTestSummaryHtml(llmSummary.tests[7])
       << "<div class=\"card full\"><h2>Dynamics Characteristics Metrics</h2><table>"
       << "<tr><th>Analysis Signal</th><td>Mono (L+R)/2</td></tr>"
       << "<tr><th>Analysis Reference</th><td>Step Sine RMS I/O Curve</td></tr>"
       << "<tr><th>Reported Latency</th><td>A: " << summary.dynamicsResult.metrics.pluginLatencySamplesA
       << " samples, B: " << summary.dynamicsResult.metrics.pluginLatencySamplesB << " samples</td></tr>"
       << "<tr><th>Applied Alignment Delay</th><td>A: " << summary.dynamicsResult.latencyAlignment.appliedDelayA
       << " samples, B: " << summary.dynamicsResult.latencyAlignment.appliedDelayB << " samples</td></tr>"
       << "<tr><th>Clamped Latency</th><td>A: " << summary.dynamicsResult.latencyAlignment.clampedA
       << " samples, B: " << summary.dynamicsResult.latencyAlignment.clampedB << " samples (occurred: "
       << (summary.dynamicsResult.latencyAlignment.clampedOccurred ? "yes" : "no") << ")</td></tr>"
       << "<tr><th>Estimated Threshold</th><td>A: " << summary.dynamicsResult.metrics.estimatedA.thresholdDbfs
       << " dBFS, B: " << summary.dynamicsResult.metrics.estimatedB.thresholdDbfs
       << " dBFS, Delta: " << summary.dynamicsResult.metrics.thresholdDeltaDb << " dB</td></tr>"
       << "<tr><th>Estimated Ratio</th><td>A: " << summary.dynamicsResult.metrics.estimatedA.ratio
       << ", B: " << summary.dynamicsResult.metrics.estimatedB.ratio
       << ", Delta: " << summary.dynamicsResult.metrics.ratioDelta << "</td></tr>"
       << "<tr><th>Estimated Knee Width</th><td>A: " << summary.dynamicsResult.metrics.estimatedA.kneeWidthDb
       << " dB, B: " << summary.dynamicsResult.metrics.estimatedB.kneeWidthDb
       << " dB, Delta: " << summary.dynamicsResult.metrics.kneeWidthDeltaDb << " dB</td></tr>"
       << "<tr><th>Peak |I/O Delta|</th><td>" << summary.dynamicsResult.metrics.peakAbsOutputDeltaDb << " dB</td></tr>"
       << "<tr><th>Mean |I/O Delta|</th><td>" << summary.dynamicsResult.metrics.meanAbsOutputDeltaDb << " dB</td></tr>"
       << "<tr><th>Peak |GR Delta|</th><td>" << summary.dynamicsResult.metrics.peakAbsGainReductionDeltaDb
       << " dB</td></tr>"
       << "<tr><th>Mean |GR Delta|</th><td>" << summary.dynamicsResult.metrics.meanAbsGainReductionDeltaDb
       << " dB</td></tr>"
       << "</table></div>"
       << "<div class=\"card full\"><h2>Dynamics I/O Curve (Mono (L+R)/2)</h2>"
       << "<h3>Plugin A vs Plugin B</h3>"
       << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--a)\"></span>Plugin A</span>"
       << "<span><span class=\"dot\" style=\"background:var(--b)\"></span>Plugin B</span></div>"
       << "<svg viewBox=\"0 0 " << width << ' ' << height
       << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"Dynamics IO A/B chart\">"
       << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
       << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
       << dynamicsIoAxis
       << "<polyline fill=\"none\" stroke=\"var(--a)\" stroke-width=\"1.4\" points=\"" << dynamicsIoPolyA << "\"/>"
       << "<polyline fill=\"none\" stroke=\"var(--b)\" stroke-width=\"1.4\" points=\"" << dynamicsIoPolyB << "\"/>"
       << "</svg>"
       << "<h3>Delta (A-B)</h3>"
       << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--d)\"></span>Delta (A-B)</span></div>"
       << "<svg viewBox=\"0 0 " << width << ' ' << height
       << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"Dynamics IO delta chart\">"
       << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
       << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
       << dynamicsDeltaAxis
       << "<polyline fill=\"none\" stroke=\"var(--d)\" stroke-width=\"1.2\" points=\"" << dynamicsIoPolyD << "\"/>"
       << "</svg></div>";

  html << renderTestSummaryHtml(llmSummary.tests[8])
       << "<div class=\"card full\"><h2>Time Response Metrics</h2><table>"
       << "<tr><th>Analysis Signal</th><td>Mono (L+R)/2</td></tr>"
       << "<tr><th>Analysis Reference</th><td>Tone Burst Envelope Time Response</td></tr>"
       << "<tr><th>Reported Latency</th><td>A: " << summary.timeResponseResult.metrics.pluginLatencySamplesA
       << " samples, B: " << summary.timeResponseResult.metrics.pluginLatencySamplesB << " samples</td></tr>"
       << "<tr><th>Applied Alignment Delay</th><td>A: "
       << summary.timeResponseResult.latencyAlignment.appliedDelayA << " samples, B: "
       << summary.timeResponseResult.latencyAlignment.appliedDelayB << " samples</td></tr>"
       << "<tr><th>Clamped Latency</th><td>A: " << summary.timeResponseResult.latencyAlignment.clampedA
       << " samples, B: " << summary.timeResponseResult.latencyAlignment.clampedB << " samples (occurred: "
       << (summary.timeResponseResult.latencyAlignment.clampedOccurred ? "yes" : "no") << ")</td></tr>"
       << "<tr><th>Residual Latency (A-B)</th><td>" << summary.timeResponseResult.metrics.residualLatencyMs
       << " ms</td></tr>"
       << "<tr><th>Attack Time</th><td>A: " << summary.timeResponseResult.metrics.attackMsA
       << " ms, B: " << summary.timeResponseResult.metrics.attackMsB << " ms, Delta: "
       << summary.timeResponseResult.metrics.attackDeltaMs << " ms</td></tr>"
       << "<tr><th>Release Time</th><td>A: " << summary.timeResponseResult.metrics.releaseMsA
       << " ms, B: " << summary.timeResponseResult.metrics.releaseMsB << " ms, Delta: "
       << summary.timeResponseResult.metrics.releaseDeltaMs << " ms</td></tr>"
       << "<tr><th>Post-release Residual</th><td>A: " << summary.timeResponseResult.metrics.postReleaseResidualDbA
       << " dB, B: " << summary.timeResponseResult.metrics.postReleaseResidualDbB << " dB, Delta: "
       << summary.timeResponseResult.metrics.postReleaseResidualDeltaDb << " dB</td></tr>"
       << "</table></div>"
       << "<div class=\"card full\"><h2>Time Response Envelope (Mono (L+R)/2)</h2>"
       << "<h3>Plugin A vs Plugin B</h3>"
       << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--a)\"></span>Plugin A</span>"
       << "<span><span class=\"dot\" style=\"background:var(--b)\"></span>Plugin B</span></div>"
       << "<svg viewBox=\"0 0 " << width << ' ' << height
       << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"Time response envelope A/B chart\">"
       << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
       << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
       << timeResponseAxis
       << "<polyline fill=\"none\" stroke=\"var(--a)\" stroke-width=\"1.3\" points=\"" << timeResponsePolyA << "\"/>"
       << "<polyline fill=\"none\" stroke=\"var(--b)\" stroke-width=\"1.3\" points=\"" << timeResponsePolyB << "\"/>"
       << "</svg>"
       << "<h3>Delta (A-B)</h3>"
       << "<div class=\"legend\"><span><span class=\"dot\" style=\"background:var(--d)\"></span>Delta (A-B)</span></div>"
       << "<svg viewBox=\"0 0 " << width << ' ' << height
       << "\" width=\"100%\" height=\"320\" role=\"img\" aria-label=\"Time response envelope delta chart\">"
       << "<rect x=\"0\" y=\"0\" width=\"" << width << "\" height=\"" << height
       << "\" fill=\"#fbfdff\" stroke=\"#dde7f5\"/>"
       << timeResponseDeltaAxis
       << "<polyline fill=\"none\" stroke=\"var(--d)\" stroke-width=\"1.2\" points=\"" << timeResponsePolyD << "\"/>"
       << "</svg></div>";

  if (!summary.appliedParameters.empty()) {
    html << "<div class=\"card\"><h2>Applied Parameters</h2><ul>";
    for (const auto& p : summary.appliedParameters) {
      html << "<li>Plugin " << escapeHtml(p.plugin) << " / " << escapeHtml(p.name) << " (id " << p.parameterId
           << ") = " << p.normalized;
      if (!p.displayInput.empty()) {
        html << " (input: " << escapeHtml(p.displayInput) << ")";
      }
      html << "</li>";
    }
    html << "</ul></div>";
  }

  if (!summary.warnings.empty()) {
    html << "<div class=\"card full\"><h2>Warnings</h2><ul>";
    for (const auto& w : summary.warnings) {
      html << "<li>" << escapeHtml(w) << "</li>";
    }
    html << "</ul></div>";
  }

  html << "<script id=\"vstcompare-json\" type=\"application/json\">" << json << "</script>";
  html << "</div></body></html>";
  return html.str();
}

}  // namespace vstcompare
