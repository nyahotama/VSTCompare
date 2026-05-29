#include "vstcompare/cli.hpp"

#include <cstdlib>
#include <string>

namespace vstcompare {

namespace {

bool parseInt(const std::string& s, int& out) {
  if (s.empty()) {
    return false;
  }
  char* end = nullptr;
  const long value = std::strtol(s.c_str(), &end, 10);
  if (!end || *end != '\0') {
    return false;
  }
  if (value <= 0 || value > 192000) {
    return false;
  }
  out = static_cast<int>(value);
  return true;
}

}  // namespace

bool parseCli(int argc, char** argv, CliOptions& out, std::string& error) {
  if (argc <= 1) {
    error = "Missing required arguments.";
    return false;
  }

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto requireValue = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        error = std::string("Missing value for ") + name;
        return nullptr;
      }
      return argv[++i];
    };

    if (arg == "--plugin-a") {
      const char* v = requireValue("--plugin-a");
      if (!v) return false;
      out.pluginAPath = v;
    } else if (arg == "--plugin-b") {
      const char* v = requireValue("--plugin-b");
      if (!v) return false;
      out.pluginBPath = v;
    } else if (arg == "--out") {
      const char* v = requireValue("--out");
      if (!v) return false;
      out.outDirOrFile = v;
    } else if (arg == "--plugin-a-class-id") {
      const char* v = requireValue("--plugin-a-class-id");
      if (!v) return false;
      out.pluginAClassId = v;
    } else if (arg == "--plugin-b-class-id") {
      const char* v = requireValue("--plugin-b-class-id");
      if (!v) return false;
      out.pluginBClassId = v;
    } else if (arg == "--sample-rate") {
      const char* v = requireValue("--sample-rate");
      if (!v) return false;
      if (!parseInt(v, out.sampleRate)) {
        error = "Invalid --sample-rate. Use a positive integer.";
        return false;
      }
    } else if (arg == "--block-size") {
      const char* v = requireValue("--block-size");
      if (!v) return false;
      if (!parseInt(v, out.blockSize)) {
        error = "Invalid --block-size. Use a positive integer.";
        return false;
      }
    } else if (arg == "--non-interactive") {
      out.nonInteractive = true;
    } else if (arg == "--help" || arg == "-h") {
      error.clear();
      return false;
    } else {
      error = "Unknown argument: " + arg;
      return false;
    }
  }

  if (out.pluginAPath.empty()) {
    error = "Required: --plugin-a <path>";
    return false;
  }

  if (out.pluginBPath.empty()) {
    error = "Required: --plugin-b <path>";
    return false;
  }

  if (out.outDirOrFile.empty()) {
    error = "Required: --out <dir-or-file>";
    return false;
  }

  return true;
}

std::string usageText() {
  return
      "Usage:\n"
      "  vstcompare_cli --plugin-a <path-to-A.vst3> --plugin-b <path-to-B.vst3> --out <dir-or-file> "
      "[--plugin-a-class-id <32hex>] [--plugin-b-class-id <32hex>] "
      "[--sample-rate 48000] [--block-size 512] [--non-interactive]\n";
}

}  // namespace vstcompare
