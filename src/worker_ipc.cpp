#include "vstcompare/worker_ipc.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <thread>

namespace vstcompare::workeripc {
namespace {

constexpr uint32_t kMaxMessageBytes = 256u * 1024u * 1024u;

template <typename T>
void appendPod(std::vector<uint8_t>& bytes, const T& value) {
  const auto* ptr = reinterpret_cast<const uint8_t*>(&value);
  bytes.insert(bytes.end(), ptr, ptr + sizeof(T));
}

template <typename T>
bool copyPod(const std::vector<uint8_t>& bytes, std::size_t offset, T& value) {
  if (offset + sizeof(T) > bytes.size()) {
    return false;
  }
  std::memcpy(&value, bytes.data() + offset, sizeof(T));
  return true;
}

#if defined(_WIN32)
bool writeExact(HANDLE pipe, const uint8_t* data, std::size_t size, std::string& error) {
  std::size_t written = 0;
  while (written < size) {
    DWORD chunk = 0;
    const DWORD request = static_cast<DWORD>(std::min<std::size_t>(size - written, static_cast<std::size_t>(1u << 20u)));
    if (!WriteFile(pipe, data + written, request, &chunk, nullptr)) {
      error = "ipc/write: WriteFile failed.";
      return false;
    }
    if (chunk == 0) {
      error = "ipc/write: pipe closed while writing.";
      return false;
    }
    written += static_cast<std::size_t>(chunk);
  }
  return true;
}

bool readExactBlocking(HANDLE pipe, uint8_t* out, std::size_t size, std::string& error) {
  std::size_t read = 0;
  while (read < size) {
    DWORD chunk = 0;
    const DWORD request = static_cast<DWORD>(std::min<std::size_t>(size - read, static_cast<std::size_t>(1u << 20u)));
    if (!ReadFile(pipe, out + read, request, &chunk, nullptr)) {
      error = "ipc/read: ReadFile failed.";
      return false;
    }
    if (chunk == 0) {
      error = "ipc/read: pipe closed while reading.";
      return false;
    }
    read += static_cast<std::size_t>(chunk);
  }
  return true;
}

bool readFrameBlocking(HANDLE pipe, std::vector<uint8_t>& frame, std::string& error) {
  uint32_t size = 0;
  if (!readExactBlocking(pipe, reinterpret_cast<uint8_t*>(&size), sizeof(size), error)) {
    return false;
  }
  if (size > kMaxMessageBytes) {
    error = "ipc/read: message too large.";
    return false;
  }
  frame.assign(size, 0u);
  if (size == 0) {
    return true;
  }
  return readExactBlocking(pipe, frame.data(), frame.size(), error);
}

bool readFrameWithTimeout(HANDLE pipe, HANDLE processHandle, int timeoutMs, std::vector<uint8_t>& frame, std::string& error) {
  frame.clear();
  uint8_t header[sizeof(uint32_t)] = {};
  std::size_t headerRead = 0;
  std::vector<uint8_t> payload;
  std::size_t payloadRead = 0;
  uint32_t expectedPayload = 0;

  const auto safeTimeoutMs = std::max(1, timeoutMs);
  const auto start = std::chrono::steady_clock::now();

  while (true) {
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (elapsedMs > safeTimeoutMs) {
      error = "ipc/read: timeout while waiting for worker response.";
      return false;
    }

    if (processHandle) {
      const DWORD wait = WaitForSingleObject(processHandle, 0);
      if (wait == WAIT_OBJECT_0) {
        error = "ipc/read: worker process exited unexpectedly.";
        return false;
      }
    }

    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
      error = "ipc/read: PeekNamedPipe failed.";
      return false;
    }

    if (available == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      continue;
    }

    if (headerRead < sizeof(uint32_t)) {
      const DWORD need = static_cast<DWORD>(sizeof(uint32_t) - headerRead);
      const DWORD readReq = std::min<DWORD>(need, available);
      DWORD chunk = 0;
      if (!ReadFile(pipe, header + headerRead, readReq, &chunk, nullptr)) {
        error = "ipc/read: ReadFile failed while reading frame header.";
        return false;
      }
      if (chunk == 0) {
        error = "ipc/read: pipe closed while reading frame header.";
        return false;
      }
      headerRead += static_cast<std::size_t>(chunk);
      if (headerRead < sizeof(uint32_t)) {
        continue;
      }

      std::memcpy(&expectedPayload, header, sizeof(uint32_t));
      if (expectedPayload > kMaxMessageBytes) {
        error = "ipc/read: message too large.";
        return false;
      }
      payload.assign(expectedPayload, 0u);
      if (expectedPayload == 0) {
        frame.clear();
        return true;
      }
    }

    if (payloadRead < payload.size()) {
      DWORD availPayload = 0;
      if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &availPayload, nullptr)) {
        error = "ipc/read: PeekNamedPipe failed while reading payload.";
        return false;
      }
      if (availPayload == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }
      const DWORD need = static_cast<DWORD>(payload.size() - payloadRead);
      const DWORD readReq = std::min<DWORD>(need, availPayload);
      DWORD chunk = 0;
      if (!ReadFile(pipe, payload.data() + payloadRead, readReq, &chunk, nullptr)) {
        error = "ipc/read: ReadFile failed while reading payload.";
        return false;
      }
      if (chunk == 0) {
        error = "ipc/read: pipe closed while reading payload.";
        return false;
      }
      payloadRead += static_cast<std::size_t>(chunk);
      if (payloadRead < payload.size()) {
        continue;
      }
      frame = std::move(payload);
      return true;
    }
  }
}
#endif

}  // namespace

void ByteWriter::writeU8(uint8_t v) { bytes_.push_back(v); }
void ByteWriter::writeU32(uint32_t v) { appendPod(bytes_, v); }
void ByteWriter::writeI32(int32_t v) { appendPod(bytes_, v); }
void ByteWriter::writeDouble(double v) { appendPod(bytes_, v); }
void ByteWriter::writeFloat(float v) { appendPod(bytes_, v); }
void ByteWriter::writeBool(bool v) { writeU8(v ? 1u : 0u); }

void ByteWriter::writeString(const std::string& s) {
  writeU32(static_cast<uint32_t>(s.size()));
  bytes_.insert(bytes_.end(), s.begin(), s.end());
}

void ByteWriter::writeAudioBuffer(const AudioBuffer& buffer) {
  writeI32(buffer.sampleRate);
  writeU32(static_cast<uint32_t>(buffer.numChannels()));
  writeU32(static_cast<uint32_t>(buffer.numSamples()));
  for (const auto& channel : buffer.channels) {
    for (float sample : channel) {
      writeFloat(sample);
    }
  }
}

void ByteWriter::writeMetadata(const PluginMetadata& metadata) {
  writeString(metadata.name);
  writeString(metadata.vendor);
  writeString(metadata.uniqueId);
  writeString(metadata.version);
  writeString(metadata.path);
}

void ByteWriter::writeParameter(const PluginParameter& parameter) {
  writeU32(parameter.id);
  writeString(parameter.title);
  writeString(parameter.units);
  writeString(parameter.display);
  writeDouble(parameter.defaultNormalized);
  writeI32(parameter.stepCount);
  writeU32(static_cast<uint32_t>(parameter.enumDisplayOptions.size()));
  for (const auto& item : parameter.enumDisplayOptions) {
    writeString(item);
  }
}

ByteReader::ByteReader(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

template <typename T>
bool ByteReader::readPod(T& v) {
  if (!copyPod(bytes_, offset_, v)) {
    return false;
  }
  offset_ += sizeof(T);
  return true;
}

bool ByteReader::readU8(uint8_t& v) { return readPod(v); }
bool ByteReader::readU32(uint32_t& v) { return readPod(v); }
bool ByteReader::readI32(int32_t& v) { return readPod(v); }
bool ByteReader::readDouble(double& v) { return readPod(v); }
bool ByteReader::readFloat(float& v) { return readPod(v); }

bool ByteReader::readBool(bool& v) {
  uint8_t raw = 0;
  if (!readU8(raw)) return false;
  v = (raw != 0);
  return true;
}

bool ByteReader::readString(std::string& s) {
  uint32_t size = 0;
  if (!readU32(size)) return false;
  if (size > remaining()) return false;
  s.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
  offset_ += size;
  return true;
}

bool ByteReader::readAudioBuffer(AudioBuffer& buffer) {
  int32_t sampleRate = 0;
  uint32_t channelCount = 0;
  uint32_t sampleCount = 0;
  if (!readI32(sampleRate) || !readU32(channelCount) || !readU32(sampleCount)) {
    return false;
  }
  if (channelCount > 128u || sampleCount > (1u << 26u)) {
    return false;
  }
  buffer.sampleRate = sampleRate;
  buffer.channels.assign(channelCount, std::vector<float>(sampleCount, 0.0f));
  for (uint32_t ch = 0; ch < channelCount; ++ch) {
    for (uint32_t i = 0; i < sampleCount; ++i) {
      if (!readFloat(buffer.channels[ch][i])) {
        return false;
      }
    }
  }
  return true;
}

bool ByteReader::readMetadata(PluginMetadata& metadata) {
  return readString(metadata.name) && readString(metadata.vendor) && readString(metadata.uniqueId) &&
         readString(metadata.version) && readString(metadata.path);
}

bool ByteReader::readParameter(PluginParameter& parameter) {
  if (!(readU32(parameter.id) && readString(parameter.title) && readString(parameter.units) &&
        readString(parameter.display) && readDouble(parameter.defaultNormalized) && readI32(parameter.stepCount))) {
    return false;
  }
  uint32_t optionCount = 0;
  if (!readU32(optionCount) || optionCount > 2048u) {
    return false;
  }
  parameter.enumDisplayOptions.clear();
  parameter.enumDisplayOptions.reserve(optionCount);
  for (uint32_t i = 0; i < optionCount; ++i) {
    std::string value;
    if (!readString(value)) {
      return false;
    }
    parameter.enumDisplayOptions.push_back(std::move(value));
  }
  return true;
}

#if defined(_WIN32)
bool writeRequest(HANDLE pipe, const RequestMessage& request, std::string& error) {
  std::vector<uint8_t> frame;
  frame.reserve(1 + request.payload.size());
  frame.push_back(static_cast<uint8_t>(request.command));
  frame.insert(frame.end(), request.payload.begin(), request.payload.end());
  if (frame.size() > std::numeric_limits<uint32_t>::max()) {
    error = "ipc/write: request message too large.";
    return false;
  }
  const uint32_t frameSize = static_cast<uint32_t>(frame.size());
  if (!writeExact(pipe, reinterpret_cast<const uint8_t*>(&frameSize), sizeof(frameSize), error)) {
    return false;
  }
  if (frame.empty()) return true;
  return writeExact(pipe, frame.data(), frame.size(), error);
}

bool readRequestBlocking(HANDLE pipe, RequestMessage& request, std::string& error) {
  std::vector<uint8_t> frame;
  if (!readFrameBlocking(pipe, frame, error)) {
    return false;
  }
  if (frame.empty()) {
    error = "ipc/read: empty request frame.";
    return false;
  }
  request.command = static_cast<Command>(frame[0]);
  request.payload.assign(frame.begin() + 1, frame.end());
  return true;
}

bool writeResponse(HANDLE pipe, const ResponseMessage& response, std::string& error) {
  std::vector<uint8_t> frame;
  frame.push_back(static_cast<uint8_t>(response.status));

  if (response.status == ResponseStatus::Ok) {
    frame.insert(frame.end(), response.payload.begin(), response.payload.end());
  } else {
    ByteWriter writer;
    writer.writeString(response.error);
    const auto& payload = writer.bytes();
    frame.insert(frame.end(), payload.begin(), payload.end());
  }

  if (frame.size() > std::numeric_limits<uint32_t>::max()) {
    error = "ipc/write: response message too large.";
    return false;
  }
  const uint32_t frameSize = static_cast<uint32_t>(frame.size());
  if (!writeExact(pipe, reinterpret_cast<const uint8_t*>(&frameSize), sizeof(frameSize), error)) {
    return false;
  }
  if (frame.empty()) return true;
  return writeExact(pipe, frame.data(), frame.size(), error);
}

bool readResponseWithTimeout(HANDLE pipe, HANDLE processHandle, int timeoutMs, ResponseMessage& response, std::string& error) {
  std::vector<uint8_t> frame;
  if (!readFrameWithTimeout(pipe, processHandle, timeoutMs, frame, error)) {
    return false;
  }
  if (frame.empty()) {
    error = "ipc/read: empty response frame.";
    return false;
  }

  response = {};
  response.status = static_cast<ResponseStatus>(frame[0]);
  if (response.status == ResponseStatus::Ok) {
    response.payload.assign(frame.begin() + 1, frame.end());
    return true;
  }

  std::vector<uint8_t> errBytes(frame.begin() + 1, frame.end());
  ByteReader reader(errBytes);
  if (!reader.readString(response.error) || !reader.consumedAll()) {
    error = "ipc/read: malformed error response from worker.";
    return false;
  }
  if (response.error.empty()) {
    response.error = "worker reported failure.";
  }
  return true;
}
#endif

}  // namespace vstcompare::workeripc
