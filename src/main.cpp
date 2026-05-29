#include "vstcompare/cli.hpp"
#include "vstcompare/pipeline.hpp"
#include "vstcompare/worker_server.hpp"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--worker-child") {
    vstcompare::WorkerServer worker;
    return worker.run();
  }

  vstcompare::CliOptions options;
  std::string error;
  if (!vstcompare::parseCli(argc, argv, options, error)) {
    if (!error.empty()) {
      std::cerr << "Error: " << error << "\n\n";
    }
    std::cerr << vstcompare::usageText() << "\n";
    return 1;
  }

  vstcompare::Pipeline pipeline;
  return pipeline.run(options);
}
