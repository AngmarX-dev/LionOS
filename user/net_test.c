#include "user_libc.h"
#include "user_api.h"
#include "net.h"

int main(void) {
    static const char message[] = "hello over LionOS loopback";
    char buffer[NET_PACKET_MAX + 1u];
    uint32_t source_ip = 0;
    uint16_t source_port = 0;

    if (lion_net_getip() != NET_IP_LOOPBACK) { puts("NET TEST: FAIL (IP)"); return 1; }
    if (lion_net_send(NET_IP_LOOPBACK, 4000u, 5000u, message, sizeof(message) - 1u) != (int32_t)(sizeof(message) - 1u)) {
        puts("NET TEST: FAIL (send)"); return 1;
    }
    if (lion_net_pending(5000u) != 1u) { puts("NET TEST: FAIL (pending)"); return 1; }
    int32_t received = lion_net_recv(5000u, buffer, NET_PACKET_MAX, &source_ip, &source_port);
    if (received != (int32_t)(sizeof(message) - 1u) || source_ip != NET_IP_LOOPBACK || source_port != 4000u) {
        puts("NET TEST: FAIL (receive)"); return 1;
    }
    buffer[(uint32_t)received] = 0;
    puts("NET TEST: PASS");
    puts(buffer);
    return 0;
}
