# ZCU102 双主机 FPGA 聚合（Corundum/mqnic + PL）

把 **Corundum/mqnic** 的 ZCU102 工程与 PL 侧自研聚合逻辑结合：两台主机各跑 2 个进程
（共 4 个 worker），通过 10G SFP+ 把带自定义协议头的 UDP 帧发到 FPGA；PL 在 **4 个
BRAM bank** 里按 request 匹配并做 **64 位流水加法**，再把结果**同时广播回 4 个口**，
4 个 rank 各收到同一份相同结果。

两台机器之间**不需要时间同步（不需要 NTP）**：启动对齐由「固定 base + 重试 + 首个
广播即 Go」的握手完成，稳态靠 FPGA 的硬件槽位天然锁步。

---

## 1. 拓扑

```text
主机A HOSTA_IP (X710)                        主机B HOSTB_IP (X520)
  rank0  0000:01:00.3 → eth1  bank0            rank2  0000:01:00.1 → eth3  bank2
  rank1  0000:01:00.2 → eth2  bank1            rank3  0000:01:00.0 → eth4  bank3
        DPDK primary + secondary                    DPDK primary + secondary
                        ↓ 4 × 10G SFP+ ↓
        ZCU102 PL：GTH → PCS → MAC → 4 bank → 求和 → 四口广播
```

`rank r ↔ FPGA 端口 r ↔ eth{r+1}`；每台机器一个 DPDK primary 配置本机两个口，另一个
进程是 attached secondary。真实地址/用户名放在 `多机/local.conf`（已被 `.gitignore`
排除），仓库里只有占位符。

## 2. 数据链路（一次 job 的全过程）

| 步骤 | 在哪里 | 发生了什么 |
|---|---|---|
| 1 | `多机/bench_random.c` | 本地生成 `rounds × 64` 个 `double`（默认 16×64 = 1024 个） |
| 2 | `多机/sfp_lib.c` | 定点化：`q = trunc(v × 1e16)`，限幅 `abs(v×1e16) < 2^63/workers`，以 int64 补码的 64 位比特入帧（保证 workers 个相加不溢出） |
| 3 | `多机/sfp_lib.c` | 组帧：`ETH14 + IPv4 20 + UDP 8 + AG 6 + rounds×520`；UDP 目的口 `0x2345`；16 round 时 8368 B → 主机 MTU 9000 |
| 4 | 主机 → FPGA | DPDK `tx_burst` → X710/X520 → SFP+ → GTH/PCS/MAC → 每口 CDC FIFO → `axis_udp_batch_bank` 解析校验 → 64×u64 存进 BRAM `{slot, round, entry}` |
| 5 | PL 聚合器 | 扫描 16 个槽；`workers∈[2,4]` 且所有 bank 的 tag/rounds/workers 一致才 match → 64 位模 2^64 加法（3 级流水） |
| 6 | PL 广播 | 结果帧（type 改 2、逐 round 写回 id、逐 entry 写回和的网络序）经 `axis_broadcast` 同时送到 4 个发送 FIFO → 4 个 SFP 口 |
| 7 | 主机 | 4 个 rank 各自 `rx_burst` 收到同一份 → 校验 → 按 `(base, rounds, workers)` 匹配在途槽 → `fixed_to_double()` → `out[]` |

槽号与 round id 的关系：`slot = (base >> 4) & 15`（16 个硬件 batch 槽，每槽最多 16 round）。

### 帧格式

```text
Ethernet | IPv4 | UDP | AG(6B) | round0 | round1 | ... | round{n-1}

AG  = magic 'A''G' (0x4147) | version=4 | workers | rounds | type(1=request, 2=response)
round (520B) = round_id u32 | reserved=0 u32 | value[64] u64   （全部网络字节序）
```

## 3. 启动同步（不用时间网）

- **握手（默认，`--sync=handshake`）**：所有参与 rank 用同一个固定 base `0x0FFF00`
  发一个 job，`sfp_probe_timeout(130 ms)` 超时后 `rearm` 并**重发同一个 base**（不做
  轮转）。FPGA 只有在 `workers` 个 bank 同槽到齐时才完成聚合并广播；所有 rank 在几 µs
  内收到同一帧，这就是共同的 **Go**。Go 之后进入 `0x100000 + n*16` 的 **16 深流水线**，
  稳态**绝不再等任何广播**（逐 job 停等只有 ~0.6 Gbit/s，16 深流水线才有 ~9 Gbit/s）。
- 旧方案保留为 `--sync=grid`（600 ms 墙钟网格，需要 NTP）做 A/B。

## 4. 2 / 3 / 4 worker

- `workers` 由每帧 AG 头携带，RTL 只累加 bank0..workers-1，因此 **2/3/4 路都支持**
  （当前 RTL 要求是编号最低的**连续**端口子集）。
- `rank ≥ workers` 的 rank 是**观察者**：不发包，靠 FPGA 广播拿到同一份结果，并用
  「16 job 探针」从在跑的流中追加入口。

## 5. FPGA 自恢复（RTL V5）

线上曾出现"跑一次后就再也收不到帧、只能断电"的挂死。根因是 `axis_udp_batch_bank`
的**帧解析器状态只由 `s_last` 或全局 `rst` 清除**：一帧被截断（RX FIFO 在帧中被复位、
链路毛刺、主机口停链路）后 `beat` 停在最大值，这个 bank 此后**永久拒收新帧**。
V5 的修复：

1. **bank 解析器复位**：恢复时把 `valid_mem/beat/good/accepting/record_*` 一起清；
2. **空闲看门狗（按 `tlast` 帧完成计）**：1 s 没有帧**完成**就恢复——卡死的 FIFO 会把
   `tvalid` 一直拉高却没有 `tlast`，按 `tvalid` 判定会永不触发；
3. **无进展看门狗**：有帧在完成但 2 s 没有广播（某个口 RX 死了）→ 恢复；
4. **停滞看门狗**：`active` 连续 1 ms 无输出进展 → 恢复；
5. 恢复脉冲经库里的 `sync_reset` 同步进每个端口时钟域，**同时复位 `worker_rx` 与
   `port_tx` 两个 CDC FIFO 的两侧**（单边复位正是原卡死的来源之一）。

另外聚合器入口加了一级流水寄存器，解除 300 MHz 下 `worker_rx` FIFO BRAM → bank 写
使能那条最差路径。最终 bitstream 时序收敛（WNS 为正、0 失败端点）。

## 6. 实测

| 项目 | 结果 |
|---|---|
| 连续 16 次 `./run_all.sh` | 全部 `rc=0`，**期间零断电、零重载 PL** |
| 4-worker 1M 请求 | `completed=1000001`、`errors=0` |
| 2 / 3 / 4-worker `--verify-const` | 求和分别 = 1.0 / 3.0 / 6.0，全部通过；observer 正常加入 |
| 吞吐 | ~8.8 Gbit/s（100k 短测）；RTT ~110 µs（随板卡/链路状态变化） |

## 7. 目录

- `多机/`：双主机 benchmark。`bench_random.c`（驱动/握手/窗口/observer）、
  `sfp_lib.{c,h}`（DPDK 2 口库 + 定点转换 + 结果匹配）、`bridge_protocol.h`（冻结协议）、
  `run_*.sh` / `setup_*.sh` / `release_*.sh`、`README_多机.md`（详细设计文档）。
- `corundum/`：Corundum/mqnic 工程。核心自研 RTL：
  `corundum/fpga/mqnic/ZCU102/fpga/rtl/axis_udp_batch_aggregator.v`，以及
  `corundum/fpga/mqnic/ZCU102/fpga/rtl/fpga_core.v` 里的端口/广播连线。
- `ethernet-switch/`：交换机参考实现（当前聚合路径未使用）。

## 8. 快速开始

```bash
# 0) 站点参数（该文件已被 .gitignore 排除，不会入库）
cp 多机/local.conf.example 多机/local.conf
$EDITOR 多机/local.conf          # 填 REMOTE="userB@hostB"、IP_PREFIX="192.168."

# 1) 每台机器一次性 root 准备（幂等；主机重启后必须重跑，见下）
sudo ./多机/setup_local.sh                                    # 主机A
ssh "$(grep ^REMOTE 多机/local.conf | cut -d'"' -f2)" \
    'cd 多机 && sudo bash setup_remote.sh'                    # 主机B

# 2) 板卡侧：加载 bitstream + mqnic 驱动
sudo cp <bitstream>/fpga.bin /lib/firmware/xilinx/mqnic/fpga.bin
sudo xmutil loadapp mqnic
sudo insmod ~/corundum/modules/mqnic/mqnic.ko
for i in 1 2 3 4; do sudo ip link set eth$i mtu 9000 up; done

# 3) 跑（默认 3M 请求、4 worker、握手同步）
./多机/run_all.sh
WORKERS=2 EXTRA_ARGS=--verify-const ./多机/run_all.sh 100000
WORKERS=3 EXTRA_ARGS=--verify-const ./多机/run_all.sh 100000
```

> **重启主机后必须重跑 `setup_local.sh` / `setup_remote.sh`**：`/dev/hugepages` 会回到
> `0755 root:root`、`/dev/vfio/N` 回到 `0600`，非 root 的 DPDK 起不来
> （`Permission denied` on `<prefix>map_0`）。`run_local.sh` / `run_remote.sh` 会先自检，
> 不满足直接报错退出，不会留下空转的孤儿进程。持久化办法见 `多机/README_多机.md`。

## 9. 许可

Corundum、ethernet-switch 等第三方组件的许可证与版权见各自目录；本仓库其余部分见
`LICENSE`。
