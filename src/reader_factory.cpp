#include "reader_factory.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

#include "linux_aligned_file_reader.h"
#include "sim_ssd_reader.h"

std::shared_ptr<AlignedFileReader> create_aligned_file_reader(ReaderType type) {
  if (type == ReaderType::AUTO) {
    const char *env = std::getenv("PIPEANN_READER_TYPE");
    if (env != nullptr && (std::strcmp(env, "simssd") == 0 || std::strcmp(env, "sim") == 0)) {
      type = ReaderType::SIM_SSD;
    } else {
      type = ReaderType::URING;
    }
  }

  switch (type) {
    case ReaderType::SIM_SSD:
      std::cerr << "[PipeANN] Using SimSSDFileReader (SimpleSSD simulated SSD)" << std::endl;
      return std::shared_ptr<AlignedFileReader>(new SimSSDFileReader());
    case ReaderType::URING:
    default:
      return std::shared_ptr<AlignedFileReader>(new LinuxAlignedFileReader());
  }
}
