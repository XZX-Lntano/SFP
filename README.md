# ZCU102 四路 MPI–FPGA 聚合（v4）

基于 Corundum/mqnic 的 ZCU102 四口 SFP 聚合工程。应用通过本机 UDP 提交
2/3/4 路 worker 数据，四个 MPI rank 经 DPDK/X710 将数据送入 FPGA；FPGA
逐项执行 uint64 求和，并把相同结果广播到四个端口。rank0 校验四份结果后返回应用。

```text
应用 → localhost UDP → rank0 → 非阻塞 MPI 分发 → 四个 rank / DPDK
     → 四口 FPGA BRAM 聚合 → 四口广播 → 四个 rank → rank0 → 应用
```

MPI 热路径使用非阻塞点对点通信；实际数值求和由 FPGA 完成，不使用 CPU
MPI_Allreduce 重复求和。`--simulate` 是单独的 CPU 功能测试模式。

## 协议和硬件

- 每个 round 固定 64 个 uint64，无逐项 index；溢出按模 2^64。
- 每帧包含 1–16 个 round，每条 record 为 round_id:u32、reserved:u32、64×u64，合计 520 字节。
- FPGA 有 256 个 round 槽，组织为 16 个 batch 槽，每个 batch 预留 16 round。
- base round ID 必须是 16 的倍数；帧内 ID 顺序递增。槽号为 `(base >> 4) & 15`。
- 最大 Ethernet 帧长 8368 字节（不含 FCS），使用 MTU 9000。
- 四路独立 64 位解析器和 BRAM bank；同步 BRAM 读取、两级求和、输出寄存器流水线。
- 2/3 路输入时，也始终返回四份结果。未完成 batch 约 100 ms 超时释放。
- 应用请求本身携带多个 round，bridge 不把多个单 round 请求自动合并。

应用、bridge 与 bitstream 必须配套使用 v4。应用接口默认绑定 127.0.0.1；最大
四路请求为 32792 字节，适合本机 UDP。外部应用网络传输需另行处理分片/接口设计。
完整字段定义和测量说明见 [THROUGHPUT.md](进程间计算/THROUGHPUT.md)。

## 仓库结构

| 路径 | 用途 |
|---|---|
| `corundum/` | Corundum 完整源码、驱动、工具及板卡工程 |
| `corundum/fpga/mqnic/ZCU102/fpga/rtl/axis_udp_batch_aggregator.v` | v4 流水线聚合器 |
| `corundum/fpga/mqnic/ZCU102/fpga/rtl/fpga_core.v` | 四路 RX、聚合与广播集成 |
| `ethernet-switch/` | 交换机参考工程及随仓库收录的依赖源码 |
| `进程间计算/mpi_fpga_bridge.c` | 四 rank 非阻塞 MPI 桥接 |
| `进程间计算/dpdk_port.c` | DPDK primary/secondary 初始化、握手和收发资源 |
| `进程间计算/bridge_protocol.h` | 应用/FPGA 协议编解码 |
| `进程间计算/benchmark_bridge_native.c` | 原生 C 应用滑动窗口发包器 |
| `进程间计算/benchmark_dpdk.c` | 直接聚合和纯发包测试 |
| `进程间计算/reports/` | 已记录的资源、时序和带宽报告 |

Vivado 生成目录、二进制文件和本机配置恢复状态不提交。上游项目的许可声明保留在各目录中。

## 主机准备

已验证环境：Ubuntu、Open MPI 5.0.10、DPDK 21.11.9、Vivado 2025.2；需要 C
编译器、Python 3、pkg-config、libdpdk 开发包和启用 IOMMU 的 X710。

```bash
cd 进程间计算
export PATH=/usr/local/openmpi/bin:$PATH
make
python3 pre_synth_equivalence_check.py
```

物理端口顺序：

| Rank | 主机接口（绑定 VFIO 前） | PCI 地址 | ZCU102 |
|---|---|---|---|
| 0 | enp1s0f3np3 | 0000:01:00.3 | eth1 |
| 1 | enp1s0f2np2 | 0000:01:00.2 | eth2 |
| 2 | enp1s0f1np1 | 0000:01:00.1 | eth3 |
| 3 | enp1s0f0np0 | 0000:01:00.0 | eth4 |

以下 setup 脚本针对这台主机的四个 PCI 地址，其他机器先调整配置。它保存原状态、
准备 512 MiB hugepages、提高 socket 缓冲区上限并绑定 vfio-pci，不修改管理口 eno1。
四个 PF 共用 IOMMU group，由 rank0 primary 初始化，其他 rank 使用 secondary。

```bash
sudo python3 setup_host.py dpdk
```

## 构建与加载 FPGA

主机设置好 Vivado PATH 后：

```bash
cd corundum/fpga/mqnic/ZCU102/fpga/fpga
make app
```

检查最终时序后，将生成的 `app/fpga.bin`、`app/overlay.dtbo`、`app/shell.json`
部署到开发板 `/lib/firmware/xilinx/mqnic/`。源码放在开发板 `/home/ubuntu/corundum`；
不要用主机 x86 编译产物覆盖板上的 ARM 驱动和工具。

在开发板终端执行：

```bash
cd /home/ubuntu/corundum/modules/mqnic
make -j4
cd /home/ubuntu/corundum/utils
make -j4

# 若模块/应用已经加载，先执行以下两条；没有加载时可跳过
sudo rmmod mqnic
sudo xmutil unloadapp

sudo xmutil loadapp mqnic
cd /home/ubuntu/corundum/modules/mqnic
sudo insmod mqnic.ko
sudo ip link set eth1 mtu 9000 up
sudo ip link set eth2 mtu 9000 up
sudo ip link set eth3 mtu 9000 up
sudo ip link set eth4 mtu 9000 up
sudo dmesg | tail -80
```

## 应用功能与端到端带宽测试

主机终端一，在 `进程间计算` 中：

```bash
sudo /usr/local/openmpi/bin/mpirun --allow-run-as-root -np 4 \
  --bind-to core --map-by core ./mpi_fpga_bridge --port 10000 --window 16
```

等待四路链路及 FPGA 启动握手通过。主机终端二：

```bash
python3 bridge_app_client.py --port 10000 --request-id 4096 \
  --worker0-values 10 20 30 40 50 --worker1-values 1 2 3 4 6 \
  --worker2-values 1 2 3 4 6 --worker3-values 1 2 3 4 6
# 预期：[13, 26, 39, 52, 68]；其余项补零

python3 test_bridge_64_entries.py --host 127.0.0.1 --port 10000
python3 benchmark_bridge_bandwidth.py --port 10000 --workers 4 \
  --rounds 16 --window 16 --duration 30 --cpu 4
python3 bridge_app_client.py --port 10000 --stop
```

Python benchmark 入口启动的是原生 C 程序，不在 Python 中逐包循环。
`useful aggregate result` 只计一份有效结果，不把四个广播副本相加。

## 直接 DPDK 和纯发包

先停止桥接，不能让两个程序同时使用同一组端口：

```bash
sudo /usr/local/openmpi/bin/mpirun --allow-run-as-root -np 4 \
  --bind-to core --map-by core ./benchmark_dpdk \
  --workers 4 --rounds 16 --window 16 --batches 1000000

sudo /usr/local/openmpi/bin/mpirun --allow-run-as-root -np 4 \
  --bind-to core --map-by core ./benchmark_dpdk --tx-only \
  --workers 4 --rounds 16 --batches 1000000
```

`--tx-only` 使用 FPGA 丢弃的实验 EtherType，仅测实际 NIC 发送能力，不测聚合或接收。
直接聚合测试会验证返回值，遇到超时主动 MPI_Abort；应查看 abort 之前的端口计数和 batch ID。

## 验证与已有测量

```bash
# Vivado 工具在 PATH 中
python3 test_batch_rtl.py
# 软件求和模式，仅用于软件回归，不代表 FPGA 带宽
python3 test_software_pipeline.py --window 16
# 板上已加载匹配固件，主机已配置 DPDK，无其他测试进程
sudo python3 test_hardware_pipeline.py
```

2026-09-07 的完整实现使用 66319 LUT、68492 寄存器、457.5/912 BRAM tile（50.16%）。
最终 setup WNS +0.025 ns、hold WHS +0.010 ns，时序通过。

| 真实硬件测试 | 实测速率 |
|---|---:|
| 四路完整应用链路，16 round、window16 | 有效结果约 3.59 Gbit/s |
| 四路直接 FPGA 聚合，百万帧复验 | 有效结果约 6.70 Gbit/s |
| 四口同时纯发包，每口百万帧 | 每口线速估算约 9.80 Gbit/s |

这些是不同路径的测量，纯发包结果不能证明完整应用发包器支持 10G 有效结果吞吐。
当前报文布局的 10G 有效结果理论上限约 9.762 Gbit/s。测试存在主机负载波动，
并曾出现单包 NIC 错误/超时；当前协议没有通用重传机制，不能据短测宣称长期无丢包。

结束 DPDK 使用后，可在主机恢复原始配置：

```bash
sudo python3 setup_host.py restore
```

恢复依赖本机生成的 `reports/host_before_dpdk.json`，该文件不能从别的主机复制使用。
