# 四端口改动验证

应用v5每个rank一个端口，FPGA帧保持v4。已修复之前软件512槽而RTL256槽的不一致；本次硬件仍16batch。

构建开启-Wall -Wextra -Werror；C/Python旧FPGA序列化36例通过；test_four_port_client验证四路分片、rank3先返回、错误响应不消耗ID、迟到副本不影响新请求、窗口容量和超时删除。

首次实机RX64：window8 4.189845 Gbit/s，window16 4.108757 Gbit/s，客户端无失败。但退出网卡计数显示四rank分别missed 9/66/58/10（按rank0..3），不能视为四口全部无损。首响应完成使慢rank失去原有同步保护，故改RX256及每口2047 mbuf后复测。后续日志rx256_w8.log、rx256_w16.log和numeric.log为最终配置测试。

pending删除前验证响应源、版本、ID、长度、轮数、worker数和status=OK。benchmark另校验已知数值，普通应用不重新计算聚合，也不比较其他副本。迟到副本计late是预期行为。request_id受当前FPGA协议要求仍须16对齐；超时停止后等待FPGA过期并使用新ID。缺少独立租约的多客户端并发不受支持。

最终RX256：9组数值用例（2/3/4 worker × 1/3/16轮，含uint64溢出）通过。window8五秒315662批、4.137387 Gbit/s、RTT126.021us；window16五秒315710批、4.138024 Gbit/s、RTT252.161us。四rank各completed631381、failed0、rejected0；所有端口missed/errors/no_mbuf/short_bursts均为0，退出码0。端口TX计数差异来自2/3worker用例不活跃端口只接收广播。测试进程已停止，固件未变。bridge_rx256.log保存退出计数。
