#include "k64_net.h"
#include "k64_string.h"

#define ETH_TYPE_ARP  0x0806u
#define ETH_TYPE_IPV4 0x0800u
#define IP_PROTO_ICMP 1u
#define IP_PROTO_UDP  17u

typedef struct {
    bool                  ready;
    char                  driver[24];
    uint8_t               mac[6];
    uint32_t              ipv4;
    uint32_t              gateway;
    uint32_t              netmask;
    k64_net_send_frame_fn send;
    k64_net_status_t      stats;
    uint16_t              next_ip_id;
    uint16_t              next_icmp_id;
    uint8_t               arp_ip[8][4];
    uint8_t               arp_mac[8][6];
    uint8_t               arp_count;
} k64_net_state_t;

static k64_net_state_t net;

static uint16_t rd16be(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t rd32be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void wr16be(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFFu);
}

static void wr32be(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

static uint16_t csum16(const uint8_t* data, uint16_t len) {
    uint32_t sum = 0;

    while (len > 1) {
        sum += rd16be(data);
        data += 2;
        len -= 2;
    }
    if (len) {
        sum += (uint16_t)data[0] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static void copy_mac(uint8_t dst[6], const uint8_t src[6]) {
    for (int i = 0; i < 6; ++i) {
        dst[i] = src[i];
    }
}

static bool mac_is_broadcast(const uint8_t mac[6]) {
    for (int i = 0; i < 6; ++i) {
        if (mac[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

static bool mac_eq(const uint8_t a[6], const uint8_t b[6]) {
    for (int i = 0; i < 6; ++i) {
        if (a[i] != b[i]) {
            return false;
        }
    }
    return true;
}

static void arp_remember(uint32_t ip, const uint8_t mac[6]) {
    uint8_t slot = 0;

    for (uint8_t i = 0; i < net.arp_count; ++i) {
        if (rd32be(net.arp_ip[i]) == ip) {
            slot = i;
            goto found;
        }
    }
    if (net.arp_count < 8) {
        slot = net.arp_count++;
    } else {
        slot = 0;
    }
found:
    wr32be(net.arp_ip[slot], ip);
    copy_mac(net.arp_mac[slot], mac);
}

static bool arp_lookup(uint32_t ip, uint8_t mac[6]) {
    for (uint8_t i = 0; i < net.arp_count; ++i) {
        if (rd32be(net.arp_ip[i]) == ip) {
            copy_mac(mac, net.arp_mac[i]);
            return true;
        }
    }
    return false;
}

static bool send_frame(const uint8_t* frame, uint16_t len) {
    if (!net.ready || !net.send || !net.send(frame, len)) {
        return false;
    }
    net.stats.tx_frames++;
    return true;
}

static bool send_arp_reply(const uint8_t target_mac[6], uint32_t target_ip) {
    uint8_t frame[42];

    copy_mac(frame, target_mac);
    copy_mac(frame + 6, net.mac);
    wr16be(frame + 12, ETH_TYPE_ARP);
    wr16be(frame + 14, 1);
    wr16be(frame + 16, ETH_TYPE_IPV4);
    frame[18] = 6;
    frame[19] = 4;
    wr16be(frame + 20, 2);
    copy_mac(frame + 22, net.mac);
    wr32be(frame + 28, net.ipv4);
    copy_mac(frame + 32, target_mac);
    wr32be(frame + 38, target_ip);
    return send_frame(frame, sizeof(frame));
}

static bool send_ipv4(const uint8_t dst_mac[6], uint32_t dst_ip, uint8_t proto, const uint8_t* payload, uint16_t payload_len) {
    uint8_t frame[1514];
    uint16_t ip_len = (uint16_t)(20u + payload_len);

    if (ip_len > 1500u) {
        return false;
    }

    copy_mac(frame, dst_mac);
    copy_mac(frame + 6, net.mac);
    wr16be(frame + 12, ETH_TYPE_IPV4);

    frame[14] = 0x45;
    frame[15] = 0;
    wr16be(frame + 16, ip_len);
    wr16be(frame + 18, net.next_ip_id++);
    wr16be(frame + 20, 0x4000);
    frame[22] = 64;
    frame[23] = proto;
    wr16be(frame + 24, 0);
    wr32be(frame + 26, net.ipv4);
    wr32be(frame + 30, dst_ip);
    wr16be(frame + 24, csum16(frame + 14, 20));

    memcpy(frame + 34, payload, payload_len);
    return send_frame(frame, (uint16_t)(14u + ip_len));
}

static bool resolve_mac(uint32_t dst_ip, uint8_t mac[6]) {
    uint32_t target = ((dst_ip & net.netmask) == (net.ipv4 & net.netmask)) ? dst_ip : net.gateway;

    if (arp_lookup(target, mac)) {
        return true;
    }
    (void)k64_net_send_arp_request(target);
    return false;
}

void k64_net_init(void) {
    memset(&net, 0, sizeof(net));
    net.ipv4 = (10u << 24) | (0u << 16) | (2u << 8) | 15u;
    net.gateway = (10u << 24) | (0u << 16) | (2u << 8) | 2u;
    net.netmask = 0xFFFFFF00u;
    net.next_ip_id = 1;
    net.next_icmp_id = 1;
}

bool k64_net_register_device(const char* driver, const uint8_t mac[6], k64_net_send_frame_fn send) {
    if (!mac || !send) {
        return false;
    }
    if (net.ready) {
        return false;
    }
    net.ready = true;
    net.send = send;
    copy_mac(net.mac, mac);
    for (int i = 0; i < 23 && driver && driver[i]; ++i) {
        net.driver[i] = driver[i];
        net.driver[i + 1] = '\0';
    }
    memset(&net.stats, 0, sizeof(net.stats));
    net.stats.link_up = true;
    copy_mac(net.stats.mac, mac);
    net.stats.ipv4 = net.ipv4;
    net.stats.gateway = net.gateway;
    net.stats.netmask = net.netmask;
    return true;
}

void k64_net_unregister_device(void) {
    net.ready = false;
    net.send = NULL;
    net.stats.link_up = false;
}

bool k64_net_is_ready(void) {
    return net.ready;
}

void k64_net_receive_frame(const uint8_t* frame, uint16_t len) {
    uint16_t type;

    if (!net.ready || !frame || len < 14) {
        return;
    }
    if (!mac_eq(frame, net.mac) && !mac_is_broadcast(frame)) {
        return;
    }

    type = rd16be(frame + 12);
    net.stats.rx_frames++;
    net.stats.last_ethertype = type;
    net.stats.last_rx_len = len;

    if (type == ETH_TYPE_ARP && len >= 42) {
        uint16_t op = rd16be(frame + 20);
        uint32_t sender_ip = rd32be(frame + 28);
        uint32_t target_ip = rd32be(frame + 38);

        net.stats.rx_arp++;
        arp_remember(sender_ip, frame + 22);
        if (op == 1 && target_ip == net.ipv4) {
            (void)send_arp_reply(frame + 22, sender_ip);
        }
        return;
    }

    if (type == ETH_TYPE_IPV4 && len >= 34 && (frame[14] >> 4) == 4) {
        uint8_t ihl = (uint8_t)((frame[14] & 0x0Fu) * 4u);
        uint16_t ip_len = rd16be(frame + 16);
        uint8_t proto = frame[23];
        uint32_t src_ip = rd32be(frame + 26);
        uint32_t dst_ip = rd32be(frame + 30);

        if (ihl < 20 || ip_len < ihl || (uint16_t)(14u + ip_len) > len || dst_ip != net.ipv4) {
            return;
        }

        arp_remember(src_ip, frame + 6);
        net.stats.rx_ipv4++;
        if (proto == IP_PROTO_UDP) {
            net.stats.rx_udp++;
        } else if (proto == IP_PROTO_ICMP) {
            const uint8_t* icmp = frame + 14 + ihl;
            uint16_t icmp_len = (uint16_t)(ip_len - ihl);
            net.stats.rx_icmp++;
            if (icmp_len >= 8 && icmp[0] == 8) {
                uint8_t reply[1480];
                memcpy(reply, icmp, icmp_len);
                reply[0] = 0;
                reply[2] = 0;
                reply[3] = 0;
                wr16be(reply + 2, csum16(reply, icmp_len));
                (void)send_ipv4(frame + 6, src_ip, IP_PROTO_ICMP, reply, icmp_len);
            }
        }
    }
}

bool k64_net_send_arp_request(uint32_t target_ip) {
    uint8_t frame[42];

    if (!net.ready) {
        return false;
    }
    memset(frame, 0, sizeof(frame));
    for (int i = 0; i < 6; ++i) {
        frame[i] = 0xFFu;
    }
    copy_mac(frame + 6, net.mac);
    wr16be(frame + 12, ETH_TYPE_ARP);
    wr16be(frame + 14, 1);
    wr16be(frame + 16, ETH_TYPE_IPV4);
    frame[18] = 6;
    frame[19] = 4;
    wr16be(frame + 20, 1);
    copy_mac(frame + 22, net.mac);
    wr32be(frame + 28, net.ipv4);
    wr32be(frame + 38, target_ip);
    return send_frame(frame, sizeof(frame));
}

bool k64_net_send_udp(uint32_t dst_ip, uint16_t dst_port, const char* payload) {
    uint8_t dst_mac[6];
    uint8_t packet[1480];
    uint16_t payload_len = (uint16_t)k64_strlen(payload);
    uint16_t udp_len;

    if (!payload) {
        payload = "";
        payload_len = 0;
    }
    if (payload_len > 1400u || !resolve_mac(dst_ip, dst_mac)) {
        return false;
    }

    udp_len = (uint16_t)(8u + payload_len);
    wr16be(packet + 0, 49152);
    wr16be(packet + 2, dst_port);
    wr16be(packet + 4, udp_len);
    wr16be(packet + 6, 0);
    memcpy(packet + 8, payload, payload_len);
    return send_ipv4(dst_mac, dst_ip, IP_PROTO_UDP, packet, udp_len);
}

bool k64_net_send_ping(uint32_t dst_ip, uint16_t sequence) {
    uint8_t dst_mac[6];
    uint8_t packet[32];

    if (!resolve_mac(dst_ip, dst_mac)) {
        return false;
    }
    memset(packet, 0, sizeof(packet));
    packet[0] = 8;
    packet[1] = 0;
    wr16be(packet + 4, net.next_icmp_id);
    wr16be(packet + 6, sequence);
    for (uint8_t i = 8; i < sizeof(packet); ++i) {
        packet[i] = (uint8_t)i;
    }
    wr16be(packet + 2, csum16(packet, sizeof(packet)));
    return send_ipv4(dst_mac, dst_ip, IP_PROTO_ICMP, packet, sizeof(packet));
}

bool k64_net_status(k64_net_status_t* out) {
    if (!out) {
        return false;
    }
    *out = net.stats;
    out->link_up = net.ready;
    for (int i = 0; i < 23 && net.driver[i]; ++i) {
        out->driver[i] = net.driver[i];
        out->driver[i + 1] = '\0';
    }
    return true;
}

bool k64_net_parse_ipv4(const char* text, uint32_t* out) {
    uint32_t parts[4] = {0, 0, 0, 0};
    int part = 0;
    bool saw_digit = false;

    if (!text || !out) {
        return false;
    }
    for (const char* p = text; ; ++p) {
        char ch = *p;
        if (ch >= '0' && ch <= '9') {
            saw_digit = true;
            parts[part] = parts[part] * 10u + (uint32_t)(ch - '0');
            if (parts[part] > 255u) {
                return false;
            }
        } else if (ch == '.' && part < 3 && saw_digit) {
            part++;
            saw_digit = false;
        } else if (ch == '\0' && part == 3 && saw_digit) {
            *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
            return true;
        } else {
            return false;
        }
    }
}

void k64_net_format_ipv4(uint32_t ip, char* out, size_t out_size) {
    size_t pos = 0;

    if (!out || out_size == 0) {
        return;
    }
    for (int i = 3; i >= 0; --i) {
        uint8_t part = (uint8_t)((ip >> (i * 8)) & 0xFFu);
        char tmp[4];
        int n = 0;
        if (part >= 100) {
            tmp[n++] = (char)('0' + part / 100);
            part %= 100;
            tmp[n++] = (char)('0' + part / 10);
            tmp[n++] = (char)('0' + part % 10);
        } else if (part >= 10) {
            tmp[n++] = (char)('0' + part / 10);
            tmp[n++] = (char)('0' + part % 10);
        } else {
            tmp[n++] = (char)('0' + part);
        }
        for (int j = 0; j < n && pos + 1 < out_size; ++j) {
            out[pos++] = tmp[j];
        }
        if (i > 0 && pos + 1 < out_size) {
            out[pos++] = '.';
        }
    }
    out[pos] = '\0';
}
