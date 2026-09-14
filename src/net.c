#include <stdint.h>
#include "net.h"

static struct net_packet queue[NET_QUEUE_MAX];

void net_init(void) {
    for (uint32_t i = 0; i < NET_QUEUE_MAX; ++i) queue[i].used = 0;
}

uint32_t net_local_ip(void) { return NET_IP_LOOPBACK; }

int32_t net_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                const void *data, uint32_t length) {
    if (!data || length == 0 || length > NET_PACKET_MAX || dst_port == 0) return -1;
    if (dst_ip != NET_IP_LOOPBACK) return -1;
    for (uint32_t i = 0; i < NET_QUEUE_MAX; ++i) {
        if (!queue[i].used) {
            queue[i].used = 1;
            queue[i].src_ip = NET_IP_LOOPBACK;
            queue[i].dst_ip = dst_ip;
            queue[i].src_port = src_port;
            queue[i].dst_port = dst_port;
            queue[i].length = (uint16_t)length;
            for (uint32_t j = 0; j < length; ++j) queue[i].data[j] = ((const uint8_t *)data)[j];
            return (int32_t)length;
        }
    }
    return -1;
}

int32_t net_recv(uint16_t port, void *data, uint32_t capacity,
                uint32_t *src_ip, uint16_t *src_port) {
    if (!port || !data || !capacity) return -1;
    for (uint32_t i = 0; i < NET_QUEUE_MAX; ++i) {
        if (queue[i].used && queue[i].dst_port == port) {
            uint32_t n = queue[i].length < capacity ? queue[i].length : capacity;
            for (uint32_t j = 0; j < n; ++j) ((uint8_t *)data)[j] = queue[i].data[j];
            if (src_ip) *src_ip = queue[i].src_ip;
            if (src_port) *src_port = queue[i].src_port;
            queue[i].used = 0;
            return (int32_t)n;
        }
    }
    return NET_RECV_EMPTY;
}

uint32_t net_pending(uint16_t port) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < NET_QUEUE_MAX; ++i)
        if (queue[i].used && queue[i].dst_port == port) ++count;
    return count;
}
