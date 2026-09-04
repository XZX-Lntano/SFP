# 16-round jumbo aggregation protocol (v4)

This protocol replaces the v3 one-round FPGA payload.  It is intentionally
incompatible with the old bitstream and bridge.

## Physical frame

Each worker sends one IPv4/UDP jumbo Ethernet frame.  A batch may contain
from 1 to 16 rounds, so latency-sensitive functional requests do not need to
wait for a full batch:

```text
Ethernet (14 B) + IPv4 (20 B) + UDP (8 B) + batch payload
```

The UDP payload starts with a six-byte header and is followed by
`round_count` records in packet order:

```text
batch header = magic (u16=0xa416) + version (u8=4) + round_count (u8=1..16)
             + flags (u16=0)
record[n] = round_id (u16, big endian) + reserved (6 B, zero)
          + value[0..63] (64 x u64, big endian)
record size = 8 + 64*8 = 520 B
maximum UDP payload size = 6 + 16*520 = 8326 B
maximum Ethernet frame size (without FCS) = 8368 B
```

The six-byte batch header makes the first record start at Ethernet byte 48;
the eight-byte record header keeps every 64-bit value aligned to an AXI beat.
`round_id % 16` selects the hardware BRAM slot.  The RTL stores the full
round ID in slot metadata and rejects a collision with a live, different
round ID.  All workers in a batch carry identical round IDs and ordering.
The IPv4 TTL remains the worker count (2, 3 or 4).

## Application batching

The application protocol remains one request per round.  Rank 0 collects up
to 16 requests from the application's sliding window and coalesces them into
one physical jumbo frame per worker.  This avoids a fragmented ~41 KiB
application UDP datagram and remains backward compatible with existing
application clients.  Each valid round represents 512 B useful output.
For successful v3 application responses, the existing 16-bit `reserved` field
reports the physical batch size; clients that ignore this field remain fully
compatible.  The benchmark uses it to account for the six-byte batch header
without assuming that every physical frame contains all 16 rounds.
