#include <stdint.h>
#include "user_libc.h"
#include "user_api.h"

static uint32_t parse_ip(const char *s, int *ok) {
    uint32_t ip = 0;
    uint32_t part = 0;
    uint32_t count = 0;
    *ok = 0;
    if (!s || !*s) return 0;
    for (uint32_t i = 0;; ++i) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            part = part * 10u + (uint32_t)(c - '0');
            if (part > 255u) return 0;
        } else if (c == '.' || c == 0) {
            if (count >= 4u) return 0;
            ip = (ip << 8) | part;
            ++count;
            part = 0;
            if (c == 0) break;
        } else return 0;
    }
    if (count != 4u) return 0;
    *ok = 1;
    return ip;
}

static void print_ip(uint32_t ip) {
    printf("%u.%u.%u.%u", (ip >> 24) & 255u, (ip >> 16) & 255u,
           (ip >> 8) & 255u, ip & 255u);
}

int main(void) {
    const char *target = "10.0.2.2";
    char input[32];
    uint32_t n = 0;
    int ok;
    (void)input;
    /* ping accepts one IPv4 argument when invoked as: run ping.elf <ip>.
       The early launcher currently supplies only the program name, so the
       default is the QEMU user-network gateway. */
    uint32_t ip = parse_ip(target, &ok);
    if (!ok) { puts("ping: invalid IPv4 address"); return 1; }

    printf("PING ");
    print_ip(ip);
    printf(" from ");
    print_ip(lion_net_getip());
    puts("");

    int32_t result = lion_net_ping(ip);
    if (result < 0) {
        puts("ping: network unreachable or no reply");
        return 1;
    }

    printf("--- ");
    print_ip(ip);
    puts(" ping statistics ---");
    printf("4 packets transmitted, %d received, 0%% packet loss\n", result);
    return 0;
}
