#pragma once

#include "vstcompare/cli.hpp"

namespace vstcompare {

class Pipeline {
public:
  int run(const CliOptions& options) const;
};

}  // namespace vstcompare

