#ifndef BRIDGE_PROTOCOL_H
#define BRIDGE_PROTOCOL_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <arpa/inet.h>

#define APP_MAGIC 0x4d504247u
#define PROTOCOL_VERSION 4
#define FPGA_MAGIC 0x4147u
#define ENTRIES 64
#define MAX_ROUNDS 16
#define ROUND_SLOTS 256
#define MAX_WINDOW (ROUND_SLOTS / MAX_ROUNDS)
#define FPGA_DPORT 0x2345
#define JUMBO_MTU 9000
#define APP_HEADER 24
#define FPGA_HEADER 48
#define ROUND_BYTES 520
#define MAX_FRAME (FPGA_HEADER + MAX_ROUNDS * ROUND_BYTES)
#define MAX_APP (APP_HEADER + 4 * MAX_ROUNDS * ENTRIES * 8)
#define MSG_REQUEST 1
#define MSG_RESPONSE 2
#define MSG_STOP 3
#define STATUS_OK 0
#define STATUS_INVALID 1
#define STATUS_TIMEOUT 6
#define STATUS_SEND 7
#define STATUS_MISMATCH 13

static inline uint16_t get16(const void *p) {
    const uint8_t *b = p; return (uint16_t)b[0] << 8 | b[1];
}
static inline uint32_t get32(const void *p) {
    const uint8_t *b = p; return (uint32_t)get16(b) << 16 | get16(b + 2);
}
static inline uint64_t get64(const void *p) {
    const uint8_t *b = p; return (uint64_t)get32(b) << 32 | get32(b + 4);
}
static inline void put16(void *p, uint16_t v) {
    uint8_t *b = p; b[0] = v >> 8; b[1] = v;
}
static inline void put32(void *p, uint32_t v) {
    uint8_t *b = p; put16(b, v >> 16); put16(b + 2, v);
}
static inline void put64(void *p, uint64_t v) {
    uint8_t *b = p; put32(b, v >> 32); put32(b + 4, v);
}
static inline size_t app_size(unsigned rounds, unsigned workers, int response) {
    return APP_HEADER + (size_t)rounds * ENTRIES * 8 * (response ? 1 : workers);
}
/* App header: magic:u32, version/type:u16, base_round:u32,
 * rounds/workers/entries/status:u16, reserved:u32. Worker-major values follow. */
static inline int app_valid(const uint8_t *b, size_t n, int response) {
    if (n < APP_HEADER || get32(b) != APP_MAGIC || get16(b+4) != 4 ||
        get16(b+6) != (response ? MSG_RESPONSE : MSG_REQUEST) ||
        (get32(b+8) & 15) || !get16(b+12) || get16(b+12) > MAX_ROUNDS ||
        get16(b+14) < 2 || get16(b+14) > 4 || get16(b+16) != ENTRIES ||
        get32(b+20) || (!response && get16(b+18))) return 0;
    return n == app_size(get16(b+12), get16(b+14), response);
}
static inline void app_header(uint8_t *b, unsigned type, uint32_t base,
                              unsigned rounds, unsigned workers, unsigned status) {
    memset(b, 0, APP_HEADER); put32(b, APP_MAGIC); put16(b+4, 4);
    put16(b+6, type); put32(b+8, base); put16(b+12, rounds);
    put16(b+14, workers); put16(b+16, ENTRIES); put16(b+18, status);
}
static inline uint16_t ip_checksum(const uint8_t *b) {
    uint32_t s = 0;
    for (unsigned i=0; i<20; i+=2) s += get16(b+i);
    while (s >> 16) s = (s & 65535) + (s >> 16);
    return ~s;
}
/* The six-byte aggregation header aligns every 520-byte round to an AXIS beat.
 * Round IDs are explicit, sequential, and base-aligned to 16. Values are u64 BE. */
static inline size_t fpga_frame(uint8_t *b, const uint8_t *app, unsigned worker,
                                const uint8_t dst[6], uint32_t src, uint32_t dest) {
    unsigned rounds = get16(app+12), workers = get16(app+14);
    size_t len = FPGA_HEADER + rounds * ROUND_BYTES;
    memset(b, 0, FPGA_HEADER); memcpy(b, dst, 6);
    b[6]=2; b[11]=1; put16(b+12, 0x0800); b[14]=0x45;
    put16(b+16, len-14); put16(b+20, 0x4000); b[22]=64; b[23]=17;
    put32(b+26, src); put32(b+30, dest); put16(b+24, ip_checksum(b+14));
    put16(b+34, 4000+worker); put16(b+36, FPGA_DPORT); put16(b+38, len-34);
    put16(b+42, FPGA_MAGIC); b[44]=4; b[45]=workers; b[46]=rounds; b[47]=MSG_REQUEST;
    const uint8_t *values = app + APP_HEADER + worker * rounds * ENTRIES * 8;
    for (unsigned r=0; r<rounds; r++) {
        uint8_t *p=b+FPGA_HEADER+r*ROUND_BYTES;
        put32(p, get32(app+8)+r); put32(p+4, 0);
        memcpy(p+8, values+r*ENTRIES*8, ENTRIES*8);
    }
    return len;
}
static inline int fpga_result_view(const uint8_t *b, size_t n,
                               uint32_t *base, unsigned *rounds, unsigned *workers) {
    if (n < FPGA_HEADER + ROUND_BYTES || get16(b+12)!=0x800 || b[14]!=0x45 ||
        b[23]!=17 || (get16(b+20)&0x3fff) || get16(b+36)!=FPGA_DPORT ||
        get16(b+42)!=FPGA_MAGIC || b[44]!=4 || b[47]!=MSG_RESPONSE ||
        b[45]<2 || b[45]>4 || !b[46] || b[46]>MAX_ROUNDS) return 0;
    *rounds=b[46]; *workers=b[45]; *base=get32(b+48);
    if ((*base&15) || n != FPGA_HEADER + *rounds*ROUND_BYTES ||
        get16(b+16)!=n-14 || get16(b+38)!=n-34 || ip_checksum(b+14)) return 0;
    for (unsigned r=0; r<*rounds; r++) {
        const uint8_t *p=b+FPGA_HEADER+r*ROUND_BYTES;
        if (get32(p)!=*base+r || get32(p+4)) return 0;
    }
    return 1;
}
static inline int fpga_result(const uint8_t *b, size_t n, uint8_t *values,
                               uint32_t *base, unsigned *rounds, unsigned *workers) {
    if(!fpga_result_view(b,n,base,rounds,workers)) return 0;
    for(unsigned r=0;r<*rounds;r++)
        memcpy(values+r*ENTRIES*8,b+FPGA_HEADER+r*ROUND_BYTES+8,ENTRIES*8);
    return 1;
}
#endif
