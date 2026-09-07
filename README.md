# [OSDI'25 Artifact] Achieving Low-Latency Graph-Based Vector Search via Aligning Best-First Search Algorithm with SSD

Welcome to the artifact repository of OSDI'25 accepted paper: *Achieving Low-Latency Graph-Based Vector Search via Aligning Best-First Search Algorithm with SSD*!
This repository contains the implementation of PipeANN, a **low-latency** and **billion-scale** vector search system on SSD, and scripts to reproduce the experiment results in our paper.

Should there be any questions, please **contact the authors in HotCRP** (or GitHub issue). The authors will respond to each question as soon as possible.

## Evaluate the Artifact

We have already set up the environment for AE reviewers on the provided platform.
Please see HotCRP for how to connect to it.

If you want to build from scratch on your platform, please refer to [Quick Start](#quick-start-with-a-disk-index-of-diskann) (for those who have on-disk indexes of DiskANN) or [Environment Setup](#start-from-scratch-environment-setup) (for those who start from scratch).

### Hello-World Example (Artifact Functional, eta: 1 minute)

First, compile PipeANN.

```bash
bash ./build.sh
```

To verify that everything is prepared, you can run a hello-world example that verifies PipeANN's functionality, please run the following command:

```bash
bash scripts/hello_world.sh
```

This script runs the binary compiled by `tests/search_disk_index.cpp`, which calls `pipe_search` in `src/v2/pipe_search.cpp` (the core logic of PipeANN lies here) for vector search.
You could get a similar output as below. Note that the QPS, latency, and Recall might differ slightly.
```
NOTE: this is a hello-world example for PipeANN. It searches top-10 on SIFT100M dataset.
Search parameters: #threads: 1,  beamwidth: 32
... some outputs during index loading ...
[search_disk_index.cpp:216:INFO] Use two ANNS for warming up...
[search_disk_index.cpp:219:INFO] Warming up finished.
     L   I/O Width         QPS    Mean Lat     P99 Lat   Mean Hops    Mean IOs   Recall@10
=========================================================================================
    10          32     1952.03      490.99     3346.00        0.00       22.28       67.11
    20          32     1717.53      547.84     1093.00        0.00       31.11       84.53
    30          32     1538.67      608.31     1231.00        0.00       41.02       91.04
    40          32     1420.46      655.24     1270.00        0.00       52.50       94.23
Finished, you can check the latency and recall above.
```
If you can see this output, then everything is OK, and you can start running the artifact.

### Run All Experiments (Result Reproduced, eta: 5 hours)

There is an all-in-one AE script for your convenience:

```bash
bash scripts/run_all.sh
```

This script will run for 5 hours and store all results in the `data` directory.
You could use `tmux` to run it on the backend.
Running it overnight is recommended.

> We have prepared 8 scripts that run different experiments to reproduce all figures in our paper, which are the scripts/tests/figX.sh files (X = 11, 12, ..., 18). The all-in-one script simply invocates them one by one. If you want to run individual experiments, please refer to these script files and the comments in them.


### Plot the Figures

#### (Recommended) For Visual Studio Code users

Please install the Jupyter extension in VSCode. Then, please open `scripts/plotting.ipynb`.

Please use the Python 3.10.12 (`/usr/bin/python`) kernel.

Then, you can run each cell from top to bottom. The first cell contains prelude functions and definitions, so please run it first. Each other cell plots a figure.

#### For others

Please run the plotter script in the `scripts` directory:
```
cd scripts
python3 plotting.py
```
The command above will plot all figures and tables by default, and the results will be stored in the `figures` directory. So, please ensure that you have finished running the all-in-one AE script before running the plotter.

The plotter supports specifying certain figures or tables to plot by command-line arguments. For example:
```
python3 plotting.py figure11 figure13 figure15
```
Please refer to `plotting.py` for accepted arguments.

### Appendix: File Structure

We pre-built the indexes in our evaluation.
Here are the related files:

```bash
/mnt/nvme/data/
├── bigann
│   ├── 100M.bbin # SIFT100M dataset
│   ├── 100M_gt.bin # SIFT100M ground truth
│   ├── truth.bin # SIFT1B ground truth
│   ├── bigann.bbin # SIFT1B dataset
│   └── bigann_query.bbin # SIFT query
├── deep
│   ├── 100M.fbin # DEEP100M dataset
│   ├── 100M_gt.bin # DEEP100M ground truth
│   └── queries.fbin # DEEP query
└── SPACEV1B
    ├── 100M.bin # SPACEV100M dataset
    ├── 100M_gt.bin # SPACEV100M ground truth
    ├── query.bin # SPACEV query
    ├── truth.bin # SPACEV1B ground truth
    └── vectors.bin # SPACEV1B dataset

/mnt/nvme2
├── indices # Indexes for DiskANN and PipeANN
├── indices_spann # Indexes for SPANN
├── indices_starling # Indexes for Starling
├── SPTAG # SPANN code
└── PipeANN # source code of PipeANN

# For each index of PipeANN, important files:
/mnt/nvme2/indices/SIFT1B/
├── 1B_disk.index # full graph index
├── 1B_mem.index # in-memory graph
├── 1B_mem.index.tags # in-memory vector ID -> on-disk vector ID
├── 1B_pq_compressed.bin # PQ-compressed vectors
└── 1B_pq_pivots.bin # PQ pivots
```

## Quick Start (with a disk index of DiskANN)

For those who have built on-disk indexes of DiskANN, enabling PipeANN only requires two quick steps (<15min).

* First, install the dependencies (as in [Software Configuration](#software-configuration)) and [Build the Repository](#build-the-repository).

* Second, you only need to [build an in-memory index](#build-in-memory-index-for-pipeann) (<10min for billion-scale datasets).

Then, PipeANN is ready! An example for SIFT100M on Ubuntu 22.04:

```bash
# For Ubuntu 22.04, first install the dependencies:
sudo apt install make cmake g++ libaio-dev libgoogle-perftools-dev clang-format libboost-all-dev libmkl-full-dev

# Build io_uring.
cd third_party/liburing
./configure
make -j
cd ../..

# Build PipeANN.
bash ./build.sh

# Build in-memory index. Modify the INDEX_PREFIX and DATA_PATH beforehand.
export INDEX_PREFIX=/mnt/nvme2/indices/bigann/100m # on-disk index file name prefix.
export DATA_PATH=/mnt/nvme/data/bigann/100M.bbin
build/tests/utils/gen_random_slice uint8 ${DATA_PATH} ${INDEX_PREFIX}_SAMPLE_RATE_0.01 0.01
build/tests/build_memory_index uint8 ${INDEX_PREFIX}_SAMPLE_RATE_0.01_data.bin ${INDEX_PREFIX}_SAMPLE_RATE_0.01_ids.bin ${INDEX_PREFIX}_mem.index 0 0 32 64 1.2 24 l2

# Search the on-disk index. Modify the query and ground_truth path beforehand.
# build/tests/search_disk_index <data_type> <index_prefix> <nthreads> <I/O pipeline width (max for PipeANN)> <query file> <truth file> <top-K> <similarity> <search_mode (2 for PipeANN)> <L of in-memory index> <Ls for on-disk index> 
build/tests/search_disk_index uint8 ${INDEX_PREFIX} 1 32 /mnt/nvme/data/bigann/bigann_query.bbin /mnt/nvme/data/bigann/100M_gt.bin 10 l2 2 10 10 10 10 15 20 25 30 35 40 45 50 55 60 65
```

Besides [PipeANN](#quick-start-with-a-disk-index-of-diskann) (`src/v2/pipe_search.cpp`), this repository also supports:

* [DiskANN](https://github.com/microsoft/DiskANN/tree/main/src), an on-disk graph-based index using best-first search (`src/v2/beam_search.cpp`).
* [Starling](https://github.com/zilliztech/starling), which reorders the on-disk index for reduced I/O amplification (`src/v2/page_search.cpp`).
* **CoroSearch**, which enhances CPU utilization and throughput by **inter-query scheduling** when processing multiple requests simultaneously (`src/v2/coro_search.cpp`).

To use these baselines, you should first build the corresponding index as below, and then change the `search_mode` parameter from `2` (for PipeANN) to `1` (for Starling), `0` (for DiskANN), or `3` (for CoroSearch).

## Start from Scratch (Environment Setup)

This section introduces how to install the dependencies, build PipeANN, and prepare the indexes for PipeANN and its baselines.
After these steps, you could run the benchmark scripts above by modifying hard-coded index and dataset paths (similar to [Quick Start](#quick-start-with-a-disk-index-of-diskann)).

### Hardware Configuration

* SSD (NVMe SSD is better), ~700GB for SIFT with 1B vectors, ~900GB for SPACEV with 1.4B vectors.

* DRAM, ~40GB per billion vectors, depending on the product quantization (PQ) table size.

### Software Configuration

Install the dependencies in Ubuntu:

```bash
sudo apt install make cmake g++ libaio-dev libgoogle-perftools-dev clang-format libboost-all-dev libmkl-full-dev
```

### Build the Repository


First, build `liburing`. The compiled `liburing` library is in its `src` folder.

```bash
cd third_party/liburing
./configure
make -j
```


Then, build PipeANN.

```bash
bash ./build.sh
```

### Prepare the Datasets

#### Download the Datasets

* SIFT100M and SIFT1B from [BIGANN](http://corpus-texmex.irisa.fr/);
* DEEP1B using [deep1b_gt repository](https://github.com/matsui528/deep1b_gt) (Thanks, matsui528!);
* SPACEV100M and SPACEV1B from [SPTAG](https://github.com/microsoft/SPTAG).

If the datasets follow `ivecs` or `fvecs` format, you could transfer them into `bin` format using:
```bash
build/tests/utils/bvecs_to_bin bigann_base.bvecs bigann.bin # for byte vecs (SIFT), bigann_base.bvecs -> bigann.bin
build/tests/utils/fvecs_to_bin base.fvecs deep.bin # for float vecs (DEEP) base.fvecs -> deep.bin
build/tests/utils/ivecs_to_bin idx_1000M.ivecs idx_1000M.ibin # for int vecs (SIFT groundtruth) idx_1000M.ivecs -> idx_1000M.ibin
```

We need `bin` for `base`, `query`, and `groundtruth`.

The SPACEV1B dataset is divided into several sub-files in SPTAG. You could follow [SPTAG SPACEV1B](https://github.com/microsoft/SPTAG/tree/main/datasets/SPACEV1B) for how to read the dataset.
To concatenate them, just save the dataset's numpy `array` to `bin` format (the following Python code might be used).

```py
# bin format:
# | 4 bytes for num_vecs | 4 bytes for vector dimension (e.g., 100 for SPACEV) | flattened vectors |
def bin_write(vectors, filename):
    with open(filename, 'wb') as f:
        num_vecs, vector_dim = vectors.shape
        f.write(struct.pack('<i', num_vecs))
        f.write(struct.pack('<i', vector_dim))
        f.write(vectors.tobytes())
```

The dataset should contain a ground truth file for its full set.
Some datasets also contain the ground truth of subsets (first $k$ vectors). For example, SIFT100M's (the first 100M vectors of SIFT1B) ground truth could be found in `idx_100M.ivecs` of SIFT1B dataset.

#### Prepare the 100M Subsets

To generate the 100M subsets (SIFT100M, SPACEV100M, and DEEP100M) using the 1B datasets, use `change_pts` (for `bin`) and `pickup_vecs.py` (in the `deep1b_gt` repository, for `fvecs`).
```bash
# for SIFT, assume that the dataset is converted into bigann.bin
build/tests/change_pts uint8 /mnt/nvme/data/bigann/bigann.bin 100000000
mv /mnt/nvme/data/bigann/bigann.bin100000000 /mnt/nvme/data/bigann/100M.bbin

# for DEEP, assume that the dataset is in fvecs format.
python pickup_vecs.py --src ../base.fvecs --dst ../100M.fvecs --topk 100000000
# If DEEP is already in fbin format, change_pts also works.

# for SPACEV, assume that the dataset is concatenated into a huge file vectors.bin
build/tests/change_pts int8 /mnt/nvme/data/SPACEV1B/vectors.bin 100000000
mv /mnt/nvme/data/SPACEV1B/vectors.bin100000000 /mnt/nvme/data/SPACEV1B/100M.bin
```

Then, use `compute_groundtruth` to calculate the ground truths of 100M subsets. (DEEP100M's ground truth could be calculated using deep1b_gt).

Take SIFT100M as an example (in fact, its ground truth could also be found in SIFT1B):
```bash
# for SIFT100M, assume that the dataset is at /mnt/nvme/data/bigann/100M.bbin, the query is at /mnt/nvme/data/bigann/bigann_query.bbin 
# output: /mnt/nvme/data/bigann/100M_gt.bin
build/tests/utils/compute_groundtruth uint8 /mnt/nvme/data/bigann/100M.bbin /mnt/nvme/data/bigann/bigann_query.bbin 1000 /mnt/nvme/data/bigann/100M_gt.bin
```

### Build On-Disk Index for PipeANN and DiskANN

PipeANN uses the same on-disk index as DiskANN.

```bash
# Usage:
# build/tests/build_disk_index <data_type (float/int8/uint8)> <data_file.bin> <index_prefix_path> <R>  <L>  <B>  <M>  <T> <similarity metric (cosine/l2) case sensitive>. <single_file_index (0/1)>
build/tests/build_disk_index uint8 /mnt/nvme/data/bigann/100M.bbin /mnt/nvme2/indices/bigann/100m 96 128 3.3 256 112 l2 0
```

The final index files will share a prefix of `/mnt/nvme2/indices/bigann/100m`.

Parameter explanation:

* R: maximum out-neighbors
* L: candidate pool size during build (build in fact conducts vector searches to optimize graph edges)
* B: in-memory PQ-compressed vector size. Our goal is to use 32 bytes per vector; higher-dimensional vectors might require more bytes.
* M: maximum memory used during build, 256GB is sufficient for the 100M index to be built totally in memory.
* T: number of threads used during build. Our machine has 112 threads.

We use the following parameters when building indexes:

| Dataset       | type | R  | L | B | M | T | similarity
|---------------|---|---|---|---|---| --- | --- 
| SIFT/DEEP/SPACEV100M | uint8/float/int8 | 96 | 128 | 3.3 | 256 | 112 | L2
| SIFT1B   | uint8 | 128 |  200 | 33 | 500 | 112 | L2
| SPACEV1B | int8 | 128 | 200  | 43 | 500 | 112 | L2

### Build In-Memory Index for PipeANN

PipeANN requires an in-memory index for entry point optimization.
You could build it in the following way (take SIFT100M as an example):

```bash
# dataset: SIFT100M, {output prefix}: {INDEX_PREFIX}_SAMPLE_RATE_0.01
export INDEX_PREFIX=/mnt/nvme2/indices/bigann/100m # on-disk index file name prefix.
# first, generate random slice, sample rate is 0.01.
build/tests/utils/gen_random_slice uint8 /mnt/nvme/data/bigann/100M.bbin ${INDEX_PREFIX}_SAMPLE_RATE_0.01 0.01
# output files: {output prefix}_data.bin and {outputprefix}_ids.bin
# mem index: {INDEX_PREFIX}_mem.index
# All the in-memory indexes are built using R = 32, L = 64.
build/tests/build_memory_index uint8 ${INDEX_PREFIX}_SAMPLE_RATE_0.01_data.bin ${INDEX_PREFIX}_SAMPLE_RATE_0.01_ids.bin ${INDEX_PREFIX}_mem.index 0 0 32 64 1.2 24 l2
```

The output in-memory index should reside in three files: `100m_mem.index`, `100m_mem.index.data`, and `100m_mem.index.tags`.

### Build Indexes for Starling

#### Step 1: Reorder the Index

*Method 1 (build an index from scratch):* Directly use `run_benchmark.sh` in [Starling](https://github.com/zilliztech/starling). It generates:

* The reordered on-disk index (`*_disk.index`, for Starling)
* The original on-disk index (`*_disk_beam_search.index`, for DiskANN and PipeANN).
* The in-memory index (`*_mem.index`, for PipeANN and Starling).

*Method 2 (reuse previously-built indexes):*
To re-use the [on-disk index](#build-on-disk-index-for-pipeann-and-diskann) and [in-memory index](#build-in-memory-index-for-pipeann) built for DiskANN and PipeANN, refer to `run_benchmark.sh` in Starling for how to reorder the index:

* Specifically, you should run `frequency file`, `graph partition`, and `relayout` manually. 

#### Step 2: Pad the Reorder Info (partition.bin) for Fast Loading 

After generating the `partition.bin` file, use `build/tests/pad_partition <partition.bin filename>` to generate `partition.bin.aligned`, which pads the partition file for concurrent loading.

### Build Indexes for SPANN (cluster-based)

We use the parameters in [SPFresh ini file](https://github.com/SPFresh/SPFresh/blob/main/Script_AE/iniFile/build_SPANN_spacev100m.ini) to build SPANN SPACEV100M index.

* SIFT100M shares the same configuration as SPACEV100M.
* DEEP100M uses `PostingPageLimit=12`, while other configurations remain the same.



## Paper

If you use PipeANN in your research, please cite our paper:

Hao Guo and Youyou Lu. Achieving Low-Latency Graph-Based Vector Search via Aligning Best-First Search Algorithm with SSD. To appear in the 19th USENIX Symposium on Operating Systems Design and Implementation (OSDI '25), Boston MA USA, July 2025.

## Acknowledgments
This repository is based on [DiskANN and FreshDiskANN](https://github.com/microsoft/DiskANN/tree/diskv2), we really appreciate it.
