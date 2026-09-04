# ZCU102 Corundum 16-Round MPI-FPGA Aggregation

本项目在 ZCU102 Corundum/mqnic 四端口数据通路中实现无 index 的 2/3/4
worker `uint64` 求和聚合。主机应用仍使用单 round 请求；MPI bridge 将最多 16
个待处理请求合并为每个 worker 一帧 Jumbo Frame，FPGA 使用 16 个 round slot
进行 64 项聚合，并把结果帧广播到四个 SFP 端口。

## 主要变化

- 物理协议 v4：每帧包含 1–16 个 `round_id + 64×uint64` record，不携带 index。
- 最大 UDP payload 8326 B，最大 Ethernet frame 8368 B（不含 FCS），使用 MTU 9000。
- FPGA 有四个独立的一拍一值解析器、四个 worker BRAM、三级加法流水线和 result BRAM。
- 支持 2/3/4 worker 输入，结果始终广播到四个端口。
- MPI 使用非阻塞 collective，并支持 `MPI_FPGA_CPU_BASE` 进行 rank 绑核。
- 同时提供 raw socket 和 DPDK 后端；X710 四功能同属 IOMMU group 时，由 rank0
  作为唯一 EAL owner 管理四端口，四个 rank 仍通过 MPI 分别参与发送/接收处理。
- benchmark 使用滑动窗口，并根据 bridge 返回的实际 batch size 统计物理流量。
- ILA 可读取输入/输出包字节数、invalid、slot collision、FIFO overflow 和
  output backpressure 计数器。

v4 bridge 与 v4 bitstream 必须配套使用，不能与旧的一帧一 round v3 bitstream 混用。
完整线协议见 [进程间计算/BATCH_PROTOCOL.md](进程间计算/BATCH_PROTOCOL.md)。

## 文件布局

- `rtl/axis_udp_batch_aggregator.v`：16 round × 64 value BRAM 聚合器
- `rtl/fpga_core.v`：ZCU102 四端口数据通路集成和物理端口计数器
- `进程间计算/mpi_fpga_bridge.c`：raw socket/DPDK MPI bridge
- `进程间计算/bridge_app_client.py`：功能客户端
- `进程间计算/benchmark_bridge_bandwidth.py`：滑动窗口 benchmark
- `进程间计算/test_batch_protocol.c`：1/16 round 物理协议回归测试
- `vivado/`：综合、实现、bitstream、ILA 和仿真 Tcl 脚本
- `tb/test_axis_udp_batch_aggregator.sv`：四 worker、两 round RTL testbench

## 主机软件构建

```bash
cd 进程间计算
make clean
make test
```

`make test` 使用 `/usr/local/openmpi/bin/mpicc` 编译 raw socket 与 DPDK 两个目标，
运行 v4 C serializer/parser 测试，并检查两个 Python 程序的语法。

### raw socket

先把四个 X710 端口绑定到 `i40e`，再执行：

```bash
cd 进程间计算
sudo ./setup_raw_jumbo.sh
sudo env MPI_FPGA_CPU_BASE=4 /usr/local/openmpi/bin/mpirun \
  --allow-run-as-root --bind-to none --tag-output -np 4 \
  ./mpi_fpga_bridge \
  10000 ff:ff:ff:ff:ff:ff \
  enp1s0f3np3 192.168.10.1 192.168.20.1 \
  enp1s0f2np2 192.168.10.2 192.168.20.2 \
  enp1s0f1np1 192.168.10.3 192.168.20.3 \
  enp1s0f0np0 192.168.10.4 192.168.20.4 \
  0x2345 4000 4001 4002 4003 5000
```

### DPDK

将 `0000:01:00.[0-3]` 全部绑定到 `vfio-pci` 并准备 2 MiB hugepages，然后：

```bash
cd 进程间计算
sudo env MPI_FPGA_DPDK=1 MPI_FPGA_CPU_BASE=4 \
  /usr/local/openmpi/bin/mpirun --allow-run-as-root --bind-to none \
  --tag-output -np 4 ./mpi_fpga_bridge_dpdk \
  10000 ff:ff:ff:ff:ff:ff \
  enp1s0f3np3 192.168.10.1 192.168.20.1 \
  enp1s0f2np2 192.168.10.2 192.168.20.2 \
  enp1s0f1np1 192.168.10.3 192.168.20.3 \
  enp1s0f0np0 192.168.10.4 192.168.20.4 \
  0x2345 4000 4001 4002 4003 5000
```

DPDK 后端会自行把各硬件端口 MTU 配置为 9000。

## 四 worker 功能验证

```bash
cd 进程间计算
python3 bridge_app_client.py \
  --host 127.0.0.1 --port 10000 --request-id 4660 \
  --worker0-values 10 20 30 40 50 \
  --worker1-values 1 2 3 4 5 \
  --worker2-values 1 1 1 1 1 \
  --worker3-values 2 2 2 2 2
```

预期结果为 `[14, 25, 36, 47, 58]`。

## FPGA 工程集成与构建

将 `rtl/axis_udp_batch_aggregator.v`、`rtl/fpga_core.v` 和本仓库根目录的
`Makefile` 同步到 Corundum 的 `fpga/mqnic/ZCU102/fpga` 对应位置。将 `vivado/`
中的 Tcl 文件放入该工程的 `fpga` 目录，然后依次运行：

```bash
vivado -mode batch -source run_synth.tcl
vivado -mode batch -source report_batch_post_synth.tcl
vivado -mode batch -source insert_batch_ila.tcl
vivado -mode batch -source run_impl.tcl
vivado -mode batch -source generate_bit.tcl
```

本机 Vivado 2025.2 验证结果：

- 完整 ZCU102 RTL elaboration：通过
- 聚合存储：10 个 `RAMB36E2`（4 个 worker bank + 1 个 result bank）
- ILA：22×64-bit probe、1024 深度，额外使用 39 个 `RAMB36E2` 和 2 个 `RAMB18E2`
- 全设计 BRAM：379.5/912 tile（41.61%）
- route：0 failed/unrouted net
- 300 MHz post-route setup WNS：`+0.042 ns`
- post-route hold slack：`+0.010 ns`

生成文件位于 `fpga.runs/impl_1/fpga.bit`、`fpga.bin`、`fpga.ltx`，并导出
`fpga.xsa`。RTL testbench 已通过编译和 elaboration；本机 XSim runtime 对简单
smoke test 也会触发内部 Tcl exception，因此未把动态 XSim 运行标记为通过。
