#define main bridge_program_main
#include "mpi_fpga_bridge.c"
#undef main

#include <assert.h>

int main(void) {
  BridgeConfig cfg = {0};
  cfg.fpga_dport = 0x2345;
  cfg.fpga_sport[0] = 4000;
  cfg.worker_src_ip[0] = 0xc0a80a01;
  cfg.worker_dst_ip[0] = 0xc0a81401;
  memset(cfg.fpga_dst_mac, 0xff, sizeof(cfg.fpga_dst_mac));

  FpgaBatch input = {0}, output = {0};
  input.count = FPGA_BATCH_MAX_ROUNDS;
  for (unsigned r = 0; r < input.count; r++) {
    input.round[r] = 0x9000 + r;
    for (unsigned i = 0; i < BRIDGE_MAX_ENTRIES; i++)
      input.entry[r].value[i] = (uint64_t)r * 1000 + i;
  }

  uint8_t frame[FPGA_MAX_FRAME_LEN];
  size_t frame_len = 0;
  assert(build_batch_frame(frame, &frame_len, &cfg, 0, 4, &input) == 0);
  assert(frame_len == 8368);
  assert(frame[42] == 0xa4 && frame[43] == 0x16);
  assert(frame[44] == 4 && frame[45] == 16);
  assert(frame[48] == 0x90 && frame[49] == 0x00);
  assert(ntohs(((Ipv4Header *)(frame + 14))->total_length) == 8354);
  assert(ntohs(((UdpHeader *)(frame + 34))->length) == 8334);

  RawPort port = {0};
  assert(parse_batch_result(&port, frame, frame_len, NULL, cfg.fpga_dport,
                            &output) == 1);
  assert(output.count == input.count);
  for (unsigned r = 0; r < input.count; r++) {
    assert(output.round[r] == input.round[r]);
    for (unsigned i = 0; i < BRIDGE_MAX_ENTRIES; i++)
      assert(output.entry[r].value[i] == input.entry[r].value[i]);
  }

  input.count = 1;
  assert(build_batch_frame(frame, &frame_len, &cfg, 0, 2, &input) == 0);
  assert(frame_len == 568);
  frame[42] ^= 1;
  assert(parse_batch_result(&port, frame, frame_len, NULL, cfg.fpga_dport,
                            &output) == 0);
  puts("PASS: v4 1/16-round JumboFrame build and parse");
  return 0;
}
