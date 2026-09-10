#ifndef DPDK_PORT_H
#define DPDK_PORT_H
#include <rte_ethdev.h>
#include "bridge_protocol.h"
typedef struct {
    uint16_t id;
    struct rte_mempool *pool;
    uint64_t tx_packets, rx_packets, tx_short, rx_invalid;
} DpdkPort;
int dpdk_open(DpdkPort *p, const char *const bdfs[4], int rank, int cpu, int require_link);
void dpdk_close(DpdkPort *p);
#endif
