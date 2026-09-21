// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include <atomic>
#include <cstring>
#include <iomanip>
#include <omp.h>
#include <pq_flash_index.h>
#include <set>
#include <string.h>
#include <time.h>

#include "distance.h"
#include "log.h"
#include "aux_utils.h"
#include "index.h"
#include "math_utils.h"
#include "memory_mapper.h"
#include "partition_and_pq.h"
#include "timer.h"
#include "utils.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include "linux_aligned_file_reader.h"
#include "reader_factory.h"

#define WARMUP false

void print_stats(std::string category, std::vector<float> percentiles, std::vector<float> results) {
  diskann::cout << std::setw(20) << category << ": " << std::flush;
  for (uint32_t s = 0; s < percentiles.size(); s++) {
    diskann::cout << std::setw(8) << percentiles[s] << "%";
  }
  diskann::cout << std::endl;
  diskann::cout << std::setw(22) << " " << std::flush;
  for (uint32_t s = 0; s < percentiles.size(); s++) {
    diskann::cout << std::setw(9) << results[s];
  }
  diskann::cout << std::endl;
}

template<typename T>
int search_disk_index(int argc, char **argv) {
  // load query bin
  T *query = nullptr;
  unsigned *gt_ids = nullptr;
  float *gt_dists = nullptr;
  uint32_t *tags = nullptr;
  size_t query_num, query_dim, gt_num, gt_dim;
  std::vector<_u64> Lvec;

  bool tags_flag = true;

  int index = 2;
  std::string index_prefix_path(argv[index++]);
  _u32 num_threads = std::atoi(argv[index++]);
  _u32 beamwidth = std::atoi(argv[index++]);
  std::string query_bin(argv[index++]);
  std::string truthset_bin(argv[index++]);
  _u64 recall_at = std::atoi(argv[index++]);
  std::string dist_metric(argv[index++]);
  int search_mode = std::atoi(argv[index++]);
  bool use_page_search = search_mode != 0;
  _u32 mem_L = std::atoi(argv[index++]);

  diskann::Metric m = dist_metric == "cosine" ? diskann::Metric::COSINE : diskann::Metric::L2;
  if (dist_metric != "l2" && m == diskann::Metric::L2) {
    diskann::cout << "Unknown distance metric: " << dist_metric << ". Using default(L2) instead." << std::endl;
  }

  std::string disk_index_tag_file = index_prefix_path + "_disk.index.tags";

  bool calc_recall_flag = false;

  for (int ctr = index; ctr < argc; ctr++) {
    _u64 curL = std::atoi(argv[ctr]);
    if (curL >= recall_at)
      Lvec.push_back(curL);
  }

  if (Lvec.size() == 0) {
    diskann::cout << "No valid Lsearch found. Lsearch must be at least recall_at" << std::endl;
    return -1;
  }

  diskann::cout << "Search parameters: #threads: " << num_threads << ", ";
  if (beamwidth <= 0)
    diskann::cout << "beamwidth to be optimized for each L value" << std::endl;
  else
    diskann::cout << " beamwidth: " << beamwidth << std::endl;

  // c: load query bin, gt
  diskann::load_bin<T>(query_bin, query, query_num, query_dim);
  // diskann::load_aligned_bin<T>(query_bin, query, query_num, query_dim, query_aligned_dim);

  if (file_exists(truthset_bin)) {
    diskann::load_truthset(truthset_bin, gt_ids, gt_dists, gt_num, gt_dim, &tags);
    if (gt_num != query_num) {
      diskann::cout << "Error. Mismatch in number of queries and ground truth data" << std::endl;
    }
    calc_recall_flag = true;
  }

  // c: load disk index
  // Select the I/O backend: real SSD via io_uring (default) or the SimpleSSD
  // simulated SSD.  Use ReaderType::URING / ReaderType::SIM_SSD to hard-code,
  // or AUTO to honor the PIPEANN_READER_TYPE environment variable.
  std::shared_ptr<AlignedFileReader> reader = create_aligned_file_reader(ReaderType::AUTO);

  std::unique_ptr<diskann::PQFlashIndex<T>> _pFlashIndex(
      new diskann::PQFlashIndex<T>(m, reader, SearchMode(search_mode), tags_flag));

  int res = _pFlashIndex->load(index_prefix_path.c_str(), num_threads, true, use_page_search);
  if (res != 0) {
    return res;
  }

  if (mem_L != 0) {
    auto mem_index_path = index_prefix_path + "_mem.index";
    LOG(INFO) << "Load memory index " << mem_index_path;
    _pFlashIndex->load_mem_index(m, query_dim, mem_index_path, num_threads, mem_L);
  }

  omp_set_num_threads(num_threads);

  std::vector<std::vector<uint32_t>> query_result_ids(Lvec.size());
  std::vector<std::vector<uint32_t>> query_result_tags(Lvec.size());
  std::vector<std::vector<float>> query_result_dists(Lvec.size());

  auto run_tests = [&](uint32_t test_id, bool output) {
    diskann::QueryStats *stats = new diskann::QueryStats[query_num];
    _u64 L = Lvec[test_id];

    query_result_ids[test_id].resize(recall_at * query_num);
    query_result_dists[test_id].resize(recall_at * query_num);
    query_result_tags[test_id].resize(recall_at * query_num);

    std::vector<uint64_t> query_result_tags_64(recall_at * query_num);
    std::vector<uint32_t> query_result_tags_32(recall_at * query_num);
    auto s = std::chrono::high_resolution_clock::now();

    if (search_mode == SearchMode::PIPE_SEARCH) {
#pragma omp parallel for schedule(dynamic, 1)
      for (_s64 i = 0; i < (int64_t) query_num; i++) {
        _pFlashIndex->pipe_search(query + (i * query_dim), (uint64_t) recall_at, mem_L, (uint64_t) L,
                                  query_result_tags_32.data() + (i * recall_at),
                                  query_result_dists[test_id].data() + (i * recall_at), (uint64_t) beamwidth,
                                  stats + i);
      }
    } else if (search_mode == SearchMode::PAGE_SEARCH) {
#pragma omp parallel for schedule(dynamic, 1)
      for (_s64 i = 0; i < (int64_t) query_num; i++) {
        _pFlashIndex->page_search(query + (i * query_dim), (uint64_t) recall_at, mem_L, (uint64_t) L,
                                  query_result_tags_32.data() + (i * recall_at),
                                  query_result_dists[test_id].data() + (i * recall_at), (uint64_t) beamwidth,
                                  stats + i);
      }
    } else if (search_mode == SearchMode::CORO_SEARCH) {
      constexpr uint64_t kBatchSize = 8;
      T *q[kBatchSize];
      uint32_t *res_tags[kBatchSize];
      float *res_dists[kBatchSize];
      int N;
#pragma omp parallel for schedule(dynamic, 1) private(q, res_tags, res_dists, N)
      for (_s64 i = 0; i < (int64_t) query_num; i += kBatchSize) {
        N = std::min(kBatchSize, query_num - i);
        for (int v = 0; v < N; ++v) {
          q[v] = query + ((i + v) * query_dim);
          res_tags[v] = query_result_tags_32.data() + ((i + v) * recall_at);
          res_dists[v] = query_result_dists[test_id].data() + ((i + v) * recall_at);
        }

        _pFlashIndex->coro_search(q, (uint64_t) recall_at, mem_L, (uint64_t) L, res_tags, res_dists,
                                  (uint64_t) beamwidth, N);
      }
    } else if (search_mode == SearchMode::BEAM_SEARCH) {
#pragma omp parallel for schedule(dynamic, 1)
      for (_s64 i = 0; i < (int64_t) query_num; i++) {
        _pFlashIndex->beam_search(query + (i * query_dim), (uint64_t) recall_at, (uint64_t) L,
                                  query_result_tags_32.data() + (i * recall_at),
                                  query_result_dists[test_id].data() + (i * recall_at), (uint64_t) beamwidth, stats + i,
                                  mem_L, nullptr, false);
      }
    } else {
      diskann::cout << "Unknown search mode: " << search_mode << std::endl;
      exit(-1);
    }

    auto e = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double> diff = e - s;
    float qps = (float) ((1.0 * (double) query_num) / (1.0 * (double) diff.count()));

    diskann::convert_types<uint32_t, uint32_t>(query_result_tags_32.data(), query_result_tags[test_id].data(),
                                               (size_t) query_num, (size_t) recall_at);

    float mean_latency = (float) diskann::get_mean_stats(
        stats, query_num, [](const diskann::QueryStats &stats) { return stats.total_us; });

    float latency_999 = (float) diskann::get_percentile_stats(
        stats, query_num, 0.999f, [](const diskann::QueryStats &stats) { return stats.total_us; });

    float mean_hops = (float) diskann::get_mean_stats(stats, query_num,
                                                      [](const diskann::QueryStats &stats) { return stats.n_hops; });

    float mean_ios =
        (float) diskann::get_mean_stats(stats, query_num, [](const diskann::QueryStats &stats) { return stats.n_ios; });

    float mean_head_us = (float) diskann::get_mean_stats(
        stats, query_num, [](const diskann::QueryStats &stats) { return stats.head_us; });
    float mean_init_us = (float) diskann::get_mean_stats(
        stats, query_num, [](const diskann::QueryStats &stats) { return stats.init_us; });
    float mean_loop_us = (float) diskann::get_mean_stats(
        stats, query_num, [](const diskann::QueryStats &stats) { return stats.cpu_us2; });
    float mean_io_us =
        (float) diskann::get_mean_stats(stats, query_num, [](const diskann::QueryStats &stats) { return stats.io_us; });
    float mean_cpu_us = (float) diskann::get_mean_stats(stats, query_num,
                                                        [](const diskann::QueryStats &stats) { return stats.cpu_us1; });
    float mean_final_us = (float) diskann::get_mean_stats(
        stats, query_num, [](const diskann::QueryStats &stats) { return stats.final_us; });

    delete[] stats;

    if (output) {
      float recall = 0;
      if (calc_recall_flag) {
        /* Attention: in SPACEV, there may be multiple vectors with the same distance,
          which may cause lower than expected recall@1 (?) */
        recall =
            (float) diskann::calculate_recall((_u32) query_num, gt_ids, gt_dists, (_u32) gt_dim,
                                              query_result_tags[test_id].data(), (_u32) recall_at, (_u32) recall_at);
      }

      diskann::cout << std::setw(6) << L << std::setw(12) << beamwidth << std::setw(12) << qps << std::setw(12)
                    << mean_latency << std::setw(12) << latency_999 << std::setw(12) << mean_hops << std::setw(12)
                    << mean_ios;
      if (calc_recall_flag) {
        diskann::cout << std::setw(12) << recall;
      }
      diskann::cout << std::setw(12) << mean_head_us << std::setw(12) << mean_init_us << std::setw(12) << mean_loop_us
                    << std::setw(12) << mean_io_us << std::setw(12) << mean_cpu_us << std::setw(12) << mean_final_us
                    << std::endl;
    }
  };

  LOG(INFO) << "Use two ANNS for warming up...";
  uint32_t prev_L = Lvec[0];
  Lvec[0] = 200;
  run_tests(0, false);
  run_tests(0, false);
  Lvec[0] = prev_L;
  LOG(INFO) << "Warming up finished.";

  diskann::cout.setf(std::ios_base::fixed, std::ios_base::floatfield);
  diskann::cout.precision(2);

  std::string recall_string = "Recall@" + std::to_string(recall_at);
  diskann::cout << std::setw(6) << "L" << std::setw(12) << "I/O Width" << std::setw(12) << "QPS" << std::setw(12)
                << "AvgLat(us)" << std::setw(12) << "P99 Lat" << std::setw(12) << "Mean Hops" << std::setw(12)
                << "Mean IOs" << std::setw(12);
  if (calc_recall_flag) {
    diskann::cout << std::setw(12) << recall_string;
  }
  diskann::cout << std::setw(12) << "Head(us)" << std::setw(12) << "Init(us)" << std::setw(12) << "Loop(us)"
                << std::setw(12) << "IO(us)" << std::setw(12) << "CPU(us)" << std::setw(12) << "Final(us)" << std::endl;
  diskann::cout << "=============================================="
                   "==========================================="
                << std::endl;

  for (uint32_t test_id = 0; test_id < Lvec.size(); test_id++) {
    run_tests(test_id, true);
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 12) {
    // tags == 1!
    diskann::cout << "Usage: " << argv[0]
                  << " <index_type (float/int8/uint8)>  <index_prefix_path>"
                     " <num_threads>  <pipeline width> "
                     " <query_file.bin>  <truthset.bin (use \"null\" for none)> "
                     " <K> <similarity (cosine/l2)> "
                     " <search_mode(0 for beam search / 1 for page search / 2 for pipe search)> <mem_L (0 means not "
                     "using mem index)> <L1> [L2] etc."
                  << std::endl;
    exit(-1);
  }

  if (std::string(argv[1]) == std::string("float"))
    search_disk_index<float>(argc, argv);
  else if (std::string(argv[1]) == std::string("int8"))
    search_disk_index<int8_t>(argc, argv);
  else if (std::string(argv[1]) == std::string("uint8"))
    search_disk_index<uint8_t>(argc, argv);
  else
    diskann::cout << "Unsupported index type. Use float or int8 or uint8" << std::endl;
}
