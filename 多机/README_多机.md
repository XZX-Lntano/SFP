# 多机 4-rank 基准（本机 + AOC 跨机）

把原本单机的 4 个 bench 进程拆成 **两台机器各 2 个**，共享同一块冻结的 FPGA
（ZCU102 `axis_udp_batch_aggregator`）。FPGA 不变，仍然聚合 4 个 worker、
向 4 个口广播结果；只是这 4 个口的对端分属两台主机。

```
本机 HOSTA_IP (X710)                   主机B HOSTB_IP (X520)
  rank0  0000:01:00.3  -> eth1  bank0            rank2  0000:01:00.1  -> eth3  bank2
  rank1  0000:01:00.2  -> eth2  bank1            rank3  0000:01:00.0  -> eth4  bank3
        [DPDK primary + secondary]                         [DPDK primary + secondary]
```

每台机器 **一个 DPDK primary** 配置本机两个口（MTU 9000、jumbo pool、队列），
同机第二个进程作为 **secondary** 通过 `--file-prefix` 附加到同一 hugepage 上。
两台机器之间的启动同步**默认不再依赖 NTP**：所有参与 rank 反复重发同一个固定
base 的同步帧，FPGA 在全员到齐后广播的那一刻就是共同的“Go”（见下）。
`--sync=grid` 可切回旧的 600 ms 墙钟网格作为回退与 A/B 对照。

## 与单机版的差异（改动点）

| 项 | 单机版 (`../进程间计算`) | 多机版（本目录） |
|---|---|---|
| 每机进程数 | 4（rank0-3 同机） | 2（本机 rank0/1，AOC rank2/3） |
| 每 DPDK 实例端口 | 4 个口 | 2 个口（`--bdfs` 只列本机两口） |
| primary/secondary | 1 primary + 3 secondary | 每机各 1 primary + 1 secondary |
| 启动同步 | `synchronized_start()` 10 s 边界 + `sleep(1)` | **固定 base 握手**（默认）/ 600 ms 网格（`--sync=grid`） |
| 跨机 worker 缺失 | 无 | 非参与 rank **流中追加入口**（见下） |
| hugepage 回收 | 单机清理 | 每机本地清理 + 孤儿 map 文件清扫 |

### 1) 启动同步：固定 base 握手（默认，无需时钟）

**为什么能不用时间网**：FPGA 的聚合槽有 16 个（`slot=(base>>4)&15`），且
`accepting = good && !valid_mem[slot]`；一个 batch 只有在 `workers` 个 bank
的帧落进**同一个 100 ms 窗口**时才完成并**向 4 个口广播一次**。因此：
- 稳态下快的主机最多领先 16 个 slot 就会被槽占用卡住，**锁步是 FPGA 硬件给的**，
  完全不需要时钟；
- 唯一要解决的是“开跑时大家落在 100 ms 内”，而 FPGA 的广播到 4 个口的时延差
  只有几 µs，正好可以当共同的“Go”边沿。

**握手过程**（`bench_random.c`，`--sync=handshake`，默认）：
1. 每个参与 rank 用**同一个固定 base** `0x0FFF00` 发**一个** job（一帧占一个槽），
   输入值随意；
2. 没等到广播就是 `STATUS_TIMEOUT`（预期内）：`sfp_probe_timeout(130 ms)` 让
   该槽 130 ms 就释放（> RTL 的 100 ms 槽回收，< 常规 500 ms），然后
   `sfp_rearm()` 并**重发同一个 base**——**不做 base 轮转**（各自轮转会让各主机
   base 不同、永远无法匹配）；
3. 最后一个参与 rank 就位的瞬间，`workers` 个 bank 的同一 base 在 100 ms 内到齐，
   FPGA 完成聚合并把结果广播到 4 个口；所有 rank 在几 µs 内收到**同一个**帧；
4. 该帧即共同 Go，各 rank 立刻切到测量段 `base = 0x100000 + n*16` 的 **16 深流水线**。

先发的主机不是“停住”，而是“原地重发同一个 base 空转等待”；因此无论另一台是
1 ms 还是 10 s 后才启动，都能被下一轮重试接住（上限 60 s）。**稳态绝不逐包等广播**
（逐 job 停等只有 ~0.6 Gbit/s；16 深流水线才有 ~9 Gbit/s）。

`--sync=grid` 保留旧的 **600 ms 墙钟网格**：所有 rank 读 `CLOCK_REALTIME`、在
绝对 600 ms 网格点各发一个 warmup 批（base 在 8 个 slot 间轮转），第一个对
所有 rank 都 OK 的网格点即共同起点。它依赖 NTP，启动开销约 1.2–2.4 s，作为
回退与 A/B 对照。

### 2) 非参与 rank（workers < 4 时）：被动观察 + 探针式流中追加入口
当 `workers < 4`，`rank >= workers` 的 rank 不发包，只等 FPGA 广播。跨机时
非参与 rank 看到结果流会比参与方**晚一个跨机时延**，主循环早期的广播可能
早已过去，直接对齐会全部 `late`。软件行为：
- **warmup 被动**：非参与 rank 不发 warmup 批，只等本口收到**任意** FPGA
  结果帧（`sfp_latest_base() >= 0x0FFE00`，60 s 上限）以确认 FPGA→本口 RX
  路径可用；参与 rank 照常网格 warmup。
- **verify-const 跳过**：参与 rank 验证 FPGA 求和，非参与 rank 收的是同一
  份广播，无需重复验证。
- **探针式流中追加入口**：FPGA 每个 job 的结果帧**只广播一次**（线速
  ~152k jobs/s、~6.6 µs/job），追加入口要求“首 16 个 slot 必须在接下来 16
  帧到达前（~106 µs 内）全部 INFLIGHT”。一次性“当前位置 +16”的旧入口有竞态
  （1 ms 轮询周期 = 152 个 job，远大于 16 的余量，实测会整批丢失）。
  现在入口是 **16 job 探针 + 自动重试**：探针 slot 用 150 µs 短超时
  （`sfp_probe_timeout`，库支持 per-slot 超时；探针超时不打印 stalled、
  不 latch 失败标志），且探针不填 in[]（非参与 rank 从不发包，worker 直接
  标 INFLIGHT，每 job ~2 µs）；探针首 slot 完成 OK 即整批锁定（后面的帧
  间隔更宽裕），主循环从 `n0+16` 开始；探针失败（帧已过去）则等新位置
  自动重试，通常 1 次即成。主循环阶段非参与 rank 同样不填 in[]，16 深
  流水线按帧到达率自节拍下发。
- **退出判据**：非参与 rank `done + 32 >= requests - n_start`（加入边沿与
  偶发丢帧的余量）；参与 rank 要求 `done == requests` 精确相等。非参与
  rank 的 `wire_estimate=0` 是**设计使然**（由 tx_packets 计算，非参与
  rank 不发包）；其 `useful_result`/`avg_rtt` 正常反映本口 RX 路径性能。
- **参与方 keep-alive**：`workers < 4` 时，参与 rank 在测量段结束后继续向
  主流续发帧（base 从 requests 继续、in[]=0），给迟到的非参与 rank 留出
  活的流；keep-alive 前先在**本机 2 rank 间做屏障**（两个参与方同时收尾，
  FPGA 的 workers 批才不断流），随后再 5 s 宽限流式续发。
- **单帧丢失容忍**：广播不可重传，链路一次毛刺 = 丢一个 job（500 ms slot
  超时）。超时只 latch `G.failed`（拒绝**新**提交），**不再连带超时窗口内
  另外 15 个 slot**（它们的帧可能还在路上）；非参与 rank 主循环遇 -EIO 会
  `sfp_rearm()` 重试（连 8 次即判定流已死并退出）。因此单次丢帧不再把整个
  run 判死（退出判据还有 32 的余量）。

### 3) 其它
- `sfp_lib.c`：EAL 只 `-a` 本机两口；primary 配置两口、建两个 pool；
  `require_link` 时等链路最多 10 s（AOC 的 X520 SFP+ 上电后需数秒才 link）。
- 每台机器结束时的本地 2-rank 完成屏障：secondary 写 done→等 release→fini→
  写 clean；primary 等 done→写 release→等 clean→fini→删本前缀 hugepage map。
  **所有提前退出路径**（warmup/verify 失败、非参与 rank 追加入口超时、
  屏障超时）走同一套协调（`abort_with_peer`）：DPDK primary 的 EAL cleanup
  会销毁 secondary 仍在用的共享资源——primary 先退会**段错误** secondary
  （实测 rc=139），所以两 rank 必须先会合再按“secondary 先退”的顺序收尾。
  崩溃残留的 map 文件（会各自钉住 2 MiB 大页）由 run 脚本在启动时清扫
  （只删 >60 min 的，避免误删并发运行）。
- `run_all.sh`（本机运行）：rsync 同步代码到 AOC → **md5 校验关键源码两端一致**
  （rsync 静默失败会让两台机器跑不同代码，直接 `exit 3`）→ AOC 上 make →
  ssh 后台启动 AOC 的 `run_remote.sh`（`WORKERS/ROUNDS/SYNC/EXTRA_ARGS` 内嵌进
  命令串转发，因为 `VAR=x ssh` 不传环境变量）→ 本机前台跑 `run_local.sh` →
  汇总 rc。

## 文件
- `bridge_protocol.h` 冻结协议头（与单机版逐字一致）。
- `sfp_lib.{h,c}` 2 口/机 DPDK 库（primary 配置两口 + secondary 附加）。
- `bench_random.c` 驱动：CLI（含 `--sync=handshake|grid`）、固定 base 握手 /
  600 ms 网格启动同步（非参与 rank 被动观察）、verify-const（非参与 rank
  跳过）、测量段、探针式流中追加入口（非参与 rank）、参与方 keep-alive
  （workers<4）、本地完成屏障与协调退出、退出码。
- `Makefile` 普通 gcc + `pkg-config libdpdk`（无 MPI）。
- `setup_local.sh` / `setup_remote.sh` **一次性 root** 脚本（幂等）：
  hugepage、绑定 `vfio-pci`、`chmod 1777 /dev/hugepages`、`chmod 0666 /dev/vfio/*`。
  本机 X710 的 4 个 PF 在同一 IOMMU group，必须**整组**留在 vfio-pci
  （DPDK 只 `-a` 其中两口，其余两口空闲但必须同驱动）。
- `run_local.sh` / `run_remote.sh` 各起本机 2 个进程（非 root）。
- `run_all.sh` 一键跨机编排。

## 运行
```bash
# 一次性绑定DPDK和设置大页（各机器，root）：
sudo ./setup_local.sh          # 本机
ssh USER_B@HOSTB_IP 'sudo bash 多机/setup_remote.sh'   # AOC（密码见本地 local.conf）

# 启动命令，常规 3M 4-worker（默认 --sync=handshake，不需要 NTP）：
./run_all.sh                     # = ./run_all.sh 3000000

# 切回旧的 600 ms 墙钟网格做 A/B（需要 NTP）：
SYNC=grid ./run_all.sh 300000

# 短测 + 数值校验（每 rank 16 个常量批，期望和 = workers*(workers-1)/2）：
EXTRA_ARGS=--verify-const ./run_all.sh 1000

# 其它 worker 数：
WORKERS=2 EXTRA_ARGS=--verify-const ./run_all.sh 100000
WORKERS=3 EXTRA_ARGS=--verify-const ./run_all.sh 100000
```
输出末尾 `=== summary: local rc=0 remote rc=0 ===` 表示两端全部 rank 退出码为 0。

## 结果（3M 请求，16 round，4 worker，本机+AOC）
| rank | 机器 | useful_result | wire_est | avg_rtt |
|---|---|---|---|---|
| 0 | 本机 | 8.475 Gbit/s | 8.681 | 116.6 µs |
| 1 | 本机 | 8.475 Gbit/s | 8.681 | 116.5 µs |
| 2 | AOC  | 8.475 Gbit/s | 8.682 | 119.7 µs |
| 3 | AOC  | 8.475 Gbit/s | 8.682 | 119.7 µs |

`completed=3000001`（3M 主段 + 1 warmup），`errors=0`，`window=16`。
对比单机 4-worker 基线（6.078 Gbit/s、32.35 s、≈166.5 µs）：**每 rank 提升约
39%**。原因：单机时 4 个 app 线程 + 4 个 DPDK worker 挤在同一颗 CPU 上；拆成
两机后每机只有 2+2，CPU 竞争下降，RTT 从 ~166 µs 降到 ~117 µs，从而在
16 深窗口下吞吐更高。AOC 的 X520 比本机 X710 慢 ~3 µs RTT。

## 固定 base 握手验证（2026-09-24）

默认 `--sync=handshake`，与 `--sync=grid` 在同一板卡状态下 A/B：

| 用例 | 同步 | requests | rc | useful_result/rank | avg_rtt |
|---|---|---|---|---|---|
| 4-worker | handshake | 300000 | 0/0 | 7.711 Gbit/s | 127–129 µs |
| 4-worker | grid | 300000 | 0/0 | 7.779 Gbit/s | 126–128 µs |
| 4-worker 冒烟 | handshake | 2000 | 0/0 | — | 124–126 µs |
| 2-worker + verify-const | handshake | 100000 | 0/0 | 7.356 Gbit/s | 134 µs |
| 3-worker + verify-const | handshake | 100000 | **失败** | — | — |

- **握手与网格吞吐在噪声内一致**（±1%，RTT 差 ~1 µs），说明握手只影响启动、
  不影响测量段；本轮板卡状态下两者都是 ~7.7–7.8 Gbit/s（此前某次 300k 网格
  跑出 9.03 Gbit/s、RTT 109 µs，属板卡/链路状态差异，与同步方式无关）。
- 4-worker 冒烟里 rank0 重试 15–16 次（等远端进程启动），其余 rank 1–4 次。
- 2-worker 的 verify-const 期望和 = 1.0，参与 rank 全部通过；observer 正常探针加入。
- **3-worker 那次失败与握手无关**：随后用 `--sync=grid` 跑 3-worker 同样失败，
  且紧接着 4-worker 也失败——三者都是 `latest_base=0`、timeout 递增、`late=0`
  `invalid=0`，即**完全没有帧**，正是下面「已知问题 1」的数据路径挂死。挂死发生
  在第 2、3 次运行之间，所以 3-worker 结果**不可用**，需板卡恢复后重测。
- `axis_udp_batch_aggregator` 的 RTL 已用 Vivado xsim 单测（`/tmp/agg_sim`，
  构造 workers=2/3/4 的真实帧各驱动 bank）：**三者都能匹配并产生 71 beat 的
  正确输出**，说明 RTL 本身支持 3 worker，3-worker 的结论要等硬件恢复。

## 最终验收（rev125，2026-09-24）

rev125 = V5 自恢复 + **bank 解析器复位**（真正的根因，见下节）。板卡断电重启并按
加载流程加载 rev125 之后：

| 验收项 | 结果 |
|---|---|
| 连续 6 次 `run_all.sh 100000`（4-worker） | **全部 rc=0** |
| 再连续 10 次 `run_all.sh 100000` | **全部 rc=0**（含 rev124 会挂死的第 7 次之后） |
| 4-worker 1M（两次） | `completed=1000001`、`errors=0`、rc=0 |
| 2-worker `--verify-const` | 参与 rank 全部通过，和 = 1.0；observer 正常加入 |
| 3-worker `--verify-const` | **participant 0/1/2 全部通过，和 = 3.0**；observer 加入 |
| 4-worker `--verify-const` | 四个 rank 全部通过，和 = 6.0 |

**结论：连续 16 次运行全部成功，期间没有任何断电/重载 PL；2/3/4-worker 数值全部正确。**

吞吐：本轮板卡状态下 1M/300k 稳定在 **5.12 Gbit/s**、RTT ~190 µs（100k 短测
4.5–5.5 Gbit/s）。同一会话早期在另一次上电状态下 rev124 是 7.7–8.2 Gbit/s、
RTT ~120 µs。rev125 相对 rev124 只多了 1 级输入流水（~3 ns）和若干看门狗/解析器
复位逻辑（都不在测量数据通路的吞吐路径上），因此 **RTT/带宽差异来自板卡与链路状态
（SFP/GTH 上电后的均衡、AOC 的 X520 等），不是这次修改引入的**；`errors=0`、
1M 请求下每 rank `timeout ≤ 15` 也说明数据通路干净。

## FPGA 自恢复（RTL V5，2026-09-24）

**问题**：复现出"第一次运行成功、之后每次都失败（四口 link 正常但 `latest_base=0`、
完全没有帧），只有断电/重载 bitstream 才恢复"。根因是 PL 状态跨主机 run 残留：
`axis_udp_batch_aggregator` 的 `active` 输出状态机**没有看门狗**——引擎里只有
"不完整批次 100 ms 回收"，而且那个回收只在 `!active` 分支里；一旦某个端口的
CDC FIFO 在 run 结束时（主机停口 → 每端口 PCS 复位 → 异步 FIFO 被单边复位）卡住，
四口广播永远凑不齐 `tready`，`active` 就永久置位，聚合器再也不 scan、再也不匹配。
PL 不会被主机侧重跑复位，所以状态一直残留到 PL 重配置。

**根因（关键）**：bank 的帧解析器状态（`beat/good/accepting/record_beat`）**只由
`s_last` 或全局 `rst` 清除**。一旦某帧被截断（RX FIFO 在帧中途被复位、链路毛刺、
主机口停链路），`beat` 会停在最大值 2047，此后**这个 bank 永久拒绝所有新帧**；聚合器
再也凑不齐 `workers` 个 bank、再也不广播。该状态跨主机 run 存活，只有 PL 重配置
（断电 / 重下 bitstream）能清——这才是"第一次成功、之后全失败"的真正根因。
第一版恢复只 flush 了 `valid_mem`、没有清解析器，所以没根治（实测连续 6 次成功后又挂死）。

**修复**（`corundum/fpga/mqnic/ZCU102/fpga/rtl/axis_udp_batch_aggregator.v` 与 `fpga_core.v`）：
1. **bank 解析器复位**：`flush` 时把 `valid_mem/beat/good/accepting/record_index/
   record_beat` 全部清零，并在 flush 期间挂起新帧解析。
2. **空闲看门狗（按帧完成计）**：连续 `IDLE_TIMEOUT_CYCLES`（1 s）没有任何帧**完成**
   （`tlast`）就触发恢复。**必须按 tlast 而不是 tvalid**：卡死的 RX FIFO 可以把
   `tvalid` 一直拉高却没有 `tlast`，按 tvalid 判定会永远不触发。
3. **无进展看门狗**：有帧在完成、但连续 `NO_MATCH_TIMEOUT_CYCLES`（2 s）没有任何广播
   （典型：某一个口的 RX 死了，组永远凑不齐）→ 触发恢复并复位四口 FIFO。
4. **停滞看门狗**：`active` 连续 `STALL_TIMEOUT_CYCLES`（1 ms）无输出进展 → 触发恢复。
5. **恢复脉冲**经 `recovery_pulse` 送到 `fpga_core`，用库里的 `sync_reset`（N=2）同步进
   每个端口的 `eth_rx_clk`/`eth_tx_clk` 域，**同时复位 `worker_rx` 与 `port_tx` 两个
   CDC FIFO 的两侧**。用 `sync_reset` 而不是手写 2-FF 同步器，是因为工程已有的
   `lib/axis/syn/vivado/sync_reset.tcl` 会自动加 `ASYNC_REG` 并对异步复位输入
   `set_false_path`，省掉手写 CDC 约束（第一版手写同步器被当同步路径分析，报出
   ~1.2 ns 无法收敛的违例）。
6. **输入流水寄存器**：聚合器在 bank 解析前对所有 `s_axis_*` 加一级寄存器，打断
   `worker_rx` FIFO 的 BRAM 输出 → bank 头部写使能（`headers_reg/CE`）这条 300 MHz
   最差路径；`s_axis_tready` 恒为高，所以只是纯延迟级。

**时序与 bitstream**：rev124（首次时序收敛）WNS=+0.006 ns、TNS=0；加入解析器复位后的
最终版 **rev125 WNS=+0.122 ns、TNS=0.000、0 个失败端点，DRC 干净**。bitstream 归档在
`corundum/.../rev/fpga_rev125.{bin,bit,xsa}`（md5 `431f27a29322d9a429947e4b728e08ca`），
并已写入板卡 `/lib/firmware/xilinx/mqnic/fpga.bin`。

xsim 验证（`/tmp/agg_sim`）：workers=2/3/4 回归通过；`test_idle.sv`（残留槽 → flush →
同槽新批次能匹配）；`test_stallrec.sv`（永久背压 → 恢复 → 放行后正常）；**`test_stuck.sv`
（tvalid 卡高且无 tlast 的截断帧 → 空闲看门狗按 tlast 判定、1 s 后恢复脉冲清解析器 →
后续正常帧可匹配）通过**。最后一项正是线上挂死场景的 RTL 级复现。

**板卡部署（ZCU102 PS，`USER_PS@BOARD_IP`，密码见本地 local.conf）**：
```bash
sudo cp <new>/fpga.bin /lib/firmware/xilinx/mqnic/fpga.bin
sudo xmutil loadapp mqnic            # 下载 bitstream + 应用 device tree overlay
sudo insmod ~/corundum/modules/mqnic/mqnic.ko   # 否则 PL 端口不 up、主机 no link
```
注意：**PS 重启不会自动加载 bitstream，也不会自动 insmod mqnic**（本次实测
`fpga_manager state=unknown`、无 `eth1..4`）；而 `xmutil loadapp`/`fpgautil -f Full`
重下 bitstream 也**不能**把 SFP/GTH 物理层恢复到干净状态（重下后主机仍然 no link，
PS 侧 `eth*` 一直 NO-CARRIER）。真正干净的状态只有**板卡断电重启**才能得到。


## 已知问题 / 注意事项
1. **FPGA 板卡数据路径会间歇性"挂死"（硬件现象，与软件/worker 数无关）**。
   现象：4 口 link 保持 up（主机侧 DPDK 看到 link UP），但双向没有任何
   有效帧——本机 eth1/eth2 与 AOC eth3/eth4 的 warmup 全部 60 s 失败。
   板卡端口在运行期间自动 LOWER_UP、停止传数后回到 UP，属正常行为，
   **不需要**为此重启开发板；但"数据路径挂死"状态一旦出现，连续多轮
   重跑（间隔 ~30 s）均不自行恢复。
   时间线（2026-07-22/23）：断电重启前，多轮 4-worker 3M 正常 → 连续多轮
   短测后 4 口全部挂死（含本机两口）；断电重启后 4-worker 3M、3-worker
   100k/1000、2-worker 短测全部再次正常（本软件全部路径验证通过）→
   2026-07-23 晚，连续 8 轮（含 4-worker 模式，用户日常使用的配置）
   全部 4 口无帧，重跑不恢复，处于同样的挂死状态。
   结论：**所有软件路径（2/3/4-worker、本机+跨机）在数据路径正常时均
   验证正确**；挂死与 worker 数、与具体帧内容无关。2026-07-22 那次是
   断电恢复的；本次（07-23 晚）目前重跑未恢复，需人工确认板卡状态
   （若持续无帧，断电循环一次即上次验证过的恢复手段）。
   建议排查：ZCU102 板卡散热（风道/风扇，重点 eth3/eth4 SFP 座所在区域）、
   重插/对调 eth3/eth4 的 SFP 模块与光纤（看故障跟随线缆还是端口）、
   10G PCS lock 状态。
   2026-09-24 再次复现：4-worker 300k 与 2-worker 100k 连续成功后被触发，
   之后 3/4-worker 全部 `latest_base=0`（见上节）。除物理断电外，也可在板卡
   PS 上远程重载 PL 恢复（等价于断电循环、会重置聚合器状态）：
   `sudo xmutil unloadapp && sudo xmutil loadapp mqnic`（overlay `mqnic_image_1`，
   固件 `/lib/firmware/xilinx/mqnic/`；SSH 走 PS GEM eth0，不受 PL 重载影响）。
2. **NTP 只在 `--sync=grid` 下需要**（两机时钟差 <100 ms 即可，不必 <1 ms）。
   默认的 `--sync=handshake` 不依赖任何时钟纪律。
3. AOC 的 `/dev/vfio/N` 节点在整组离开/回到 vfio-pci 时会被重建为 0600，
   之后非 root EAL 会 `Permission denied` —— 重跑 `sudo chmod 0666 /dev/vfio/*`
   （setup 脚本里有提醒）。
4. 本机有 docker 常驻占用 ~49 个大页，`HugePages_Free` 基线是 207 而非 256；
   AOC 基线 256。

## 解绑 DPDK（释放网卡）
每台机器各有一个释放脚本（root，幂等）：
```bash
sudo ./release_local.sh            # 本机：停 bench → 删 map 文件 → 清 driver_override
                                   #        → 4 个 X710 PF 从 vfio-pci 解绑（留空）
sudo ./release_local.sh kernel     # 再加绑回内核 i40e（恢复普通网卡）
ssh USER_B@HOSTB_IP 'sudo bash 多机/release_remote.sh kernel'   # AOC 同理（ixgbe）
```
要点：setup 脚本写过 `driver_override=vfio-pci`，**必须先清掉**，否则 unbind 后
内核会立刻自动绑回 vfio-pci；两口/四口同属一个 IOMMU group，要一次全解。
整组离开 vfio-pci 后 `/dev/vfio/N` 节点消失，之后要再用 DPDK 就重跑对应的
`setup_*.sh`（会重建节点并 chmod）。不再用 DPDK 可 `echo 0 > /proc/sys/vm/nr_hugepages`
归还预留内存。

## 回到单机
单机代码原封未动在 `../进程间计算/`（`run_window_benchmark.sh`）。多机只
改动了本目录副本；若要恢复单机 4-worker，把 AOC 两口的线换回本机对应口、
重跑 `../进程间计算` 的流程即可（FPGA 侧无需改动）。
