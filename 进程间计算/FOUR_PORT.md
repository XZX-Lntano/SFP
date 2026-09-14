# 四端口应用桥接（应用协议v5、FPGA协议v4）

每个rank监听127.0.0.1:(base_port+rank)，默认10000..10003。客户端用四个socket向四个rank发送同一request_id对应的worker分片；四个rank独立收发FPGA并分别回复各自请求的源地址/端口。MPI只用于DPDK初始化、启动握手和退出，不参与每批数据搬运。

应用报文保留24字节头：magic:u32、version:u16=5、type:u16、request_id:u32、rounds:u16、workers:u16、entries:u16=64、status:u16、rank_id:u32。request_id仍是16对齐的base round ID。请求携带该worker的rounds*64个大端uint64；2/3worker场景仍向四口发送请求，不参与聚合的rank仅接收头，以便关联广播回复。响应携带一份聚合结果和来源rank_id。旧v4单端口应用请求被拒绝，FPGA侧帧格式不变。

FourPortClient.pending以request_id为键，上限window，且不允许在途请求占用相同硬件batch槽。先登记再发送，避免快速响应竞态；收到来源rank、版本、长度、轮数、worker数和OK状态均匹配的响应即删除表项并返回，其他副本直接丢弃，不比较四份payload。这里“正确”指协议完整且状态OK；应用客户端无法仅凭响应自行证明任意计算结果正确。数值正确性由独立测试和benchmark已知数据验证。

超时删除表项。部分发送失败也清理表项并报错；这不撤销已到达FPGA的贡献。发生丢包或超时后应停止发送，等待硬件100ms回收并使用新request_id重试，不盲目复用旧ID。一个client持有整个FPGA的槽位信用；不支持多个独立客户端各自占满窗口。第一份回复到达后其他rank可能尚未返回，桥接器另有有界软件记录缓冲，按完整request_id保留迟到回复；客户端仍应持续调用receive以丢弃迟到副本。发送失败、超时清理不等于可靠重传。

当前真实RTL是256 round槽=16batch。之前未完成的改动只将软件常量置512而未修改RTL，本次修复为256以匹配已部署bitstream。32batch扩容未实施，不能使用window32。每batch仍最多16round，每round64项。本次没有重新烧录bitstream。

构建和启动：

```bash
cd ~/Desktop/SFP/进程间计算
make
sudo /usr/local/openmpi/bin/mpirun --allow-run-as-root -np 4 --bind-to core --map-by core ./mpi_fpga_bridge --port 10000
python3 bridge_app_client.py --port 10000 --request-id 0x1000 --worker0-values 10 20 --worker1-values 1 2 --worker2-values 3 4 --worker3-values 5 6
python3 benchmark_bridge_bandwidth.py --port 10000 --workers 4 --rounds 16 --window 8 --duration 10 --cpu 4
python3 bridge_app_client.py --port 10000 --stop
```

停止命令必须发送到四口；仅停止一个rank会使其他rank停留在DPDK退出同步。SIGTERM退出整个mpirun也可用于异常清理。旧--simulate模式已移除，不能用旧集中式软件求和冒充新路径；test_four_port_client.py验证客户端网络行为，真实FPGA测试验证聚合。

benchmark_bridge_native已改为四目的端口，独立worker分片，首个数值正确结果消耗在途条目，其余响应记late。应用流量按实际socket收发字节计；四份FPGA结果只计一份有效聚合带宽。报文发送使用C sendmsg scatter/gather；Python包装仅构建并启动C程序。

本次四路首响应实测发现原RX64会漏收部分非获胜副本，因此DPDK改用RX256、TX128、每口2047个jumbo mbuf；这是新模式的可靠性修正，不是声称加深RX环本身能提升吞吐。
