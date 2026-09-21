// PipeANN: SSD simulator-backed file reader.
//
// SimSSDFileReader implements the AlignedFileReader interface on top of the
// open-source SimpleSSD SSD simulator (third_party/SimpleSSD-Standalone).
//
// Data is still read from the real index file (via buffered pread, so results
// are always correct), but the *completion time* of every request is governed
// by the SimpleSSD timing model (NVMe protocol + PCIe + FTL + NAND flash).
// This lets users benchmark PipeANN against a simulated SSD without owning
// the physical device, or compare "real SSD" vs "simulated SSD" behavior.
#pragma once

#include <string>

#include "aligned_file_reader.h"

// Configuration of the simulated SSD reader.
struct SimSSDReaderConfig {
  // Path of a SimpleSSD *device* configuration file (see
  // third_party/SimpleSSD-Standalone/simplessd/config/sample.cfg).
  // Empty -> use the built-in default (NVMe Gen3 x4, 8-channel MLC, 512GiB).
  std::string device_config;

  // Wall-clock latency = simulated latency * latency_scale.
  // Use < 1.0 to emulate a faster SSD, > 1.0 for a slower one.
  double latency_scale = 1.0;

  // true: model the full NVMe host interface (PCIe + NVMe queues).
  // false: "none" interface, requests go straight to the HIL layer.
  bool use_nvme_interface = true;

  // Print per-reader statistics on close().
  bool verbose = false;

  // Minimum host byte range that must be mapped in the simulated FTL.
  // SimSSDFileReader sets this from the opened index file size so random
  // reads hit mapped LPNs and therefore model NAND page-read latency.
  uint64_t min_fill_bytes = 0;

  // Read configuration from environment variables:
  //   PIPEANN_SIMSSD_DEVICE_CONFIG  : path of SimpleSSD device config file
  //   PIPEANN_SIMSSD_LATENCY_SCALE  : double, latency multiplier (def. 1.0)
  //   PIPEANN_SIMSSD_INTERFACE      : "nvme" (default) or "none"
  //   PIPEANN_SIMSSD_VERBOSE        : "1"/"true" enables statistics printout
  static SimSSDReaderConfig from_env();
};

class SimSSDFileReader : public AlignedFileReader {
 public:
  explicit SimSSDFileReader(SimSSDReaderConfig cfg = SimSSDReaderConfig::from_env());
  virtual ~SimSSDFileReader();

  void *get_ctx() override;
  void register_thread() override;
  void deregister_thread() override;
  void deregister_all_threads() override;
  void register_buf(void *buf, uint64_t buf_size, int mrid) override;

  void open(const std::string &fname, bool enable_writes, bool enable_create) override;
  void close() override;

  void read(std::vector<IORequest> &read_reqs, void *ctx, bool async = false) override;
  void write(std::vector<IORequest> &write_reqs, void *ctx, bool async = false) override;
  void read_fd(int fd, std::vector<IORequest> &read_reqs, void *ctx) override;
  void write_fd(int fd, std::vector<IORequest> &write_reqs, void *ctx) override;

  void send_io(IORequest &reqs, void *ctx, bool write) override;
  void send_io(std::vector<IORequest> &reqs, void *ctx, bool write) override;
  int poll(void *ctx) override;
  void poll_all(void *ctx) override;
  void poll_wait(void *ctx) override;

 private:
  int file_desc = -1;
  SimSSDReaderConfig config;
  uint64_t n_reads = 0;
  uint64_t n_writes = 0;
  uint64_t sum_latency_ns = 0;
};
