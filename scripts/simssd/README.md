# SSD 模拟器（SimpleSSD）使用说明

PipeANN 支持两种读取后端，通过统一的 `AlignedFileReader` 接口切换：

| 后端 | 说明 | 选择方式 |
| --- | --- | --- |
| `uring`（默认） | 真实 SSD，io_uring + O_DIRECT | `PIPEANN_READER_TYPE=uring` 或不设置 |
| `simssd` | 开源 SSD 模拟器 [SimpleSSD](https://github.com/SimpleSSD/SimpleSSD-Standalone) | `PIPEANN_READER_TYPE=simssd` |

模拟器后端（`SimSSDFileReader`）仍然从真实索引文件读出**正确的数据**
（buffered pread，页缓存加速），但每个请求的**完成时间**由 SimpleSSD 的
事件驱动时序模型决定（NVMe 协议 + PCIe + FTL + NAND flash 时序），
因此测得的延迟反映的是模拟 SSD 的行为，不依赖本机是否有真实 SSD。
只读挂载（如 `/mnt/ssd1` 以 `ro` 挂载）也能正常跑。

## 使用方法

```bash
# 真实 SSD（默认）
./build/tests/search_disk_index float /mnt/ssd1/pipeann-index/sift100m/100m \
    16 2 query.bin truth.bin 10 l2 1 0 10 20

# 模拟 SSD
PIPEANN_READER_TYPE=simssd ./build/tests/search_disk_index float \
    /mnt/ssd1/pipeann-index/sift100m/100m 16 2 query.bin truth.bin 10 l2 1 0 10 20
```

代码中也可以用工厂显式指定（`include/reader_factory.h`）：

```cpp
auto reader = create_aligned_file_reader(ReaderType::SIM_SSD);  // 模拟 SSD
auto reader = create_aligned_file_reader(ReaderType::URING);    // 真实 SSD
auto reader = create_aligned_file_reader();                     // AUTO, 读环境变量
```

## 环境变量

| 变量 | 默认值 | 说明 |
| --- | --- | --- |
| `PIPEANN_READER_TYPE` | `uring` | `uring`（真实 SSD）/ `simssd`（模拟器） |
| `PIPEANN_SIMSSD_DEVICE_CONFIG` | 内嵌配置 | SimpleSSD 设备配置文件路径，可参考 `scripts/simssd/simplessd_device.cfg` |
| `PIPEANN_SIMSSD_LATENCY_SCALE` | `1.0` | 延迟缩放：`<1` 模拟更快的 SSD，`>1` 更慢 |
| `PIPEANN_SIMSSD_INTERFACE` | `nvme` | `nvme`（完整 NVMe/PCIe 建模）或 `none`（直连 HIL，更快但粗略） |
| `PIPEANN_SIMSSD_VERBOSE` | 关 | 设为 `1` 时打印模拟 SSD 容量与平均模拟延迟统计 |

## 实现说明

- 新增文件：`include/sim_ssd_reader.h`、`src/sim_ssd_reader.cpp`
  （模拟器 reader），`include/reader_factory.h`、`src/reader_factory.cpp`
  （后端选择工厂）。
- SimpleSSD 以源码形式 vendored 在 `third_party/SimpleSSD-Standalone`
  （GPL-3.0 license），由顶层 CMake 编译为静态库 `simplessd_standalone`，
  可用 `-DPIPEANN_WITH_SIMSSD=OFF` 关闭。
- 异步接口（`send_io`/`poll`/`poll_wait`，PipeANN 流水线搜索使用）下，
  请求的真实数据在提交时即刻读出，完成时间 = 提交时刻 + 模拟延迟 × scale，
  多个并发请求在模拟器中共享 NAND 通道与 NVMe 队列，排队/竞争行为与真实
  SSD 一致。
- 模拟器 reader 打开索引文件时会读取文件大小，并在 SimpleSSD 的 FTL 初始化
  配置中追加 `MinFillBytes=<文件大小>`。FTL 顺序预热至少覆盖 `[0, 文件大小)`
  这段 LBA 范围，因此索引随机读会命中映射并真正计入 NAND 页读延迟；不需要
  为整块模拟 SSD 设置很大的 `FillRatio`。如果索引文件超过模拟 SSD 的逻辑容量，
  初始化会明确报错，而不是静默回退到未映射读。
