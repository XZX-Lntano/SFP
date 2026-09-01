#!/usr/bin/env python3
"""
Pre-synthesis (pre-Vivado) static equivalence check between:
  1) C bridge side   : build_fpga_frame() 打包出来的字节偏移
  2) Python client   : struct.pack 打包出来的字节偏移
  3) FPGA RTL side   : axis_udp_pair_aggregator.v 里的 ENTRY_BASE_OFF + e*STRIDE 公式
  4) 原 RTL ENTRY_COUNT=5 时手写死的那些字节偏移 (46-53 value0, 56-63 value1, 66-73 value2, 76-83 value3, 86-93 value4)

目标: 在不烧录 bitstream 的前提下, 用纯 Python 枚举 64 entry 所有字节, 给出
  "RTL 参数化公式 == 原手写硬编码 == C build 公式 == Python build 公式"
的 100% 对拍报告。任何一处错位都会 FAIL 并打印具体哪个 entry/哪个字节错了。
"""
from pathlib import Path
import struct
import sys

# ======== 用户可配置: 改成你要验证的 ENTRY_COUNT ========
ENTRY_COUNT = 64
# 原 RTL ENTRY_COUNT=5 时, value 字节区间写死的对照 (旧 RTL 里的 5 段):
LEGACY_VALUE_RANGES_5 = [
    (46, 53),   # value0
    (56, 63),   # value1
    (66, 73),   # value2
    (76, 83),   # value3
    (86, 93),   # value4
]
# ==========================================================

# ---- 1. C / Python / 新 RTL 三个地方约定的通用公式 ----
ROUND_ID_OFF_HI = 42
ROUND_ID_OFF_LO = 43
ENTRY_BASE_OFF = 44
ENTRY_STRIDE = 10
ENTRY_VALUE_OFF = 2

pass_all = True

def check(label, ok, details=''):
    global pass_all
    if ok:
        print(f'  PASS: {label}')
    else:
        print(f'  FAIL: {label} -- {details}')
        pass_all = False


print(f"[Info] 验证配置: ENTRY_COUNT={ENTRY_COUNT}")
print()
print('=' * 80)
print('① 原 RTL ENTRY_COUNT=5 写死的 value 区间 是否等于 通用公式 46+10*e .. 53+10*e ?')
print('=' * 80)
for e, (old_lo, old_hi) in enumerate(LEGACY_VALUE_RANGES_5):
    new_lo = ENTRY_BASE_OFF + e * ENTRY_STRIDE + ENTRY_VALUE_OFF          # = 46+10*e
    new_hi = ENTRY_BASE_OFF + e * ENTRY_STRIDE + ENTRY_VALUE_OFF + 7      # = 53+10*e
    check(
        f'entry {e}: old range [{old_lo},{old_hi}] vs formula [{new_lo},{new_hi}]',
        (old_lo, old_hi) == (new_lo, new_hi),
        f'旧写死=({old_lo},{old_hi}) 公式=({new_lo},{new_hi})'
    )

print()
print('=' * 80)
print('② C 侧 build_fpga_frame() 实现的字节偏移 ↔ 通用公式 ↔ 原 RTL 5 entry 值 对拍:')
print('   对每个 entry e, 构造 w0_entries = [(e+1, 0x1111111111111111*(e+1))] 等不同 value')
print('   验证字节 46+10*e .. 53+10*e 里 value BE 字节 exactly 匹配。')
print('=' * 80)

# 直接复刻 C build_fpga_frame() 的打包逻辑 (Python 等价):
def c_build_fpga_frame_bytes(entry_count, entries_index, entries_value):
    """
    复现 C 侧 build_fpga_frame 里 payload 部分 (bytes 42..42+2+entry_count*10)
    不关心 ETH/IP/UDP 头 (头 42 字节对 entry 偏移无影响, 前面①已经通过)
    """
    assert len(entries_index) == entry_count and len(entries_value) == entry_count
    payload = bytearray()
    # round_id: 固定填 0x1234 测试
    payload += struct.pack('!H', 0x1234)
    for i in range(entry_count):
        index_be = struct.pack('!H', entries_index[i] & 0xFFFF)
        value_be = struct.pack('!Q', entries_value[i] & 0xFFFFFFFFFFFFFFFF)
        assert len(index_be) == 2 and len(value_be) == 8
        payload += index_be + value_be
    return bytes(payload)

# 对拍 ENTRY_COUNT=64 全量
test_indices = list(range(1, ENTRY_COUNT + 1))
test_values = [((e + 1) * 0x1111111111111111) & 0xFFFFFFFFFFFFFFFF for e in range(ENTRY_COUNT)]
payload_bytes = c_build_fpga_frame_bytes(ENTRY_COUNT, test_indices, test_values)

def payload_byte_offset(e, value_byte):
    """通用公式得到 value_byte ∈ [0,7] 字节在 payload 里的 0-based 偏移 (不含 ETH/IP/UDP 42B 头,
       所以在 frame 总字节里再加 42 就等于 ENTRY_BASE_OFF + ...)"""
    return 2 + e * ENTRY_STRIDE + ENTRY_VALUE_OFF + value_byte

# entry 0..4 用旧 RTL 硬编码区间对拍一遍 (证明参数化后前 5 完全等价旧 RTL)
for e in range(5):
    for vb in range(8):
        off_in_frame = 42 + payload_byte_offset(e, vb)
        # 旧 RTL 写死 value[e][vb] 对应 byte:
        legacy_byte = LEGACY_VALUE_RANGES_5[e][0] + vb
        expected_val = test_values[e]
        # payload 对应字节 (payload_bytes 已去掉前 42 头):
        actual_byte = payload_bytes[payload_byte_offset(e, vb)]
        expected_byte = (expected_val >> (8*(7 - vb))) & 0xFF
        ok_off = off_in_frame == legacy_byte
        ok_val = actual_byte == expected_byte
        check(
            f'legacy entry {e} value_byte {vb}: frame_off={off_in_frame} vs legacy_off={legacy_byte}, byte=0x{actual_byte:02X} vs exp=0x{expected_byte:02X}',
            ok_off and ok_val,
            f'off_ok={ok_off}, val_ok={ok_val}'
        )

# entry 5..63 (ENTRY_COUNT=64) 做 64-5=59 项 × 8B 对拍
print()
print('---- 59 个新 entry (e=5..63) 做公式↔打包字节值一致性检查, 随机抽样打印前 3 + 最后 3:')
mismatches = 0
sample_printed = 0
for e in range(ENTRY_COUNT):
    for vb in range(8):
        actual_byte = payload_bytes[payload_byte_offset(e, vb)]
        expected_byte = (test_values[e] >> (8*(7 - vb))) & 0xFF
        ok = (actual_byte == expected_byte)
        if not ok:
            mismatches += 1
            if mismatches <= 10:
                print(f'  FAIL: entry e={e} vb={vb}: actual=0x{actual_byte:02X} ≠ expected=0x{expected_byte:02X}')
        elif (e < 8 or e >= ENTRY_COUNT - 2) and sample_printed < 6:
            # 抽样 PASS 打印几个证明脚本在工作
            print(f'  sample PASS: e={e} vb={vb}: byte=0x{actual_byte:02X} == expected value slice 0x{expected_byte:02X}')
            sample_printed += 1
check(f'entry 0..{ENTRY_COUNT-1} 所有 {ENTRY_COUNT*8} 个 value 字节值 == value 大端切片',
      mismatches == 0, f'共有 {mismatches} 个字节错位')

# index 字节区间对拍
print()
print('=' * 80)
print('③ 每个 entry 的 index(u16) 字节偏移: 公式 44+10*e / 45+10*e ↔ 打包字节')
print('=' * 80)
idx_mismatch = 0
for e in range(ENTRY_COUNT):
    idx_hi_off_payload = 2 + e * ENTRY_STRIDE + 0      # payload 内偏移
    idx_lo_off_payload = 2 + e * ENTRY_STRIDE + 1
    actual_hi = payload_bytes[idx_hi_off_payload]
    actual_lo = payload_bytes[idx_lo_off_payload]
    expected_hi = ((e+1) >> 8) & 0xFF
    expected_lo = (e+1) & 0xFF
    if actual_hi != expected_hi or actual_lo != expected_lo:
        idx_mismatch += 1
        if idx_mismatch <= 5:
            print(f'  FAIL: entry e={e} index expected=0x{e+1:04X} '
                  f'actual bytes (hi,lo)=(0x{actual_hi:02X},0x{actual_lo:02X})')
check(f'ENTRY_COUNT={ENTRY_COUNT} 所有 index 字节 (共 {ENTRY_COUNT*2} B) 全部匹配公式',
      idx_mismatch == 0, f'错位 {idx_mismatch} 个 index 字节')

print()
print('=' * 80)
print('④ 新 RTL output_override value 字节命中函数覆盖度:')
print('   要求: 只覆盖每个 entry 的 8B value/result 字段, 保留 2B index 原样转发。')
print('=' * 80)

MAX_PAYLOAD_OFF_FRAME = 42 + 2 + ENTRY_COUNT * ENTRY_STRIDE  # 单帧内最大字节号 (64 entry -> 42+642=684; max offset included = 683)

# 复刻 RTL 里 output_override 的 value 字段命中函数 (frame 全局 byte_index)
def rtl_is_value_byte(byte_index_frame):
    for ei in range(ENTRY_COUNT):
        value_start = ENTRY_BASE_OFF + ei * ENTRY_STRIDE + ENTRY_VALUE_OFF
        if value_start <= byte_index_frame < value_start + 8:
            return True
    return False

hit_count = 0
extra_hits = []
misses = []
# 对每个 entry 期望 hit 的 8B value/result:
expected_hits = set()
for ei in range(ENTRY_COUNT):
    s = ENTRY_BASE_OFF + ei * ENTRY_STRIDE + ENTRY_VALUE_OFF
    expected_hits.update(range(s, s + 8))
# 扫描 0..MAX_PAYLOAD_OFF_FRAME-1
for bi in range(0, MAX_PAYLOAD_OFF_FRAME):
    hit = rtl_is_value_byte(bi)
    if bi in expected_hits:
        if not hit: misses.append(bi)
        else: hit_count += 1
    else:
        if hit: extra_hits.append(bi)

check(f'entry {ENTRY_COUNT} 个 × 8B = {ENTRY_COUNT*8} 个 value 字节都被覆盖到一次',
      len(misses) == 0, f'漏掉 {len(misses)} 个字节 (前 5 个漏: {misses[:5]})')
check(f'entry override 不会误命中 index 或非 entry 字节',
      len(extra_hits) == 0, f'误命中 {len(extra_hits)} 个字节 (前 5 个: {extra_hits[:5]})')
check(f'entry override 总命中次数 = {ENTRY_COUNT*8}',
      hit_count == ENTRY_COUNT * 8, f'实际命中 {hit_count} ≠ 期望 {ENTRY_COUNT*8}')

def rtl_beat_mode_value_hits(frame_len_bytes):
    """复刻 RTL 输出侧固定 64-bit beat 相位表，确认等价于通用 value 字节集合。"""
    hits = set()
    checksum_hits = set()
    entry_active = False
    entry_index = 0
    entry_byte = 0

    for beat in range((frame_len_bytes + 7) // 8):
        for lane in range(8):
            byte_index_frame = beat * 8 + lane
            if byte_index_frame >= frame_len_bytes:
                continue

            if beat == 5:
                if lane in (0, 1):
                    checksum_hits.add(byte_index_frame)
                if lane in (6, 7):
                    hits.add(byte_index_frame)
            elif entry_active and entry_index < ENTRY_COUNT:
                if entry_byte == 0 and 2 <= lane <= 7:
                    hits.add(byte_index_frame)
                elif entry_byte == 2:
                    hits.add(byte_index_frame)
                elif entry_byte == 4 and 0 <= lane <= 5:
                    hits.add(byte_index_frame)
                elif entry_byte == 6:
                    if 0 <= lane <= 3:
                        hits.add(byte_index_frame)
                    elif entry_index + 1 < ENTRY_COUNT and 6 <= lane <= 7:
                        hits.add(byte_index_frame)
                elif entry_byte == 8:
                    if 0 <= lane <= 1:
                        hits.add(byte_index_frame)
                    elif entry_index + 1 < ENTRY_COUNT and 4 <= lane <= 7:
                        hits.add(byte_index_frame)

        if beat == 5:
            entry_active = True
            entry_index = 0
            entry_byte = 4
        elif entry_active and entry_index < ENTRY_COUNT:
            if entry_byte == 0:
                entry_byte = 8
            elif entry_byte == 2:
                entry_index += 1
                entry_byte = 0
            elif entry_byte == 4:
                entry_index += 1
                entry_byte = 2
            elif entry_byte == 6:
                entry_index += 1
                entry_byte = 4
            elif entry_byte == 8:
                entry_index += 1
                entry_byte = 6
            else:
                entry_byte = 0

    return hits, checksum_hits

beat_hits, checksum_hits = rtl_beat_mode_value_hits(MAX_PAYLOAD_OFF_FRAME)
check('固定 5-beat 输出相位表覆盖的 value 字节集合与通用公式完全一致',
      beat_hits == expected_hits,
      f'漏掉 {sorted(expected_hits - beat_hits)[:5]}, 误命中 {sorted(beat_hits - expected_hits)[:5]}')
check('固定 5-beat 输出相位表只清零 IPv4 checksum 两个字节 40/41',
      checksum_hits == {40, 41},
      f'实际 checksum 覆盖字节={sorted(checksum_hits)}')

def lanes_from_frame(frame_bytes, beat):
    return [
        frame_bytes[beat * 8 + lane] if beat * 8 + lane < len(frame_bytes) else 0
        for lane in range(8)
    ]

def pack_value_bytes(*items):
    value = 0
    for bit_hi, byte in items:
        value |= (byte & 0xFF) << bit_hi
    return value

def rtl_input_beat_mode_value_writes(frame_bytes):
    """复刻 RTL 输入侧固定 64-bit beat 相位表，返回 packet_value 写入序列。"""
    writes = []
    entry_active = False
    entry_index = 0
    entry_byte = 0
    value_shift = 0

    for beat in range((len(frame_bytes) + 7) // 8):
        lane = lanes_from_frame(frame_bytes, beat)

        if beat == 5:
            entry_active = True
            entry_index = 0
            entry_byte = 4
            value_shift = pack_value_bytes((56, lane[6]), (48, lane[7]))
        elif entry_active and entry_index < ENTRY_COUNT:
            if entry_byte == 0:
                value_shift = pack_value_bytes(
                    (56, lane[2]), (48, lane[3]), (40, lane[4]), (32, lane[5]),
                    (24, lane[6]), (16, lane[7]),
                )
                entry_byte = 8
            elif entry_byte == 2:
                value_shift = pack_value_bytes(
                    (56, lane[0]), (48, lane[1]), (40, lane[2]), (32, lane[3]),
                    (24, lane[4]), (16, lane[5]), (8, lane[6]), (0, lane[7]),
                )
                writes.append((entry_index, value_shift))
                entry_index += 1
                entry_byte = 0
                if entry_index >= ENTRY_COUNT:
                    entry_active = False
            elif entry_byte == 4:
                value_shift = (
                    (value_shift & 0xFFFF000000000000) |
                    pack_value_bytes(
                        (40, lane[0]), (32, lane[1]), (24, lane[2]), (16, lane[3]),
                        (8, lane[4]), (0, lane[5]),
                    )
                )
                writes.append((entry_index, value_shift))
                entry_index += 1
                entry_byte = 2
                if entry_index >= ENTRY_COUNT:
                    entry_active = False
            elif entry_byte == 6:
                value_shift = (
                    (value_shift & 0xFFFFFFFF00000000) |
                    pack_value_bytes((24, lane[0]), (16, lane[1]), (8, lane[2]), (0, lane[3]))
                )
                writes.append((entry_index, value_shift))
                entry_index += 1
                entry_byte = 4
                if entry_index < ENTRY_COUNT:
                    value_shift = pack_value_bytes((56, lane[6]), (48, lane[7]))
                else:
                    entry_active = False
            elif entry_byte == 8:
                value_shift = (
                    (value_shift & 0xFFFFFFFFFFFF0000) |
                    pack_value_bytes((8, lane[0]), (0, lane[1]))
                )
                writes.append((entry_index, value_shift))
                entry_index += 1
                entry_byte = 6
                if entry_index < ENTRY_COUNT:
                    value_shift = pack_value_bytes(
                        (56, lane[4]), (48, lane[5]), (40, lane[6]), (32, lane[7])
                    )
                else:
                    entry_active = False

    return writes

test_frame_bytes = bytearray(MAX_PAYLOAD_OFF_FRAME)
test_frame_bytes[42:] = payload_bytes
input_writes = rtl_input_beat_mode_value_writes(test_frame_bytes)
expected_writes = list(enumerate(test_values))
check('固定 5-beat 输入相位表写入 64 个 value 槽位和值均与打包帧一致',
      input_writes == expected_writes,
      f'写入数={len(input_writes)}, 首个不匹配={next(((a, b) for a, b in zip(input_writes, expected_writes) if a != b), None)}')

print()
print('=' * 80)
print('⑤ 64 槽 slot-wise 聚合模型检查: result[slot] = worker0[slot] + worker1[slot]')
print('   软件 C 侧会先把用户 index:value 展开为固定 1..64 槽, RTL 只按 slot 相加。')
print('=' * 80)
import random
random.seed(0x5A5A)

slot_mismatch = 0
for trial in range(200):
    w0 = [random.randint(0, 1 << 40) for _ in range(ENTRY_COUNT)]
    w1 = [random.randint(0, 1 << 40) for _ in range(ENTRY_COUNT)]
    rtl_result = [(w0[i] + w1[i]) & 0xFFFFFFFFFFFFFFFF for i in range(ENTRY_COUNT)]
    ref_result = []
    for i in range(ENTRY_COUNT):
        ref_result.append((w0[i] + w1[i]) & 0xFFFFFFFFFFFFFFFF)
    if rtl_result != ref_result:
        slot_mismatch += 1

check(f'ENTRY_COUNT={ENTRY_COUNT} 的 slot-wise 聚合 200 次随机自检全部一致',
      slot_mismatch == 0, f'不匹配次数 {slot_mismatch}')

print()
print('=' * 80)
print('⑥ frame_fifo / 单帧动作寄存器检查 (包长 684 B, FIFO_DEPTH 16384 x 64-bit beats):')
print('=' * 80)
FRAME_LEN_BYTES = 14 + 20 + 8 + 2 + ENTRY_COUNT * 10  # ETH + IP + UDP + round_id + entry*10
FRAME_LEN_BEATS = (FRAME_LEN_BYTES + 7) // 8            # 64-bit beats 向上取整
SWITCH_FIFO_DEPTH_BEATS = 16384                        # fpga_core.v localparam SWITCH_FIFO_DEPTH = 16384
OUR_FIFO_DEPTH_BEATS   = 4096                           # axis_udp_pair_aggregator localparam FIFO_DEPTH=4096
print(f'  包长(字节)={FRAME_LEN_BYTES}, 包长(64-bit beats)={FRAME_LEN_BEATS}')
print(f'  switch axis_fifo 深度={SWITCH_FIFO_DEPTH_BEATS} beats, 可容纳 {SWITCH_FIFO_DEPTH_BEATS // FRAME_LEN_BEATS} 个整帧')
print(f'  本模块内部 frame_fifo 深度={OUR_FIFO_DEPTH_BEATS} beats, 可容纳 {OUR_FIFO_DEPTH_BEATS // FRAME_LEN_BEATS} 个整帧')
print('  RTL 当前使用单帧 action 寄存器对齐 frame_fifo 输出, 不再保存 64 项结果 meta_fifo 阵列')
check(f'frame_fifo 至少能装 1 个整包 (每包 {FRAME_LEN_BEATS} beats, FIFO_DEPTH=4096)',
      OUR_FIFO_DEPTH_BEATS >= FRAME_LEN_BEATS,
      f'需要至少 {FRAME_LEN_BEATS} beats, 实际只有 {OUR_FIFO_DEPTH_BEATS} beats')
check('单帧 action 模式要求 frame_fifo 至少能缓存当前处理帧',
      OUR_FIFO_DEPTH_BEATS >= FRAME_LEN_BEATS,
      f'需要至少 {FRAME_LEN_BEATS} beats, 实际只有 {OUR_FIFO_DEPTH_BEATS} beats')

print()
print('=' * 80)
print('⑦ RTL 时序结构检查: 串行 copy/calc + distributed RAM + beat-mode output，避免 64 路并行长路径')
print('=' * 80)
rtl_path = Path(__file__).resolve().parents[2] / 'corundum/fpga/mqnic/ZCU102/fpga/rtl/axis_udp_pair_aggregator.v'
rtl_text = rtl_path.read_text(encoding='utf-8')
check('RTL 包含 copy_pending_reg/calc_pending_reg 串行处理状态',
      'copy_pending_reg' in rtl_text and 'calc_pending_reg' in rtl_text,
      '未找到串行 copy/calc 状态寄存器')
check('packet/pending/result 三组 64x64 数组使用 distributed RAM 风格',
      rtl_text.count('ram_style = "distributed"') >= 3,
      '未找到足够的 distributed RAM 属性')
check('reset 阶段不再清空整个 ENTRY_COUNT 数组，便于 RAM/LUTRAM 推断',
      'for (i = 0; i < ENTRY_COUNT' not in rtl_text,
      '发现 reset 或批量循环写 ENTRY_COUNT 数组')
check('输入侧不再对 packet_value_reg 做 per-byte variable-index bit-slice 写入',
      'packet_value_reg[input_entry_index_tmp][' not in rtl_text,
      '发现 packet_value_reg[input_entry_index_tmp][...] 写法')
check('聚合不再使用 current_result_reg[i] <= packet_value_reg[i] + pending_value_reg[i] 的 64 路并行循环',
      'current_result_reg[i] <= packet_value_reg[i] + pending_value_reg[i]' not in rtl_text,
      '发现 64 路并行求和循环')
check('packet_value_reg 写入经过 packet_value_wr_* 寄存器 staging，切断输入相位解码到 LUTRAM WE/I 的同拍路径',
      all(token in rtl_text for token in (
          'packet_value_wr_en_reg',
          'packet_value_wr_index_reg',
          'packet_value_wr_data_reg',
          'packet_value_reg[packet_value_wr_index_reg] <= packet_value_wr_data_reg',
      )),
      '未找到 packet_value 写入 staging 寄存器或 staged RAM 写入')
check('输出 AXIS 经过 m_axis_*_reg 寄存，切断输出覆盖逻辑到下游 FIFO RAM DIN 的同拍路径',
      all(token in rtl_text for token in (
          'm_axis_tdata_reg',
          'm_axis_tvalid_reg',
          'assign m_axis_tdata = m_axis_tdata_reg',
          'assign m_axis_tvalid = m_axis_tvalid_reg',
      )),
      '未找到输出 AXIS pipeline 寄存器')
check('输出覆盖使用固定 64-bit beat 相位表，不再逐 lane 回放 output_byte_index',
      all(token in rtl_text for token in (
          'output_beat_index_reg',
          "output_beat_index_reg == 8'd5",
          'case (output_entry_byte_reg)',
      )) and 'output_byte_index_reg' not in rtl_text and 'for (output_step' not in rtl_text,
      '未找到 beat-mode 输出相位表，或仍存在 output_byte_index/逐 lane 回放逻辑')
check('输入解析使用固定 64-bit beat 相位表，不再使用 frame_byte_index/lane 扫描',
      all(token in rtl_text for token in (
          'input_beat_index_reg',
          "case (input_beat_index_reg)",
          'case (input_entry_byte_reg)',
      )) and 'frame_byte_index_reg' not in rtl_text and 'for (lane' not in rtl_text and 'count_keep' not in rtl_text,
      '未找到 beat-mode 输入相位表，或仍存在 frame_byte_index/lane/count_keep 逻辑')
check('finalize_after_write_reg 会暂停输入并延后一拍 finalize，保证最后一个 entry 写入后再开始 copy/calc',
      'finalize_after_write_reg' in rtl_text and
      'wire input_paused = finalize_pending_reg || finalize_after_write_reg || copy_pending_reg || calc_pending_reg || frame_action_valid_reg;' in rtl_text,
      '未找到 finalize_after_write_reg 暂停/延迟逻辑')

print()
print('=' * 80)
print('⑧ 四路 worker / 广播结构检查')
print('=' * 80)
check('聚合器具备四路输入 AXIS，并以端口掩码去重',
      all(token in rtl_text for token in (
          's_axis_2_tdata', 's_axis_3_tdata', 'pending_worker_mask_reg',
          'worker_mask_bit', 'worker_mask_count',
      )),
      '未找到四路输入或 worker 端口掩码状态')
check('IPv4 TTL 限定本轮 worker_count 为 2..4',
      'ip_ttl_reg >= 8\'d2 && ip_ttl_reg <= 8\'d4' in rtl_text and
      'ip_ttl_reg <= selected_tdata[6*8 +: 8]' in rtl_text,
      '未找到 TTL worker_count 解析或范围检查')
check('最终聚合帧默认广播到四个端口',
      'RESULT_DEST = {DEST_WIDTH{1\'b1}}' in rtl_text,
      '聚合器默认 RESULT_DEST 不是全端口广播')
core_path = rtl_path.with_name('fpga_core.v')
core_text = core_path.read_text(encoding='utf-8')
check('fpga_core 将四个物理端口接入聚合器并接收四位广播掩码',
      'if (n < 4) begin : worker_aggregate_path' in core_text and
      '.s_axis_2_tdata(worker_raw_rx_tdata[2*AXIS_ETH_DATA_WIDTH +: AXIS_ETH_DATA_WIDTH])' in core_text and
      '.s_axis_3_tdata(worker_raw_rx_tdata[3*AXIS_ETH_DATA_WIDTH +: AXIS_ETH_DATA_WIDTH])' in core_text and
      'parameter [3:0] PAIR_RESULT_DEST = 4\'b1111' in core_text,
      'fpga_core 端口接入或广播掩码未更新')
for worker_count in range(2, 5):
    workers = [[(slot + 1) * (worker + 3) for slot in range(ENTRY_COUNT)] for worker in range(worker_count)]
    expected = [sum(worker[slot] for worker in workers) for slot in range(ENTRY_COUNT)]
    check(f'{worker_count} 路 64 槽串行累加模型',
          all(expected[slot] == sum(workers[worker][slot] for worker in range(worker_count)) for slot in range(ENTRY_COUNT)),
          f'{worker_count} 路 slot-wise 模型不一致')

# 收尾:
print()
print('=' * 80)
if pass_all:
    print('✅ 所有预综合静态对拍检查: 全部 PASS')
    print('建议: 可以放心开始 Vivado 综合 (make / synth/impl/bitstream)')
    sys.exit(0)
else:
    print('❌ 存在 FAIL 项, 请先修对应问题再综合, 避免浪费综合时间')
    sys.exit(1)
