#include "vstcompare/vst3_plugin_host.hpp"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/funknownimpl.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace Steinberg {
FUnknown* gStandardPluginContext = new Vst::HostApplication();
}

namespace vstcompare {
namespace {
constexpr int kParameterScanRetryCount = 3;
constexpr int kParameterScanRetryWaitMs = 40;
constexpr int kGuiInitSleepStepMs = 5;
constexpr int kDefaultVisibleGuiTimeoutMs = 30000;
constexpr int kEnumDisplayListMaxSteps = 64;

class HostComponentHandler : public Steinberg::Vst::IComponentHandler {
public:
  HostComponentHandler(std::atomic<Steinberg::int32>* pendingRestartFlags,
                       const std::function<void(const std::string&)>& logFn)
      : pendingRestartFlags_(pendingRestartFlags), logFn_(logFn) {}

  Steinberg::tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID) override { return Steinberg::kResultOk; }
  Steinberg::tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue) override {
    return Steinberg::kResultOk;
  }
  Steinberg::tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID) override { return Steinberg::kResultOk; }
  Steinberg::tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override {
    if (pendingRestartFlags_) {
      pendingRestartFlags_->fetch_or(flags, std::memory_order_release);
    }
    if (logFn_) {
      logFn_("parameter scan stage: restartComponent(flags=" + std::to_string(flags) + " " +
             restartFlagsToString(flags) + ")");
    }
    return Steinberg::kResultOk;
  }
  DECLARE_FUNKNOWN_METHODS

private:
  static std::string restartFlagsToString(Steinberg::int32 flags) {
    if (flags == 0) return "[none]";
    std::vector<std::string> names;
    auto append = [&](Steinberg::int32 bit, const char* name) {
      if ((flags & bit) != 0) names.emplace_back(name);
    };
    append(Steinberg::Vst::kReloadComponent, "kReloadComponent");
    append(Steinberg::Vst::kIoChanged, "kIoChanged");
    append(Steinberg::Vst::kParamValuesChanged, "kParamValuesChanged");
    append(Steinberg::Vst::kLatencyChanged, "kLatencyChanged");
    append(Steinberg::Vst::kParamTitlesChanged, "kParamTitlesChanged");
    append(Steinberg::Vst::kMidiCCAssignmentChanged, "kMidiCCAssignmentChanged");
    if (names.empty()) return "[unknown]";
    std::string out = "[";
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (i > 0) out += "|";
      out += names[i];
    }
    out += "]";
    return out;
  }

  std::atomic<Steinberg::int32>* pendingRestartFlags_ = nullptr;
  std::function<void(const std::string&)> logFn_;
};

IMPLEMENT_FUNKNOWN_METHODS(HostComponentHandler, Steinberg::Vst::IComponentHandler, Steinberg::Vst::IComponentHandler::iid)

class HostPlugFrame : public Steinberg::IPlugFrame {
public:
  explicit HostPlugFrame(const std::function<void(const std::string&)>& logFn) : logFn_(logFn) {}

  Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override {
    if (logFn_) {
      if (newSize) {
        logFn_("parameter scan stage: guiInit.resizeView(" + std::to_string(newSize->left) + "," +
               std::to_string(newSize->top) + "," + std::to_string(newSize->right) + "," +
               std::to_string(newSize->bottom) + ")");
      } else {
        logFn_("parameter scan stage: guiInit.resizeView(null)");
      }
    }
    if (!view || !newSize) {
      return Steinberg::kInvalidArgument;
    }
    return view->onSize(newSize);
  }

  DECLARE_FUNKNOWN_METHODS

private:
  std::function<void(const std::string&)> logFn_;
};

IMPLEMENT_FUNKNOWN_METHODS(HostPlugFrame, Steinberg::IPlugFrame, Steinberg::IPlugFrame::iid)

class SimpleMemoryStream : public Steinberg::IBStream {
public:
  SimpleMemoryStream() = default;
  std::size_t size() const { return data_.size(); }

  Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 numBytes,
                                     Steinberg::int32* numBytesRead = nullptr) override {
    if (numBytes < 0) return Steinberg::kInvalidArgument;
    const std::size_t remaining = (cursor_ <= data_.size()) ? (data_.size() - cursor_) : 0;
    const std::size_t toRead = std::min<std::size_t>(static_cast<std::size_t>(numBytes), remaining);
    if (toRead > 0 && buffer) {
      std::memcpy(buffer, data_.data() + cursor_, toRead);
      cursor_ += toRead;
    }
    if (numBytesRead) *numBytesRead = static_cast<Steinberg::int32>(toRead);
    return Steinberg::kResultOk;
  }

  Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 numBytes,
                                      Steinberg::int32* numBytesWritten = nullptr) override {
    if (numBytes < 0) return Steinberg::kInvalidArgument;
    const std::size_t count = static_cast<std::size_t>(numBytes);
    if (cursor_ + count > data_.size()) {
      data_.resize(cursor_ + count);
    }
    if (count > 0 && buffer) {
      std::memcpy(data_.data() + cursor_, buffer, count);
      cursor_ += count;
    }
    if (numBytesWritten) *numBytesWritten = static_cast<Steinberg::int32>(count);
    return Steinberg::kResultOk;
  }

  Steinberg::tresult PLUGIN_API seek(Steinberg::int64 pos, Steinberg::int32 mode,
                                     Steinberg::int64* result = nullptr) override {
    std::size_t next = cursor_;
    if (mode == Steinberg::IBStream::kIBSeekSet) {
      next = static_cast<std::size_t>(std::max<Steinberg::int64>(0, pos));
    } else if (mode == Steinberg::IBStream::kIBSeekCur) {
      const Steinberg::int64 value = static_cast<Steinberg::int64>(cursor_) + pos;
      next = static_cast<std::size_t>(std::max<Steinberg::int64>(0, value));
    } else if (mode == Steinberg::IBStream::kIBSeekEnd) {
      const Steinberg::int64 value = static_cast<Steinberg::int64>(data_.size()) + pos;
      next = static_cast<std::size_t>(std::max<Steinberg::int64>(0, value));
    } else {
      return Steinberg::kInvalidArgument;
    }
    cursor_ = next;
    if (result) *result = static_cast<Steinberg::int64>(cursor_);
    return Steinberg::kResultOk;
  }

  Steinberg::tresult PLUGIN_API tell(Steinberg::int64* pos) override {
    if (!pos) return Steinberg::kInvalidArgument;
    *pos = static_cast<Steinberg::int64>(cursor_);
    return Steinberg::kResultOk;
  }

  DECLARE_FUNKNOWN_METHODS

private:
  std::vector<uint8_t> data_;
  std::size_t cursor_ = 0;
};

IMPLEMENT_FUNKNOWN_METHODS(SimpleMemoryStream, Steinberg::IBStream, Steinberg::IBStream::iid)

std::string uidToString(const VST3::UID& uid) {
  return uid.toString();
}

std::string toUtf8(const Steinberg::Vst::TChar* text) {
  return Steinberg::Vst::StringConvert::convert(text);
}

bool isOkOrTrue(Steinberg::tresult r) {
  return r == Steinberg::kResultOk || r == Steinberg::kResultTrue;
}

std::string tresultToString(Steinberg::tresult r) {
  if (r == Steinberg::kResultOk) return "kResultOk";
  if (r == Steinberg::kResultTrue) return "kResultTrue";
  if (r == Steinberg::kResultFalse) return "kResultFalse";
  if (r == Steinberg::kNotImplemented) return "kNotImplemented";
  if (r == Steinberg::kInvalidArgument) return "kInvalidArgument";
  if (r == Steinberg::kNoInterface) return "kNoInterface";
  return std::to_string(static_cast<int32_t>(r));
}

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

bool parseNormalizedText(const std::string& text, double& normalized) {
  if (text.empty()) return false;
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (!end || *end != '\0') return false;
  if (value < 0.0 || value > 1.0) return false;
  normalized = value;
  return true;
}

std::string normalizeCaseFoldAscii(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (char ch : text) {
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

bool equalDisplayLabel(const std::string& a, const std::string& b) {
  return normalizeCaseFoldAscii(trimCopy(a)) == normalizeCaseFoldAscii(trimCopy(b));
}

void appendIssue(std::string& out, const std::string& issue) {
  if (issue.empty()) return;
  if (!out.empty()) out += " ";
  out += issue;
}

std::string normalizeClassIdHex(const std::string& text) {
  std::string out;
  out.reserve(32);
  for (char ch : text) {
    if (std::isxdigit(static_cast<unsigned char>(ch)) != 0) {
      out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
  }
  return out;
}

std::string restartFlagsToSummary(Steinberg::int32 flags) {
  if (flags == 0) return std::string("none");
  std::vector<std::string> names;
  auto add = [&](Steinberg::int32 bit, const char* name) {
    if ((flags & bit) != 0) names.emplace_back(name);
  };
  add(Steinberg::Vst::kReloadComponent, "kReloadComponent");
  add(Steinberg::Vst::kParamValuesChanged, "kParamValuesChanged");
  add(Steinberg::Vst::kParamTitlesChanged, "kParamTitlesChanged");
  add(Steinberg::Vst::kIoChanged, "kIoChanged");
  add(Steinberg::Vst::kLatencyChanged, "kLatencyChanged");
  add(Steinberg::Vst::kMidiCCAssignmentChanged, "kMidiCCAssignmentChanged");
  if (names.empty()) return std::string("other");
  std::string out;
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (i > 0) out += "|";
    out += names[i];
  }
  return out;
}

std::vector<PluginParameter> enumerateParameters(Steinberg::Vst::IEditController* controller,
                                                 std::vector<std::string>& diagnostics) {
  std::vector<PluginParameter> out;
  if (!controller) {
    diagnostics.push_back("parameter scan: controllerMissing");
    return out;
  }

  const int32_t paramCount = controller->getParameterCount();
  if (paramCount < 0) {
    diagnostics.push_back("parameter scan: invalidParameterCount");
    return out;
  }

  std::unordered_set<uint32_t> ids;
  out.reserve(static_cast<std::size_t>(paramCount));
  for (int32_t i = 0; i < paramCount; ++i) {
    Steinberg::Vst::ParameterInfo pi {};
    if (controller->getParameterInfo(i, pi) != Steinberg::kResultOk) {
      diagnostics.push_back("parameter scan: getParameterInfoFailed(index=" + std::to_string(i) + ")");
      continue;
    }

    PluginParameter p;
    p.id = pi.id;
    p.title = toUtf8(pi.title);
    p.units = toUtf8(pi.units);
    p.defaultNormalized = pi.defaultNormalizedValue;
    p.stepCount = pi.stepCount;
    if (p.title.empty()) {
      p.title = "Param-" + std::to_string(p.id);
      diagnostics.push_back("parameter scan: emptyTitle(id=" + std::to_string(p.id) + ")");
    }
    if (!ids.insert(p.id).second) {
      diagnostics.push_back("parameter scan: duplicateId(id=" + std::to_string(p.id) + ")");
      continue;
    }

    Steinberg::Vst::String128 display {};
    if (controller->getParamStringByValue(pi.id, pi.defaultNormalizedValue, display) == Steinberg::kResultTrue) {
      p.display = toUtf8(display);
    }

    if (p.stepCount > 0 && p.stepCount <= kEnumDisplayListMaxSteps) {
      std::unordered_set<std::string> seenOptions;
      p.enumDisplayOptions.reserve(static_cast<std::size_t>(p.stepCount + 1));
      for (int32_t step = 0; step <= p.stepCount; ++step) {
        const double normalized = static_cast<double>(step) / static_cast<double>(p.stepCount);
        Steinberg::Vst::String128 optionText {};
        const auto optionRes = controller->getParamStringByValue(pi.id, normalized, optionText);
        if (optionRes != Steinberg::kResultOk && optionRes != Steinberg::kResultTrue) {
          continue;
        }
        const std::string option = trimCopy(toUtf8(optionText));
        if (option.empty()) continue;
        if (seenOptions.insert(option).second) {
          p.enumDisplayOptions.push_back(option);
        }
      }
    }

    out.push_back(std::move(p));
  }
  return out;
}

}  // namespace

struct Vst3PluginHost::Impl {
  int sampleRate = 48000;
  int blockSize = 512;
  uint32_t latencySamples = 0;

  PluginMetadata metadata;
  std::vector<PluginParameter> parameters;

  VST3::Hosting::Module::Ptr module;
  Steinberg::IPtr<Steinberg::Vst::PlugProvider> plugProvider;
  Steinberg::IPtr<Steinberg::Vst::IComponent> component;
  Steinberg::IPtr<Steinberg::Vst::IEditController> controller;
  Steinberg::FUnknownPtr<Steinberg::Vst::IAudioProcessor> processor;
  Steinberg::Vst::HostProcessData processData;
  Steinberg::Vst::ParameterChanges parameterChanges;
  std::map<Steinberg::Vst::ParamID, Steinberg::Vst::ParamValue> stagedParameters;
  std::vector<Steinberg::Vst::Sample32> silentChannel;
  Steinberg::IPtr<HostComponentHandler> componentHandler;
  bool isActive = false;
  bool isProcessing = false;
  std::string parameterScanSummary;
  std::vector<std::string> diagnostics;
  std::atomic<Steinberg::int32> pendingRestartFlags {0};
  Steinberg::int32 observedRestartFlags = 0;
  std::string requestedClassId;
  std::string requestedClassIdNormalized;
  bool reloadRequestedDuringLoad = false;
#if defined(_WIN32)
  struct HeldFallbackEditor {
    Steinberg::IPtr<Steinberg::IPlugView> view;
    Steinberg::IPtr<HostPlugFrame> frame;
    HWND hwnd = nullptr;
    bool attached = false;
  };
  HeldFallbackEditor heldFallbackEditor;
  bool keepEditorOpenUntilShutdown = false;
#endif
  std::mutex diagnosticsMutex;

  Steinberg::int32 consumeRestartFlags() {
    const Steinberg::int32 flags = pendingRestartFlags.exchange(0, std::memory_order_acq_rel);
    if (flags != 0) {
      observedRestartFlags |= flags;
    }
    return flags;
  }

  bool setProcessingState(bool enabled, std::string* warningOrError = nullptr, const char* stage = "setProcessing") {
    if (!processor) return true;
    if (enabled == isProcessing) return true;
    const Steinberg::tresult res = processor->setProcessing(enabled);
    if (!isOkOrTrue(res) && !(res == Steinberg::kNotImplemented && !enabled) && !(res == Steinberg::kResultFalse && !enabled)) {
      if (warningOrError) {
        appendIssue(*warningOrError, std::string(stage) + ": setProcessing(" + (enabled ? "true" : "false") + ") failed.");
      }
      addDiagnostic("parameter scan issue: " + std::string(stage) + ".setProcessing(" + (enabled ? "true" : "false") +
                    ") result=" + tresultToString(res));
      return false;
    }
    isProcessing = enabled;
    return true;
  }

  bool setActiveState(bool enabled, std::string* warningOrError = nullptr, const char* stage = "setActive") {
    if (!component) return true;
    if (enabled == isActive) return true;
    const Steinberg::tresult res = component->setActive(enabled);
    if (!isOkOrTrue(res)) {
      if (warningOrError) {
        appendIssue(*warningOrError, std::string(stage) + ": setActive(" + (enabled ? "true" : "false") + ") failed.");
      }
      addDiagnostic("parameter scan issue: " + std::string(stage) + ".setActive(" + (enabled ? "true" : "false") +
                    ") result=" + tresultToString(res));
      return false;
    }
    isActive = enabled;
    return true;
  }

  bool configureProcessingGraph(std::string& warningOrError, const char* actionTag) {
    if (!component || !processor) {
      appendIssue(warningOrError, std::string(actionTag) + ": missing component or processor.");
      return false;
    }

    const int32_t inBusCount = component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput);
    const int32_t outBusCount = component->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput);
    addDiagnostic("parameter scan stage: " + std::string(actionTag) + ".busCounts(input=" + std::to_string(inBusCount) +
                  ", output=" + std::to_string(outBusCount) + ")");
    if (inBusCount <= 0 || outBusCount <= 0) {
      appendIssue(warningOrError, std::string(actionTag) + ": plugin has no active audio input/output bus.");
      return false;
    }

    for (int32_t i = 0; i < inBusCount; ++i) {
      const Steinberg::tresult busRes =
          component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, i, true);
      addDiagnostic("parameter scan stage: " + std::string(actionTag) + ".activateBus(input#" + std::to_string(i) +
                    ", result=" + tresultToString(busRes) + ")");
    }
    for (int32_t i = 0; i < outBusCount; ++i) {
      const Steinberg::tresult busRes =
          component->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, i, true);
      addDiagnostic("parameter scan stage: " + std::string(actionTag) + ".activateBus(output#" + std::to_string(i) +
                    ", result=" + tresultToString(busRes) + ")");
    }

    processData.unprepare();
    if (!processData.prepare(*component, 0, Steinberg::Vst::kSample32)) {
      appendIssue(warningOrError, std::string(actionTag) + ": processData.prepare failed.");
      return false;
    }
    addDiagnostic("parameter scan stage: " + std::string(actionTag) + ".processData.prepare(ok)");

    Steinberg::Vst::ProcessSetup setup;
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = blockSize;
    setup.sampleRate = sampleRate;
    const Steinberg::tresult setupRes = processor->setupProcessing(setup);
    addDiagnostic("parameter scan stage: " + std::string(actionTag) + ".setupProcessing(result=" + tresultToString(setupRes) + ")");
    if (!isOkOrTrue(setupRes)) {
      appendIssue(warningOrError, std::string(actionTag) + ": setupProcessing failed.");
      return false;
    }

    if (!setActiveState(true, &warningOrError, actionTag)) {
      return false;
    }
    if (!setProcessingState(true, &warningOrError, actionTag)) {
      return false;
    }

    latencySamples = processor->getLatencySamples();
    addDiagnostic("parameter scan stage: " + std::string(actionTag) + ".latencySamples=" + std::to_string(latencySamples));
    return true;
  }

  bool applyRestartFlags(Steinberg::int32 flags, bool duringLoad, std::string& warningOrError) {
    if (flags == 0) {
      return true;
    }

    addDiagnostic("parameter scan stage: restartComponent(observed=" + restartFlagsToSummary(flags) + ")");
    if ((flags & Steinberg::Vst::kReloadComponent) != 0) {
      reloadRequestedDuringLoad = true;
      addDiagnostic("parameter scan stage: action=reload requested");
      return false;
    }

    if ((flags & Steinberg::Vst::kParamTitlesChanged) != 0) {
      addDiagnostic("parameter scan stage: action=rescan parameter titles");
    }
    if ((flags & Steinberg::Vst::kParamValuesChanged) != 0) {
      addDiagnostic("parameter scan stage: action=refresh parameter values");
    }

    if ((flags & Steinberg::Vst::kIoChanged) != 0) {
      addDiagnostic("parameter scan stage: action=io reconfigure");
      if (!setProcessingState(false, &warningOrError, "restart(ioChanged)") ||
          !setActiveState(false, &warningOrError, "restart(ioChanged)") ||
          !configureProcessingGraph(warningOrError, "restart(ioChanged)")) {
        appendIssue(warningOrError, "restart(ioChanged): failed to reconfigure processing graph.");
        return false;
      }
    }

    if ((flags & Steinberg::Vst::kLatencyChanged) != 0) {
      addDiagnostic("parameter scan stage: action=latency refresh");
      if (processor) {
        latencySamples = processor->getLatencySamples();
        addDiagnostic("parameter scan stage: action=latencySamplesUpdated(" + std::to_string(latencySamples) + ")");
      }
      if (!duringLoad && isActive && !isProcessing) {
        setProcessingState(true, nullptr, "restart(latencyChanged)");
      }
    }
    return true;
  }

  void pumpMessagesOnce() {
#if defined(_WIN32)
    MSG msg {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
#endif
  }

  bool rescanParameters(const std::string& stageLabel, std::size_t& outCount) {
    std::vector<std::string> enumerateDiagnostics;
    const int32_t rawCount = controller ? controller->getParameterCount() : -1;
    addDiagnostic("parameter scan stage: getParameterCount(" + stageLabel + "=" + std::to_string(rawCount) + ")");
    parameters = enumerateParameters(controller, enumerateDiagnostics);
    for (const auto& d : enumerateDiagnostics) {
      addDiagnostic(d);
    }
    outCount = parameters.size();
    addDiagnostic("parameter scan stage: " + stageLabel + ".count=" + std::to_string(outCount));
    return true;
  }

#if defined(_WIN32)
  void closeHeldFallbackEditor(const std::string& stagePrefix) {
    if (heldFallbackEditor.view) {
      if (heldFallbackEditor.attached) {
        const Steinberg::tresult removedRes = heldFallbackEditor.view->removed();
        addDiagnostic("parameter scan stage: " + stagePrefix + ".removed(result=" + tresultToString(removedRes) + ")");
      }
      const Steinberg::tresult clearFrameRes = heldFallbackEditor.view->setFrame(nullptr);
      addDiagnostic("parameter scan stage: " + stagePrefix + ".setFrame(clear,result=" +
                    tresultToString(clearFrameRes) + ")");
    }
    if (heldFallbackEditor.hwnd && IsWindow(heldFallbackEditor.hwnd)) {
      DestroyWindow(heldFallbackEditor.hwnd);
    }
    heldFallbackEditor.view.reset();
    heldFallbackEditor.frame.reset();
    heldFallbackEditor.hwnd = nullptr;
    heldFallbackEditor.attached = false;
  }

  bool attachVisibleFallbackEditor(const std::string& stagePrefix, std::string& warningOrError) {
    warningOrError.clear();
    closeHeldFallbackEditor(stagePrefix);

    if (!controller) {
      appendIssue(warningOrError, stagePrefix + ": controller missing.");
      addDiagnostic("parameter scan issue: " + stagePrefix + ".controllerMissing");
      return false;
    }

    Steinberg::IPtr<Steinberg::IPlugView> view =
        Steinberg::owned(controller->createView(Steinberg::Vst::ViewType::kEditor));
    addDiagnostic("parameter scan stage: " + stagePrefix + ".createView(result=" +
                  std::string(view ? "ok" : "null") + ")");
    if (!view) {
      appendIssue(warningOrError, stagePrefix + ": createView returned null.");
      addDiagnostic("parameter scan issue: " + stagePrefix + ".createViewFailed");
      return false;
    }

    const Steinberg::tresult platformRes = view->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND);
    addDiagnostic("parameter scan stage: " + stagePrefix + ".platform(HWND)=" + tresultToString(platformRes));
    if (!isOkOrTrue(platformRes)) {
      appendIssue(warningOrError, stagePrefix + ": HWND platform not supported.");
      addDiagnostic("parameter scan issue: " + stagePrefix + ".platformNotSupported(HWND)");
      return false;
    }

    Steinberg::ViewRect viewRect {};
    const Steinberg::tresult getSizeRes = view->getSize(&viewRect);
    addDiagnostic("parameter scan stage: " + stagePrefix + ".getSize(result=" + tresultToString(getSizeRes) + ")");
    int width = 1024;
    int height = 768;
    if (isOkOrTrue(getSizeRes)) {
      width = std::max(1, viewRect.getWidth());
      height = std::max(1, viewRect.getHeight());
      addDiagnostic("parameter scan stage: " + stagePrefix + ".initialRect(" + std::to_string(viewRect.left) + "," +
                    std::to_string(viewRect.top) + "," + std::to_string(viewRect.right) + "," +
                    std::to_string(viewRect.bottom) + ")");
    }

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"STATIC", L"VSTCompare Parameter Scan Fallback",
                                WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) {
      appendIssue(warningOrError, stagePrefix + ": failed to create host window.");
      addDiagnostic("parameter scan issue: " + stagePrefix + ".hostWindowCreateFailed");
      return false;
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    Steinberg::IPtr<HostPlugFrame> frame = Steinberg::owned(new HostPlugFrame([this, stagePrefix](const std::string& line) {
      addDiagnostic("parameter scan stage: " + stagePrefix + "." + line);
    }));

    const Steinberg::tresult setFrameRes = view->setFrame(frame);
    addDiagnostic("parameter scan stage: " + stagePrefix + ".setFrame(result=" + tresultToString(setFrameRes) + ")");
    if (!isOkOrTrue(setFrameRes)) {
      view->setFrame(nullptr);
      if (IsWindow(hwnd)) {
        DestroyWindow(hwnd);
      }
      appendIssue(warningOrError, stagePrefix + ": setFrame failed.");
      addDiagnostic("parameter scan issue: " + stagePrefix + ".setFrameFailed");
      return false;
    }

    const Steinberg::tresult attachRes = view->attached(hwnd, Steinberg::kPlatformTypeHWND);
    addDiagnostic("parameter scan stage: " + stagePrefix + ".attached(result=" + tresultToString(attachRes) + ")");
    if (!isOkOrTrue(attachRes)) {
      view->setFrame(nullptr);
      if (IsWindow(hwnd)) {
        DestroyWindow(hwnd);
      }
      appendIssue(warningOrError, stagePrefix + ": attached failed.");
      addDiagnostic("parameter scan issue: " + stagePrefix + ".attachedFailed");
      return false;
    }

    heldFallbackEditor.view = view;
    heldFallbackEditor.frame = frame;
    heldFallbackEditor.hwnd = hwnd;
    heldFallbackEditor.attached = true;
    return true;
  }
#endif

  bool runVisibleGuiFallbackForParameterScan(std::size_t& countAfterFallback,
                                             std::size_t& guiRescanAttempts,
                                             bool& keepUntilShutdownOut,
                                             std::string& warningOrError) {
    countAfterFallback = parameters.size();
    guiRescanAttempts = 0;
    keepUntilShutdownOut = false;
    if (!controller) {
      addDiagnostic("parameter scan stage: guiFallback.attempted=false(reason=controllerMissing)");
      return false;
    }
#if !defined(_WIN32)
    addDiagnostic("parameter scan stage: guiFallback.attempted=false(reason=unsupportedPlatform)");
    return false;
#else
    addDiagnostic("parameter scan stage: guiFallback.attempted=true");

    std::string attachIssue;
    if (!attachVisibleFallbackEditor("guiFallback", attachIssue)) {
      warningOrError = attachIssue;
      return true;
    }

    const auto start = std::chrono::steady_clock::now();
    bool foundParameters = false;
    bool windowClosed = false;
    while (true) {
      pumpMessagesOnce();
      if (!heldFallbackEditor.hwnd || !IsWindow(heldFallbackEditor.hwnd)) {
        windowClosed = true;
        addDiagnostic("parameter scan stage: guiFallback.messagePump(reason=windowClosed)");
        break;
      }
      const auto elapsedMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
      if (elapsedMs >= kDefaultVisibleGuiTimeoutMs) {
        addDiagnostic("parameter scan stage: guiFallback.messagePump(reason=timeout,durationMs=" +
                      std::to_string(elapsedMs) + ")");
        break;
      }

      rescanParameters("guiFallback.live", countAfterFallback);
      ++guiRescanAttempts;
      if (countAfterFallback > 0) {
        foundParameters = true;
        addDiagnostic("parameter scan stage: guiFallback.successAfterAttach=true");
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kGuiInitSleepStepMs));
    }

    std::string restartIssue;
    const Steinberg::int32 flagsAfterAttach = consumeRestartFlags();
    addDiagnostic("parameter scan stage: guiFallback.restartFlagsAfterAttach=" + restartFlagsToSummary(flagsAfterAttach));
    if (!applyRestartFlags(flagsAfterAttach, true, restartIssue)) {
      if (reloadRequestedDuringLoad) {
        addDiagnostic("parameter scan stage: guiFallback.reloadRequestedAfterAttach=true");
      } else if (!restartIssue.empty()) {
        addDiagnostic("parameter scan issue: guiFallback.restartHandlingFailed(" + restartIssue + ")");
      }
    }

    if (!foundParameters || windowClosed) {
      closeHeldFallbackEditor("guiFallback");
      return true;
    }

    closeHeldFallbackEditor("guiFallback");
    rescanParameters("guiFallback.postCloseCheck", countAfterFallback);
    ++guiRescanAttempts;
    addDiagnostic("parameter scan stage: postCloseCheck.count=" + std::to_string(countAfterFallback));

    if (countAfterFallback > 0) {
      addDiagnostic("parameter scan stage: postCloseCheck.stable=true");
      return true;
    }

    addDiagnostic("parameter scan stage: postCloseCheck.stable=false");
    std::string holdIssue;
    if (!attachVisibleFallbackEditor("keepUntilShutdown", holdIssue)) {
      warningOrError = holdIssue;
      return true;
    }

    keepEditorOpenUntilShutdown = true;
    keepUntilShutdownOut = true;
    addDiagnostic("parameter scan stage: keepUntilShutdown.enabled=true");
    addDiagnostic("parameter scan issue: guiFallback.keepUntilShutdown(active)");
    return true;
#endif
  }

  std::size_t probeControllerOnlyView(const VST3::Hosting::PluginFactory& factory,
                                      const VST3::Hosting::PluginFactory::ClassInfos& classInfos) {
    if (!controller) return parameters.size();
    addDiagnostic("parameter scan stage: controllerOnlyViewProbe.attempted=true");
    const VST3::Hosting::ClassInfo* controllerClass = nullptr;
    for (const auto& ci : classInfos) {
      if (ci.category() == kVstComponentControllerClass) {
        controllerClass = &ci;
        break;
      }
    }
    if (!controllerClass) {
      addDiagnostic("parameter scan stage: controllerOnlyViewProbe.available=false(reason=classMissing)");
      return parameters.size();
    }

    addDiagnostic("parameter scan stage: controllerOnlyViewProbe.classID=" + uidToString(controllerClass->ID()));
    auto probeController = factory.createInstance<Steinberg::Vst::IEditController>(controllerClass->ID());
    addDiagnostic("parameter scan stage: controllerOnlyViewProbe.createInstance(result=" +
                  std::string(probeController ? "ok" : "null") + ")");
    if (!probeController) {
      return parameters.size();
    }

    if (componentHandler) {
      const auto setHandlerRes = probeController->setComponentHandler(componentHandler);
      addDiagnostic("parameter scan stage: controllerOnlyViewProbe.setComponentHandler(result=" +
                    tresultToString(setHandlerRes) + ")");
    }

    if (component) {
      SimpleMemoryStream stream;
      const Steinberg::tresult getStateRes = component->getState(&stream);
      addDiagnostic("parameter scan stage: controllerOnlyViewProbe.componentState(result=" +
                    tresultToString(getStateRes) + ", size=" + std::to_string(stream.size()) + ")");
      if (isOkOrTrue(getStateRes)) {
        stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
        const Steinberg::tresult setStateRes = probeController->setComponentState(&stream);
        addDiagnostic("parameter scan stage: controllerOnlyViewProbe.setComponentState(result=" +
                      tresultToString(setStateRes) + ")");
      }
    }

    Steinberg::IPtr<Steinberg::IPlugView> probeView =
        Steinberg::owned(probeController->createView(Steinberg::Vst::ViewType::kEditor));
    addDiagnostic("parameter scan stage: controllerOnlyViewProbe.createView(result=" +
                  std::string(probeView ? "ok" : "null") + ")");
    if (probeView) {
      const auto platformRes = probeView->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND);
      addDiagnostic("parameter scan stage: controllerOnlyViewProbe.platform(HWND)=" + tresultToString(platformRes));
    }
    const int32_t probeCount = probeController->getParameterCount();
    addDiagnostic("parameter scan stage: controllerOnlyViewProbe.count=" + std::to_string(probeCount));
    return parameters.size();
  }

  bool hasLoadedPlugin() const {
    return module || plugProvider || component || controller || processor;
  }

  void addDiagnostic(const std::string& line) {
    std::lock_guard<std::mutex> lock(diagnosticsMutex);
    diagnostics.push_back(line);
  }

  void clearState() {
#if defined(_WIN32)
    closeHeldFallbackEditor("keepUntilShutdown");
    keepEditorOpenUntilShutdown = false;
#endif
    processData.unprepare();
    module.reset();
    plugProvider.reset();
    component.reset();
    controller.reset();
    processor = nullptr;
    componentHandler.reset();
    metadata = {};
    parameters.clear();
    stagedParameters.clear();
    silentChannel.clear();
    latencySamples = 0;
    isActive = false;
    isProcessing = false;
    parameterScanSummary.clear();
    {
      std::lock_guard<std::mutex> lock(diagnosticsMutex);
      diagnostics.clear();
    }
    pendingRestartFlags.store(0, std::memory_order_release);
    observedRestartFlags = 0;
    reloadRequestedDuringLoad = false;
  }

  bool shutdownNow(std::string& warningOrError) {
    warningOrError.clear();
    bool ok = true;
    if (!hasLoadedPlugin()) {
      clearState();
      return true;
    }

    if (processor && isProcessing) {
      const Steinberg::tresult res = processor->setProcessing(false);
      if (!(isOkOrTrue(res) || res == Steinberg::kNotImplemented || res == Steinberg::kResultFalse)) {
        ok = false;
        appendIssue(warningOrError, "shutdown: setProcessing(false) failed.");
      }
    }
    isProcessing = false;

    if (component && isActive) {
      const Steinberg::tresult res = component->setActive(false);
      if (!isOkOrTrue(res)) {
        ok = false;
        appendIssue(warningOrError, "shutdown: setActive(false) failed.");
      }
    }
    isActive = false;
    if (controller && componentHandler) {
      controller->setComponentHandler(nullptr);
    }
    clearState();
    return ok;
  }
};

Vst3PluginHost::Vst3PluginHost() : impl_(new Impl()) {}

Vst3PluginHost::~Vst3PluginHost() {
  std::string ignored;
  shutdown(ignored);
}

void Vst3PluginHost::setRequestedClassId(std::string classId) {
  impl_->requestedClassId = std::move(classId);
  impl_->requestedClassIdNormalized = normalizeClassIdHex(impl_->requestedClassId);
}

bool Vst3PluginHost::wasReloadRequestedDuringLoad() const {
  return impl_->reloadRequestedDuringLoad;
}

bool Vst3PluginHost::load(const std::string& path, int sampleRate, int blockSize, std::string& error) {
  error.clear();
  std::string shutdownWarning;
  if (!shutdown(shutdownWarning)) {
    error = shutdownWarning.empty() ? "shutdown: failed to reset previous plugin state before load."
                                    : shutdownWarning;
    return false;
  }

  impl_->sampleRate = sampleRate;
  impl_->blockSize = blockSize;
  impl_->reloadRequestedDuringLoad = false;
  impl_->pendingRestartFlags.store(0, std::memory_order_release);
  impl_->observedRestartFlags = 0;
  if (!impl_->requestedClassId.empty() && impl_->requestedClassIdNormalized.size() != 32u) {
    error = "load: invalid classID format. Expected 32 hex chars (braces/hyphens are allowed).";
    return false;
  }

  std::string moduleError;
  impl_->module = VST3::Hosting::Module::create(path, moduleError);
  if (!impl_->module) {
    error = "load: Module::create failed: " + moduleError;
    return false;
  }

  auto factory = impl_->module->getFactory();
  Steinberg::Vst::PluginContextFactory::instance().setPluginContext(Steinberg::gStandardPluginContext);
  impl_->addDiagnostic("parameter scan stage: hostContextSet=true");
  factory.setHostContext(Steinberg::gStandardPluginContext);
  impl_->addDiagnostic("parameter scan stage: factoryHostContextSet=true");
  const auto classInfos = factory.classInfos();
  impl_->addDiagnostic("parameter scan stage: factory.classCount=" + std::to_string(classInfos.size()));
  for (const auto& ci : classInfos) {
    impl_->addDiagnostic(
        "parameter scan stage: class(name=" + ci.name() + ", vendor=" + ci.vendor() + ", category=" + ci.category() +
        ", subCategories=" + ci.subCategoriesString() + ", classID=" + uidToString(ci.ID()) + ")");
  }

  VST3::Hosting::ClassInfo selected;
  bool foundClass = false;
  const std::string requestedClassNormalized = impl_->requestedClassIdNormalized;
  if (!requestedClassNormalized.empty()) {
    for (const auto& ci : classInfos) {
      const std::string idCom = normalizeClassIdHex(ci.ID().toString(true));
      const std::string idRaw = normalizeClassIdHex(ci.ID().toString(false));
      if (requestedClassNormalized == idCom || requestedClassNormalized == idRaw) {
        selected = ci;
        foundClass = true;
        break;
      }
    }
    if (!foundClass) {
      std::ostringstream oss;
      oss << "load: requested classID not found: " << impl_->requestedClassId
          << ". Available classes:";
      for (const auto& ci : classInfos) {
        oss << " [" << ci.name() << "/" << ci.category() << "/" << uidToString(ci.ID()) << "]";
      }
      error = oss.str();
      return false;
    }
  } else {
    for (const auto& ci : classInfos) {
      if (ci.category() == kVstAudioEffectClass) {
        selected = ci;
        foundClass = true;
        break;
      }
    }
  }
  if (!foundClass) {
    error = "load: No VST3 audio effect class found in module.";
    return false;
  }
  impl_->addDiagnostic("parameter scan stage: selectedClass(name=" + selected.name() + ", vendor=" + selected.vendor() +
                       ", category=" + selected.category() + ", classID=" + uidToString(selected.ID()) + ")");

  impl_->plugProvider = Steinberg::owned(new Steinberg::Vst::PlugProvider(factory, selected, true));
  if (!impl_->plugProvider) {
    error = "load: Failed to create PlugProvider.";
    return false;
  }

  impl_->component = impl_->plugProvider->getComponent();
  impl_->controller = impl_->plugProvider->getController();
  impl_->processor = Steinberg::FUnknownPtr<Steinberg::Vst::IAudioProcessor>(impl_->component);
  impl_->addDiagnostic("parameter scan stage: controller acquired=" +
                       std::string(impl_->controller ? "yes" : "no"));

  if (!impl_->component || !impl_->processor) {
    error = "load: Plugin does not provide component/processor interfaces.";
    return false;
  }

  impl_->componentHandler = Steinberg::owned(
      new HostComponentHandler(&impl_->pendingRestartFlags, [impl = impl_.get()](const std::string& line) {
        impl->addDiagnostic(line);
      }));
  if (impl_->controller && impl_->componentHandler) {
    const Steinberg::tresult chRes = impl_->controller->setComponentHandler(impl_->componentHandler);
    impl_->addDiagnostic("parameter scan stage: setComponentHandler(result=" + tresultToString(chRes) + ")");
    if (!isOkOrTrue(chRes)) {
      impl_->addDiagnostic("parameter scan issue: setComponentHandlerFailed");
    }
  } else {
    impl_->addDiagnostic("parameter scan stage: setComponentHandler(skipped)");
  }

  impl_->metadata.name = selected.name();
  impl_->metadata.vendor = selected.vendor().empty() ? factory.info().vendor() : selected.vendor();
  impl_->metadata.version = selected.version();
  impl_->metadata.uniqueId = uidToString(selected.ID());
  impl_->metadata.path = path;

  auto checkReloadRequested = [&]() -> bool {
    if (!impl_->reloadRequestedDuringLoad) {
      return false;
    }
    error = "load: reload requested by plugin via restartComponent(kReloadComponent).";
    return true;
  };

  if (!impl_->component || !impl_->controller) {
    impl_->addDiagnostic("parameter scan stage: setComponentState(skipped:missingComponentOrController)");
  } else {
    SimpleMemoryStream stream;
    const Steinberg::tresult getStateRes = impl_->component->getState(&stream);
    impl_->addDiagnostic("parameter scan stage: component->getState(result=" + tresultToString(getStateRes) + ")");
    impl_->addDiagnostic("parameter scan stage: component->getState(size=" + std::to_string(stream.size()) + ")");
    if (isOkOrTrue(getStateRes)) {
      stream.seek(0, Steinberg::IBStream::kIBSeekSet, nullptr);
      const Steinberg::tresult setStateRes = impl_->controller->setComponentState(&stream);
      impl_->addDiagnostic("parameter scan stage: controller->setComponentState(result=" +
                           tresultToString(setStateRes) + ")");
      if (!isOkOrTrue(setStateRes) && setStateRes != Steinberg::kNotImplemented) {
        impl_->addDiagnostic("parameter scan issue: setComponentStateFailed");
      }
    } else {
      impl_->addDiagnostic("parameter scan issue: stateSyncFailed(getState)");
    }
  }

  std::string restartHandlingIssue;
  {
    const Steinberg::int32 flags = impl_->consumeRestartFlags();
    if (!impl_->applyRestartFlags(flags, true, restartHandlingIssue)) {
      if (checkReloadRequested()) return false;
    }
  }
  if (!restartHandlingIssue.empty()) {
    impl_->addDiagnostic("parameter scan issue: " + restartHandlingIssue);
  }
  if (checkReloadRequested()) return false;

  const int32_t firstRawCount = impl_->controller ? impl_->controller->getParameterCount() : -1;
  impl_->addDiagnostic("parameter scan stage: getParameterCount(first=" + std::to_string(firstRawCount) + ")");
  std::vector<std::string> enumerateDiagnostics;
  impl_->parameters = enumerateParameters(impl_->controller, enumerateDiagnostics);
  for (const auto& d : enumerateDiagnostics) {
    impl_->addDiagnostic(d);
  }
  const std::size_t firstCount = impl_->parameters.size();
  std::size_t retryAttempts = 0;
  for (int i = 0; i < kParameterScanRetryCount; ++i) {
    const Steinberg::int32 flags = impl_->consumeRestartFlags();
    restartHandlingIssue.clear();
    if (!impl_->applyRestartFlags(flags, true, restartHandlingIssue)) {
      if (checkReloadRequested()) return false;
      error = "load: failed while handling restartComponent flags.";
      return false;
    }
    if (!restartHandlingIssue.empty()) {
      impl_->addDiagnostic("parameter scan issue: " + restartHandlingIssue);
    }
    const bool restartRescanRequested =
        (flags & (Steinberg::Vst::kParamTitlesChanged | Steinberg::Vst::kParamValuesChanged)) != 0;
    if (checkReloadRequested()) return false;

    if (!impl_->parameters.empty() && !restartRescanRequested) {
      break;
    }
    if (impl_->parameters.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(kParameterScanRetryWaitMs));
    }
    ++retryAttempts;
    const int32_t retryRawCount = impl_->controller ? impl_->controller->getParameterCount() : -1;
    impl_->addDiagnostic("parameter scan stage: getParameterCount(retry#" + std::to_string(retryAttempts) + "=" +
                         std::to_string(retryRawCount) + ")");
    enumerateDiagnostics.clear();
    impl_->parameters = enumerateParameters(impl_->controller, enumerateDiagnostics);
    for (const auto& d : enumerateDiagnostics) {
      impl_->addDiagnostic(d);
    }
  }

  const int32_t finalRawCount = impl_->controller ? impl_->controller->getParameterCount() : -1;
  impl_->addDiagnostic("parameter scan stage: getParameterCount(final=" + std::to_string(finalRawCount) + ")");

  const std::size_t headlessCount = impl_->parameters.size();
  impl_->addDiagnostic("parameter scan stage: timeline.headlessCount=" + std::to_string(headlessCount));

  std::size_t guiFallbackCount = impl_->parameters.size();
  std::size_t guiFallbackRescanAttempts = 0;
  bool keepUntilShutdown = false;
  if (impl_->parameters.empty() && impl_->controller) {
    std::string guiWarning;
    const bool guiAttempted =
        impl_->runVisibleGuiFallbackForParameterScan(guiFallbackCount, guiFallbackRescanAttempts, keepUntilShutdown,
                                                     guiWarning);
    if (!guiWarning.empty()) {
      impl_->addDiagnostic("parameter scan issue: " + guiWarning);
    }
    if (guiAttempted && impl_->reloadRequestedDuringLoad) {
      error = "load: reload requested by plugin via restartComponent(kReloadComponent) after gui-fallback.";
      return false;
    }
  }
  impl_->addDiagnostic("parameter scan stage: timeline.guiFallbackCount=" + std::to_string(guiFallbackCount));
  impl_->addDiagnostic("parameter scan stage: timeline.keepUntilShutdown=" +
                       std::string(keepUntilShutdown ? "true" : "false"));

  std::size_t controllerProbeCount = impl_->parameters.size();
  if (impl_->parameters.empty() && impl_->controller) {
    controllerProbeCount = impl_->probeControllerOnlyView(factory, classInfos);
    std::string restartHandlingIssueAfterProbe;
    const Steinberg::int32 flagsAfterProbe = impl_->consumeRestartFlags();
    impl_->addDiagnostic("parameter scan stage: controllerOnlyViewProbe.restartFlagsAfter=" +
                         restartFlagsToSummary(flagsAfterProbe));
    if (!impl_->applyRestartFlags(flagsAfterProbe, true, restartHandlingIssueAfterProbe)) {
      if (checkReloadRequested()) return false;
      if (!restartHandlingIssueAfterProbe.empty()) {
        impl_->addDiagnostic("parameter scan issue: " + restartHandlingIssueAfterProbe);
      }
    }
    std::vector<std::string> postProbeDiagnostics;
    const int32_t postProbeRawCount = impl_->controller ? impl_->controller->getParameterCount() : -1;
    impl_->addDiagnostic("parameter scan stage: getParameterCount(afterControllerProbe=" +
                         std::to_string(postProbeRawCount) + ")");
    impl_->parameters = enumerateParameters(impl_->controller, postProbeDiagnostics);
    for (const auto& d : postProbeDiagnostics) {
      impl_->addDiagnostic(d);
    }
    controllerProbeCount = impl_->parameters.size();
  }
  impl_->addDiagnostic("parameter scan stage: timeline.controllerOnlyProbeCount=" +
                       std::to_string(controllerProbeCount));

  if (impl_->parameters.empty()) {
    impl_->addDiagnostic("parameter scan issue: stillZeroAfterRetry");
  }

  restartHandlingIssue.clear();
  {
    const Steinberg::int32 flags = impl_->consumeRestartFlags();
    if (!impl_->applyRestartFlags(flags, true, restartHandlingIssue)) {
      if (checkReloadRequested()) return false;
      error = "load: failed while handling post-scan restart flags.";
      return false;
    }
  }
  if (!restartHandlingIssue.empty()) {
    impl_->addDiagnostic("parameter scan issue: " + restartHandlingIssue);
  }
  if (checkReloadRequested()) return false;

  std::string configureIssue;
  if (!impl_->configureProcessingGraph(configureIssue, "load")) {
    error = configureIssue.empty() ? "load: failed to configure processing graph." : configureIssue;
    return false;
  }

  restartHandlingIssue.clear();
  {
    const Steinberg::int32 flags = impl_->consumeRestartFlags();
    if (!impl_->applyRestartFlags(flags, true, restartHandlingIssue)) {
      if (checkReloadRequested()) return false;
      error = "load: failed while handling post-setup restart flags.";
      return false;
    }
  }
  if (!restartHandlingIssue.empty()) {
    impl_->addDiagnostic("parameter scan issue: " + restartHandlingIssue);
  }
  if (checkReloadRequested()) return false;

  impl_->parameterScanSummary = "first=" + std::to_string(firstCount) + ", retryAttempts=" +
                                std::to_string(retryAttempts) + ", final=" +
                                std::to_string(impl_->parameters.size()) + ", restartFlags=" +
                                restartFlagsToSummary(impl_->observedRestartFlags) +
                                ", selectedClassID=" + impl_->metadata.uniqueId + ", headlessCount=" +
                                std::to_string(headlessCount) + ", guiFallbackRescanAttempts=" +
                                std::to_string(guiFallbackRescanAttempts) + ", guiFallbackCount=" +
                                std::to_string(guiFallbackCount) + ", keepUntilShutdown=" +
                                std::string(keepUntilShutdown ? "true" : "false") + ", controllerProbeCount=" +
                                std::to_string(controllerProbeCount);

  return true;
}

PluginMetadata Vst3PluginHost::getMetadata() const { return impl_->metadata; }

std::vector<PluginParameter> Vst3PluginHost::listParameters() const { return impl_->parameters; }

std::vector<std::string> Vst3PluginHost::takeDiagnostics() {
  std::lock_guard<std::mutex> lock(impl_->diagnosticsMutex);
  std::vector<std::string> out = impl_->diagnostics;
  impl_->diagnostics.clear();
  return out;
}

std::string Vst3PluginHost::getParameterScanSummary() const { return impl_->parameterScanSummary; }

bool Vst3PluginHost::setParameter(uint32_t id, double normalized, std::string& warningOrError) {
  if (!impl_->controller) {
    warningOrError = "No edit controller available; parameter was ignored.";
    return false;
  }
  if (normalized < 0.0 || normalized > 1.0) {
    warningOrError = "Normalized parameter value must be in [0.0, 1.0].";
    return false;
  }

  const auto pid = static_cast<Steinberg::Vst::ParamID>(id);
  const Steinberg::tresult setRes = impl_->controller->setParamNormalized(pid, normalized);
  if (!(setRes == Steinberg::kResultTrue || setRes == Steinberg::kResultOk)) {
    warningOrError = "setParamNormalized returned failure.";
    return false;
  }
  impl_->stagedParameters[pid] = normalized;
  warningOrError.clear();
  return true;
}

bool Vst3PluginHost::setParameterDisplay(uint32_t id, const std::string& displayInput, double& resolvedNormalized,
                                         std::string& warningOrError) {
  resolvedNormalized = 0.0;
  if (!impl_->controller) {
    warningOrError = "No edit controller available; parameter was ignored.";
    return false;
  }

  const std::string trimmedInput = trimCopy(displayInput);
  if (trimmedInput.empty()) {
    warningOrError = "Display input is empty.";
    return false;
  }

  const auto pid = static_cast<Steinberg::Vst::ParamID>(id);
  bool converted = false;
  Steinberg::Vst::String128 valueAsString {};
  Steinberg::UString(valueAsString, 128).fromAscii(trimmedInput.c_str());
  const Steinberg::tresult byStringRes =
      impl_->controller->getParamValueByString(pid, valueAsString, resolvedNormalized);
  converted = (byStringRes == Steinberg::kResultTrue || byStringRes == Steinberg::kResultOk);

  int32_t stepCount = -1;
  if (!converted) {
    const int32_t count = impl_->controller->getParameterCount();
    for (int32_t i = 0; i < count; ++i) {
      Steinberg::Vst::ParameterInfo info {};
      if (impl_->controller->getParameterInfo(i, info) != Steinberg::kResultOk) continue;
      if (static_cast<uint32_t>(info.id) == id) {
        stepCount = info.stepCount;
        break;
      }
    }
  }

  if (!converted && stepCount >= 1 && stepCount <= 1024) {
    for (int32_t s = 0; s <= stepCount; ++s) {
      const double candidate = static_cast<double>(s) / static_cast<double>(stepCount);
      Steinberg::Vst::String128 display {};
      const Steinberg::tresult displayRes = impl_->controller->getParamStringByValue(pid, candidate, display);
      if (displayRes != Steinberg::kResultTrue && displayRes != Steinberg::kResultOk) {
        continue;
      }
      const std::string candidateLabel = toUtf8(display);
      if (equalDisplayLabel(candidateLabel, trimmedInput)) {
        resolvedNormalized = candidate;
        converted = true;
        break;
      }
    }
  }

  if (!converted) {
    converted = parseNormalizedText(trimmedInput, resolvedNormalized);
  }

  if (!converted) {
    warningOrError =
        "Could not convert display input to parameter value. Try plugin display text or normalized 0.0-1.0.";
    return false;
  }

  return setParameter(id, resolvedNormalized, warningOrError);
}

AudioBuffer Vst3PluginHost::process(const AudioBuffer& input, std::string& error) {
  error.clear();
  AudioBuffer out;
  if (!impl_->processor || !impl_->component) {
    error = "process: Plugin is not loaded.";
    return out;
  }
  if (input.numSamples() == 0) {
    error = "process: Input buffer is empty.";
    return out;
  }
#if defined(_WIN32)
  if (impl_->keepEditorOpenUntilShutdown) {
    impl_->pumpMessagesOnce();
  }
#endif

  const Steinberg::int32 restartFlags = impl_->consumeRestartFlags();
  if (restartFlags != 0) {
    std::string restartIssue;
    if (!impl_->applyRestartFlags(restartFlags, false, restartIssue)) {
      if (impl_->reloadRequestedDuringLoad) {
        error = "process: reload requested by plugin via restartComponent(kReloadComponent).";
      } else if (!restartIssue.empty()) {
        error = "process: failed while handling restartComponent flags. " + restartIssue;
      } else {
        error = "process: failed while handling restartComponent flags.";
      }
      return {};
    }
    if (!restartIssue.empty()) {
      impl_->addDiagnostic("parameter scan issue: " + restartIssue);
    }
  }

  const std::size_t totalSamples = input.numSamples();
  const int32_t maxBlock = std::max<int32_t>(1, impl_->blockSize);

  std::vector<std::vector<float>> inChunkStorage;
  std::vector<std::vector<float>> outChunkStorage;
  int32_t totalOutChannels = 0;
  for (int32_t b = 0; b < impl_->processData.numOutputs; ++b) {
    totalOutChannels += impl_->processData.outputs[b].numChannels;
  }

  int32_t totalInChannels = 0;
  for (int32_t b = 0; b < impl_->processData.numInputs; ++b) {
    totalInChannels += impl_->processData.inputs[b].numChannels;
  }

  out.sampleRate = impl_->sampleRate;
  out.channels.assign(static_cast<std::size_t>(std::max(1, totalOutChannels)),
                      std::vector<float>(totalSamples, 0.0f));

  outChunkStorage.assign(static_cast<std::size_t>(std::max(1, totalOutChannels)),
                         std::vector<float>(static_cast<std::size_t>(maxBlock), 0.0f));
  inChunkStorage.assign(static_cast<std::size_t>(std::max(1, totalInChannels)),
                        std::vector<float>(static_cast<std::size_t>(maxBlock), 0.0f));

  bool firstChunk = true;
  for (std::size_t offset = 0; offset < totalSamples; offset += static_cast<std::size_t>(maxBlock)) {
    const std::size_t remaining = totalSamples - offset;
    const int32_t chunkSamples = static_cast<int32_t>(std::min<std::size_t>(remaining, static_cast<std::size_t>(maxBlock)));
    impl_->processData.numSamples = chunkSamples;

    impl_->parameterChanges.clearQueue();
    if (firstChunk) {
      for (const auto& kv : impl_->stagedParameters) {
        int32_t index = 0;
        auto* queue = impl_->parameterChanges.addParameterData(kv.first, index);
        if (!queue) continue;
        queue->addPoint(0, kv.second, index);
      }
    }
    impl_->processData.inputParameterChanges = &impl_->parameterChanges;

    int32_t outChGlobal = 0;
    for (int32_t b = 0; b < impl_->processData.numOutputs; ++b) {
      auto& bus = impl_->processData.outputs[b];
      for (int32_t ch = 0; ch < bus.numChannels; ++ch) {
        auto& chunk = outChunkStorage[static_cast<std::size_t>(outChGlobal)];
        std::fill_n(chunk.begin(), static_cast<std::size_t>(chunkSamples), 0.0f);
        impl_->processData.setChannelBuffer(Steinberg::Vst::kOutput, b, ch, chunk.data());
        ++outChGlobal;
      }
    }

    for (std::size_t ch = 0; ch < inChunkStorage.size(); ++ch) {
      auto& chunk = inChunkStorage[ch];
      const std::size_t srcCh = std::min<std::size_t>(ch, input.numChannels() - 1);
      for (int32_t i = 0; i < chunkSamples; ++i) {
        chunk[static_cast<std::size_t>(i)] = input.channels[srcCh][offset + static_cast<std::size_t>(i)];
      }
    }

    int32_t inChGlobal = 0;
    for (int32_t b = 0; b < impl_->processData.numInputs; ++b) {
      auto& bus = impl_->processData.inputs[b];
      for (int32_t ch = 0; ch < bus.numChannels; ++ch) {
        impl_->processData.setChannelBuffer(Steinberg::Vst::kInput, b, ch,
                                            inChunkStorage[static_cast<std::size_t>(inChGlobal)].data());
        ++inChGlobal;
      }
    }

    const Steinberg::tresult res = impl_->processor->process(impl_->processData);
    if (res != Steinberg::kResultOk) {
      error = "process: processor->process failed at sample offset " + std::to_string(offset) + ".";
      return {};
    }

    for (std::size_t ch = 0; ch < out.channels.size(); ++ch) {
      const auto& chunk = outChunkStorage[ch];
      std::copy_n(chunk.begin(), static_cast<std::size_t>(chunkSamples),
                  out.channels[ch].begin() + static_cast<std::ptrdiff_t>(offset));
    }
    firstChunk = false;
  }
  return out;
}

bool Vst3PluginHost::shutdown(std::string& warningOrError, int timeoutMs) {
  warningOrError.clear();
  if (!impl_) {
    impl_.reset(new Impl());
    return true;
  }
  if (!impl_->hasLoadedPlugin()) {
    impl_->clearState();
    return true;
  }

  struct ShutdownState {
    std::atomic<bool> done {false};
    bool ok = true;
    std::string detail;
    std::mutex mutex;
  };
  auto state = std::make_shared<ShutdownState>();
  Impl* target = impl_.get();
  std::thread([target, state]() {
    std::string detail;
    const bool ok = target->shutdownNow(detail);
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->ok = ok;
      state->detail = std::move(detail);
    }
    state->done.store(true, std::memory_order_release);
  }).detach();

  const int safeTimeoutMs = std::max(1, timeoutMs);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(safeTimeoutMs);
  while (!state->done.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() >= deadline) {
      static std::mutex abandonedMutex;
      static std::vector<std::unique_ptr<Impl>> abandonedImpls;
      warningOrError = "shutdown: Host shutdown timeout (" + std::to_string(safeTimeoutMs) + "ms).";
      {
        std::lock_guard<std::mutex> lock(abandonedMutex);
        abandonedImpls.push_back(std::move(impl_));
      }
      impl_.reset(new Impl());
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  {
    std::lock_guard<std::mutex> lock(state->mutex);
    if (!state->detail.empty()) {
      warningOrError = state->detail;
    }
    return state->ok;
  }
}

uint32_t Vst3PluginHost::getLatencySamples() const {
  return impl_->latencySamples;
}

}  // namespace vstcompare
