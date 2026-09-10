/* Archived v3 raw-socket bridge; the default build uses the v4 DPDK bridge. */
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
#include <unistd.h>

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
#define FPGA_PAYLOAD_LEN (2u + BRIDGE_MAX_ENTRIES * sizeof(uint64_t))
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
  char ifname[IFNAMSIZ];
  uint8_t src_mac[6];
  uint64_t rx_total;
  uint64_t rx_outgoing;
  uint64_t rx_rejected;
  uint64_t rx_other_round;
} RawPort;

typedef struct {
  uint16_t index[BRIDGE_MAX_ENTRIES];
  uint64_t value[BRIDGE_MAX_ENTRIES];
} EntryArray;

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

static int init_raw_port(RawPort *p, const char *name, int timeout) {

  memset(p, 0, sizeof(*p));
  p->fd = -1;
  snprintf(p->ifname, sizeof(p->ifname), "%s", name);
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
  if (p->fd >= 0)
    close(p->fd);
  p->fd = -1;
}

static void load_entries(const BridgeEntry *in, uint16_t n, EntryArray *out) {
  memset(out, 0, sizeof(*out));
  for (unsigned slot = 0; slot < 64; slot++) {
    out->index[slot] = slot + 1;
    if (slot < n)
      out->value[slot] = in[slot];
  }
}

static int send_frame(const RawPort *p, const BridgeConfig *c, unsigned w,
                      uint16_t round, uint8_t workers, const EntryArray *e) {
  uint8_t frame[2048] = {0};
  EthernetHeader *eth = (EthernetHeader *)frame;
  Ipv4Header *ip = (Ipv4Header *)(frame + 14);
  UdpHeader *udp = (UdpHeader *)(frame + 34);

  uint8_t *payload = frame + 42;
  memcpy(eth->dst, c->fpga_dst_mac, 6);
  memcpy(eth->src, fpga_request_src_mac, sizeof(eth->src));
  eth->ethertype = htons(FPGA_ETHERTYPE_IPV4);
  ip->version_ihl = 0x45;
  ip->ttl = workers;
  ip->protocol = FPGA_IP_PROTO_UDP;
  ip->total_length = htons(20 + 8 + FPGA_PAYLOAD_LEN);
  ip->src_ip = htonl(c->worker_src_ip[w]);
  ip->dst_ip = htonl(c->worker_dst_ip[w]);
  ip->checksum = htons(ipv4_checksum(ip, sizeof(*ip)));
  udp->src_port = htons(c->fpga_sport[w]);
  udp->dst_port = htons(c->fpga_dport);
  udp->length = htons(8 + FPGA_PAYLOAD_LEN);
  payload[0] = round >> 8;
  payload[1] = round;

  for (unsigned i = 0; i < 64; i++) {
    uint64_t val = hton64(e->value[i]);
    size_t off = 2 + i * sizeof(uint64_t);
    memcpy(payload + off, &val, sizeof(val));
  }

  size_t len = 14 + 20 + 8 + FPGA_PAYLOAD_LEN;
  struct sockaddr_ll a = {.sll_family = AF_PACKET,
                          .sll_ifindex = p->ifindex,
                          .sll_halen = ETH_ALEN};
  memcpy(a.sll_addr, c->fpga_dst_mac, 6);
  return sendto(p->fd, frame, len, 0, (struct sockaddr *)&a, sizeof(a)) ==
                 (ssize_t)len
             ? 0
             : -1;
}

static int parse_result(RawPort *port, const uint8_t *b, ssize_t n,
                        const struct sockaddr_ll *a, uint16_t dport,
                        uint16_t *round, EntryArray *out) {

  port->rx_total++;
  if (a->sll_pkttype == PACKET_OUTGOING) {
    port->rx_outgoing++;
    return 0;
  }
  if (n < 14 + 20 + 8 + (ssize_t)FPGA_PAYLOAD_LEN) {
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
  if (ip->protocol != 17 || ihl < 20 ||
      n < 14 + (ssize_t)ihl + 8 + (ssize_t)FPGA_PAYLOAD_LEN) {
    port->rx_rejected++;
    return 0;
  }
  const UdpHeader *u = (const UdpHeader *)(b + 14 + ihl);
  if (ntohs(u->dst_port) != dport) {
    port->rx_rejected++;
    return 0;
  }
  const uint8_t *p = b + 14 + ihl + 8;
  *round = ((uint16_t)p[0] << 8) | p[1];
  for (unsigned i = 0; i < 64; i++) {
    uint64_t val;
    size_t off = 2 + i * sizeof(uint64_t);
    memcpy(&val, p + off, sizeof(val));
    out->index[i] = i + 1;
    out->value[i] = ntoh64(val);
  }
  return 1;
}

static void drain(RawPort *p, uint16_t port, uint16_t round) {
  uint8_t b[2048];
  for (;;) {
    struct sockaddr_ll a;
    socklen_t l = sizeof(a);
    ssize_t n =
        recvfrom(p->fd, b, sizeof(b), MSG_DONTWAIT, (struct sockaddr *)&a, &l);
    if (n < 0)
      return;
    uint16_t seen;
    EntryArray x;
    if (parse_result(p, b, n, &a, port, &seen, &x) && seen == round)
      continue;
  }
}

static int receive_result(RawPort *p, uint16_t port, uint16_t round,
                          EntryArray *out) {
  uint8_t b[2048];
  for (;;) {
    struct sockaddr_ll a;
    socklen_t l = sizeof(a);
    ssize_t n = recvfrom(p->fd, b, sizeof(b), 0, (struct sockaddr *)&a, &l);
    if (n < 0)
      return -1;
    uint16_t seen;
    if (parse_result(p, b, n, &a, port, &seen, out)) {
      if (seen == round)
        return 0;
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

static void print_entries(const BridgeEntry *entries, uint16_t count) {
  printf("[");
  for (uint16_t i = 0; i < count; i++) {
    if (i)
      printf(", ");
    printf("%llu", (unsigned long long)entries[i]);
  }
  printf("]");
}

static void print_startup(const BridgeConfig *cfg) {
  printf("MPI-FPGA bridge 已启动: app_port=%u, mpi_ranks=4\n", cfg->app_port);
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

static void print_request_summary(const BridgeMessage *request,
                                  const BridgeMessage *response) {
  printf("request=%u", request->request_id);
  for (uint16_t worker = 0; worker < request->worker_count; worker++) {
    printf(" worker%u=", worker);
    print_entries(request->worker_entries[worker], request->entry_count);
  }
  printf(" fpga=");
  print_entries(response->result_entries, response->entry_count);
  printf(" resp=");
  print_entries(response->result_entries, response->entry_count);
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
  BridgeConfig cfg;
  if (parse_config(argc, argv, &cfg, rank)) {
    MPI_Finalize();
    return 2;
  }

  int app = -1;
  RawPort raw = {.fd = -1};

  if (!rank) {
    app = open_udp_server(cfg.app_port);
    if (app < 0) {
      perror("app bind");
      MPI_Abort(MPI_COMM_WORLD, 3);
    }
  }

  if (init_raw_port(&raw, cfg.worker_iface[rank], cfg.fpga_timeout_ms)) {
    fprintf(stderr, "rank%d: 无法打开 %s: %s\n", rank,
            cfg.worker_iface[rank], strerror(errno));
    MPI_Abort(MPI_COMM_WORLD, 4);
  }

  if (!rank)
    print_startup(&cfg);

  for (;;) {
    BridgeMessage req = {0};
    struct sockaddr_in peer = {0};
    socklen_t peer_len = sizeof(peer);
    uint16_t ctrl = INTERNAL_MSG_NOOP, immediate = 0;
    if (!rank) {
      ssize_t n = recvfrom(app, &req, sizeof(req), 0, (struct sockaddr *)&peer,
                           &peer_len);
      if (n != (ssize_t)sizeof(req))
        immediate = BRIDGE_STATUS_RECV_FAILED;
      else {
        bridge_network_to_host(&req);
        immediate = validate_message(&req);
        if (!immediate)
          ctrl = req.msg_type;
      }
    }

    MPI_Bcast(&ctrl, 1, MPI_UNSIGNED_SHORT, 0, MPI_COMM_WORLD);
    if (ctrl == APP_MSG_STOP)
      break;
    if (ctrl != APP_MSG_REQUEST) {
      if (!rank) {
        BridgeMessage res;
        error_response(&req, &res, immediate);
        bridge_host_to_network(&res);
        sendto(app, &res, sizeof(res), 0, (struct sockaddr *)&peer, peer_len);
      }
      continue;
    }

    MPI_Bcast(&req, sizeof(req), MPI_BYTE, 0, MPI_COMM_WORLD);
    uint16_t round = req.request_id;
    EntryArray local = {0};
    int local_status = 0;
    raw.rx_total = 0;
    raw.rx_outgoing = 0;
    raw.rx_rejected = 0;
    raw.rx_other_round = 0;
    drain(&raw, cfg.fpga_dport, round);
    MPI_Barrier(MPI_COMM_WORLD);

    if ((unsigned)rank < req.worker_count) {
      EntryArray in;
      load_entries(req.worker_entries[rank], req.entry_count, &in);
      if (send_frame(&raw, &cfg, rank, round, req.worker_count, &in)) {
        local_status = BRIDGE_STATUS_FPGA_SEND_FAILED;
        fprintf(stderr, "rank%d: send round=%u failed on %s: %s\n", rank,
                round, raw.ifname, strerror(errno));
      } else {
        /* Success is reported once by rank0 after all broadcast copies agree. */
      }
    }

    /* Ensure every raw socket is armed before any rank waits for the reply. */
    MPI_Barrier(MPI_COMM_WORLD);

    if (!local_status) {
      if (receive_result(&raw, cfg.fpga_dport, round, &local)) {
        local_status = BRIDGE_STATUS_FPGA_TIMEOUT;
        fprintf(stderr, "rank%d: timeout waiting for round=%u on %s\n", rank,
                round, raw.ifname);
        log_rx_stats(&raw, rank, round);
      } else {
        /* Keep normal output compact; failures retain per-rank diagnostics. */
      }
    }
    int status = 0;
    MPI_Allreduce(&local_status, &status, 1, MPI_INT, MPI_MAX, MPI_COMM_WORLD);
    EntryArray all[4];

    if (!status) {
      MPI_Gather(&local, sizeof(local), MPI_BYTE, all, sizeof(local), MPI_BYTE,
                 0, MPI_COMM_WORLD);
      if (!rank)
        for (unsigned w = 1; w < 4; w++)
          if (memcmp(&all[0], &all[w], sizeof(EntryArray)))
            status = BRIDGE_STATUS_RESULT_MISMATCH;
    }

    MPI_Bcast(&status, 1, MPI_INT, 0, MPI_COMM_WORLD);
    if (!rank) {
      if (status) {
        BridgeMessage res;
        error_response(&req, &res, status);
        bridge_host_to_network(&res);
        sendto(app, &res, sizeof(res), 0, (struct sockaddr *)&peer, peer_len);
        continue;
      }
      BridgeMessage res = {0};
      res.magic = BRIDGE_MAGIC;
      res.version = BRIDGE_VERSION;
      res.msg_type = APP_MSG_RESPONSE;
      res.request_id = req.request_id;
      res.entry_count = req.entry_count;
      res.worker_count = req.worker_count;
      memcpy(res.worker_entries, req.worker_entries,
             sizeof(res.worker_entries));
      for (unsigned i = 0; i < req.entry_count; i++)
        res.result_entries[i] = all[0].value[i];
      print_request_summary(&req, &res);
      bridge_host_to_network(&res);
      sendto(app, &res, sizeof(res), 0, (struct sockaddr *)&peer, peer_len);
    }
  }

  close_raw_port(&raw);
  if (!rank)
    close(app);
  MPI_Finalize();
  return 0;
}
