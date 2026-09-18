#include <stdint.h>
#include "net.h"
#include "spinlock.h"
#include "io.h"

#define RTL8139_VENDOR 0x10ECu
#define RTL8139_DEVICE 0x8139u

#define REG_MAC0   0x00u
#define REG_TX0    0x10u
#define REG_RXBUF  0x30u
#define REG_CMD    0x37u
#define REG_CAPR   0x38u
#define REG_CBR    0x3Au
#define REG_IMR    0x3Cu
#define REG_ISR    0x3Eu
#define REG_TCR    0x40u
#define REG_RCR    0x44u
#define REG_CONFIG1 0x52u
#define CMD_RESET  0x10u
#define CMD_RXTXON 0x0Cu
#define ISR_ROK    0x0001u
#define ISR_TOK    0x0004u
#define RCR_AB     0x00000008u
#define RCR_APM    0x00000001u
#define RCR_AM     0x00000004u
#define RCR_AAP    0x00000010u
#define RCR_WRAP   0x00000080u

#define RTL_RX_SIZE 8192u
#define RTL_RX_ALLOC (RTL_RX_SIZE + 16u + 1500u)
#define RTL_TX_SIZE 1536u
#define RTL_TX_COUNT 4u
#define ETH_HDR_LEN 14u
#define ETH_MTU 1500u

#define NET_PHYS_IP 0x0A00020Fu
#define NET_GATEWAY_IP 0x0A000202u
#define NET_NETMASK 0xFFFFFF00u
#define NET_EPHEMERAL_PORT 40000u

struct net_packet {
    uint32_t used;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint8_t data[NET_PACKET_MAX];
};

struct eth_hdr {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t type;
} __attribute__((packed));

struct arp_packet {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t oper;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
} __attribute__((packed));

struct ipv4_hdr {
    uint8_t version_ihl;
    uint8_t tos;
    uint16_t total_len;
    uint16_t identification;
    uint16_t flags_frag;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint32_t src;
    uint32_t dst;
} __attribute__((packed));

struct icmp_hdr {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
} __attribute__((packed));

static struct net_packet queue[NET_QUEUE_MAX];
static struct spinlock net_lock;
static uint16_t rtl_base;
static uint8_t rtl_mac[6];
static uint8_t rtl_rx[RTL_RX_ALLOC] __attribute__((aligned(16)));
static uint8_t rtl_tx[RTL_TX_COUNT][RTL_TX_SIZE] __attribute__((aligned(16)));
static uint32_t rtl_rx_offset;
static uint32_t rtl_tx_index;
static uint32_t rtl_ready;
static uint16_t ip_id = 1u;

static uint16_t bswap16(uint16_t x) { return (uint16_t)((x << 8) | (x >> 8)); }
static uint32_t bswap32(uint32_t x) {
    return ((x & 0x000000FFu) << 24) | ((x & 0x0000FF00u) << 8) |
           ((x & 0x00FF0000u) >> 8) | ((x & 0xFF000000u) >> 24);
}
static uint16_t be16(uint16_t x) { return bswap16(x); }
static uint32_t be32(uint32_t x) { return bswap32(x); }

static void pci_write_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t reg) {
    uint32_t a = 0x80000000u | ((uint32_t)bus << 16) |
                 ((uint32_t)slot << 11) | ((uint32_t)func << 8) |
                 (reg & 0xFCu);
    outl(0xCF8, a);
}
static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t reg) {
    pci_write_addr(bus, slot, func, reg);
    return inl(0xCFC);
}

static int rtl_find(void) {
    for (uint32_t bus = 0; bus < 256u; ++bus) {
        for (uint32_t slot = 0; slot < 32u; ++slot) {
            uint32_t id = pci_read32((uint8_t)bus, (uint8_t)slot, 0, 0);
            if ((id & 0xFFFFu) != RTL8139_VENDOR || (id >> 16) != RTL8139_DEVICE) continue;
            uint32_t bar = pci_read32((uint8_t)bus, (uint8_t)slot, 0, 0x10);
            if (!(bar & 1u)) return -1;
            rtl_base = (uint16_t)(bar & 0xFFFCu);
            return 0;
        }
    }
    return -1;
}

static int rtl_init(void) {
    if (rtl_find() != 0) return -1;
    outb(rtl_base + REG_CONFIG1, 0x00u);
    outb(rtl_base + REG_CMD, CMD_RESET);
    for (uint32_t i = 0; i < 100000u; ++i)
        if (!(inb(rtl_base + REG_CMD) & CMD_RESET)) break;
    if (inb(rtl_base + REG_CMD) & CMD_RESET) return -1;

    for (uint32_t i = 0; i < 6u; ++i) rtl_mac[i] = inb(rtl_base + REG_MAC0 + i);

    outl(rtl_base + REG_RCR, RCR_AB | RCR_APM | RCR_AM | RCR_AAP | RCR_WRAP);
    outl(rtl_base + REG_TCR, 0x03000700u);
    outl(rtl_base + REG_RXBUF, (uint32_t)(uintptr_t)rtl_rx);
    outw(rtl_base + REG_CAPR, 0xFFF0u);
    outb(rtl_base + REG_CMD, CMD_RXTXON);
    outw(rtl_base + REG_IMR, 0u);
    (void)inw(rtl_base + REG_ISR);
    rtl_rx_offset = 0u;
    rtl_tx_index = 0u;
    rtl_ready = 1u;
    return 0;
}

static uint16_t checksum16(const void *data, uint32_t length) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t sum = 0;
    while (length > 1u) {
        sum += ((uint32_t)p[0] << 8) | p[1];
        p += 2;
        length -= 2;
    }
    if (length) sum += (uint32_t)p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return (uint16_t)~sum;
}

static int rtl_tx_frame(const uint8_t *frame, uint32_t length) {
    if (!rtl_ready || !frame || length < ETH_HDR_LEN || length > RTL_TX_SIZE) return -1;
    uint32_t slot = rtl_tx_index++ & (RTL_TX_COUNT - 1u);
    for (uint32_t i = 0; i < length; ++i) rtl_tx[slot][i] = frame[i];
    outl(rtl_base + REG_TX0 + slot * 4u, (uint32_t)(uintptr_t)rtl_tx[slot]);
    outl(rtl_base + REG_TX0 + slot * 4u + 0x10u, length);
    for (uint32_t i = 0; i < 100000u; ++i) {
        uint32_t status = inl(rtl_base + REG_TX0 + slot * 4u + 0x10u);
        if (status & 0x80000000u) return 0;
        if (status & 0x40000000u) return -1;
    }
    return -1;
}

static uint32_t rtl_poll(uint8_t *out, uint32_t capacity) {
    if (!rtl_ready) return 0;
    while (!(inb(rtl_base + REG_CMD) & 1u)) {
        uint8_t *packet = rtl_rx + rtl_rx_offset;
        uint16_t status = (uint16_t)packet[0] | ((uint16_t)packet[1] << 8);
        uint16_t length = (uint16_t)packet[2] | ((uint16_t)packet[3] << 8);
        if (!(status & 1u) || length < 4u || length > ETH_MTU + ETH_HDR_LEN + 4u) {
            rtl_rx_offset = (rtl_rx_offset + 4u) & (RTL_RX_SIZE - 1u);
            outw(rtl_base + REG_CAPR, (uint16_t)(rtl_rx_offset - 16u));
            if (rtl_rx_offset >= RTL_RX_SIZE) rtl_rx_offset = 0u;
            continue;
        }
        uint32_t frame_len = (uint32_t)length - 4u;
        uint32_t copy = frame_len < capacity ? frame_len : capacity;
        for (uint32_t i = 0; i < copy; ++i) out[i] = packet[4u + i];
        rtl_rx_offset = (rtl_rx_offset + (uint32_t)length + 3u) & (RTL_RX_SIZE - 1u);
        outw(rtl_base + REG_CAPR, (uint16_t)(rtl_rx_offset - 16u));
        return copy;
    }
    return 0;
}

static int mac_is_broadcast(const uint8_t *mac) {
    for (uint32_t i = 0; i < 6u; ++i) if (mac[i] != 0xFFu) return 0;
    return 1;
}
static int mac_equal(const uint8_t *a, const uint8_t *b) {
    for (uint32_t i = 0; i < 6u; ++i) if (a[i] != b[i]) return 0;
    return 1;
}

static int arp_request(uint32_t target_ip) {
    uint8_t frame[ETH_HDR_LEN + sizeof(struct arp_packet)];
    struct eth_hdr *eth = (struct eth_hdr *)frame;
    struct arp_packet *arp = (struct arp_packet *)(frame + ETH_HDR_LEN);
    for (uint32_t i = 0; i < 6u; ++i) eth->dst[i] = 0xFFu, arp->tha[i] = 0;
    for (uint32_t i = 0; i < 6u; ++i) { eth->src[i] = rtl_mac[i]; arp->sha[i] = rtl_mac[i]; }
    eth->type = be16(0x0806u);
    arp->htype = be16(1u); arp->ptype = be16(0x0800u); arp->hlen = 6u; arp->plen = 4u; arp->oper = be16(1u);
    arp->spa = be32(NET_PHYS_IP); arp->tpa = be32(target_ip);
    return rtl_tx_frame(frame, sizeof(frame));
}

static int arp_resolve(uint32_t ip, uint8_t mac[6]) {
    for (uint32_t attempt = 0; attempt < 3u; ++attempt) {
        if (arp_request(ip) != 0) return -1;
        for (uint32_t wait = 0; wait < 200000u; ++wait) {
            uint8_t frame[1600];
            uint32_t n = rtl_poll(frame, sizeof(frame));
            if (n < ETH_HDR_LEN + sizeof(struct arp_packet)) continue;
            struct eth_hdr *eth = (struct eth_hdr *)frame;
            if (be16(eth->type) != 0x0806u) continue;
            struct arp_packet *arp = (struct arp_packet *)(frame + ETH_HDR_LEN);
            if (be16(arp->oper) != 2u || be32(arp->spa) != ip) continue;
            for (uint32_t i = 0; i < 6u; ++i) mac[i] = arp->sha[i];
            return 0;
        }
    }
    return -1;
}

static int send_icmp(uint32_t target_ip, const uint8_t dst_mac[6], uint16_t id, uint16_t seq) {
    uint8_t frame[ETH_HDR_LEN + sizeof(struct ipv4_hdr) + sizeof(struct icmp_hdr) + 32u];
    struct eth_hdr *eth = (struct eth_hdr *)frame;
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(frame + ETH_HDR_LEN);
    struct icmp_hdr *icmp = (struct icmp_hdr *)(frame + ETH_HDR_LEN + sizeof(struct ipv4_hdr));
    for (uint32_t i = 0; i < 6u; ++i) { eth->dst[i] = dst_mac[i]; eth->src[i] = rtl_mac[i]; }
    eth->type = be16(0x0800u);
    ip->version_ihl = 0x45u; ip->tos = 0; ip->total_len = be16((uint16_t)(sizeof(struct ipv4_hdr) + sizeof(struct icmp_hdr) + 32u));
    ip->identification = be16(ip_id++); ip->flags_frag = 0; ip->ttl = 64u; ip->protocol = 1u; ip->checksum = 0;
    ip->src = be32(NET_PHYS_IP); ip->dst = be32(target_ip); ip->checksum = checksum16(ip, sizeof(*ip));
    icmp->type = 8u; icmp->code = 0; icmp->checksum = 0; icmp->id = be16(id); icmp->sequence = be16(seq);
    for (uint32_t i = 0; i < 32u; ++i) frame[ETH_HDR_LEN + sizeof(struct ipv4_hdr) + sizeof(struct icmp_hdr) + i] = (uint8_t)('A' + (i % 26u));
    icmp->checksum = checksum16(icmp, sizeof(struct icmp_hdr) + 32u);
    return rtl_tx_frame(frame, ETH_HDR_LEN + sizeof(struct ipv4_hdr) + sizeof(struct icmp_hdr) + 32u);
}

static int wait_icmp_reply(uint32_t target_ip, uint16_t id, uint16_t seq) {
    for (uint32_t wait = 0; wait < 300000u; ++wait) {
        uint8_t frame[1600];
        uint32_t n = rtl_poll(frame, sizeof(frame));
        if (n < ETH_HDR_LEN + sizeof(struct ipv4_hdr) + sizeof(struct icmp_hdr)) continue;
        struct eth_hdr *eth = (struct eth_hdr *)frame;
        if (be16(eth->type) != 0x0800u) continue;
        struct ipv4_hdr *ip = (struct ipv4_hdr *)(frame + ETH_HDR_LEN);
        uint32_t ihl = (uint32_t)(ip->version_ihl & 0x0Fu) * 4u;
        if ((ip->version_ihl >> 4) != 4u || ihl < 20u || n < ETH_HDR_LEN + ihl + sizeof(struct icmp_hdr)) continue;
        if (ip->protocol != 1u || be32(ip->src) != target_ip || be32(ip->dst) != NET_PHYS_IP) continue;
        struct icmp_hdr *icmp = (struct icmp_hdr *)(frame + ETH_HDR_LEN + ihl);
        if (icmp->type == 0u && icmp->code == 0u && be16(icmp->id) == id && be16(icmp->sequence) == seq) return 0;
    }
    return -1;
}

int32_t net_ping(uint32_t target_ip) {
    if (!rtl_ready) return -1;
    uint32_t next_hop = ((target_ip & NET_NETMASK) == (NET_PHYS_IP & NET_NETMASK)) ? target_ip : NET_GATEWAY_IP;
    uint8_t mac[6];
    if (arp_resolve(next_hop, mac) != 0) return -1;
    uint16_t id = (uint16_t)(process_current_pid() & 0xFFFFu);
    for (uint16_t seq = 1u; seq <= 4u; ++seq) {
        if (send_icmp(target_ip, mac, id, seq) != 0) return -1;
        if (wait_icmp_reply(target_ip, id, seq) != 0) return -1;
    }
    return 0;
}

void net_init(void) {
    spinlock_init(&net_lock);
    for (uint32_t i = 0; i < NET_QUEUE_MAX; ++i) queue[i].used = 0;
    rtl_ready = 0;
    if (rtl_init() != 0) return;
}

uint32_t net_local_ip(void) { return rtl_ready ? NET_PHYS_IP : NET_IP_LOOPBACK; }
int32_t net_physical_ready(void) { return rtl_ready ? 1 : 0; }

int32_t net_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                 const void *data, uint32_t length) {
    if (!data || length == 0 || length > NET_PACKET_MAX || dst_port == 0) return -1;
    if (dst_ip != NET_IP_LOOPBACK) return -1;
    uint32_t irq=spinlock_irqsave_acquire(&net_lock);
    for (uint32_t i = 0; i < NET_QUEUE_MAX; ++i) {
        if (!queue[i].used) {
            queue[i].used = 1; queue[i].src_ip = NET_IP_LOOPBACK; queue[i].dst_ip = dst_ip;
            queue[i].src_port = src_port; queue[i].dst_port = dst_port; queue[i].length = (uint16_t)length;
            for (uint32_t j = 0; j < length; ++j) queue[i].data[j] = ((const uint8_t *)data)[j];
            spinlock_irqrestore_release(&net_lock,irq);
            return (int32_t)length;
        }
    }
    spinlock_irqrestore_release(&net_lock,irq);
    return -1;
}
int32_t net_recv(uint16_t port, void *data, uint32_t capacity, uint32_t *src_ip, uint16_t *src_port) {
    if (!port || !data || !capacity) return -1;
    uint32_t irq=spinlock_irqsave_acquire(&net_lock);
    for (uint32_t i = 0; i < NET_QUEUE_MAX; ++i) if (queue[i].used && queue[i].dst_port == port) {
        uint32_t n=queue[i].length<capacity?queue[i].length:capacity;
        for (uint32_t j=0;j<n;++j)((uint8_t*)data)[j]=queue[i].data[j];
        if(src_ip)*src_ip=queue[i].src_ip;if(src_port)*src_port=queue[i].src_port;queue[i].used=0;
        spinlock_irqrestore_release(&net_lock,irq);return (int32_t)n;
    }
    spinlock_irqrestore_release(&net_lock,irq);return NET_RECV_EMPTY;
}
uint32_t net_pending(uint16_t port) {
    uint32_t count=0,irq=spinlock_irqsave_acquire(&net_lock);
    for(uint32_t i=0;i<NET_QUEUE_MAX;++i)if(queue[i].used&&queue[i].dst_port==port)++count;
    spinlock_irqrestore_release(&net_lock,irq);return count;
}
