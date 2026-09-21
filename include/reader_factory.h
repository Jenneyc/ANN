// PipeANN: factory for selecting the file reader backend.
//
// PipeANN normally reads the on-disk index from a real SSD through io_uring
// (LinuxAlignedFileReader).  For evaluation against a simulated SSD, an
// alternative reader backed by the open-source SimpleSSD simulator is
// provided (SimSSDFileReader).
//
// Selection (in order of precedence):
//   1. explicit argument to create_aligned_file_reader()
//   2. environment variable PIPEANN_READER_TYPE:
//        "uring"  -> real SSD via io_uring (default)
//        "simssd" -> simulated SSD via SimpleSSD
#pragma once

#include <memory>

#include "aligned_file_reader.h"

enum class ReaderType {
  AUTO = 0,  // decide from PIPEANN_READER_TYPE (default: URING)
  URING,     // real SSD, io_uring
  SIM_SSD,   // simulated SSD (SimpleSSD)
};

// Create a file reader. AUTO honors PIPEANN_READER_TYPE.
std::shared_ptr<AlignedFileReader> create_aligned_file_reader(ReaderType type = ReaderType::AUTO);
