// PipeANN: SSD simulator-backed file reader (see include/sim_ssd_reader.h).
//
// Implementation notes
// --------------------
// * Real bytes are served with pread()/pwrite() (buffered I/O, no O_DIRECT),
//   so query results are bit-exact identical to the io_uring reader.  Because
//   the index is usually resident in the page cache during simulation runs,
//   the host-side data movement is fast.
// * The *timing* of every request is decided by the SimpleSSD event-driven
//   simulator.  When a request is submitted we also inject it into the
//   simulated SSD and remember the simulated completion tick.  The request is
//   only reported as finished (req.finished = true) once wall-clock time
//   >= submit_wall_time + simulated_latency * latency_scale.
// * The simulator engine only advances when it is "driven" (doNextEvent()).
//   We drive it lazily from poll()/poll_wait()/read().  This preserves
//   simulated concurrency: requests that are in flight at the same simulated
//   time share NAND channels / NVMe queues exactly as real concurrent I/Os.

#include "sim_ssd_reader.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <thread>
#include <memory>
#include <unordered_map>

#include "logger.h"

#ifdef PIPEANN_WITH_SIMSSD

#include "bil/entry.hh"
#include "sil/none/none.hh"
#include "sil/nvme/nvme.hh"
#include "sim/cfg_reader.hh"
#include "sim/engine.hh"
#include "simplessd/util/simplessd.hh"

namespace {

  // ---------------------------------------------------------------------------
  // Built-in default SimpleSSD device configuration.
  // Models a mainstream NVMe SSD: PCIe Gen3 x4, 8-channel MLC NAND, 16KiB
  // pages, page-level FTL, 512MiB DRAM, ~512GiB raw capacity.
  // Read cache / prefetch are disabled because DiskANN-style graph search
  // issues random 4KiB reads where prefetching only pollutes timing.
  // Override with PIPEANN_SIMSSD_DEVICE_CONFIG.
  // ---------------------------------------------------------------------------
  const char kDefaultDeviceConfig[] = R"CFG(
[cpu]
ClockSpeed = 400000000
HILCoreCount = 1
ICLCoreCount = 1
FTLCoreCount = 1

[nvme]
PCIEGeneration = 2
PCIELane = 4
AXIBusWidth = 2
AXIClock = 250000000
FIFOTransferUnit = 2048
WorkInterval = 1000000
MaxRequestCount = 8
MaxIOCQueue = 16
MaxIOSQueue = 16
WRRHigh = 2
WRRMedium = 2
DefaultNamespace = 1
LBASize = 512
EnableDiskImage = 0
StrictSizeCheck = 0
DiskImageFile1 = nvme.img
UseCopyOnWriteDisk = 0

[pal]
Channel = 8
Package = 4
Die = 2
Plane = 2
Block = 512
Page = 512
PageSize = 16384
EnableMultiPlaneOperation = 1
NANDType = 1
LSBRead = 40000000
LSBWrite = 500000000
CSBRead = 0
CSBWrite = 0
MSBRead = 65000000
MSBWrite = 1300000000
Erase = 3500000000
DMASpeed = 400
DMAWidth = 8
SuperblockSize = C
PageAllocation = CWDP

[ftl]
MappingMode = 0
OverProvisioningRatio = 0.25
EraseThreshold = 100000
FillingMode = 0
FillRatio = 0.0
MinFillBytes = 0
InvalidPageRatio = 0.0
EvictPolicy = 0
DChoiceParam = 3
GCThreshold = 0.05
GCMode = 0
GCReclaimBlocks = 1
GCReclaimThreshold = 0.1
EnableRandomIOTweak = 1

[icl]
CacheSize = 536870912
CacheWaySize = 8
EnableReadCache = 0
EnableReadPrefetch = 0
ReadPrefetchMode = 1
ReadPrefetchCount = 3
ReadPrefetchRatio = 0.25
EnableWriteCache = 1
EvictPolicy = 2
EvictMode = 1
CacheLatency = 10

[dram]
Model = 0
Channel = 1
Rank = 1
Bank = 8
Chip = 1
BusWidth = 32
BurstLength = 8
ChipSize = 1073741824
PageSize = 4096
)CFG";

  std::string read_device_config(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      throw std::runtime_error("SimSSD: cannot read device config file: " + path);
    }
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  // Materialize the effective device config into a temp file (SimpleSSD only
  // accepts a config file path).  A repeated MinFillBytes key is safe: the FTL
  // config parser keeps the maximum value.
  std::string write_device_config(const std::string &contents) {
    char path[] = "/tmp/pipeann_simssd_device_XXXXXX.cfg";
    int fd = mkstemps(path, 4);
    if (fd < 0) {
      throw std::runtime_error("SimSSD: cannot create temp config file");
    }
    std::string ret(path);
    ssize_t len = (ssize_t) contents.size();
    if (::write(fd, contents.data(), len) != len) {
      ::close(fd);
      throw std::runtime_error("SimSSD: cannot write temp config file");
    }
    ::close(fd);
    return ret;
  }

  std::string materialize_device_config(const SimSSDReaderConfig &cfg) {
    std::string contents =
        cfg.device_config.empty() ? std::string(kDefaultDeviceConfig) : read_device_config(cfg.device_config);
    if (cfg.min_fill_bytes > 0) {
      contents += "\n[ftl]\nMinFillBytes = " + std::to_string(cfg.min_fill_bytes) + "\n";
    }
    return write_device_config(contents);
  }

  // ---------------------------------------------------------------------------
  // Global simulator engine (one simulated SSD per process).
  // ---------------------------------------------------------------------------
  class SimSSDEngine {
   public:
    static SimSSDEngine &instance(const SimSSDReaderConfig &cfg) {
      static SimSSDEngine *eng = new SimSSDEngine(cfg);
      return *eng;
    }

    // Submit a request to the simulated SSD. Must NOT hold mu_.
    void submit(BIL::BIO_TYPE type, uint64_t offset, uint64_t len, IORequest *req) {
      std::lock_guard<std::mutex> guard(mu_);
      submit_locked(type, offset, len, req);
    }

    void submit_locked(BIL::BIO_TYPE type, uint64_t offset, uint64_t len, IORequest *req) {
      BIL::BIO bio;
      bio.id = next_id_++;
      bio.type = type;
      bio.offset = offset;
      bio.length = len;
      bio.callback = [this](uint64_t id) {
        // Called from drive() while mu_ is already held by the driving thread.
        auto it = id_to_req_.find(id);
        if (it != id_to_req_.end()) {
          done_tick_[it->second] = engine_.getCurrentTick();
          id_to_req_.erase(it);
        }
      };
      submit_tick_[req] = engine_.getCurrentTick();
      id_to_req_[bio.id] = req;
      bio_entry_->submitIO(bio);
    }

    // Advance the simulated SSD by one event. Returns false if no event left.
    bool drive() {
      std::lock_guard<std::mutex> guard(mu_);
      return engine_.doNextEvent();
    }

    // If req has completed in simulation, return true and set latency_ps.
    bool try_complete(IORequest *req, uint64_t &latency_ps) {
      std::lock_guard<std::mutex> guard(mu_);
      return try_complete_locked(req, latency_ps);
    }

    // Drive the simulator until the simulated latency of `req` is known.
    // The engine mutex is held for the whole loop: driving is pure CPU work
    // (microseconds), and this avoids lock thrashing between search threads.
    void drive_until_done(IORequest *req, uint64_t &latency_ps) {
      std::lock_guard<std::mutex> guard(mu_);
      uint64_t idle = 0;
      while (!try_complete_locked(req, latency_ps)) {
        if (!engine_.doNextEvent()) {
          // The NVMe controller keeps periodic events alive, so the queue
          // should never run empty while a request is in flight; guard anyway.
          if (++idle > 1000000ULL) {
            throw std::runtime_error("SimSSD: simulator made no progress");
          }
        }
      }
    }

    // Variant of try_complete() with mu_ already held (also used by the
    // completion callback context).
    bool try_complete_locked(IORequest *req, uint64_t &latency_ps) {
      auto dit = done_tick_.find(req);
      if (dit == done_tick_.end()) {
        return false;
      }
      latency_ps = dit->second - submit_tick_[req];
      done_tick_.erase(dit);
      submit_tick_.erase(req);
      return true;
    }

    double latency_scale = 1.0;
    bool verbose = false;

   private:
    explicit SimSSDEngine(const SimSSDReaderConfig &cfg) {
      latency_scale = cfg.latency_scale;
      verbose = cfg.verbose;

      std::string dev_cfg = materialize_device_config(cfg);

      // Silence SimpleSSD log output (pass nullptr streams).
      // NOTE: ssd_conf_ must outlive the driver -- SimpleSSD components hold
      // pointers into this ConfigReader, so it must be a member (not a local).
      ssd_conf_ = std::make_unique<SimpleSSD::ConfigReader>(::initSimpleSSDEngine(&engine_, nullptr, nullptr, dev_cfg));

      if (cfg.use_nvme_interface) {
        driver_ = new SIL::NVMe::Driver(engine_, *ssd_conf_);
      } else {
        driver_ = new SIL::None::Driver(engine_, *ssd_conf_);
      }

      // sim_conf_ keeps default values (noop scheduler); BlockIOEntry only
      // consults it for the scheduler choice.
      bio_entry_ = new BIL::BlockIOEntry(sim_conf_, engine_, driver_, nullptr);

      bool began = false;
      std::function<void()> begin_cb = [&began]() { began = true; };
      driver_->init(begin_cb);
      uint64_t guard = 0;
      while (!began && engine_.doNextEvent()) {
        if (++guard > 100000000ULL) {
          throw std::runtime_error("SimSSD: SSD controller initialization timeout");
        }
      }
      if (!began) {
        throw std::runtime_error("SimSSD: SSD controller initialization failed");
      }

      uint64_t bytesize = 0;
      uint32_t lba = 0;
      driver_->getInfo(bytesize, lba);
      if (verbose) {
        std::cerr << "[SimSSD] simulated SSD ready: capacity=" << bytesize << " bytes, LBA=" << lba
                  << " bytes, interface=" << (cfg.use_nvme_interface ? "nvme" : "none")
                  << ", latency_scale=" << latency_scale << ", min_fill_bytes=" << cfg.min_fill_bytes << std::endl;
      }
    }

    Engine engine_;
    ConfigReader sim_conf_;
    std::unique_ptr<SimpleSSD::ConfigReader> ssd_conf_;
    BIL::DriverInterface *driver_ = nullptr;
    BIL::BlockIOEntry *bio_entry_ = nullptr;
    std::mutex mu_;
    uint64_t next_id_ = 1;
    std::unordered_map<uint64_t, IORequest *> id_to_req_;
    std::unordered_map<IORequest *, uint64_t> submit_tick_;
    std::unordered_map<IORequest *, uint64_t> done_tick_;
  };

  // Per-thread context (mirrors the io_uring thread_local pattern).
  struct SimSSDContext {
    struct Pending {
      IORequest *req;
      std::chrono::steady_clock::time_point submit_wall;
      bool has_deadline = false;
      std::chrono::steady_clock::time_point deadline;
    };
    std::deque<Pending> pending;
  };

  namespace simctx {
    static thread_local SimSSDContext *tls_ctx = nullptr;
  }

  SimSSDContext *get_tls_ctx() {
    if (simctx::tls_ctx == nullptr) {
      simctx::tls_ctx = new SimSSDContext();
    }
    return simctx::tls_ctx;
  }

  // Convert a simulated latency (ps) into a wall-clock deadline.
  std::chrono::steady_clock::time_point make_deadline(std::chrono::steady_clock::time_point submit_wall,
                                                      uint64_t latency_ps, double scale) {
    double ns = (double) latency_ps / 1000.0 * scale;
    return submit_wall + std::chrono::nanoseconds((int64_t) ns);
  }

  void full_pread(int fd, void *buf, uint64_t len, uint64_t offset) {
    char *p = (char *) buf;
    uint64_t done = 0;
    while (done < len) {
      ssize_t ret = ::pread(fd, p + done, len - done, offset + done);
      if (ret <= 0) {
        if (ret < 0 && errno == EINTR) {
          continue;
        }
        LOG(ERROR) << "SimSSD: pread failed at offset " << offset << " len " << len << ": "
                   << (ret < 0 ? strerror(errno) : "unexpected EOF");
        throw std::runtime_error("SimSSD: pread failed");
      }
      done += (uint64_t) ret;
    }
  }

  void full_pwrite(int fd, const void *buf, uint64_t len, uint64_t offset) {
    const char *p = (const char *) buf;
    uint64_t done = 0;
    while (done < len) {
      ssize_t ret = ::pwrite(fd, p + done, len - done, offset + done);
      if (ret <= 0) {
        if (ret < 0 && errno == EINTR) {
          continue;
        }
        LOG(ERROR) << "SimSSD: pwrite failed at offset " << offset << " len " << len << ": "
                   << (ret < 0 ? strerror(errno) : "write returned 0");
        throw std::runtime_error("SimSSD: pwrite failed");
      }
      done += (uint64_t) ret;
    }
  }

}  // namespace

SimSSDReaderConfig SimSSDReaderConfig::from_env() {
  SimSSDReaderConfig cfg;
  if (const char *e = getenv("PIPEANN_SIMSSD_DEVICE_CONFIG")) {
    cfg.device_config = e;
  }
  if (const char *e = getenv("PIPEANN_SIMSSD_LATENCY_SCALE")) {
    cfg.latency_scale = std::stod(e);
  }
  if (const char *e = getenv("PIPEANN_SIMSSD_INTERFACE")) {
    cfg.use_nvme_interface = !(std::string(e) == "none" || std::string(e) == "0");
  }
  if (const char *e = getenv("PIPEANN_SIMSSD_VERBOSE")) {
    cfg.verbose = (std::string(e) == "1" || std::string(e) == "true");
  }
  return cfg;
}

SimSSDFileReader::SimSSDFileReader(SimSSDReaderConfig cfg) : config(std::move(cfg)) {
  // Delay engine initialization until open(): the index file size determines
  // the minimum FTL fill range needed to model NAND reads for this file.
}

SimSSDFileReader::~SimSSDFileReader() {
  if (file_desc >= 0) {
    ::close(file_desc);
  }
}

void *SimSSDFileReader::get_ctx() {
  return get_tls_ctx();
}

void SimSSDFileReader::register_thread() {
  get_tls_ctx();
}

void SimSSDFileReader::deregister_thread() {
  delete simctx::tls_ctx;
  simctx::tls_ctx = nullptr;
}

void SimSSDFileReader::deregister_all_threads() {
}

void SimSSDFileReader::register_buf(void *, uint64_t, int) {
}

void SimSSDFileReader::open(const std::string &fname, bool enable_writes, bool enable_create) {
  int flags = O_LARGEFILE | (enable_writes ? O_RDWR : O_RDONLY);
  if (enable_create) {
    flags |= O_CREAT;
  }
  file_desc = ::open(fname.c_str(), flags, 0644);
  if (file_desc < 0) {
    // Fallback: the index directory may live on a read-only mount.
    file_desc = ::open(fname.c_str(), O_LARGEFILE | O_RDONLY);
  }
  if (file_desc < 0) {
    LOG(ERROR) << "SimSSD: open failed for " << fname << ": " << strerror(errno);
    throw std::runtime_error("SimSSD: open failed");
  }

  struct stat st;
  if (::fstat(file_desc, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 &&
      (uint64_t) st.st_size > config.min_fill_bytes) {
    config.min_fill_bytes = (uint64_t) st.st_size;
  }

  // Initialize only after min_fill_bytes is known.  The engine is process-wide;
  // subsequent calls return the already initialized instance.
  SimSSDEngine::instance(config);
}

void SimSSDFileReader::close() {
  if (file_desc >= 0) {
    ::close(file_desc);
    file_desc = -1;
  }
  if (config.verbose && (n_reads + n_writes) > 0) {
    std::cerr << "[SimSSD] reads=" << n_reads << " writes=" << n_writes
              << " avg_sim_latency=" << (double) sum_latency_ns / (n_reads + n_writes) / 1000.0 << " us" << std::endl;
  }
}

namespace {

  // Submit one request: fetch real bytes immediately, then register with the
  // simulated SSD for timing.
  void submit_one(SimSSDEngine &eng, int fd, IORequest &req, void *ctx, bool write, uint64_t &sum_latency_ns) {
    (void) sum_latency_ns;
    if (write) {
      full_pwrite(fd, req.buf, req.len, req.offset);
    } else {
      full_pread(fd, req.buf, req.len, req.offset);
    }

    SimSSDContext *c = (SimSSDContext *) ctx;
    SimSSDContext::Pending p;
    p.req = &req;
    p.submit_wall = std::chrono::steady_clock::now();
    req.finished = false;
    eng.submit(write ? BIL::BIO_WRITE : BIL::BIO_READ, req.offset, req.len, &req);
    c->pending.push_back(p);
  }

  // Refresh deadlines of pending requests whose simulated latency is known,
  // driving the simulator if needed.
  void refresh_deadlines(SimSSDEngine &eng, SimSSDContext *c) {
    for (auto &p : c->pending) {
      if (p.has_deadline) {
        continue;
      }
      uint64_t latency_ps = 0;
      if (!eng.try_complete(p.req, latency_ps)) {
        // Not computed yet: drive the simulator until it completes.
        // NOTE: try_complete() consumes the record, so use the out-param.
        eng.drive_until_done(p.req, latency_ps);
      }
      p.has_deadline = true;
      p.deadline = make_deadline(p.submit_wall, latency_ps, eng.latency_scale);
    }
  }

  // Mark every pending request whose wall-clock deadline has passed.
  // Returns number of requests completed by this call.
  int reap_matured(SimSSDContext *c, uint64_t &sum_latency_ns) {
    auto now = std::chrono::steady_clock::now();
    int n = 0;
    for (auto it = c->pending.begin(); it != c->pending.end();) {
      if (it->has_deadline && now >= it->deadline) {
        sum_latency_ns +=
            (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(it->deadline - it->submit_wall).count();
        it->req->finished = true;
        it = c->pending.erase(it);
        n++;
      } else {
        ++it;
      }
    }
    return n;
  }

}  // namespace

void SimSSDFileReader::send_io(IORequest &req, void *ctx, bool write) {
  SimSSDEngine &eng = SimSSDEngine::instance(config);
  submit_one(eng, file_desc, req, ctx, write, sum_latency_ns);
  if (write) {
    n_writes++;
  } else {
    n_reads++;
  }
}

void SimSSDFileReader::send_io(std::vector<IORequest> &reqs, void *ctx, bool write) {
  for (auto &r : reqs) {
    send_io(r, ctx, write);
  }
}

int SimSSDFileReader::poll(void *ctx) {
  SimSSDEngine &eng = SimSSDEngine::instance(config);
  SimSSDContext *c = (SimSSDContext *) ctx;
  if (c->pending.empty()) {
    return -1;
  }
  refresh_deadlines(eng, c);
  return reap_matured(c, sum_latency_ns) > 0 ? 0 : -1;
}

void SimSSDFileReader::poll_all(void *ctx) {
  SimSSDEngine &eng = SimSSDEngine::instance(config);
  SimSSDContext *c = (SimSSDContext *) ctx;
  refresh_deadlines(eng, c);
  reap_matured(c, sum_latency_ns);
}

void SimSSDFileReader::poll_wait(void *ctx) {
  SimSSDEngine &eng = SimSSDEngine::instance(config);
  SimSSDContext *c = (SimSSDContext *) ctx;
  if (c->pending.empty()) {
    return;
  }
  refresh_deadlines(eng, c);
  // Wait until the earliest deadline, then reap everything matured.
  auto earliest = std::chrono::steady_clock::time_point::max();
  for (auto &p : c->pending) {
    if (p.has_deadline && p.deadline < earliest) {
      earliest = p.deadline;
    }
  }
  auto now = std::chrono::steady_clock::now();
  if (earliest > now) {
    std::this_thread::sleep_until(earliest);
  }
  reap_matured(c, sum_latency_ns);
}

void SimSSDFileReader::read(std::vector<IORequest> &read_reqs, void *ctx, bool async) {
  (void) async;
  for (auto &r : read_reqs) {
    send_io(r, ctx, false);
  }
  while (true) {
    bool all_done = true;
    for (auto &r : read_reqs) {
      if (!r.finished) {
        all_done = false;
        break;
      }
    }
    if (all_done) {
      break;
    }
    poll_wait(ctx);
  }
}

void SimSSDFileReader::write(std::vector<IORequest> &write_reqs, void *ctx, bool async) {
  (void) async;
  for (auto &r : write_reqs) {
    send_io(r, ctx, true);
  }
  while (true) {
    bool all_done = true;
    for (auto &r : write_reqs) {
      if (!r.finished) {
        all_done = false;
        break;
      }
    }
    if (all_done) {
      break;
    }
    poll_wait(ctx);
  }
}

void SimSSDFileReader::read_fd(int fd, std::vector<IORequest> &read_reqs, void *ctx) {
  SimSSDEngine &eng = SimSSDEngine::instance(config);
  for (auto &r : read_reqs) {
    submit_one(eng, fd, r, ctx, false, sum_latency_ns);
    n_reads++;
  }
  while (true) {
    bool all_done = true;
    for (auto &r : read_reqs) {
      if (!r.finished) {
        all_done = false;
        break;
      }
    }
    if (all_done) {
      break;
    }
    poll_wait(ctx);
  }
}

void SimSSDFileReader::write_fd(int fd, std::vector<IORequest> &write_reqs, void *ctx) {
  SimSSDEngine &eng = SimSSDEngine::instance(config);
  for (auto &r : write_reqs) {
    submit_one(eng, fd, r, ctx, true, sum_latency_ns);
    n_writes++;
  }
  while (true) {
    bool all_done = true;
    for (auto &r : write_reqs) {
      if (!r.finished) {
        all_done = false;
        break;
      }
    }
    if (all_done) {
      break;
    }
    poll_wait(ctx);
  }
}

#else  // !PIPEANN_WITH_SIMSSD

SimSSDReaderConfig SimSSDReaderConfig::from_env() {
  return SimSSDReaderConfig();
}

SimSSDFileReader::SimSSDFileReader(SimSSDReaderConfig) {
  throw std::runtime_error(
      "SimSSDFileReader was not compiled in. Rebuild with "
      "-DPIPEANN_WITH_SIMSSD=ON and third_party/SimpleSSD-Standalone present.");
}
SimSSDFileReader::~SimSSDFileReader() {
}
void *SimSSDFileReader::get_ctx() {
  return nullptr;
}
void SimSSDFileReader::register_thread() {
}
void SimSSDFileReader::deregister_thread() {
}
void SimSSDFileReader::deregister_all_threads() {
}
void SimSSDFileReader::register_buf(void *, uint64_t, int) {
}
void SimSSDFileReader::open(const std::string &, bool, bool) {
}
void SimSSDFileReader::close() {
}
void SimSSDFileReader::read(std::vector<IORequest> &, void *, bool) {
}
void SimSSDFileReader::write(std::vector<IORequest> &, void *, bool) {
}
void SimSSDFileReader::read_fd(int, std::vector<IORequest> &, void *) {
}
void SimSSDFileReader::write_fd(int, std::vector<IORequest> &, void *) {
}
void SimSSDFileReader::send_io(IORequest &, void *, bool) {
}
void SimSSDFileReader::send_io(std::vector<IORequest> &, void *, bool) {
}
int SimSSDFileReader::poll(void *) {
  return -1;
}
void SimSSDFileReader::poll_all(void *) {
}
void SimSSDFileReader::poll_wait(void *) {
}

#endif  // PIPEANN_WITH_SIMSSD
