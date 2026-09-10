# MPI–FPGA 应用桥接

当前实现为 v4 DPDK 四 rank 桥接。

- [主 README：构建、板上加载、功能和带宽测试](../README.md)
- [协议、硬件容量和实测说明](THROUGHPUT.md)

在本目录执行 `make` 构建；`python3 pre_synth_equivalence_check.py` 检查协议。
主机 `setup_host.py` 中的 PCI 地址需要与实际硬件匹配。
