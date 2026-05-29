#include "vstcompare/worker_server.hpp"

#include "vstcompare/vst3_plugin_host.hpp"
#include "vstcompare/worker_ipc.hpp"
#include "public.sdk/source/vst/hosting/plugprovider.h"

#include <iostream>
#include <string>
#include <vector>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace vstcompare {
namespace {

#if defined(_WIN32)
constexpr int kInternalShutdownTimeoutMs = 3000;

workeripc::ResponseMessage makeErrorResponse(const std::string& error) {
  workeripc::ResponseMessage response;
  response.status = workeripc::ResponseStatus::Error;
  response.error = error;
  return response;
}

workeripc::ResponseMessage makeReloadRequiredResponse(const std::string& message) {
  workeripc::ResponseMessage response;
  response.status = workeripc::ResponseStatus::ReloadRequired;
  response.error = message.empty() ? "load: reload required by plugin." : message;
  return response;
}

workeripc::ResponseMessage makeOkResponse(const workeripc::ByteWriter& writer) {
  workeripc::ResponseMessage response;
  response.status = workeripc::ResponseStatus::Ok;
  response.payload = writer.bytes();
  return response;
}

workeripc::ResponseMessage handleRequest(Vst3PluginHost& host, const workeripc::RequestMessage& request, bool& shouldExit) {
  shouldExit = false;
  workeripc::ByteReader reader(request.payload);
  workeripc::ByteWriter writer;
  std::string error;

  switch (request.command) {
    case workeripc::Command::Ping: {
      if (!reader.consumedAll()) {
        return makeErrorResponse("ipc/ping: malformed payload.");
      }
      return makeOkResponse(writer);
    }

    case workeripc::Command::Load: {
      std::string path;
      int32_t sampleRate = 0;
      int32_t blockSize = 0;
      bool hasClassId = false;
      std::string classId;
      if (!reader.readString(path) || !reader.readI32(sampleRate) || !reader.readI32(blockSize)) {
        return makeErrorResponse("ipc/load: malformed payload.");
      }
      if (reader.remaining() > 0) {
        if (!reader.readBool(hasClassId)) {
          return makeErrorResponse("ipc/load: malformed classID flag.");
        }
        if (hasClassId && !reader.readString(classId)) {
          return makeErrorResponse("ipc/load: malformed classID payload.");
        }
      }
      if (!reader.consumedAll()) {
        return makeErrorResponse("ipc/load: malformed trailing payload.");
      }
      host.setRequestedClassId(hasClassId ? classId : std::string());
      if (!host.load(path, sampleRate, blockSize, error)) {
        if (host.wasReloadRequestedDuringLoad()) {
          return makeReloadRequiredResponse(error);
        }
        return makeErrorResponse(error.empty() ? "load: failed." : error);
      }
      return makeOkResponse(writer);
    }

    case workeripc::Command::GetMetadata: {
      if (!reader.consumedAll()) {
        return makeErrorResponse("ipc/getMetadata: malformed payload.");
      }
      writer.writeMetadata(host.getMetadata());
      return makeOkResponse(writer);
    }

    case workeripc::Command::ListParameters: {
      if (!reader.consumedAll()) {
        return makeErrorResponse("ipc/listParameters: malformed payload.");
      }
      const auto parameters = host.listParameters();
      const auto diagnostics = host.takeDiagnostics();
      const auto summary = host.getParameterScanSummary();
      writer.writeU32(static_cast<uint32_t>(parameters.size()));
      for (const auto& p : parameters) {
        writer.writeParameter(p);
      }
      writer.writeU32(static_cast<uint32_t>(diagnostics.size()));
      for (const auto& d : diagnostics) {
        writer.writeString(d);
      }
      writer.writeString(summary);
      return makeOkResponse(writer);
    }

    case workeripc::Command::SetParameter: {
      uint32_t id = 0;
      double normalized = 0.0;
      if (!reader.readU32(id) || !reader.readDouble(normalized) || !reader.consumedAll()) {
        return makeErrorResponse("ipc/setParameter: malformed payload.");
      }
      std::string warningOrError;
      const bool ok = host.setParameter(id, normalized, warningOrError);
      writer.writeBool(ok);
      writer.writeString(warningOrError);
      return makeOkResponse(writer);
    }

    case workeripc::Command::SetParameterDisplay: {
      uint32_t id = 0;
      std::string displayInput;
      if (!reader.readU32(id) || !reader.readString(displayInput) || !reader.consumedAll()) {
        return makeErrorResponse("ipc/setParameterDisplay: malformed payload.");
      }
      std::string warningOrError;
      double resolvedNormalized = 0.0;
      const bool ok = host.setParameterDisplay(id, displayInput, resolvedNormalized, warningOrError);
      writer.writeBool(ok);
      writer.writeDouble(resolvedNormalized);
      writer.writeString(warningOrError);
      return makeOkResponse(writer);
    }

    case workeripc::Command::Process: {
      AudioBuffer input;
      if (!reader.readAudioBuffer(input) || !reader.consumedAll()) {
        return makeErrorResponse("ipc/process: malformed payload.");
      }
      AudioBuffer output = host.process(input, error);
      if (!error.empty()) {
        return makeErrorResponse(error);
      }
      writer.writeAudioBuffer(output);
      return makeOkResponse(writer);
    }

    case workeripc::Command::GetLatency: {
      if (!reader.consumedAll()) {
        return makeErrorResponse("ipc/getLatency: malformed payload.");
      }
      writer.writeU32(host.getLatencySamples());
      return makeOkResponse(writer);
    }

    case workeripc::Command::Shutdown: {
      int32_t timeoutMs = 0;
      if (!reader.readI32(timeoutMs) || !reader.consumedAll()) {
        return makeErrorResponse("ipc/shutdown: malformed payload.");
      }
      std::string warningOrError;
      const bool ok = host.shutdown(warningOrError, timeoutMs > 0 ? timeoutMs : kInternalShutdownTimeoutMs);
      writer.writeBool(ok);
      writer.writeString(warningOrError);
      shouldExit = true;
      return makeOkResponse(writer);
    }
  }

  return makeErrorResponse("ipc: unknown command.");
}
#endif

}  // namespace

int WorkerServer::run() {
#if !defined(_WIN32)
  return 2;
#else
  const HRESULT coinit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool comReady = SUCCEEDED(coinit) || coinit == RPC_E_CHANGED_MODE;
  if (!comReady) {
    std::cerr << "worker: CoInitializeEx failed.\n";
    return 2;
  }

  auto cleanupCom = [&]() {
    if (SUCCEEDED(coinit)) {
      CoUninitialize();
    }
    Steinberg::Vst::PluginContextFactory::instance().setPluginContext(nullptr);
  };

  Steinberg::Vst::PlugProvider::setErrorStream(nullptr);

  HANDLE inPipe = GetStdHandle(STD_INPUT_HANDLE);
  HANDLE outPipe = GetStdHandle(STD_OUTPUT_HANDLE);
  if (inPipe == INVALID_HANDLE_VALUE || outPipe == INVALID_HANDLE_VALUE) {
    std::cerr << "worker: failed to get stdio handles.\n";
    cleanupCom();
    return 2;
  }

  // Keep protocol pipe handles, then detach process stdout/stderr from protocol stream.
  HANDLE protocolOutPipe = outPipe;
  HANDLE duplicatedProtocolOutPipe = INVALID_HANDLE_VALUE;
  if (DuplicateHandle(GetCurrentProcess(), outPipe, GetCurrentProcess(), &duplicatedProtocolOutPipe, 0, FALSE,
                      DUPLICATE_SAME_ACCESS)) {
    protocolOutPipe = duplicatedProtocolOutPipe;
  }
  HANDLE nulOut = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (nulOut != INVALID_HANDLE_VALUE) {
    SetStdHandle(STD_OUTPUT_HANDLE, nulOut);
    SetStdHandle(STD_ERROR_HANDLE, nulOut);
    FILE* ignored = nullptr;
    freopen_s(&ignored, "NUL", "w", stdout);
    freopen_s(&ignored, "NUL", "w", stderr);
  }

  auto cleanupWorker = [&]() {
    cleanupCom();
    if (duplicatedProtocolOutPipe != INVALID_HANDLE_VALUE) {
      CloseHandle(duplicatedProtocolOutPipe);
      duplicatedProtocolOutPipe = INVALID_HANDLE_VALUE;
    }
    if (nulOut != INVALID_HANDLE_VALUE) {
      CloseHandle(nulOut);
      nulOut = INVALID_HANDLE_VALUE;
    }
  };

  Vst3PluginHost host;
  while (true) {
    workeripc::RequestMessage request;
    std::string ioError;
    if (!workeripc::readRequestBlocking(inPipe, request, ioError)) {
      std::cerr << ioError << "\n";
      cleanupWorker();
      return 2;
    }

    bool shouldExit = false;
    const workeripc::ResponseMessage response = handleRequest(host, request, shouldExit);
    if (!workeripc::writeResponse(protocolOutPipe, response, ioError)) {
      std::cerr << ioError << "\n";
      cleanupWorker();
      return 2;
    }

    if (shouldExit) {
      cleanupWorker();
      return 0;
    }
  }
#endif
}

}  // namespace vstcompare
