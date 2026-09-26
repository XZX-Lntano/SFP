#!/usr/bin/env bash
# 把 AOC 2 个 X520 PF 从 DPDK(vfio-pci) 解绑。
# 用法:
#   sudo ./release_remote.sh          # 解绑后留空（无驱动）
#   sudo ./release_remote.sh kernel   # 解绑后绑回内核 ixgbe 驱动（恢复普通网卡）
# 幂等。返回 DPDK 时重跑 setup_remote.sh 即可（会重建 /dev/vfio 节点并 chmod）。
# 管理网口 enp4s0 (0000:04:00.0) 不受影响。
set -euo pipefail
[[ $EUID -eq 0 ]] || { echo "run as root (sudo)"; exit 1; }

# 1) 停掉 bench 进程（这些口的唯一用户）
if pgrep -x bench_random >/dev/null; then
    pkill -x bench_random
    for i in 1 2 3 4 5 6 7 8 9 10; do pgrep -x bench_random >/dev/null || break; sleep 1; done
    if pgrep -x bench_random >/dev/null; then echo "bench_random 仍在运行"; exit 1; fi
    echo "bench_random 已停止"
fi

# 2) 删 DPDK hugepage map 文件（每个钉住一个 2 MiB 页）
rm -f /dev/hugepages/sfp-mm-*

PF="0000:01:00.1 0000:01:00.0"

# 3) 清 driver_override（setup 时设为 vfio-pci；不清则 unbind 后内核自动绑回 vfio-pci，
#    且残留的 override 会让 bind 其它驱动报 ENXIO）
#    注意：sysfs 属性不能 rm（EPERM）；必须写“只含换行”的内容——内核 strip 掉
#    尾随换行/空格后为空即清除。`echo -n`（零字节写）会被 VFS 直接短路，无效！
for b in $PF; do
    if grep -q . "/sys/bus/pci/devices/$b/driver_override" 2>/dev/null; then
        echo > "/sys/bus/pci/devices/$b/driver_override"
    fi
done

# 4) 从 vfio-pci 解绑（2 个 PF 同属一个 IOMMU group，一次全解）
for b in $PF; do
    if [[ -e /sys/bus/pci/devices/$b/driver ]] && \
       [[ "$(basename "$(readlink /sys/bus/pci/devices/$b/driver)")" == "vfio-pci" ]]; then
        echo "$b" > /sys/bus/pci/drivers/vfio-pci/unbind
        echo "$b: 已从 vfio-pci 解绑"
    else
        echo "$b: 不在 vfio-pci（跳过）"
    fi
done

# 5) 可选：绑回内核驱动
if [[ ${1:-} == kernel ]]; then
    modprobe ixgbe
    for b in $PF; do
        echo "$b" > /sys/bus/pci/drivers/ixgbe/bind
        sleep 3   # X520 SFP+ 上电后链路需要几秒
        echo "$b: 已绑 ixgbe"
    done
    sleep 2
    for i in /sys/class/net/*; do
        n=$(basename "$i")
        dev=$(readlink -f "$i/device" 2>/dev/null | xargs basename 2>/dev/null)
        case $dev in
            0000:01:00.*)
                echo "  $n: link=$(cat $i/carrier 2>/dev/null) speed=$(ethtool $n 2>/dev/null | awk -F': ' '/Speed/{print $2}')"
                ;;
        esac
    done
fi

echo
echo "当前状态:"
for b in $PF; do
    if [[ -e /sys/bus/pci/devices/$b/driver ]]; then
        drv=$(basename "$(readlink /sys/bus/pci/devices/$b/driver)")
    else
        drv="(未绑定)"
    fi
    echo "  $b -> $drv"
done
echo "HugePages_Free: $(awk '/HugePages_Free/{print $2}' /proc/meminfo)"
echo "（不再用 DPDK 可执行 echo 0 > /proc/sys/vm/nr_hugepages 归还预留内存）"
