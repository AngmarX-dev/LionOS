#ifndef LIONOS_NET_H
#define LIONOS_NET_H

#include <stdint.h>

#define NET_IP_LOOPBACK 0x7F000001u
#define NET_PACKET_MAX 256u
#define NET_QUEUE_MAX 16u
#define NET_RECV_EMPTY (-2)

struct net_packet {
    uint32_t used;
    uint32_t src_ip;
    uint32_t dst_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint8_t data[NET_PACKET_MAX];
};

void net_init(void);
uint32_t net_local_ip(void);
int32_t net_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                const void *data, uint32_t length);
int32_t net_recv(uint16_t port, void *data, uint32_t capacity,
                uint32_t *src_ip, uint16_t *src_port);
uint32_t net_pending(uint16_t port);

#endif
