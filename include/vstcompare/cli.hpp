#pragma once

#include <string>

namespace vstcompare {

struct CliOptions {
  std::string pluginAPath;
  std::string pluginBPath;
  std::string pluginAClassId;
  std::string pluginBClassId;
  std::string outDirOrFile;
  int sampleRate = 48000;
  int blockSize = 512;
  bool nonInteractive = false;
};

bool parseCli(int argc, char** argv, CliOptions& out, std::string& error);
std::string usageText();

}  // namespace vstcompare
