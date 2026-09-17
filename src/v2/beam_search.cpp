// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include "aligned_file_reader.h"
#include "libcuckoo/cuckoohash_map.hh"
#include "logger.h"
#include "pq_flash_index.h"
#include <malloc.h>
#include <algorithm>
#include <filesystem>

#include <omp.h>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <tuple>
#include "timer.h"
#include "tsl/robin_map.h"
#include "utils.h"

#include <unistd.h>
#include <sys/syscall.h>
#include "linux_aligned_file_reader.h"

namespace diskann {
  template<typename T, typename TagT>
  void PQFlashIndex<T, TagT>::disk_iterate_to_fixed_point_v(
      const T *query1, uint32_t mem_L, uint32_t l_search, const uint32_t beam_width,
      std::vector<Neighbor> &expanded_nodes_info, tsl::robin_map<uint32_t, T *> *coord_map, QueryStats *stats,
      ThreadData<T> *passthrough_data, tsl::robin_set<uint32_t> *exclude_nodes /* tags */, bool dyn_search_l) {
    uint32_t original_l_search = l_search;
    auto diskSearchBegin = std::chrono::high_resolution_clock::now();
    ThreadData<T> data;
    if (passthrough_data == nullptr) {
      data = this->thread_data.pop();
      while (data.scratch.sector_scratch == nullptr) {
        this->thread_data.wait_for_push_notify();
        data = this->thread_data.pop();
      }
    } else {
      data = *passthrough_data;
    }

    if (data_is_normalized) {
      // Data has been normalized. Normalize search vector too.
      float norm = diskann::compute_l2_norm(query1, this->data_dim);
      for (uint32_t i = 0; i < this->data_dim; i++) {
        data.scratch.aligned_query_float[i] = query1[i] / norm;
      }
      for (uint32_t i = 0; i < this->data_dim; i++) {
        data.scratch.aligned_query_T[i] = (T) data.scratch.aligned_query_float[i];
      }
    } else {
      for (uint32_t i = 0; i < this->data_dim; i++) {
        data.scratch.aligned_query_float[i] = query1[i];
      }
      memcpy(data.scratch.aligned_query_T, query1, this->data_dim * sizeof(T));
    }
    const T *query = data.scratch.aligned_query_T;
    const float *query_float = data.scratch.aligned_query_float;

    void *ctx = data.ctx;
    auto query_scratch = &(data.scratch);

    // reset query
    query_scratch->reset();

    // scratch space to compute distances between FP32 Query and INT8 data
    float *scratch = query_scratch->aligned_scratch;
    _mm_prefetch((char *) scratch, _MM_HINT_T0);

    // pointers to buffers for data
    T *data_buf = query_scratch->coord_scratch;
    _u64 &data_buf_idx = query_scratch->coord_idx;
    _mm_prefetch((char *) data_buf, _MM_HINT_T1);

    // sector scratch
    char *sector_scratch = query_scratch->sector_scratch;
    _u64 &sector_scratch_idx = query_scratch->sector_idx;

    // query <-> neighbor list
    float *dist_scratch = query_scratch->aligned_dist_scratch;
    _u8 *pq_coord_scratch = query_scratch->aligned_pq_coord_scratch;

    Timer query_timer, io_timer, cpu_timer;
    // 0. init: build the PQ distance table (counted in total latency)
    auto init_st = std::chrono::high_resolution_clock::now();
    // query <-> PQ chunk centers distances
    float *pq_dists = query_scratch->aligned_pqtable_dist_scratch;
    pq_table.populate_chunk_distances(query, pq_dists);
    if (stats != nullptr) {
      stats->init_us = (double) std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::high_resolution_clock::now() - init_st)
                           .count();
    }

    // lambda to batch compute query<-> node distances in PQ space
    auto compute_dists = [this, pq_coord_scratch, pq_dists](const unsigned *ids, const _u64 n_ids, float *dists_out) {
      ::aggregate_coords(ids, n_ids, this->data.data(), this->n_chunks, pq_coord_scratch);
      ::pq_dist_lookup(pq_coord_scratch, n_ids, this->n_chunks, pq_dists, dists_out);
    };

    // 1. entry-point search (medoid selection / in-memory index search + initial retset)
    auto head_st = std::chrono::high_resolution_clock::now();
    std::vector<Neighbor> retset;
    retset.resize(4096);
    tsl::robin_set<_u64> visited(4096);

    // re-naming `expanded_nodes_info` to not change rest of the code
    std::vector<Neighbor> &full_retset = expanded_nodes_info;
    full_retset.reserve(4096);
    _u32 best_medoid = 0;
    float best_dist = (std::numeric_limits<float>::max)();
    std::vector<SimpleNeighbor> medoid_dists;
    for (_u64 cur_m = 0; cur_m < num_medoids; cur_m++) {
      float cur_expanded_dist =
          dist_cmp_float->compare(query_float, centroid_data + aligned_dim * cur_m, (unsigned) aligned_dim);
      if (cur_expanded_dist < best_dist) {
        best_medoid = medoids[cur_m];
        best_dist = cur_expanded_dist;
      }
    }

    unsigned cur_list_size = 0;
    auto compute_and_add_to_retset = [&](const unsigned *node_ids, const _u64 n_ids) {
      compute_dists(node_ids, n_ids, dist_scratch);
      for (_u64 i = 0; i < n_ids; ++i) {
        retset[cur_list_size].id = node_ids[i];
        retset[cur_list_size].distance = dist_scratch[i];
        retset[cur_list_size++].flag = true;
        visited.insert(node_ids[i]);
      }
    };

    if (mem_L) {
      std::vector<unsigned> mem_tags(mem_L);
      std::vector<float> mem_dists(mem_L);
      mem_index_->search_with_tags(query, mem_L, mem_L, mem_tags.data(), mem_dists.data());
      compute_and_add_to_retset(mem_tags.data(), std::min((unsigned) mem_L, (unsigned) l_search));
    } else {
      // Do not use optimized start point.
      compute_and_add_to_retset(&best_medoid, 1);
    }

    std::sort(retset.begin(), retset.begin() + cur_list_size);
    if (stats != nullptr) {
      stats->head_us = (double) std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::high_resolution_clock::now() - head_st)
                           .count();
    }

    unsigned cmps = 0;
    unsigned hops = 0;
    unsigned num_ios = 0;
    unsigned k = 0;

    // cleared every iteration
    std::vector<unsigned> frontier;
    using fnhood_t = std::tuple<unsigned, unsigned, char *>;
    std::vector<fnhood_t> frontier_nhoods;
    std::vector<IORequest> frontier_read_reqs;

    // 2. main loop begins
    auto loop_st = std::chrono::high_resolution_clock::now();
    while (k < cur_list_size && num_ios < IO_LIMIT) {
      auto nk = cur_list_size;
      // clear iteration state
      frontier.clear();
      frontier_nhoods.clear();
      frontier_read_reqs.clear();
      sector_scratch_idx = 0;
      // find new beam
      // WAS: _u64 marker = k - 1;
      _u32 marker = k;
      _u32 num_seen = 0;
      while (marker < cur_list_size && frontier.size() < beam_width && num_seen < beam_width) {
        if (retset[marker].flag) {
          num_seen++;
          frontier.push_back(retset[marker].id);
          retset[marker].flag = false;
          if (this->count_visited_nodes) {
            reinterpret_cast<std::atomic<_u32> &>(this->node_visit_counter[retset[marker].id].second).fetch_add(1);
          }
        }
        marker++;
      }

      // read nhoods of frontier ids
      if (!frontier.empty()) {
        if (stats != nullptr)
          stats->n_hops++;
        for (_u64 i = 0; i < frontier.size(); i++) {
          uint32_t id = frontier[i];
          uint32_t loc = this->id2loc(id);
          uint64_t offset = loc_sector_no(loc) * SECTOR_LEN;
          auto sector_buf = sector_scratch + sector_scratch_idx * size_per_io;
          fnhood_t fnhood = std::make_tuple(id, loc, sector_buf);
          sector_scratch_idx++;
          frontier_nhoods.push_back(fnhood);
          frontier_read_reqs.emplace_back(IORequest(offset, size_per_io, sector_buf, u_loc_offset(loc), max_node_len));
          if (stats != nullptr) {
            stats->n_4k++;
            stats->n_ios++;
          }
          num_ios++;
        }
        io_timer.reset();

        reader->read(frontier_read_reqs, ctx);

        if (stats != nullptr) {
          stats->io_us += (double) io_timer.elapsed();
        }
      }

      for (auto &frontier_nhood : frontier_nhoods) {
        auto [id, loc, sector_buf] = frontier_nhood;
        char *node_disk_buf = offset_to_loc(sector_buf, loc);
        unsigned *node_buf = offset_to_node_nhood(node_disk_buf);
        _u64 nnbrs = (_u64) (*node_buf);
        T *node_fp_coords = offset_to_node_coords(node_disk_buf);
        assert(data_buf_idx < MAX_N_CMPS);

        T *node_fp_coords_copy = data_buf + (data_buf_idx * aligned_dim);
        data_buf_idx++;
        cpu_timer.reset();
        memcpy(node_fp_coords_copy, node_fp_coords, data_dim * sizeof(T));
        float cur_expanded_dist = dist_cmp->compare(query, node_fp_coords_copy, (unsigned) aligned_dim);
        if (stats != nullptr) {
          stats->cpu_us += (double) cpu_timer.elapsed();  // exact distance computation
        }

        uint32_t tag = 0;
        if (likely(no_mapping)) {
          tag = id;
        } else {
          if (!tags.find(id, tag)) {
            LOG(ERROR) << "Tag not found for ID " << id;
          }
        }

        if (exclude_nodes == nullptr || exclude_nodes->find(tag) == exclude_nodes->end()) {
          // added for StreamingMerger calls
          if (coord_map != nullptr) {
            coord_map->insert(std::make_pair(id, node_fp_coords_copy));
          }
          full_retset.push_back(Neighbor(id, cur_expanded_dist, true));
        }

        unsigned *node_nbrs = (node_buf + 1);

        // compute node_nbrs <-> query dist in PQ space
        cpu_timer.reset();
        compute_dists(node_nbrs, nnbrs, dist_scratch);
        if (stats != nullptr) {
          stats->n_cmps += (double) nnbrs;
          stats->cpu_us += (double) cpu_timer.elapsed();
        }

        cpu_timer.reset();
        // process prefetch-ed nhood
        for (_u64 m = 0; m < nnbrs; ++m) {
          unsigned id = node_nbrs[m];
          if (unlikely(id > this->cur_id)) {
            LOG(ERROR) << "ID is larger than current ID, " << id << " vs " << this->cur_id;
            crash();
          }
          if (visited.find(id) != visited.end()) {
            continue;
          } else {
            visited.insert(id);
            cmps++;
            float dist = dist_scratch[m];
            if (stats != nullptr) {
              stats->n_cmps++;
            }
            if (dist >= retset[cur_list_size - 1].distance && (cur_list_size == l_search))
              continue;
            Neighbor nn(id, dist, true);
            // variable search_L for deleted nodes.
            // Return position in sorted list where nn inserted.

            auto r = InsertIntoPool(retset.data(), cur_list_size, nn);

            if (cur_list_size < l_search) {
              ++cur_list_size;
            }

            if (r < nk)
              nk = r;  // nk logs the best position in the retset that was
                       // updated due to neighbors of n.
          }
        }

        if (dyn_search_l) {
          // TODO(gh): contention still exists in tags.find(x)
          // O(n), but it is not slow as L is typically smaller than 300.
          // l_search monotonically increases to handle deleted nodes.
          _u32 tot = 0, cur = 0;
          for (cur = 0; cur < cur_list_size; ++cur) {
            uint32_t tag = 0;
            if (!tags.find(retset[cur].id, tag)) {
              LOG(ERROR) << "Tag not found for ID " << retset[cur].id << ", tags.size() is " << tags.size();
            }
            if (exclude_nodes->find(tag) == exclude_nodes->end()) {
              ++tot;
              if (tot == original_l_search) {
                break;
              }
            }
          }
          // cur is the stopped index (cur + 1 is the length it should be)
          l_search = std::max(original_l_search, cur + 1);
        }

        if (stats != nullptr) {
          stats->cpu_us += (double) cpu_timer.elapsed();
        }
      }

      // update best inserted position
      //

      if (nk <= k)
        k = nk;  // k is the best position in retset updated in this round.
      else
        ++k;

      hops++;
      if (stats != nullptr && stats->n_current_used != 0) {
        auto diskSearchEnd = std::chrono::high_resolution_clock::now();
        double elapsedSeconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(diskSearchEnd - diskSearchBegin).count();
        if (elapsedSeconds >= stats->n_current_used)
          break;
      }
    }
    // 2. main loop ends -> loop time; cpu_us1 mirrors cpu_us so the CPU column shows compute time
    auto loop_ed = std::chrono::high_resolution_clock::now();
    if (stats != nullptr) {
      stats->cpu_us2 =
          (double) std::chrono::duration_cast<std::chrono::microseconds>(loop_ed - loop_st).count();
      stats->cpu_us1 = stats->cpu_us;
    }

    // 5. finalization: sort + return scratch + (result copy in the caller)
    auto final_st = loop_ed;
    // re-sort by distance
    std::sort(full_retset.begin(), full_retset.end(),
              [](const Neighbor &left, const Neighbor &right) { return left < right; });

    // return data to ConcurrentQueue only if popped from it
    if (passthrough_data == nullptr) {
      this->thread_data.push(data);
      this->thread_data.push_notify_all();
    }

    if (stats != nullptr) {
      stats->final_us = (double) std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::high_resolution_clock::now() - final_st)
                            .count();
      stats->total_us = (double) query_timer.elapsed();
    }
  }

  template<typename T, typename TagT>
  size_t PQFlashIndex<T, TagT>::beam_search(const T *query, const _u64 k_search, const _u64 l_search, TagT *res_tags,
                                            float *distances, const _u64 beam_width, QueryStats *stats,
                                            const _u32 mem_L, tsl::robin_set<uint32_t> *deleted_nodes,
                                            bool dyn_search_l) {
    // iterate to fixed point
    std::vector<Neighbor> expanded_nodes_info;
    this->disk_iterate_to_fixed_point_v(query, mem_L, (_u32) l_search, (_u32) beam_width, expanded_nodes_info, nullptr,
                                        stats, nullptr, deleted_nodes, dyn_search_l);
    // result copy: account it in finalization time and total latency
    auto copy_st = std::chrono::high_resolution_clock::now();
    _u64 res_count = 0;
    for (uint32_t i = 0; i < l_search && res_count < k_search && i < expanded_nodes_info.size(); i++) {
      if (likely(no_mapping)) {
        res_tags[res_count] = expanded_nodes_info[i].id;
      } else {
        tags.find(expanded_nodes_info[i].id, res_tags[res_count]);
      }
      distances[res_count] = expanded_nodes_info[i].distance;
      res_count++;
    }
    if (stats != nullptr) {
      double copy_us = (double) std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::high_resolution_clock::now() - copy_st)
                           .count();
      stats->final_us += copy_us;
      stats->total_us += copy_us;
    }
    return res_count;
  }

  // instantiations
  //   template class PQFlashIndex<float, int32_t>;
  //   template class PQFlashIndex<_s8, int32_t>;
  //   template class PQFlashIndex<_u8, int32_t>;
  template class PQFlashIndex<float, uint32_t>;
  template class PQFlashIndex<_s8, uint32_t>;
  template class PQFlashIndex<_u8, uint32_t>;
  //   template class PQFlashIndex<float, int64_t>;
  //   template class PQFlashIndex<_s8, int64_t>;
  //   template class PQFlashIndex<_u8, int64_t>;
  //   template class PQFlashIndex<float, uint64_t>;
  //   template class PQFlashIndex<_s8, uint64_t>;
  //   template class PQFlashIndex<_u8, uint64_t>;
}  // namespace diskann
