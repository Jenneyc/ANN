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

  int index = 2;
  std::string index_prefix_path(argv[index++]);
  std::string warmup_query_file = index_prefix_path + "_sample_data.bin";
  std::ignore = std::atoi(argv[index++]) != 0;
  std::ignore = std::atoi(argv[index++]);
  _u32 num_threads = std::atoi(argv[index++]);
  _u32 beamwidth = std::atoi(argv[index++]);
  std::string query_bin(argv[index++]);
  std::string truthset_bin(argv[index++]);
  _u64 recall_at = std::atoi(argv[index++]);
  std::string result_output_prefix(argv[index++]);
  std::string dist_metric(argv[index++]);
  int search_mode = std::atoi(argv[index++]);
  bool use_page_search = search_mode != 0;
  std::ignore = std::atoi(argv[index++]);

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

  diskann::load_bin<T>(query_bin, query, query_num, query_dim);
  // diskann::load_aligned_bin<T>(query_bin, query, query_num, query_dim, query_aligned_dim);

  if (file_exists(truthset_bin)) {
    diskann::load_truthset(truthset_bin, gt_ids, gt_dists, gt_num, gt_dim, &tags);
    if (gt_num != query_num) {
      diskann::cout << "Error. Mismatch in number of queries and ground truth data" << std::endl;
    }
    calc_recall_flag = true;
  }

  // Select the I/O backend: real SSD via io_uring (default) or the SimpleSSD
  // simulated SSD.  Use ReaderType::URING / ReaderType::SIM_SSD to hard-code,
  // or AUTO to honor the PIPEANN_READER_TYPE environment variable.
  std::shared_ptr<AlignedFileReader> reader = create_aligned_file_reader(ReaderType::AUTO);

  diskann::Index<T> _pFlashIndex(m, query_dim, (uint64_t) 1e8, false, false, false);
  _pFlashIndex.load_from_disk_index(index_prefix_path);

  LOG(INFO) << "Num threads: " << num_threads;
  omp_set_num_threads(num_threads);

  LOG(INFO) << "Use page search: " << use_page_search;

  std::vector<std::vector<uint32_t>> query_result_ids(Lvec.size());
  std::vector<std::vector<uint32_t>> query_result_tags(Lvec.size());
  std::vector<std::vector<float>> query_result_dists(Lvec.size());

  auto run_tests = [&](uint32_t test_id, bool output) {
    _u64 L = Lvec[test_id];

    query_result_ids[test_id].resize(recall_at * query_num);
    query_result_dists[test_id].resize(recall_at * query_num);
    query_result_tags[test_id].resize(recall_at * query_num);

    diskann::QueryStats *stats = new diskann::QueryStats[query_num];

    std::vector<uint64_t> query_result_tags_64(recall_at * query_num);
    std::vector<uint32_t> query_result_tags_32(recall_at * query_num);
    auto s = std::chrono::high_resolution_clock::now();
#pragma omp parallel for schedule(dynamic, 1)
    for (_s64 i = 0; i < (int64_t) query_num; i++) {
      auto s1 = std::chrono::high_resolution_clock::now();
      _pFlashIndex.search(query + (i * query_dim), recall_at, L, query_result_tags_32.data() + (i * recall_at),
                          query_result_dists[test_id].data() + (i * recall_at));
      auto e1 = std::chrono::high_resolution_clock::now();
      stats[i].total_us = std::chrono::duration_cast<std::chrono::microseconds>(e1 - s1).count();
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

    delete[] stats;

    float recall = 0;
    if (calc_recall_flag) {
      /* Attention: in SPACEV, there may be multiple vectors with the same distance,
         which may cause lower than expected recall@1 (?) */
      recall = (float) diskann::calculate_recall((_u32) query_num, gt_ids, gt_dists, (_u32) gt_dim,
                                                 query_result_tags[test_id].data(), (_u32) recall_at, (_u32) recall_at);
    }
    if (output) {
      diskann::cout << std::setw(6) << L << std::setw(12) << 1 << std::setw(12) << qps << std::setw(12) << mean_latency
                    << std::setw(12) << latency_999 << std::setw(12) << mean_hops << std::setw(12) << mean_ios;
      if (calc_recall_flag) {
        diskann::cout << std::setw(12) << recall << std::endl;
      }
    }
  };

  LOG(INFO) << "Warming up...";
  uint32_t prev_L = Lvec[0];
  Lvec[0] = 200;
  run_tests(0, false);
  run_tests(0, false);
  Lvec[0] = prev_L;
  LOG(INFO) << "Warming up finished.";

  diskann::cout.setf(std::ios_base::fixed, std::ios_base::floatfield);
  diskann::cout.precision(2);

  std::string recall_string = "Recall@" + std::to_string(recall_at);
  diskann::cout << std::setw(6) << "L" << std::setw(12) << "Beamwidth" << std::setw(12) << "QPS" << std::setw(12)
                << "AvgLat(us)" << std::setw(12) << "P99 Lat" << std::setw(12) << "Mean Hops" << std::setw(12)
                << "Mean IOs" << std::setw(12);
  if (calc_recall_flag) {
    diskann::cout << std::setw(12) << recall_string << std::endl;
  } else
    diskann::cout << std::endl;
  diskann::cout << "==============================================================="
                   "==========================================="
                << std::endl;

  for (uint32_t test_id = 0; test_id < Lvec.size(); test_id++) {
    run_tests(test_id, true);
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 14) {
    // tags == 1!
    diskann::cout << "Usage: " << argv[0]
                  << " <index_type (float/int8/uint8)>  <index_prefix_path>"
                     " <single_file_index(0/1)>"
                     " <num_nodes_to_cache>  <num_threads>  <beamwidth (use 0 to "
                     "optimize internally)> "
                     " <query_file.bin>  <truthset.bin (use \"null\" for none)> "
                     " <K>  <result_output_prefix> <similarity (cosine/l2)> "
                     " <use_page_search(0/1/2)> <mem_L> <L1> [L2] etc.  See README for "
                     "more information on parameters."
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
