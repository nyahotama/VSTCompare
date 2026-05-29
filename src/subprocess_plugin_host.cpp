#include "vstcompare/subprocess_plugin_host.hpp"

#include "vstcompare/worker_ipc.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace vstcompare {
namespace {

#if defined(_WIN32)
constexpr int kDefaultLoadTimeoutMs = 10000;
constexpr int kDefaultBaseProcessTimeoutMs = 10000;
constexpr int kDefaultPerSampleTimeoutUs = 3;
constexpr int kProcessTimeoutMaxMs = 120000;
constexpr int kMinProcessTimeoutMs = 1000;
constexpr int kPingTimeoutMs = 2000;
constexpr int kShutdownTimeoutMs = 3000;
constexpr int kMaxReloadRestarts = 2;

struct HandleCloser {
  void operator()(void* handle) const {
    if (handle && handle != INVALID_HANDLE_VALUE) {
      CloseHandle(static_cast<HANDLE>(handle));
    }
  }
};

using UniqueHandle = std::unique_ptr<void, HandleCloser>;

std::wstring quoteArg(const std::wstring& arg) {
  std::wstring out = L"\"";
  for (wchar_t c : arg) {
    if (c == L'"') out += L'\\';
    out += c;
  }
  out += L"\"";
  return out;
}

std::wstring utf8ToWide(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) return std::wstring();
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
  return out;
}

std::string wideToUtf8(const std::wstring& text) {
  if (text.empty()) return std::string();
  const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return std::string();
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed, nullptr, nullptr);
  return out;
}

std::wstring currentExePath() {
  std::wstring path;
  path.resize(32768);
  const DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (size == 0 || size >= path.size()) {
    return std::wstring();
  }
  path.resize(size);
  return path;
}

int computeProcessTimeoutMs(std::size_t sampleCount) {
  const std::int64_t dynamicMs =
      static_cast<std::int64_t>(kDefaultBaseProcessTimeoutMs) +
      static_cast<std::int64_t>(sampleCount) * static_cast<std::int64_t>(kDefaultPerSampleTimeoutUs) / 1000;
  const std::int64_t clamped = std::max<std::int64_t>(kMinProcessTimeoutMs, std::min<std::int64_t>(dynamicMs, kProcessTimeoutMaxMs));
  return static_cast<int>(clamped);
}

void mergeUniqueStrings(std::vector<std::string>& base, const std::vector<std::string>& extra) {
  std::unordered_set<std::string> seen(base.begin(), base.end());
  for (const auto& entry : extra) {
    if (seen.insert(entry).second) {
      base.push_back(entry);
    }
  }
}
#endif

}  // namespace

struct SubprocessPluginHost::Impl {
  explicit Impl(std::string name) : instanceName(std::move(name)) {}

  std::string instanceName;
  PluginMetadata metadata;
  std::vector<PluginParameter> parameters;
  std::string parameterScanSummary;
  std::vector<std::string> diagnostics;
  uint32_t latencySamples = 0;
  bool loaded = false;
  std::string requestedClassId;

#if defined(_WIN32)
  UniqueHandle processHandle;
  UniqueHandle threadHandle;
  UniqueHandle toChildWrite;
  UniqueHandle fromChildRead;
#endif

  void clearCache() {
    metadata = {};
    parameters.clear();
    parameterScanSummary.clear();
    diagnostics.clear();
    latencySamples = 0;
    loaded = false;
  }
};

SubprocessPluginHost::SubprocessPluginHost(std::string instanceName) : impl_(new Impl(std::move(instanceName))) {}

SubprocessPluginHost::~SubprocessPluginHost() {
  std::string ignored;
  shutdown(ignored, kShutdownTimeoutMs);
}

void SubprocessPluginHost::setRequestedClassId(std::string classId) {
  impl_->requestedClassId = std::move(classId);
}

bool SubprocessPluginHost::load(const std::string& path, int sampleRate, int blockSize, std::string& error) {
  error.clear();
  std::string resetMessage;
  shutdown(resetMessage, kShutdownTimeoutMs);
  impl_->clearCache();

#if !defined(_WIN32)
  error = "load: subprocess host is Windows-only.";
  return false;
#else
  auto spawnWorker = [&](std::string& spawnError) -> bool {
    SECURITY_ATTRIBUTES sa {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE childStdInRead = nullptr;
    HANDLE childStdInWrite = nullptr;
    HANDLE childStdOutRead = nullptr;
    HANDLE childStdOutWrite = nullptr;

    if (!CreatePipe(&childStdInRead, &childStdInWrite, &sa, 0)) {
      spawnError = "spawn: failed to create stdin pipe.";
      return false;
    }
    if (!SetHandleInformation(childStdInWrite, HANDLE_FLAG_INHERIT, 0)) {
      CloseHandle(childStdInRead);
      CloseHandle(childStdInWrite);
      spawnError = "spawn: failed to set stdin pipe inheritance.";
      return false;
    }
    if (!CreatePipe(&childStdOutRead, &childStdOutWrite, &sa, 0)) {
      CloseHandle(childStdInRead);
      CloseHandle(childStdInWrite);
      spawnError = "spawn: failed to create stdout pipe.";
      return false;
    }
    if (!SetHandleInformation(childStdOutRead, HANDLE_FLAG_INHERIT, 0)) {
      CloseHandle(childStdInRead);
      CloseHandle(childStdInWrite);
      CloseHandle(childStdOutRead);
      CloseHandle(childStdOutWrite);
      spawnError = "spawn: failed to set stdout pipe inheritance.";
      return false;
    }

    const std::wstring exePath = currentExePath();
    if (exePath.empty()) {
      CloseHandle(childStdInRead);
      CloseHandle(childStdInWrite);
      CloseHandle(childStdOutRead);
      CloseHandle(childStdOutWrite);
      spawnError = "spawn: failed to resolve current executable path.";
      return false;
    }

    const std::wstring cmd = quoteArg(exePath) + L" --worker-child";

    STARTUPINFOW si {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = childStdInRead;
    si.hStdOutput = childStdOutWrite;
    si.hStdError = childStdOutWrite;

    PROCESS_INFORMATION pi {};
    std::vector<wchar_t> cmdMutable(cmd.begin(), cmd.end());
    cmdMutable.push_back(L'\0');

    const BOOL created = CreateProcessW(exePath.c_str(), cmdMutable.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &si, &pi);

    CloseHandle(childStdInRead);
    CloseHandle(childStdOutWrite);

    if (!created) {
      CloseHandle(childStdInWrite);
      CloseHandle(childStdOutRead);
      spawnError = "spawn: CreateProcessW failed.";
      return false;
    }

    impl_->processHandle.reset(pi.hProcess);
    impl_->threadHandle.reset(pi.hThread);
    impl_->toChildWrite.reset(childStdInWrite);
    impl_->fromChildRead.reset(childStdOutRead);

    workeripc::RequestMessage ping;
    ping.command = workeripc::Command::Ping;
    if (!workeripc::writeRequest(static_cast<HANDLE>(impl_->toChildWrite.get()), ping, spawnError)) {
      shutdown(resetMessage, kShutdownTimeoutMs);
      spawnError = "spawn/ipc: failed to send ping. " + spawnError;
      return false;
    }
    workeripc::ResponseMessage pingResponse;
    if (!workeripc::readResponseWithTimeout(static_cast<HANDLE>(impl_->fromChildRead.get()),
                                            static_cast<HANDLE>(impl_->processHandle.get()), kPingTimeoutMs, pingResponse,
                                            spawnError)) {
      shutdown(resetMessage, kShutdownTimeoutMs);
      spawnError = "spawn/ipc: failed to receive ping response. " + spawnError;
      return false;
    }
    if (pingResponse.status != workeripc::ResponseStatus::Ok) {
      shutdown(resetMessage, kShutdownTimeoutMs);
      spawnError = "spawn/ipc: worker ping failed. " + pingResponse.error;
      return false;
    }
    return true;
  };

  int reloadRestartCount = 0;
  std::vector<std::string> reloadDiagnostics;
  while (true) {
    if (!spawnWorker(error)) {
      return false;
    }

    workeripc::ByteWriter writer;
    writer.writeString(path);
    writer.writeI32(sampleRate);
    writer.writeI32(blockSize);
    const bool hasClassId = !impl_->requestedClassId.empty();
    writer.writeBool(hasClassId);
    if (hasClassId) {
      writer.writeString(impl_->requestedClassId);
    }

    workeripc::RequestMessage loadReq;
    loadReq.command = workeripc::Command::Load;
    loadReq.payload = writer.bytes();

    if (!workeripc::writeRequest(static_cast<HANDLE>(impl_->toChildWrite.get()), loadReq, error)) {
      shutdown(resetMessage, kShutdownTimeoutMs);
      error = "ipc/load: failed to send request. " + error;
      return false;
    }
    workeripc::ResponseMessage loadResp;
    if (!workeripc::readResponseWithTimeout(static_cast<HANDLE>(impl_->fromChildRead.get()),
                                            static_cast<HANDLE>(impl_->processHandle.get()), kDefaultLoadTimeoutMs, loadResp,
                                            error)) {
      shutdown(resetMessage, kShutdownTimeoutMs);
      error = "ipc/load: timeout or broken response. " + error;
      return false;
    }

    if (loadResp.status == workeripc::ResponseStatus::ReloadRequired) {
      ++reloadRestartCount;
      const std::string reason = loadResp.error.empty() ? "reload requested by worker." : loadResp.error;
      reloadDiagnostics.push_back("parameter scan stage: worker restart(reloadRequired#" +
                                  std::to_string(reloadRestartCount) + ", reason=" + reason + ")");
      shutdown(resetMessage, kShutdownTimeoutMs);
      if (reloadRestartCount > kMaxReloadRestarts) {
        error = "ipc/load: reload requested too many times (" + std::to_string(reloadRestartCount) +
                "). last reason: " + reason;
        return false;
      }
      continue;
    }
    if (loadResp.status != workeripc::ResponseStatus::Ok) {
      shutdown(resetMessage, kShutdownTimeoutMs);
      error = loadResp.error.empty() ? "ipc/load: worker load failed." : loadResp.error;
      return false;
    }
    break;
  }

  impl_->metadata = getMetadata();
  impl_->parameters = listParameters();
  if (!reloadDiagnostics.empty()) {
    mergeUniqueStrings(impl_->diagnostics, reloadDiagnostics);
  }
  if (impl_->parameters.empty()) {
    const std::vector<std::string> firstDiagnostics = impl_->diagnostics;
    const std::string firstSummary = impl_->parameterScanSummary;
    const auto retryParameters = listParameters();
    std::vector<std::string> mergedDiagnostics = firstDiagnostics;
    mergeUniqueStrings(mergedDiagnostics, impl_->diagnostics);
    impl_->diagnostics = std::move(mergedDiagnostics);
    if (!firstSummary.empty() && !impl_->parameterScanSummary.empty() &&
        impl_->parameterScanSummary != firstSummary) {
      impl_->parameterScanSummary = firstSummary + " | refetch=" + impl_->parameterScanSummary;
    } else if (!firstSummary.empty()) {
      impl_->parameterScanSummary = firstSummary;
    }
    if (!retryParameters.empty()) {
      impl_->parameters = retryParameters;
    }
  }
  impl_->latencySamples = getLatencySamples();
  impl_->loaded = true;
  return true;
#endif
}

PluginMetadata SubprocessPluginHost::getMetadata() const {
  if (impl_->loaded) {
    return impl_->metadata;
  }

#if !defined(_WIN32)
  return {};
#else
  if (!impl_->processHandle || !impl_->toChildWrite || !impl_->fromChildRead) {
    return impl_->metadata;
  }
  std::string error;
  workeripc::RequestMessage request;
  request.command = workeripc::Command::GetMetadata;
  if (!workeripc::writeRequest(static_cast<HANDLE>(impl_->toChildWrite.get()), request, error)) {
    return impl_->metadata;
  }
  workeripc::ResponseMessage response;
  if (!workeripc::readResponseWithTimeout(static_cast<HANDLE>(impl_->fromChildRead.get()),
                                          static_cast<HANDLE>(impl_->processHandle.get()), kDefaultLoadTimeoutMs, response,
                                          error)) {
    return impl_->metadata;
  }
  if (response.status != workeripc::ResponseStatus::Ok) {
    return impl_->metadata;
  }
  workeripc::ByteReader reader(response.payload);
  PluginMetadata meta;
  if (!reader.readMetadata(meta) || !reader.consumedAll()) {
    return impl_->metadata;
  }
  const_cast<Impl*>(impl_.get())->metadata = meta;
  return meta;
#endif
}

std::vector<PluginParameter> SubprocessPluginHost::listParameters() const {
  if (impl_->loaded) {
    return impl_->parameters;
  }
#if !defined(_WIN32)
  return {};
#else
  if (!impl_->processHandle || !impl_->toChildWrite || !impl_->fromChildRead) {
    return impl_->parameters;
  }
  std::string error;
  workeripc::RequestMessage request;
  request.command = workeripc::Command::ListParameters;
  if (!workeripc::writeRequest(static_cast<HANDLE>(impl_->toChildWrite.get()), request, error)) {
    return impl_->parameters;
  }
  workeripc::ResponseMessage response;
  if (!workeripc::readResponseWithTimeout(static_cast<HANDLE>(impl_->fromChildRead.get()),
                                          static_cast<HANDLE>(impl_->processHandle.get()), kDefaultLoadTimeoutMs, response,
                                          error)) {
    return impl_->parameters;
  }
  if (response.status != workeripc::ResponseStatus::Ok) {
    return impl_->parameters;
  }

  workeripc::ByteReader reader(response.payload);
  uint32_t count = 0;
  if (!reader.readU32(count) || count > 10000u) {
    return impl_->parameters;
  }

  std::vector<PluginParameter> params;
  params.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    PluginParameter p;
    if (!reader.readParameter(p)) {
      return impl_->parameters;
    }
    params.push_back(std::move(p));
  }
  uint32_t warnCount = 0;
  if (!reader.readU32(warnCount) || warnCount > 256u) {
    return impl_->parameters;
  }
  std::vector<std::string> diagnostics;
  diagnostics.reserve(warnCount);
  for (uint32_t i = 0; i < warnCount; ++i) {
    std::string warning;
    if (!reader.readString(warning)) {
      return impl_->parameters;
    }
    diagnostics.push_back(std::move(warning));
  }
  std::string summary;
  if (!reader.readString(summary) || !reader.consumedAll()) {
    return impl_->parameters;
  }
  const_cast<Impl*>(impl_.get())->parameters = params;
  const_cast<Impl*>(impl_.get())->diagnostics = diagnostics;
  const_cast<Impl*>(impl_.get())->parameterScanSummary = summary;
  return params;
#endif
}

std::vector<std::string> SubprocessPluginHost::takeDiagnostics() {
  std::vector<std::string> out = impl_->diagnostics;
  impl_->diagnostics.clear();
  return out;
}

std::string SubprocessPluginHost::getParameterScanSummary() const { return impl_->parameterScanSummary; }

bool SubprocessPluginHost::setParameter(uint32_t id, double normalized, std::string& warningOrError) {
  warningOrError.clear();
#if !defined(_WIN32)
  warningOrError = "setParameter: subprocess host is Windows-only.";
  return false;
#else
  if (!impl_->processHandle || !impl_->toChildWrite || !impl_->fromChildRead) {
    warningOrError = "setParameter: worker is not running.";
    return false;
  }

  workeripc::ByteWriter writer;
  writer.writeU32(id);
  writer.writeDouble(normalized);
  workeripc::RequestMessage request;
  request.command = workeripc::Command::SetParameter;
  request.payload = writer.bytes();

  std::string error;
  if (!workeripc::writeRequest(static_cast<HANDLE>(impl_->toChildWrite.get()), request, error)) {
    warningOrError = "ipc/setParameter: failed to send request. " + error;
    return false;
  }
  workeripc::ResponseMessage response;
  if (!workeripc::readResponseWithTimeout(static_cast<HANDLE>(impl_->fromChildRead.get()),
                                          static_cast<HANDLE>(impl_->processHandle.get()), kDefaultLoadTimeoutMs, response,
                                          error)) {
    warningOrError = "ipc/setParameter: failed to receive response. " + error;
    return false;
  }
  if (response.status != workeripc::ResponseStatus::Ok) {
    warningOrError = response.error.empty() ? "ipc/setParameter: worker failure." : response.error;
    return false;
  }
  workeripc::ByteReader reader(response.payload);
  bool ok = false;
  std::string remoteMessage;
  if (!reader.readBool(ok) || !reader.readString(remoteMessage) || !reader.consumedAll()) {
    warningOrError = "ipc/setParameter: malformed response.";
    return false;
  }
  warningOrError = remoteMessage;
  return ok;
#endif
}

bool SubprocessPluginHost::setParameterDisplay(uint32_t id, const std::string& displayInput, double& resolvedNormalized,
                                               std::string& warningOrError) {
  warningOrError.clear();
  resolvedNormalized = 0.0;
#if !defined(_WIN32)
  warningOrError = "setParameterDisplay: subprocess host is Windows-only.";
  return false;
#else
  if (!impl_->processHandle || !impl_->toChildWrite || !impl_->fromChildRead) {
    warningOrError = "setParameterDisplay: worker is not running.";
    return false;
  }

  workeripc::ByteWriter writer;
  writer.writeU32(id);
  writer.writeString(displayInput);
  workeripc::RequestMessage request;
  request.command = workeripc::Command::SetParameterDisplay;
  request.payload = writer.bytes();

  std::string error;
  if (!workeripc::writeRequest(static_cast<HANDLE>(impl_->toChildWrite.get()), request, error)) {
    warningOrError = "ipc/setParameterDisplay: failed to send request. " + error;
    return false;
  }
  workeripc::ResponseMessage response;
  if (!workeripc::readResponseWithTimeout(static_cast<HANDLE>(impl_->fromChildRead.get()),
                                          static_cast<HANDLE>(impl_->processHandle.get()), kDefaultLoadTimeoutMs, response,
                                          error)) {
    warningOrError = "ipc/setParameterDisplay: failed to receive response. " + error;
    return false;
  }
  if (response.status != workeripc::ResponseStatus::Ok) {
    warningOrError = response.error.empty() ? "ipc/setParameterDisplay: worker failure." : response.error;
    return false;
  }
  workeripc::ByteReader reader(response.payload);
  bool ok = false;
  std::string remoteMessage;
  if (!reader.readBool(ok) || !reader.readDouble(resolvedNormalized) || !reader.readString(remoteMessage) ||
      !reader.consumedAll()) {
    warningOrError = "ipc/setParameterDisplay: malformed response.";
    return false;
  }
  warningOrError = remoteMessage;
  return ok;
#endif
}

AudioBuffer SubprocessPluginHost::process(const AudioBuffer& input, std::string& error) {
  error.clear();
#if !defined(_WIN32)
  error = "process: subprocess host is Windows-only.";
  return {};
#else
  if (!impl_->processHandle || !impl_->toChildWrite || !impl_->fromChildRead) {
    error = "process: worker is not running.";
    return {};
  }

  workeripc::ByteWriter writer;
  writer.writeAudioBuffer(input);

  workeripc::RequestMessage request;
  request.command = workeripc::Command::Process;
  request.payload = writer.bytes();
  if (!workeripc::writeRequest(static_cast<HANDLE>(impl_->toChildWrite.get()), request, error)) {
    error = "ipc/process: failed to send request. " + error;
    return {};
  }

  const int timeoutMs = computeProcessTimeoutMs(input.numSamples());
  workeripc::ResponseMessage response;
  if (!workeripc::readResponseWithTimeout(static_cast<HANDLE>(impl_->fromChildRead.get()),
                                          static_cast<HANDLE>(impl_->processHandle.get()), timeoutMs, response, error)) {
    shutdown(error, kShutdownTimeoutMs);
    error = "ipc/process: timeout/crash while waiting response. " + error;
    return {};
  }
  if (response.status != workeripc::ResponseStatus::Ok) {
    error = response.error.empty() ? "ipc/process: worker failure." : response.error;
    return {};
  }

  workeripc::ByteReader reader(response.payload);
  AudioBuffer output;
  if (!reader.readAudioBuffer(output) || !reader.consumedAll()) {
    error = "ipc/process: malformed response.";
    return {};
  }
  return output;
#endif
}

uint32_t SubprocessPluginHost::getLatencySamples() const {
  if (impl_->loaded) {
    return impl_->latencySamples;
  }
#if !defined(_WIN32)
  return 0;
#else
  if (!impl_->processHandle || !impl_->toChildWrite || !impl_->fromChildRead) {
    return impl_->latencySamples;
  }
  std::string error;
  workeripc::RequestMessage request;
  request.command = workeripc::Command::GetLatency;
  if (!workeripc::writeRequest(static_cast<HANDLE>(impl_->toChildWrite.get()), request, error)) {
    return impl_->latencySamples;
  }
  workeripc::ResponseMessage response;
  if (!workeripc::readResponseWithTimeout(static_cast<HANDLE>(impl_->fromChildRead.get()),
                                          static_cast<HANDLE>(impl_->processHandle.get()), kDefaultLoadTimeoutMs, response,
                                          error)) {
    return impl_->latencySamples;
  }
  if (response.status != workeripc::ResponseStatus::Ok) {
    return impl_->latencySamples;
  }
  workeripc::ByteReader reader(response.payload);
  uint32_t latency = impl_->latencySamples;
  if (!reader.readU32(latency) || !reader.consumedAll()) {
    return impl_->latencySamples;
  }
  const_cast<Impl*>(impl_.get())->latencySamples = latency;
  return latency;
#endif
}

bool SubprocessPluginHost::shutdown(std::string& warningOrError, int timeoutMs) {
  warningOrError.clear();
  impl_->clearCache();

#if !defined(_WIN32)
  return true;
#else
  const bool hasProcess = static_cast<bool>(impl_->processHandle);
  if (!hasProcess) {
    impl_->threadHandle.reset();
    impl_->toChildWrite.reset();
    impl_->fromChildRead.reset();
    return true;
  }

  bool ok = true;
  std::string message;
  auto workerAlreadyExited = [&]() -> bool {
    if (!impl_->processHandle) return true;
    const DWORD wait = WaitForSingleObject(static_cast<HANDLE>(impl_->processHandle.get()), 0);
    return wait == WAIT_OBJECT_0;
  };

  if (impl_->toChildWrite && impl_->fromChildRead) {
    workeripc::ByteWriter writer;
    writer.writeI32(timeoutMs > 0 ? timeoutMs : kShutdownTimeoutMs);
    workeripc::RequestMessage request;
    request.command = workeripc::Command::Shutdown;
    request.payload = writer.bytes();
    std::string ioError;
    if (workeripc::writeRequest(static_cast<HANDLE>(impl_->toChildWrite.get()), request, ioError)) {
      workeripc::ResponseMessage response;
      if (workeripc::readResponseWithTimeout(static_cast<HANDLE>(impl_->fromChildRead.get()),
                                             static_cast<HANDLE>(impl_->processHandle.get()),
                                             timeoutMs > 0 ? timeoutMs : kShutdownTimeoutMs, response, ioError)) {
        if (response.status == workeripc::ResponseStatus::Ok) {
          workeripc::ByteReader reader(response.payload);
          bool remoteOk = false;
          std::string remoteMessage;
          if (reader.readBool(remoteOk) && reader.readString(remoteMessage) && reader.consumedAll()) {
            ok = remoteOk;
            message = remoteMessage;
          } else {
            ok = false;
            message = "shutdown: malformed worker shutdown response.";
          }
        } else {
          ok = false;
          message = response.error.empty() ? "shutdown: worker returned error." : response.error;
        }
      } else {
        if (ioError.find("worker process exited unexpectedly") != std::string::npos && workerAlreadyExited()) {
          ok = true;
        } else {
          ok = false;
          message = "shutdown: " + ioError;
        }
      }
    } else {
      if (workerAlreadyExited()) {
        ok = true;
      } else {
        ok = false;
        message = "shutdown: " + ioError;
      }
    }
  }

  const DWORD waitMs = static_cast<DWORD>(std::max(1, timeoutMs));
  const DWORD waitRes = WaitForSingleObject(static_cast<HANDLE>(impl_->processHandle.get()), waitMs);
  if (waitRes != WAIT_OBJECT_0) {
    TerminateProcess(static_cast<HANDLE>(impl_->processHandle.get()), 1);
    warningOrError = "shutdown: Host shutdown timeout (" + std::to_string(std::max(1, timeoutMs)) + "ms).";
    impl_->processHandle.reset();
    impl_->threadHandle.reset();
    impl_->toChildWrite.reset();
    impl_->fromChildRead.reset();
    return false;
  }

  impl_->processHandle.reset();
  impl_->threadHandle.reset();
  impl_->toChildWrite.reset();
  impl_->fromChildRead.reset();

  if (!message.empty()) {
    warningOrError = message;
  }
  return ok;
#endif
}

}  // namespace vstcompare
