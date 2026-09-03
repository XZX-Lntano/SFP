# ZCU102 Corundum 64-Entry MPI-FPGA Aggregation

## 1. 项目解决的问题

本项目在 ZCU102 的 PL 侧部署基于 Corundum/mqnic 的四端口 FPGA 网卡，
用于模拟分布式应用中的 AllReduce 求和流程。

应用层将 2、3 或 4 路 worker 的连续数值发送给主机上的 MPI bridge。bridge
将每路数据封装成 Ethernet + IPv4 + UDP + 自定义 payload 帧并经四个 SFP
端口发送到 FPGA。FPGA 对相同轮次的 64 个连续槽位逐项求和，将结果帧
广播回四个端口；四个 MPI rank 分别接收并比对结果，rank0 再把最终结果回传
给应用层。

通用物理连接示例（请按实际布线替换）：

```text
<host-port-0> <-> ZCU102 SFP 0
<host-port-1> <-> ZCU102 SFP 1
<host-port-2> <-> ZCU102 SFP 2
<host-port-3> <-> ZCU102 SFP 3
```

## 2. 主要功能

- FPGA 侧支持 64 个连续槽位，每项为 `uint64` 数值；物理 payload 为 `round_id + 64*8` 字节，不再携带 index。
- 支持 2、3、4 路 worker 聚合；worker 数由 IPv4 TTL 字段传递给 FPGA。
- 未提供的尾部槽位自动以 `0` 补齐参与求和；所有 worker 必须提供相同数量的 value。
- FPGA 结果通过四个 SFP 发口广播；4 个 MPI rank 均接收同一结果并进行一致性
  校验。
- `bridge_app_client.py` 提供应用层 UDP 客户端。
- `test_bridge_64_entries.py` 覆盖 2/3/4 worker、每路 64 项的端到端测试。
- bridge 使用固定、本地管理的源 MAC `02:00:00:00:00:01` 发送 FPGA 请求，
  避免多端口主机 NIC 将回包识别为本卡 MAC 自环流量。

核心文件：

```text
corundum/fpga/mqnic/ZCU102/fpga/rtl/axis_udp_pair_aggregator.v
corundum/fpga/mqnic/ZCU102/fpga/rtl/fpga_core.v
进程间计算/mpi_fpga_bridge.c
进程间计算/bridge_app_client.py
进程间计算/test_bridge_64_entries.py
```

## 3. 安装方法

### 3.1 主机软件

需要 Linux、Python 3、OpenMPI 和可访问四个网口的 root 权限。编译 bridge：

```bash
cd进入bridge的目录中

/usr/local/openmpi/bin/mpicc -Wall -Wextra -Werror -std=gnu11 -O2 \
  -o mpi_fpga_bridge mpi_fpga_bridge.c

python3 -m py_compile \
  bridge_app_client.py \
  test_bridge_64_entries.py \
  pre_synth_equivalence_check.py
```

如果 OpenMPI 安装在其他目录，请将命令中的 `mpicc` 和 `mpirun` 替换为本机
对应路径。

### 3.2 FPGA 工程

在主机上clone corundum和ethernet-switch这两个在github上的开源项目，需要将这两个项目文件clone到同一路径下。
将rtl这个文件替换掉corundum的corundum/fpga/mqnic/ZCU102/fpga/rtl中。
将Makefile替换掉corundum/fpga/mqnic/ZCU102/fpga/fpga路径中的Makefile文件。
随后按照corundum的ZCU102文件中的README.md继续操作。

通过现有的 ZCU102 Corundum/XRT 部署流程加载这两个文件。加载完成后，应先
确认四个 SFP 链路均为 `Link detected: yes`。本 README 不提供通用烧录命令，
因为加载方式取决于板卡 Ubuntu 中已配置的运行时和部署脚本。

## 4. 使用方法

### 4.1 启动四 rank bridge

下面命令使用文档示例的四口映射与保留 IP 网段。请根据实际网口名称、SFP 对应
关系、FPGA 目的 MAC 和 IP 地址修改参数。

```bash
cd <repository-root>/进程间计算

sudo /usr/local/openmpi/bin/mpirun --allow-run-as-root -np 4 \
  ./mpi_fpga_bridge \
  10000 ff:ff:ff:ff:ff:ff \
  <host-port-0> 192.0.2.1 198.51.100.1 \
  <host-port-1> 192.0.2.2 198.51.100.2 \
  <host-port-2> 192.0.2.3 198.51.100.3 \
  <host-port-3> 192.0.2.4 198.51.100.4 \
  0x2345 4000 4001 4002 4003 5000
```

参数含义：

```text
<app_port> <fpga_dst_mac>
<if0> <src0> <dst0> ... <if3> <src3> <dst3>
[fpga_udp_dst_port src_port0 src_port1 src_port2 src_port3 timeout_ms]
```

- `app_port`：应用层与 rank0 通信的 UDP 端口。
- `fpga_dst_mac`：FPGA 接收端 Ethernet 目的 MAC；当前示例使用广播 MAC。
- `if0..if3`：四个主机 worker 网口，顺序分别对应 MPI rank0..rank3。
- `srcN/dstN`：第 N 路发往 FPGA 帧的 IPv4 源/目的地址。
- `0x2345`：FPGA UDP 目的端口。
- `4000..4003`：四路 UDP 源端口。
- `5000`：每个 rank 等待 FPGA 广播回包的超时时间，单位为毫秒。

正常启动后，bridge 会打印四个接口和 IP 配置。成功请求只打印一行汇总；超时
或发送失败时才打印逐 rank 诊断信息。

### 4.2 发送应用层请求

在另一个终端执行四 worker 示例：

```bash
cd <repository-root>/进程间计算

python3 bridge_app_client.py \
  --host 127.0.0.1 \
  --port 10000 \
  --request-id 4660 \
  --worker0-values 10 20 30 40 50 \
  --worker1-values 1 2 3 4 5 \
  --worker2-values 1 1 1 1 1 \
  --worker3-values 2 2 2 2 2
```

至少提供 `worker0` 和 `worker1`；连续提供 `worker2`、`worker3` 时，bridge
会分别执行 3 路、4 路聚合。

### 4.3 64 项全量验收

依次验证 2、3、4 worker：

```bash
cd <repository-root>/进程间计算
python3 test_bridge_64_entries.py --host 127.0.0.1 --port 10000
```

仅验证四 worker：

```bash
python3 test_bridge_64_entries.py \
  --host 127.0.0.1 --port 10000 --workers 4
```

### 4.4 系统有效带宽

该 benchmark 统计端到端成功聚合结果的有效带宽，每个 64 槽 round 计为
`64*8 = 512` 字节；同时报告 FPGA 输入 payload、四端口广播负载及应用 UDP
往返流量：

```bash
python3 benchmark_bridge_bandwidth.py \
  --host 127.0.0.1 --port 10000 \
  --workers 4 --duration 60 --warmup 10 --timeout 8 \
  --request-id-start 0x9000
```

## 5. 输入输出示例

### 应用层输入

上面的四 worker 命令提供五个连续 value。FPGA 内部实际接收固定 64 个槽位；
尾部槽位自动补零。应用消息固定为 2580 字节（20 字节控制头、5 路各 64 个
uint64 槽位），物理 FPGA 帧为 556 字节（Ethernet/IPv4/UDP、自定义 round_id
和 64×8 字节 payload）。

### bridge 输出

```text
MPI-FPGA bridge 已启动: app_port=10000, mpi_ranks=4
worker_ifaces=(<host-port-0>,<host-port-1>,<host-port-2>,<host-port-3>), result_ifaces=(<host-port-0>,<host-port-1>,<host-port-2>,<host-port-3>), fpga_dst_mac=ff:ff:ff:ff:ff:ff
worker0_ip=192.0.2.1 -> 198.51.100.1 worker1_ip=192.0.2.2 -> 198.51.100.2 worker2_ip=192.0.2.3 -> 198.51.100.3 worker3_ip=192.0.2.4 -> 198.51.100.4
request=4660 worker0=[10, 20, 30, 40, 50] worker1=[1, 2, 3, 4, 5] worker2=[1, 1, 1, 1, 1] worker3=[2, 2, 2, 2, 2] fpga=[14, 25, 35, 46, 58] resp=[14, 25, 35, 46, 58]
```

### 应用层输出

```text
peer        : 127.0.0.1:10000
request_id  : 4660
worker_count: 4
entry_count : 5
status      : 0 (OK)
worker0     : [10, 20, 30, 40, 50]
worker1     : [1, 2, 3, 4, 5]
worker2     : [1, 1, 1, 1, 1]
worker3     : [2, 2, 2, 2, 2]
result      : [14, 25, 35, 46, 58]
```

对于每个槽位 `i`，输出结果满足：

```text
result[i] = worker0[i] + ... + workerN[i]
```

其中 `N` 为最后一个参与的 worker，取值为 1、2 或 3。
