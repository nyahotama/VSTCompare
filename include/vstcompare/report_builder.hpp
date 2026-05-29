#pragma once

#include "vstcompare/interfaces.hpp"

#include <string>

namespace vstcompare {

class FinalReportBuilder final : public IReportSection {
public:
  std::string renderHtml(const RunSummary& summary) const override;
  std::string toJson(const RunSummary& summary) const override;
};

std::string buildOutputReportPath(const std::string& outputArg, const std::string& pluginAName,
                                  const std::string& pluginBName);

}  // namespace vstcompare

