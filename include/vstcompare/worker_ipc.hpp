#pragma once

#include "vstcompare/types.hpp"

#include <cstdint>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace vstcompare::workeripc {

enum class Command : uint8_t {
  Ping = 1,
  Load = 2,
  GetMetadata = 3,
  ListParameters = 4,
  SetParameter = 5,
  SetParameterDisplay = 6,
  Process = 7,
  GetLatency = 8,
  Shutdown = 9,
};

enum class ResponseStatus : uint8_t {
  Ok = 0,
  Error = 1,
  ReloadRequired = 2,
};

struct RequestMessage {
  Command command = Command::Ping;
  std::vector<uint8_t> payload;
};

struct ResponseMessage {
  ResponseStatus status = ResponseStatus::Ok;
  std::vector<uint8_t> payload;
  std::string error;
};

class ByteWriter {
public:
  void writeU8(uint8_t v);
  void writeU32(uint32_t v);
  void writeI32(int32_t v);
  void writeDouble(double v);
  void writeFloat(float v);
  void writeBool(bool v);
  void writeString(const std::string& s);
  void writeAudioBuffer(const AudioBuffer& buffer);
  void writeMetadata(const PluginMetadata& metadata);
  void writeParameter(const PluginParameter& parameter);

  const std::vector<uint8_t>& bytes() const { return bytes_; }

private:
  std::vector<uint8_t> bytes_;
};

class ByteReader {
public:
  explicit ByteReader(const std::vector<uint8_t>& bytes);

  bool readU8(uint8_t& v);
  bool readU32(uint32_t& v);
  bool readI32(int32_t& v);
  bool readDouble(double& v);
  bool readFloat(float& v);
  bool readBool(bool& v);
  bool readString(std::string& s);
  bool readAudioBuffer(AudioBuffer& buffer);
  bool readMetadata(PluginMetadata& metadata);
  bool readParameter(PluginParameter& parameter);

  bool consumedAll() const { return offset_ == bytes_.size(); }
  std::size_t remaining() const { return bytes_.size() - offset_; }

private:
  template <typename T>
  bool readPod(T& v);

  const std::vector<uint8_t>& bytes_;
  std::size_t offset_ = 0;
};

#if defined(_WIN32)
bool writeRequest(HANDLE pipe, const RequestMessage& request, std::string& error);
bool readRequestBlocking(HANDLE pipe, RequestMessage& request, std::string& error);

bool writeResponse(HANDLE pipe, const ResponseMessage& response, std::string& error);
bool readResponseWithTimeout(HANDLE pipe, HANDLE processHandle, int timeoutMs, ResponseMessage& response, std::string& error);
#endif

}  // namespace vstcompare::workeripc
