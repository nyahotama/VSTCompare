#include "vstcompare/cli.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

bool parseArgs(const std::vector<std::string>& args, vstcompare::CliOptions& out, std::string& error) {
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (const auto& s : args) {
    argv.push_back(const_cast<char*>(s.c_str()));
  }
  return vstcompare::parseCli(static_cast<int>(argv.size()), argv.data(), out, error);
}

}  // namespace

int main() {
  {
    vstcompare::CliOptions options;
    std::string error;
    const std::vector<std::string> args = {
        "vstcompare_cli", "--plugin-a", "A.vst3", "--plugin-b", "B.vst3", "--out", "report.html",
    };
    if (!parseArgs(args, options, error)) {
      std::cerr << "normal mode should parse required arguments.\n";
      return 1;
    }
  }

  {
    vstcompare::CliOptions options;
    std::string error;
    const std::vector<std::string> args = {
        "vstcompare_cli", "--plugin-a", "A.vst3",
    };
    if (parseArgs(args, options, error)) {
      std::cerr << "normal mode should reject missing plugin-b and --out.\n";
      return 1;
    }
  }

  {
    vstcompare::CliOptions options;
    std::string error;
    const std::vector<std::string> args = {
        "vstcompare_cli", "--plugin-a", "A.vst3", "--plugin-b", "B.vst3",
    };
    if (parseArgs(args, options, error)) {
      std::cerr << "normal mode should reject missing --out.\n";
      return 1;
    }
  }

  {
    vstcompare::CliOptions options;
    std::string error;
    const std::vector<std::string> args = {
        "vstcompare_cli", "--plugin-a", "A.vst3", "--plugin-b", "B.vst3", "--out", "report.html",
        "--scan-only",
    };
    if (parseArgs(args, options, error)) {
      std::cerr << "scan-only flag should be rejected.\n";
      return 1;
    }
  }

  return 0;
}
