#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_packet.h>
#include <mpi.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
#include <unistd.h>
#include <sched.h>

#ifdef USE_DPDK
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#endif

#define BRIDGE_MAGIC 0x4d504247u
#define BRIDGE_VERSION 3u
#define APP_MSG_REQUEST 1u
#define APP_MSG_RESPONSE 2u
#define APP_MSG_STOP 3u
#define APP_MSG_ERROR 4u
#define INTERNAL_MSG_NOOP 255u
#define BRIDGE_STATUS_OK 0u
#define BRIDGE_STATUS_BAD_MAGIC 1u
#define BRIDGE_STATUS_BAD_VERSION 2u
#define BRIDGE_STATUS_BAD_TYPE 3u
#define BRIDGE_STATUS_BAD_ENTRY_COUNT 4u
#define BRIDGE_STATUS_RECV_FAILED 5u
#define BRIDGE_STATUS_FPGA_TIMEOUT 6u
#define BRIDGE_STATUS_FPGA_SEND_FAILED 7u
#define BRIDGE_STATUS_BAD_WORKER_LAYOUT 8u
#define BRIDGE_STATUS_BAD_INDEX_RANGE 9u
#define BRIDGE_STATUS_DUPLICATE_INDEX 10u
#define BRIDGE_STATUS_FPGA_BAD_RESULT 11u
#define BRIDGE_STATUS_BAD_WORKER_COUNT 12u
#define BRIDGE_STATUS_RESULT_MISMATCH 13u
#define FPGA_ETHERTYPE_IPV4 0x0800u
#define FPGA_IP_PROTO_UDP 17u
#define BRIDGE_MAX_ENTRIES 64u
#define BRIDGE_MAX_WORKERS 4u
#define FPGA_BATCH_MAGIC 0xa416u
#define FPGA_BATCH_VERSION 4u
#define FPGA_BATCH_MAX_ROUNDS 16u
#define FPGA_BATCH_HEADER_LEN 6u
#define FPGA_ROUND_HEADER_LEN 8u
#define FPGA_ROUND_RECORD_LEN (FPGA_ROUND_HEADER_LEN + BRIDGE_MAX_ENTRIES * sizeof(uint64_t))
#define FPGA_BATCH_PAYLOAD_LEN(n) (FPGA_BATCH_HEADER_LEN + (n) * FPGA_ROUND_RECORD_LEN)
#define FPGA_MAX_FRAME_LEN 9216u
#define FPGA_DEFAULT_DPORT 0x2345u
#define DEFAULT_FPGA_TIMEOUT_MS 5000

/* FPGA reflects the request Ethernet header; avoid using any host-port MAC. */
static const uint8_t fpga_request_src_mac[6] = {0x02, 0x00, 0x00,
                                                 0x00, 0x00, 0x01};

#pragma pack(push, 1)
typedef uint64_t BridgeEntry;

typedef struct {
  uint32_t magic;
  uint16_t version, msg_type;
  uint32_t request_id;
  uint16_t entry_count, worker_count, status, reserved;
  BridgeEntry worker_entries[BRIDGE_MAX_WORKERS][BRIDGE_MAX_ENTRIES];
  BridgeEntry result_entries[BRIDGE_MAX_ENTRIES];
} BridgeMessage;

typedef struct {
  uint8_t dst[6], src[6];
  uint16_t ethertype;
} EthernetHeader;

typedef struct {
  uint8_t version_ihl, dscp_ecn;
  uint16_t total_length, identification, flags_fragment;
  uint8_t ttl, protocol;
  uint16_t checksum;
  uint32_t src_ip, dst_ip;
} Ipv4Header;

typedef struct {
  uint16_t src_port, dst_port, length, checksum;
} UdpHeader;

#pragma pack(pop)

typedef struct {
  int fd, ifindex;
  int timeout_ms;
  char ifname[IFNAMSIZ];
  uint8_t src_mac[6];
  uint64_t rx_total;
  uint64_t rx_outgoing;
  uint64_t rx_rejected;
  uint64_t rx_other_round;
#ifdef USE_DPDK
  int use_dpdk;
  uint16_t dpdk_port_id;
  struct rte_mempool *dpdk_pool;
#endif
} RawPort;

typedef struct {
  uint16_t index[BRIDGE_MAX_ENTRIES];
  uint64_t value[BRIDGE_MAX_ENTRIES];
} EntryArray;

typedef struct {
  uint16_t count;
  uint16_t round[FPGA_BATCH_MAX_ROUNDS];
  EntryArray entry[FPGA_BATCH_MAX_ROUNDS];
} FpgaBatch;

typedef struct {
  uint16_t app_port, fpga_dport, fpga_sport[BRIDGE_MAX_WORKERS];
  int fpga_timeout_ms;
  uint8_t fpga_dst_mac[6];
  char worker_iface[BRIDGE_MAX_WORKERS][IFNAMSIZ];
  uint32_t worker_src_ip[BRIDGE_MAX_WORKERS], worker_dst_ip[BRIDGE_MAX_WORKERS];
} BridgeConfig;

static uint64_t hton64(uint64_t v) {
  return ((uint64_t)htonl((uint32_t)v) << 32) | htonl((uint32_t)(v >> 32));
}

static uint64_t ntoh64(uint64_t v) {
  return ((uint64_t)ntohl((uint32_t)v) << 32) | ntohl((uint32_t)(v >> 32));
}

static uint16_t ipv4_checksum(const void *data, size_t len) {
  const uint8_t *p = data;
  uint32_t s = 0;
  for (size_t i = 0; i + 1 < len; i += 2)
    s += ((uint16_t)p[i] << 8) | p[i + 1];
  if (len & 1)
    s += (uint16_t)p[len - 1] << 8;
  while (s >> 16)
    s = (s & 0xffffu) + (s >> 16);
  return (uint16_t)~s;
}

static int parse_mac(const char *s, uint8_t mac[6]) {
  unsigned x[6];
  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &x[0], &x[1], &x[2], &x[3], &x[4],&x[5]) != 6)
    return -1;
  for (int i = 0; i < 6; i++) {
    if (x[i] > 255)
      return -1;
    mac[i] = x[i];
  }
  return 0;
}

static int parse_ip(const char *s, uint32_t *out) {
  struct in_addr a;
  if (inet_pton(AF_INET, s, &a) != 1)
    return -1;
  *out = ntohl(a.s_addr);
  return 0;
}

static int parse_u16(const char *s, uint16_t *out) {
  char *e;
  unsigned long v = strtoul(s, &e, 0);
  if (!s[0] || *e || v > 65535)
    return -1;
  *out = v;
  return 0;
}

static int parse_int(const char *s, int *out) {
  char *e;
  long v = strtol(s, &e, 0);
  if (!s[0] || *e || v < 1 || v > 60000)
    return -1;
  *out = v;
  return 0;
}

static int bind_rank_cpu(int rank) {
  const char *base_text = getenv("MPI_FPGA_CPU_BASE");
  if (!base_text)
    return 0;
  char *end;
  long base = strtol(base_text, &end, 10);
  if (!base_text[0] || *end || base < 0 || base + 3 >= CPU_SETSIZE)
    return -1;
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET((int)base + rank, &set);
  return sched_setaffinity(0, sizeof(set), &set);
}

static void bridge_host_to_network(BridgeMessage *m) {

  m->magic = htonl(m->magic);
  m->version = htons(m->version);
  m->msg_type = htons(m->msg_type);
  m->request_id = htonl(m->request_id);
  m->entry_count = htons(m->entry_count);
  m->worker_count = htons(m->worker_count);
  m->status = htons(m->status);
  m->reserved = htons(m->reserved);

  for (unsigned w = 0; w < 4; w++)
    for (unsigned i = 0; i < 64; i++)
      m->worker_entries[w][i] = hton64(m->worker_entries[w][i]);

  for (unsigned i = 0; i < 64; i++) {
    m->result_entries[i] = hton64(m->result_entries[i]);
  }
}

static void bridge_network_to_host(BridgeMessage *m) {

  m->magic = ntohl(m->magic);
  m->version = ntohs(m->version);
  m->msg_type = ntohs(m->msg_type);
  m->request_id = ntohl(m->request_id);
  m->entry_count = ntohs(m->entry_count);
  m->worker_count = ntohs(m->worker_count);
  m->status = ntohs(m->status);
  m->reserved = ntohs(m->reserved);

  for (unsigned w = 0; w < 4; w++)
    for (unsigned i = 0; i < 64; i++) {
      m->worker_entries[w][i] = ntoh64(m->worker_entries[w][i]);
    }

  for (unsigned i = 0; i < 64; i++) {
    m->result_entries[i] = ntoh64(m->result_entries[i]);
  }
}

static int validate_message(const BridgeMessage *m) {

  if (m->magic != BRIDGE_MAGIC)
    return BRIDGE_STATUS_BAD_MAGIC;

  if (m->version != BRIDGE_VERSION)
    return BRIDGE_STATUS_BAD_VERSION;

  if (m->msg_type != APP_MSG_REQUEST && m->msg_type != APP_MSG_STOP)
    return BRIDGE_STATUS_BAD_TYPE;

  if (!m->entry_count || m->entry_count > 64)
    return BRIDGE_STATUS_BAD_ENTRY_COUNT;

  if (m->worker_count < 2 || m->worker_count > 4)
    return BRIDGE_STATUS_BAD_WORKER_COUNT;

  if (m->msg_type == APP_MSG_STOP)
    return 0;

  return 0;
}

static void error_response(const BridgeMessage *req, BridgeMessage *res,uint16_t status) {

  memset(res, 0, sizeof(*res));
  res->magic = BRIDGE_MAGIC;
  res->version = BRIDGE_VERSION;
  res->msg_type = APP_MSG_ERROR;
  res->request_id = req->request_id;
  res->entry_count = req->entry_count;
  res->worker_count = req->worker_count;
  res->status = status;
  memcpy(res->worker_entries, req->worker_entries, sizeof(res->worker_entries));
}

static int open_udp_server(uint16_t port) {

  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  struct sockaddr_in a = {.sin_family = AF_INET,
                          .sin_addr.s_addr = htonl(INADDR_ANY),
                          .sin_port = htons(port)};
  if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

#ifdef USE_DPDK
static const char *dpdk_pci_for_iface(const char *ifname) {
  if (!strcmp(ifname, "enp1s0f0np0")) return "0000:01:00.0";
  if (!strcmp(ifname, "enp1s0f1np1")) return "0000:01:00.1";
  if (!strcmp(ifname, "enp1s0f2np2")) return "0000:01:00.2";
  if (!strcmp(ifname, "enp1s0f3np3")) return "0000:01:00.3";
  return NULL;
}

static int init_dpdk_owner(RawPort ports[BRIDGE_MAX_WORKERS], const BridgeConfig *cfg) {
  const char *pci[BRIDGE_MAX_WORKERS];
  for (unsigned w = 0; w < BRIDGE_MAX_WORKERS; w++)
    if (!(pci[w] = dpdk_pci_for_iface(cfg->worker_iface[w]))) return -1;
  char lcore[] = "-l";
  char core[16];
  char memory[] = "-n";
  char channels[] = "4";
  char in_memory[] = "--in-memory";
  char file_prefix_arg[] = "--file-prefix=fpga_owner";
  snprintf(core, sizeof(core), "%d", sched_getcpu());
  char *eal_argv[] = {"mpi_fpga_bridge", lcore, core, memory, channels,
                      "-a", (char *)pci[0], "-a", (char *)pci[1],
                      "-a", (char *)pci[2], "-a", (char *)pci[3],
                      file_prefix_arg, in_memory};
  if (rte_eal_init((int)(sizeof(eal_argv)/sizeof(eal_argv[0])), eal_argv) < 0) {
    fprintf(stderr, "DPDK EAL init failed: %s\n", rte_strerror(rte_errno));
    return -1;
  }
  for (unsigned w = 0; w < BRIDGE_MAX_WORKERS; w++) {
    RawPort *p = &ports[w];
    memset(p, 0, sizeof(*p)); p->fd = -1; p->timeout_ms = cfg->fpga_timeout_ms;
    snprintf(p->ifname, sizeof(p->ifname), "%s", cfg->worker_iface[w]);
    if (rte_eth_dev_get_port_by_name(pci[w], &p->dpdk_port_id)) goto eth_error;
    char pool_name[32]; snprintf(pool_name, sizeof(pool_name), "fpga_pool_%u", w);
    p->dpdk_pool = rte_pktmbuf_pool_create(pool_name, 8192, 256, 0, 10240, rte_socket_id());
    if (!p->dpdk_pool) goto eth_error;
    struct rte_eth_conf conf = {0}; uint16_t rx_desc = 1024, tx_desc = 1024;
    if (rte_eth_dev_configure(p->dpdk_port_id, 1, 1, &conf) ||
        rte_eth_dev_set_mtu(p->dpdk_port_id, 9000) ||
        rte_eth_dev_adjust_nb_rx_tx_desc(p->dpdk_port_id, &rx_desc, &tx_desc) ||
        rte_eth_rx_queue_setup(p->dpdk_port_id, 0, rx_desc, rte_eth_dev_socket_id(p->dpdk_port_id), NULL, p->dpdk_pool) ||
        rte_eth_tx_queue_setup(p->dpdk_port_id, 0, tx_desc, rte_eth_dev_socket_id(p->dpdk_port_id), NULL) ||
        rte_eth_dev_start(p->dpdk_port_id)) goto eth_error;
    rte_eth_promiscuous_enable(p->dpdk_port_id); p->use_dpdk = 1;
  }
  return 0;
eth_error:
  fprintf(stderr, "DPDK Ethernet setup failed: %s\n", rte_strerror(rte_errno)); return -1;
}

static int dpdk_send_frame(RawPort *p, const uint8_t *frame, size_t len) {
  struct rte_mbuf *m = rte_pktmbuf_alloc(p->dpdk_pool);
  if (!m) return -1;
  char *dst = rte_pktmbuf_append(m, len);
  if (!dst) { rte_pktmbuf_free(m); return -1; }
  memcpy(dst, frame, len);
  if (rte_eth_tx_burst(p->dpdk_port_id, 0, &m, 1) != 1) {
    rte_pktmbuf_free(m);
    return -1;
  }
  return 0;
}
#endif

static int init_raw_port(RawPort *p, const char *name, int timeout) {

  memset(p, 0, sizeof(*p));
  p->fd = -1;
  p->timeout_ms = timeout;
  snprintf(p->ifname, sizeof(p->ifname), "%s", name);
#ifdef USE_DPDK
  (void)name;
#endif
  p->fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL));
  if (p->fd < 0)
    return -1;
  p->ifindex = if_nametoindex(name);
  if (!p->ifindex)
    goto fail;

  struct ifreq ifr = {0};
  snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", name);
  if (ioctl(p->fd, SIOCGIFHWADDR, &ifr) < 0)
    goto fail;
  memcpy(p->src_mac, ifr.ifr_hwaddr.sa_data, 6);

  struct sockaddr_ll a = {.sll_family = AF_PACKET,
                          .sll_protocol = htons(ETH_P_ALL),
                          .sll_ifindex = p->ifindex};
  if (bind(p->fd, (struct sockaddr *)&a, sizeof(a)) < 0)
    goto fail;

  struct packet_mreq mr = {.mr_ifindex = p->ifindex,
                           .mr_type = PACKET_MR_PROMISC};
  if (setsockopt(p->fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr,
                 sizeof(mr)) < 0)
    goto fail;

  int rcvbuf = 1 << 20;
  setsockopt(p->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

#ifdef PACKET_IGNORE_OUTGOING
  int ignore_outgoing = 1;
  setsockopt(p->fd, SOL_PACKET, PACKET_IGNORE_OUTGOING, &ignore_outgoing,
             sizeof(ignore_outgoing));
#endif

  struct timeval tv = {.tv_sec = timeout / 1000,
                       .tv_usec = (timeout % 1000) * 1000};
  setsockopt(p->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return 0;
fail:
  close(p->fd);
  p->fd = -1;
  return -1;
}

static void close_raw_port(RawPort *p) {
#ifdef USE_DPDK
  if (p->use_dpdk) {
    rte_eth_dev_stop(p->dpdk_port_id);
    rte_eth_dev_close(p->dpdk_port_id);
    return;
  }
#endif
  if (p->fd >= 0)
    close(p->fd);
  p->fd = -1;
}

static const char *raw_port_error(void) {
#ifdef USE_DPDK
  if (getenv("MPI_FPGA_DPDK"))
    return rte_strerror(rte_errno);
#endif
  return strerror(errno);
}

static void load_entries(const BridgeEntry *in, uint16_t n, EntryArray *out) {
  memset(out, 0, sizeof(*out));
  for (unsigned slot = 0; slot < 64; slot++) {
    out->index[slot] = slot + 1;
    if (slot < n)
      out->value[slot] = in[slot];
  }
}

static int build_batch_frame(uint8_t frame[FPGA_MAX_FRAME_LEN], size_t *frame_len,
                             const BridgeConfig *c, unsigned w,
                             uint8_t workers, const FpgaBatch *batch) {
  memset(frame, 0, FPGA_MAX_FRAME_LEN);
  EthernetHeader *eth = (EthernetHeader *)frame;
  Ipv4Header *ip = (Ipv4Header *)(frame + 14);
  UdpHeader *udp = (UdpHeader *)(frame + 34);
  uint8_t *payload = frame + 42;
  size_t payload_len = FPGA_BATCH_PAYLOAD_LEN(batch->count);

  if (!batch->count || batch->count > FPGA_BATCH_MAX_ROUNDS ||
      42 + payload_len > FPGA_MAX_FRAME_LEN) {
    errno = EMSGSIZE;
    return -1;
  }
  memcpy(eth->dst, c->fpga_dst_mac, 6);
  memcpy(eth->src, fpga_request_src_mac, sizeof(eth->src));
  eth->ethertype = htons(FPGA_ETHERTYPE_IPV4);
  ip->version_ihl = 0x45;
  ip->ttl = workers;
  ip->protocol = FPGA_IP_PROTO_UDP;
  ip->total_length = htons(20 + 8 + payload_len);
  ip->src_ip = htonl(c->worker_src_ip[w]);
  ip->dst_ip = htonl(c->worker_dst_ip[w]);
  ip->checksum = htons(ipv4_checksum(ip, sizeof(*ip)));
  udp->src_port = htons(c->fpga_sport[w]);
  udp->dst_port = htons(c->fpga_dport);
  udp->length = htons(8 + payload_len);
  payload[0] = FPGA_BATCH_MAGIC >> 8;
  payload[1] = (uint8_t)FPGA_BATCH_MAGIC;
  payload[2] = FPGA_BATCH_VERSION;
  payload[3] = batch->count;

  for (unsigned r = 0; r < batch->count; r++) {
    size_t record = FPGA_BATCH_HEADER_LEN + r * FPGA_ROUND_RECORD_LEN;
    payload[record] = batch->round[r] >> 8;
    payload[record + 1] = batch->round[r];
    for (unsigned i = 0; i < BRIDGE_MAX_ENTRIES; i++) {
      uint64_t val = hton64(batch->entry[r].value[i]);
      memcpy(payload + record + FPGA_ROUND_HEADER_LEN + i * sizeof(val),
             &val, sizeof(val));
    }
  }

  *frame_len = 42 + payload_len;
  return 0;
}

static int send_batch_frame(const RawPort *p, const BridgeConfig *c, unsigned w,
                            uint8_t workers, const FpgaBatch *batch) {
  uint8_t frame[FPGA_MAX_FRAME_LEN];
  size_t len;
  if (build_batch_frame(frame, &len, c, w, workers, batch))
    return -1;
  struct sockaddr_ll a = {.sll_family = AF_PACKET,
                          .sll_ifindex = p->ifindex,
                          .sll_halen = ETH_ALEN};
  memcpy(a.sll_addr, c->fpga_dst_mac, 6);
#ifdef USE_DPDK
  if (p->use_dpdk)
    return dpdk_send_frame((RawPort *)p, frame, len);
#endif
  return sendto(p->fd, frame, len, 0, (struct sockaddr *)&a, sizeof(a)) ==
                 (ssize_t)len
             ? 0
             : -1;
}

static int parse_batch_result(RawPort *port, const uint8_t *b, ssize_t n,
                              const struct sockaddr_ll *a, uint16_t dport,
                              FpgaBatch *out) {

  port->rx_total++;
  if (a && a->sll_pkttype == PACKET_OUTGOING) {
    port->rx_outgoing++;
    return 0;
  }
  if (n < 42 + (ssize_t)FPGA_BATCH_HEADER_LEN) {
    port->rx_rejected++;
    return 0;
  }
  const EthernetHeader *eth = (const EthernetHeader *)b;
  if (ntohs(eth->ethertype) != FPGA_ETHERTYPE_IPV4) {
    port->rx_rejected++;
    return 0;
  }
  const Ipv4Header *ip = (const Ipv4Header *)(b + 14);
  size_t ihl = (ip->version_ihl & 15) * 4;
  if (ip->protocol != 17 || ihl < 20 || n < 14 + (ssize_t)ihl + 8 + 6) {
    port->rx_rejected++;
    return 0;
  }
  const UdpHeader *u = (const UdpHeader *)(b + 14 + ihl);
  if (ntohs(u->dst_port) != dport) {
    port->rx_rejected++;
    return 0;
  }
  const uint8_t *p = b + 14 + ihl + 8;
  size_t udp_payload_len = ntohs(u->length) >= 8 ? ntohs(u->length) - 8 : 0;
  uint16_t magic = ((uint16_t)p[0] << 8) | p[1];
  unsigned count = p[3];
  size_t required = FPGA_BATCH_PAYLOAD_LEN(count);
  if (magic != FPGA_BATCH_MAGIC || p[2] != FPGA_BATCH_VERSION ||
      !count || count > FPGA_BATCH_MAX_ROUNDS || udp_payload_len != required ||
      n < 14 + (ssize_t)ihl + 8 + (ssize_t)required) {
    port->rx_rejected++;
    return 0;
  }
  memset(out, 0, sizeof(*out));
  out->count = count;
  for (unsigned r = 0; r < count; r++) {
    size_t record = FPGA_BATCH_HEADER_LEN + r * FPGA_ROUND_RECORD_LEN;
    out->round[r] = ((uint16_t)p[record] << 8) | p[record + 1];
    for (unsigned i = 0; i < BRIDGE_MAX_ENTRIES; i++) {
      uint64_t val;
      memcpy(&val, p + record + FPGA_ROUND_HEADER_LEN + i * sizeof(val),
             sizeof(val));
      out->entry[r].index[i] = i + 1;
      out->entry[r].value[i] = ntoh64(val);
    }
  }
  return 1;
}

static void drain(RawPort *p, uint16_t port) {
  uint8_t b[FPGA_MAX_FRAME_LEN];
#ifdef USE_DPDK
  if (p->use_dpdk) {
    struct rte_mbuf *m[32];
    uint16_t count;
    while ((count = rte_eth_rx_burst(p->dpdk_port_id, 0, m, 32))) {
      for (uint16_t i = 0; i < count; i++) {
        FpgaBatch x;
        (void)parse_batch_result(p, rte_pktmbuf_mtod(m[i], uint8_t *),
                                 rte_pktmbuf_pkt_len(m[i]), NULL, port, &x);
        rte_pktmbuf_free(m[i]);
      }
    }
    return;
  }
#endif
  for (;;) {
    struct sockaddr_ll a;
    socklen_t l = sizeof(a);
    ssize_t n =
        recvfrom(p->fd, b, sizeof(b), MSG_DONTWAIT, (struct sockaddr *)&a, &l);
    if (n < 0)
      return;
    FpgaBatch x;
    (void)parse_batch_result(p, b, n, &a, port, &x);
  }
}

static int batch_matches(const FpgaBatch *expected, const FpgaBatch *seen) {
  if (seen->count != expected->count)
    return 0;
  for (unsigned r = 0; r < seen->count; r++)
    if (seen->round[r] != expected->round[r])
      return 0;
  return 1;
}

static int receive_batch_result(RawPort *p, uint16_t port,
                                const FpgaBatch *expected, FpgaBatch *out) {
  uint8_t b[FPGA_MAX_FRAME_LEN];
#ifdef USE_DPDK
  if (p->use_dpdk) {
    uint64_t start = rte_get_timer_cycles();
    uint64_t timeout = rte_get_timer_hz() * (uint64_t)p->timeout_ms / 1000;
    while (rte_get_timer_cycles() - start < timeout) {
      struct rte_mbuf *m[32];
      uint16_t count = rte_eth_rx_burst(p->dpdk_port_id, 0, m, 32);
      for (uint16_t i = 0; i < count; i++) {
        FpgaBatch seen;
        int valid = parse_batch_result(p, rte_pktmbuf_mtod(m[i], uint8_t *),
                                       rte_pktmbuf_pkt_len(m[i]), NULL, port, &seen);
        rte_pktmbuf_free(m[i]);
        if (valid && batch_matches(expected, &seen)) { *out = seen; return 0; }
        if (valid) p->rx_other_round++;
      }
    }
    return -1;
  }
#endif
  for (;;) {
    struct sockaddr_ll a;
    socklen_t l = sizeof(a);
    ssize_t n = recvfrom(p->fd, b, sizeof(b), 0, (struct sockaddr *)&a, &l);
    if (n < 0)
      return -1;
    FpgaBatch seen;
    if (parse_batch_result(p, b, n, &a, port, &seen)) {
      if (batch_matches(expected, &seen)) { *out = seen; return 0; }
      p->rx_other_round++;
    }
  }
}

static void log_rx_stats(RawPort *p, int rank, uint16_t round) {
  struct tpacket_stats stats = {0};
  socklen_t len = sizeof(stats);
  int rc = getsockopt(p->fd, SOL_PACKET, PACKET_STATISTICS, &stats, &len);

  fprintf(stderr,
          "rank%d: rx diagnostics round=%u iface=%s total=%llu rejected=%llu "
          "other_round=%llu outgoing=%llu socket_packets=%u socket_drops=%u\n",
          rank, round, p->ifname, (unsigned long long)p->rx_total,
          (unsigned long long)p->rx_rejected,
          (unsigned long long)p->rx_other_round,
          (unsigned long long)p->rx_outgoing, rc ? 0 : stats.tp_packets,
          rc ? 0 : stats.tp_drops);
}

static void print_mac(const uint8_t mac[6]) {
  printf("%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
         mac[4], mac[5]);
}

static void print_ip(uint32_t address) {
  struct in_addr ip = {.s_addr = htonl(address)};
  char text[INET_ADDRSTRLEN];

  if (!inet_ntop(AF_INET, &ip, text, sizeof(text)))
    snprintf(text, sizeof(text), "invalid-ip");
  printf("%s", text);
}

static void print_startup(const BridgeConfig *cfg) {
  printf("MPI-FPGA bridge v4 已启动: app_port=%u, mpi_ranks=4, batch=1..16, jumbo_max=8368B\n",
         cfg->app_port);
  printf("worker_ifaces=(%s,%s,%s,%s), result_ifaces=(%s,%s,%s,%s), ",
         cfg->worker_iface[0], cfg->worker_iface[1], cfg->worker_iface[2],
         cfg->worker_iface[3], cfg->worker_iface[0], cfg->worker_iface[1],
         cfg->worker_iface[2], cfg->worker_iface[3]);
  printf("fpga_dst_mac=");
  print_mac(cfg->fpga_dst_mac);
  printf("\n");

  for (unsigned worker = 0; worker < BRIDGE_MAX_WORKERS; worker++) {
    if (worker)
      printf(" ");
    printf("worker%u_ip=", worker);
    print_ip(cfg->worker_src_ip[worker]);
    printf(" -> ");
    print_ip(cfg->worker_dst_ip[worker]);
  }
  printf("\n");
  fflush(stdout);
}

static int parse_config(int argc, char **argv, BridgeConfig *c, int rank) {
  if (argc < 15) {
    if (!rank)
      fprintf(
          stderr,
          "用法: mpirun -np 4 ./mpi_fpga_bridge <app_port> <fpga_mac> <if0> "
          "<src0> <dst0> <if1> <src1> <dst1> <if2> <src2> <dst2> <if3> <src3> "
          "<dst3> [dport sport0 sport1 sport2 sport3 timeout_ms]\n");
    return -1;
  }

  memset(c, 0, sizeof(*c));
  c->fpga_dport = FPGA_DEFAULT_DPORT;
  c->fpga_timeout_ms = DEFAULT_FPGA_TIMEOUT_MS;

  if (parse_u16(argv[1], &c->app_port) || parse_mac(argv[2], c->fpga_dst_mac))
    return -1;

  for (unsigned w = 0; w < 4; w++) {
    unsigned i = 3 + w * 3;
    snprintf(c->worker_iface[w], IFNAMSIZ, "%s", argv[i]);
    if (parse_ip(argv[i + 1], &c->worker_src_ip[w]) ||
        parse_ip(argv[i + 2], &c->worker_dst_ip[w]))
      return -1;
    c->fpga_sport[w] = 4000 + w;
  }

  if (argc > 15 && parse_u16(argv[15], &c->fpga_dport))
    return -1;
  for (unsigned w = 0; w < 4 && argc > 16 + (int)w; w++)
    if (parse_u16(argv[16 + w], &c->fpga_sport[w]))
      return -1;
  if (argc > 20 && parse_int(argv[20], &c->fpga_timeout_ms))
    return -1;
  return 0;
}

int main(int argc, char **argv) {
  MPI_Init(&argc, &argv);
  int rank, size;
  MPI_Comm_rank(MPI_COMM_WORLD, &rank);
  MPI_Comm_size(MPI_COMM_WORLD, &size);

  if (size != 4) {
    if (!rank)
    fprintf(stderr, "必须使用 -np 4，让 rank0..rank3 "
                    "各绑定一个 worker 口\n");
    MPI_Finalize();
    return 2;
  }
  if (bind_rank_cpu(rank)) {
    fprintf(stderr, "rank%d: CPU affinity setup failed: %s\n", rank, strerror(errno));
    MPI_Finalize();
    return 2;
  }
  BridgeConfig cfg;
  if (parse_config(argc, argv, &cfg, rank)) {
    MPI_Finalize();
    return 2;
  }

  int app = -1;
  RawPort raw = {.fd = -1};
  RawPort dpdk_ports[BRIDGE_MAX_WORKERS];
  int dpdk_owner = 0;
#ifdef USE_DPDK
  dpdk_owner = getenv("MPI_FPGA_DPDK") != NULL;
#endif

  if (!rank) {
    app = open_udp_server(cfg.app_port);
    if (app < 0) {
      perror("app bind");
      MPI_Abort(MPI_COMM_WORLD, 3);
    }
  }

  if (dpdk_owner && !rank) {
#ifdef USE_DPDK
    if (init_dpdk_owner(dpdk_ports, &cfg)) {
      fprintf(stderr, "rank0: DPDK four-port owner init failed: %s\n", raw_port_error());
      MPI_Abort(MPI_COMM_WORLD, 4);
    }
#endif
  } else if (!dpdk_owner && init_raw_port(&raw, cfg.worker_iface[rank], cfg.fpga_timeout_ms)) {
    fprintf(stderr, "rank%d: 无法打开 %s: %s\n", rank,
            cfg.worker_iface[rank], raw_port_error());
    MPI_Abort(MPI_COMM_WORLD, 4);
  }

  if (!rank)
    print_startup(&cfg);

  BridgeMessage pending_req = {0};
  struct sockaddr_in pending_peer = {0};
  socklen_t pending_peer_len = 0;
  int pending_valid = 0;

  for (;;) {
    BridgeMessage req[FPGA_BATCH_MAX_ROUNDS] = {{0}};
    struct sockaddr_in peer[FPGA_BATCH_MAX_ROUNDS] = {{0}};
    socklen_t peer_len[FPGA_BATCH_MAX_ROUNDS] = {0};
    int control[2] = {INTERNAL_MSG_NOOP, 0};

    if (!rank) {
      BridgeMessage first = {0};
      struct sockaddr_in first_peer = {0};
      socklen_t first_peer_len = sizeof(first_peer);
      ssize_t n;
      if (pending_valid) {
        first = pending_req;
        first_peer = pending_peer;
        first_peer_len = pending_peer_len;
        pending_valid = 0;
        n = sizeof(first);
      } else {
        n = recvfrom(app, &first, sizeof(first), 0,
                     (struct sockaddr *)&first_peer, &first_peer_len);
        if (n == (ssize_t)sizeof(first)) bridge_network_to_host(&first);
      }

      int immediate;
      if (n == (ssize_t)sizeof(first))
        immediate = validate_message(&first);
      else
        immediate = BRIDGE_STATUS_RECV_FAILED;
      if (immediate) {
        BridgeMessage res;
        error_response(&first, &res, immediate);
        bridge_host_to_network(&res);
        sendto(app, &res, sizeof(res), 0, (struct sockaddr *)&first_peer,
               first_peer_len);
      } else if (first.msg_type == APP_MSG_STOP) {
        control[0] = APP_MSG_STOP;
      } else {
        control[0] = APP_MSG_REQUEST;
        req[0] = first; peer[0] = first_peer; peer_len[0] = first_peer_len;
        control[1] = 1;
        uint16_t used_slots = 1u << (first.request_id & 15u);

        while (control[1] < (int)FPGA_BATCH_MAX_ROUNDS) {
          BridgeMessage next = {0};
          struct sockaddr_in next_peer = {0};
          socklen_t next_peer_len = sizeof(next_peer);
          n = recvfrom(app, &next, sizeof(next), MSG_DONTWAIT,
                       (struct sockaddr *)&next_peer, &next_peer_len);
          if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
          if (n != (ssize_t)sizeof(next)) break;
          bridge_network_to_host(&next);
          immediate = validate_message(&next);
          if (immediate) {
            BridgeMessage res;
            error_response(&next, &res, immediate);
            bridge_host_to_network(&res);
            sendto(app, &res, sizeof(res), 0, (struct sockaddr *)&next_peer,
                   next_peer_len);
            continue;
          }
          uint16_t slot_bit = 1u << (next.request_id & 15u);
          if (next.msg_type != APP_MSG_REQUEST ||
              next.worker_count != first.worker_count || (used_slots & slot_bit)) {
            pending_req = next; pending_peer = next_peer;
            pending_peer_len = next_peer_len; pending_valid = 1;
            break;
          }
          used_slots |= slot_bit;
          req[control[1]] = next;
          peer[control[1]] = next_peer;
          peer_len[control[1]] = next_peer_len;
          control[1]++;
        }
      }
    }

    MPI_Request mpi_req;
    MPI_Ibcast(control, 2, MPI_INT, 0, MPI_COMM_WORLD, &mpi_req);
    MPI_Wait(&mpi_req, MPI_STATUS_IGNORE);
    if (control[0] == APP_MSG_STOP) break;
    if (control[0] != APP_MSG_REQUEST) continue;

    FpgaBatch worker_batch[BRIDGE_MAX_WORKERS] = {{0}};
    if (!rank) {
      for (unsigned w = 0; w < BRIDGE_MAX_WORKERS; w++) {
        worker_batch[w].count = control[1];
        for (int b = 0; b < control[1]; b++) {
          worker_batch[w].round[b] = (uint16_t)req[b].request_id;
          if (w < req[b].worker_count)
            load_entries(req[b].worker_entries[w], req[b].entry_count,
                         &worker_batch[w].entry[b]);
        }
      }
    }

    FpgaBatch local_input = {0};
    MPI_Iscatter(worker_batch, sizeof(FpgaBatch), MPI_BYTE, &local_input,
                 sizeof(FpgaBatch), MPI_BYTE, 0, MPI_COMM_WORLD, &mpi_req);
    MPI_Wait(&mpi_req, MPI_STATUS_IGNORE);

    FpgaBatch gathered_input[BRIDGE_MAX_WORKERS] = {{0}};
    MPI_Iallgather(&local_input, sizeof(FpgaBatch), MPI_BYTE, gathered_input,
                   sizeof(FpgaBatch), MPI_BYTE, MPI_COMM_WORLD, &mpi_req);
    MPI_Wait(&mpi_req, MPI_STATUS_IGNORE);

    int local_status = 0;
    FpgaBatch local_result = {0};
    FpgaBatch port_result[BRIDGE_MAX_WORKERS] = {{0}};
    if (!dpdk_owner) {
      raw.rx_total = raw.rx_outgoing = raw.rx_rejected = raw.rx_other_round = 0;
      drain(&raw, cfg.fpga_dport);
    } else if (!rank) {
      for (unsigned w = 0; w < BRIDGE_MAX_WORKERS; w++)
        drain(&dpdk_ports[w], cfg.fpga_dport);
    }
    MPI_Ibarrier(MPI_COMM_WORLD, &mpi_req);
    MPI_Wait(&mpi_req, MPI_STATUS_IGNORE);

    unsigned workers = !rank ? req[0].worker_count : 0;
    MPI_Ibcast(&workers, 1, MPI_UNSIGNED, 0, MPI_COMM_WORLD, &mpi_req);
    MPI_Wait(&mpi_req, MPI_STATUS_IGNORE);

    if (dpdk_owner) {
      if (!rank) {
        for (unsigned w = 0; w < workers; w++)
          if (send_batch_frame(&dpdk_ports[w], &cfg, w, workers,
                               &gathered_input[w]))
            local_status = BRIDGE_STATUS_FPGA_SEND_FAILED;
        if (!local_status)
          for (unsigned w = 0; w < BRIDGE_MAX_WORKERS; w++)
            if (receive_batch_result(&dpdk_ports[w], cfg.fpga_dport,
                                     &gathered_input[0], &port_result[w]))
              local_status = BRIDGE_STATUS_FPGA_TIMEOUT;
      }
      MPI_Iscatter(port_result, sizeof(FpgaBatch), MPI_BYTE, &local_result,
                   sizeof(FpgaBatch), MPI_BYTE, 0, MPI_COMM_WORLD, &mpi_req);
      MPI_Wait(&mpi_req, MPI_STATUS_IGNORE);
    } else {
      if ((unsigned)rank < workers &&
          send_batch_frame(&raw, &cfg, rank, workers, &local_input))
        local_status = BRIDGE_STATUS_FPGA_SEND_FAILED;
      MPI_Ibarrier(MPI_COMM_WORLD, &mpi_req);
      MPI_Wait(&mpi_req, MPI_STATUS_IGNORE);
      if (!local_status && receive_batch_result(&raw, cfg.fpga_dport,
                                                &local_input, &local_result)) {
        local_status = BRIDGE_STATUS_FPGA_TIMEOUT;
        fprintf(stderr, "rank%d: timeout waiting for batch first_round=%u on %s\n",
                rank, local_input.round[0], raw.ifname);
        log_rx_stats(&raw, rank, local_input.round[0]);
      }
    }

    int status = 0;
    MPI_Iallreduce(&local_status, &status, 1, MPI_INT, MPI_MAX,
                   MPI_COMM_WORLD, &mpi_req);
    MPI_Wait(&mpi_req, MPI_STATUS_IGNORE);
    FpgaBatch all_result[BRIDGE_MAX_WORKERS] = {{0}};
    if (!status) {
      MPI_Iallgather(&local_result, sizeof(FpgaBatch), MPI_BYTE, all_result,
                     sizeof(FpgaBatch), MPI_BYTE, MPI_COMM_WORLD, &mpi_req);
      MPI_Wait(&mpi_req, MPI_STATUS_IGNORE);
      if (!rank)
        for (unsigned w = 1; w < BRIDGE_MAX_WORKERS; w++)
          if (memcmp(&all_result[0], &all_result[w], sizeof(FpgaBatch)))
            status = BRIDGE_STATUS_RESULT_MISMATCH;
    }
    MPI_Ibcast(&status, 1, MPI_INT, 0, MPI_COMM_WORLD, &mpi_req);
    MPI_Wait(&mpi_req, MPI_STATUS_IGNORE);

    if (!rank) {
      for (int b = 0; b < control[1]; b++) {
        BridgeMessage res = {0};
        if (status) {
          error_response(&req[b], &res, status);
        } else {
          res.magic = BRIDGE_MAGIC;
          res.version = BRIDGE_VERSION;
          res.msg_type = APP_MSG_RESPONSE;
          res.request_id = req[b].request_id;
          res.entry_count = req[b].entry_count;
          res.worker_count = req[b].worker_count;
          res.reserved = control[1];
          memcpy(res.worker_entries, req[b].worker_entries,
                 sizeof(res.worker_entries));
          for (unsigned i = 0; i < req[b].entry_count; i++)
            res.result_entries[i] = all_result[0].entry[b].value[i];
        }
        bridge_host_to_network(&res);
        sendto(app, &res, sizeof(res), 0, (struct sockaddr *)&peer[b],
               peer_len[b]);
      }
    }
  }

  if (dpdk_owner && !rank) {
    for (unsigned w = 0; w < BRIDGE_MAX_WORKERS; w++)
      close_raw_port(&dpdk_ports[w]);
  } else {
    close_raw_port(&raw);
  }
  if (!rank)
    close(app);
  MPI_Finalize();
  return 0;
}
