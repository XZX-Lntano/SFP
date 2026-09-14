# ZCU102 四端口 FPGA 数据聚合

本项目将 Corundum/mqnic、ethernet-switch 相关 RTL 与 ZCU102 PL 聚合逻辑结合，使用主机上的四个独立 C 客户端、四个 MPI bridge rank 和 DPDK 网卡端口完成多 worker 数字聚合。

## 数据路径

```text
独立客户端 CPU4～7
  → localhost UDP 10000～10003
  → MPI rank0～3（CPU0～3）
  → DPDK / 主机 PCIe 网卡四口
  → ZCU102 PL 接收 FIFO / 四个 worker BRAM bank
  → 按 request ID 匹配并执行64位流水加法
  → 四路广播 / 发送 FIFO
  → DPDK RX / MPI bridge
  → 各客户端自身 pending 表
```

业务聚合在 PL 中执行，不经过 PS Ubuntu 的应用层计算。当前聚合路径直接广播结果；普通交换示例的端口配对转发与聚合路径应区别看待。

| rank | 客户端 CPU | MPI CPU | 主机物理口 | 开发板端口 |
| --- | --- | --- | --- | --- |
| 0 | 4 | 0 | enp1s0f3np3 | eth1 |
| 1 | 5 | 1 | enp1s0f2np2 | eth2 |
| 2 | 6 | 2 | enp1s0f1np1 | eth3 |
| 3 | 7 | 3 | enp1s0f0np0 | eth4 |

## 目录

- `corundum/`：Corundum 工程及 ZCU102 聚合 RTL。
- `ethernet-switch/`：交换机参考实现。
- `进程间计算/`：客户端、MPI bridge、DPDK 支持代码和测试资料。

核心文件为 `benchmark_four_clients.c`、`mpi_fpga_bridge.c`、`dpdk_port.c`、`bridge_protocol.h`，以及 `corundum/fpga/mqnic/ZCU102/fpga/rtl/axis_udp_batch_aggregator.v`。

## 协议与缓存

客户端使用 v5 应用协议：24字节头部后跟当前 worker 的数字数据。头部包含 magic、version、type、base/request_id、rounds、workers、entries、status、rank。每轮包含64个大端 uint64，最多16轮；参与聚合的 worker 数为2～4。

MPI 将应用包转换为原始以太网帧：

```text
Ethernet 14B | IPv4 20B | UDP 8B | FPGA头 6B
每轮：round_id 4B | reserved=0 4B | 64 × uint64 512B
```

16轮时帧长8368字节（不含FCS），数字数据8192字节。FPGA协议版本为4，UDP目的端口为0x2345。第r轮的ID为base+r；返回第一轮ID即可恢复request ID。

FPGA有16个批次槽，每槽最多16轮，四个worker bank分别保存输入。槽号为`(base >> 4) & 15`，同时检查完整ID及轮数、worker数。客户端最多16个pending项；收到匹配ID且数值正确的结果才计成功。非零状态响应释放对应记录并报错。

## 编译

主机需要 C 编译器、Open MPI、DPDK 开发库及 pkg-config。

```bash
cd 进程间计算
make mpi_fpga_bridge benchmark_four_clients_native
```

## 运行

先配置主机 DPDK 大页、VFIO及网卡绑定。预留512 MiB大页的示例：

```bash
sudo sysctl -w vm.nr_hugepages=256
sudo mkdir -p /dev/hugepages
mountpoint -q /dev/hugepages || sudo mount -t hugetlbfs -o pagesize=2M none /dev/hugepages
```

启动 bridge，确认实际 CPU 绑定和四端口启动握手成功：

```bash
sudo /usr/local/openmpi/bin/mpirun --allow-run-as-root -np 4 \
  --bind-to core --map-by core \
  ./mpi_fpga_bridge --port 10000 --window 16
```

四条命令分别启动独立客户端。客户端内部没有fork、Barrier或客户端间通信：

```bash
./benchmark_four_clients_native --rank 0 --cpu 4 --requests 3000000 > client0.log 2>&1 &
./benchmark_four_clients_native --rank 1 --cpu 5 --requests 3000000 > client1.log 2>&1 &
./benchmark_four_clients_native --rank 2 --cpu 6 --requests 3000000 > client2.log 2>&1 &
./benchmark_four_clients_native --rank 3 --cpu 7 --requests 3000000 > client3.log 2>&1 &
wait
cat client{0,1,2,3}.log
```

固定请求模式要求四路使用相同requests、window、rounds、workers和request-id-start；默认窗口16、轮数16、worker数4。启动间隔应小于链路的请求超时。不同运行应避免与尚未清理的旧批次ID重叠。

`--requests N`优先于`--duration`。发送完成后只等待自身pending；默认drain超时2秒，可用`--drain-timeout`调整。固定请求模式连续该时长无收发进展也会停止发送并收尾。错误或丢包不会自动重传。

## 带宽解释

每个客户端输出有效结果带宽、实际应用UDP发送/接收带宽及completed、unresolved、invalid、late、status_errors。

```text
有效结果带宽 = completed × rounds × 64 × 8 × 8 / 实际耗时
```

实际耗时包含drain。四路收到相同聚合结果的副本，不能将四路带宽相加当作唯一聚合结果带宽；该指标也不是物理端口计数器测得的线速。

测试数据在启动时生成模板，发送时更新request ID和第一个数字，使用非阻塞sendmmsg/recvmmsg复用缓冲区，不包含外部应用真实数据生成开销。

## 验证范围

独立C客户端已完成严格编译和本机模拟收发验证。具体硬件吞吐以实际运行日志为准。开发板加载的bitstream必须与软件协议一致。

第三方组件的许可证与版权说明保留在各自目录中。
