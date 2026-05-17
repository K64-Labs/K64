#include "k64_net.h"
#include "k64_string.h"

#define ETH_TYPE_ARP  0x0806u
#define ETH_TYPE_IPV4 0x0800u
#define IP_PROTO_ICMP 1u
#define IP_PROTO_TCP  6u
#define IP_PROTO_UDP  17u
#define DHCP_CLIENT_PORT 68u
#define DHCP_SERVER_PORT 67u
#define DHCP_MAGIC 0x63825363u
#define TCP_FLAG_FIN  0x01u
#define TCP_FLAG_SYN  0x02u
#define TCP_FLAG_RST  0x04u
#define TCP_FLAG_PSH  0x08u
#define TCP_FLAG_ACK  0x10u
#define DNS_PORT      53u
#define DNS_SRC_PORT  49153u
#define HTTP_BUF_SIZE 1024u

typedef enum {
    HTTP_IDLE = 0,
    HTTP_RESOLVE,
    HTTP_ARP,
    HTTP_SYN_SENT,
    HTTP_ESTABLISHED,
    HTTP_REQUEST_SENT,
    HTTP_DONE,
    HTTP_ERROR
} http_state_t;

typedef struct {
    char     name[64];
    uint32_t ip;
} dns_cache_entry_t;

typedef struct {
    http_state_t state;
    char         host[64];
    char         path[128];
    uint32_t     ip;
    uint16_t     port;
    uint16_t     local_port;
    uint32_t     seq;
    uint32_t     ack;
    char         response[HTTP_BUF_SIZE];
    size_t       response_len;
} http_client_t;

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
    uint16_t              next_dns_id;
    uint16_t              next_tcp_port;
    uint32_t              dns_server;
    uint32_t              dhcp_xid;
    uint32_t              dhcp_server;
    uint32_t              dhcp_offer;
    uint8_t               arp_ip[8][4];
    uint8_t               arp_mac[8][6];
    uint8_t               arp_count;
    dns_cache_entry_t     dns_cache[8];
    uint8_t               dns_count;
    uint16_t              pending_dns_id;
    char                  pending_dns_name[64];
    http_client_t         http;
} k64_net_state_t;

static k64_net_state_t net;

static bool resolve_mac(uint32_t dst_ip, uint8_t mac[6]);
static bool send_dhcp_discover(void);
static bool send_dhcp_request(uint32_t requested_ip, uint32_t server_ip);

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

static uint32_t csum_add_bytes(uint32_t sum, const uint8_t* data, uint16_t len) {
    while (len > 1) {
        sum += rd16be(data);
        data += 2;
        len -= 2;
    }
    if (len) {
        sum += (uint16_t)data[0] << 8;
    }
    return sum;
}

static uint16_t csum_finish(uint32_t sum) {
    while (sum >> 16) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

static uint16_t tcp_checksum(uint32_t src_ip, uint32_t dst_ip, const uint8_t* tcp, uint16_t tcp_len) {
    uint8_t pseudo[12];
    uint32_t sum;

    wr32be(pseudo + 0, src_ip);
    wr32be(pseudo + 4, dst_ip);
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_TCP;
    wr16be(pseudo + 10, tcp_len);
    sum = csum_add_bytes(0, pseudo, sizeof(pseudo));
    sum = csum_add_bytes(sum, tcp, tcp_len);
    return csum_finish(sum);
}

static uint16_t udp_checksum(uint32_t src_ip, uint32_t dst_ip, const uint8_t* udp, uint16_t udp_len) {
    uint8_t pseudo[12];
    uint32_t sum;
    uint16_t result;

    wr32be(pseudo + 0, src_ip);
    wr32be(pseudo + 4, dst_ip);
    pseudo[8] = 0;
    pseudo[9] = IP_PROTO_UDP;
    wr16be(pseudo + 10, udp_len);
    sum = csum_add_bytes(0, pseudo, sizeof(pseudo));
    sum = csum_add_bytes(sum, udp, udp_len);
    result = csum_finish(sum);
    return result ? result : 0xFFFFu;
}

static void copy_mac(uint8_t dst[6], const uint8_t src[6]) {
    for (int i = 0; i < 6; ++i) {
        dst[i] = src[i];
    }
}

static void copy_text(char* dst, size_t dst_size, const char* src) {
    size_t i = 0;

    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    while (src[i] && i + 1 < dst_size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static bool text_eq(const char* a, const char* b) {
    return k64_strcmp(a ? a : "", b ? b : "") == 0;
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

static void dns_cache_put(const char* name, uint32_t ip) {
    uint8_t slot = 0;

    if (!name || !name[0] || ip == 0) {
        return;
    }
    for (uint8_t i = 0; i < net.dns_count; ++i) {
        if (text_eq(net.dns_cache[i].name, name)) {
            slot = i;
            goto found;
        }
    }
    if (net.dns_count < 8) {
        slot = net.dns_count++;
    }
found:
    copy_text(net.dns_cache[slot].name, sizeof(net.dns_cache[slot].name), name);
    net.dns_cache[slot].ip = ip;
}

static bool dns_cache_lookup(const char* name, uint32_t* ip) {
    for (uint8_t i = 0; i < net.dns_count; ++i) {
        if (text_eq(net.dns_cache[i].name, name)) {
            if (ip) {
                *ip = net.dns_cache[i].ip;
            }
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

static bool send_ipv4_from(const uint8_t dst_mac[6], uint32_t src_ip, uint32_t dst_ip, uint8_t proto, const uint8_t* payload, uint16_t payload_len) {
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
    wr32be(frame + 26, src_ip);
    wr32be(frame + 30, dst_ip);
    wr16be(frame + 24, csum16(frame + 14, 20));

    memcpy(frame + 34, payload, payload_len);
    return send_frame(frame, (uint16_t)(14u + ip_len));
}

static bool send_ipv4(const uint8_t dst_mac[6], uint32_t dst_ip, uint8_t proto, const uint8_t* payload, uint16_t payload_len) {
    return send_ipv4_from(dst_mac, net.ipv4, dst_ip, proto, payload, payload_len);
}

static bool send_tcp(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port, uint32_t seq, uint32_t ack, uint8_t flags, const uint8_t* payload, uint16_t payload_len) {
    uint8_t dst_mac[6];
    uint8_t packet[1480];
    uint16_t tcp_len = (uint16_t)(20u + payload_len);

    if (tcp_len > sizeof(packet) || !resolve_mac(dst_ip, dst_mac)) {
        return false;
    }
    memset(packet, 0, tcp_len);
    wr16be(packet + 0, src_port);
    wr16be(packet + 2, dst_port);
    wr32be(packet + 4, seq);
    wr32be(packet + 8, ack);
    packet[12] = 5u << 4;
    packet[13] = flags;
    wr16be(packet + 14, 4096);
    if (payload && payload_len) {
        memcpy(packet + 20, payload, payload_len);
    }
    wr16be(packet + 16, tcp_checksum(net.ipv4, dst_ip, packet, tcp_len));
    return send_ipv4(dst_mac, dst_ip, IP_PROTO_TCP, packet, tcp_len);
}

static bool send_dns_query(const char* host) {
    uint8_t dst_mac[6];
    uint8_t packet[512];
    size_t len;
    size_t label_start;
    size_t label_len_pos;
    uint16_t dns_id;

    if (!host || !host[0]) {
        return false;
    }
    if (!resolve_mac(net.gateway, dst_mac)) {
        return false;
    }

    memset(packet, 0, sizeof(packet));
    dns_id = net.next_dns_id++;
    wr16be(packet + 0, DNS_SRC_PORT);
    wr16be(packet + 2, DNS_PORT);
    wr16be(packet + 8, dns_id);
    wr16be(packet + 10, 0x0100);
    wr16be(packet + 12, 1);
    len = 20;
    label_len_pos = len++;
    label_start = len;
    for (size_t i = 0; host[i] && len + 6 < sizeof(packet); ++i) {
        if (host[i] == '.') {
            packet[label_len_pos] = (uint8_t)(len - label_start);
            label_len_pos = len++;
            label_start = len;
        } else {
            packet[len++] = (uint8_t)host[i];
        }
    }
    packet[label_len_pos] = (uint8_t)(len - label_start);
    packet[len++] = 0;
    wr16be(packet + len, 1);
    len += 2;
    wr16be(packet + len, 1);
    len += 2;
    wr16be(packet + 4, (uint16_t)(len - 8));
    wr16be(packet + 6, 0);
    wr16be(packet + 6, udp_checksum(net.ipv4, net.dns_server, packet, (uint16_t)len));

    net.pending_dns_id = dns_id;
    copy_text(net.pending_dns_name, sizeof(net.pending_dns_name), host);
    return send_ipv4(dst_mac, net.dns_server, IP_PROTO_UDP, packet, (uint16_t)len);
}

static size_t dns_skip_name(const uint8_t* dns, size_t dns_len, size_t pos) {
    while (pos < dns_len) {
        uint8_t len = dns[pos++];
        if (len == 0) {
            return pos;
        }
        if ((len & 0xC0u) == 0xC0u) {
            return pos + 1 <= dns_len ? pos + 1 : dns_len + 1;
        }
        pos += len;
    }
    return dns_len + 1;
}

static void handle_dns_payload(const uint8_t* udp, uint16_t udp_len) {
    const uint8_t* dns;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t dns_len;
    uint16_t id;
    uint16_t qd;
    uint16_t an;
    size_t pos;

    if (udp_len < 20) {
        return;
    }
    src_port = rd16be(udp + 0);
    dst_port = rd16be(udp + 2);
    if (src_port != DNS_PORT || dst_port != DNS_SRC_PORT) {
        return;
    }
    dns = udp + 8;
    dns_len = (uint16_t)(udp_len - 8u);
    id = rd16be(dns + 0);
    if (id != net.pending_dns_id) {
        return;
    }
    qd = rd16be(dns + 4);
    an = rd16be(dns + 6);
    pos = 12;
    while (qd-- && pos < dns_len) {
        pos = dns_skip_name(dns, dns_len, pos);
        pos += 4;
    }
    while (an-- && pos + 12 <= dns_len) {
        uint16_t type;
        uint16_t class_id;
        uint16_t rdlen;

        pos = dns_skip_name(dns, dns_len, pos);
        if (pos + 10 > dns_len) {
            return;
        }
        type = rd16be(dns + pos);
        class_id = rd16be(dns + pos + 2);
        rdlen = rd16be(dns + pos + 8);
        pos += 10;
        if (pos + rdlen > dns_len) {
            return;
        }
        if (type == 1 && class_id == 1 && rdlen == 4) {
            dns_cache_put(net.pending_dns_name, rd32be(dns + pos));
            net.pending_dns_name[0] = '\0';
            net.stats.rx_dns++;
            return;
        }
        pos += rdlen;
    }
}

static bool send_dhcp_message(uint8_t msg_type, uint32_t requested_ip, uint32_t server_ip) {
    uint8_t mac[6];
    uint8_t packet[548];
    size_t opt;
    uint16_t udp_len;
    uint32_t dst_ip = 0xFFFFFFFFu;

    if (!net.ready) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        mac[i] = 0xFFu;
    }
    memset(packet, 0, sizeof(packet));
    wr16be(packet + 0, DHCP_CLIENT_PORT);
    wr16be(packet + 2, DHCP_SERVER_PORT);
    packet[8] = 1;
    packet[9] = 1;
    packet[10] = 6;
    packet[11] = 0;
    wr32be(packet + 12, net.dhcp_xid);
    wr16be(packet + 18, 0x8000);
    copy_mac(packet + 36, net.mac);
    wr32be(packet + 244, DHCP_MAGIC);
    opt = 248;
    packet[opt++] = 53;
    packet[opt++] = 1;
    packet[opt++] = msg_type;
    if (requested_ip) {
        packet[opt++] = 50;
        packet[opt++] = 4;
        wr32be(packet + opt, requested_ip);
        opt += 4;
    }
    if (server_ip) {
        packet[opt++] = 54;
        packet[opt++] = 4;
        wr32be(packet + opt, server_ip);
        opt += 4;
    }
    packet[opt++] = 55;
    packet[opt++] = 3;
    packet[opt++] = 1;
    packet[opt++] = 3;
    packet[opt++] = 6;
    packet[opt++] = 255;
    udp_len = (uint16_t)opt;
    wr16be(packet + 4, udp_len);
    wr16be(packet + 6, 0);
    wr16be(packet + 6, udp_checksum(0, dst_ip, packet, udp_len));
    return send_ipv4_from(mac, 0, dst_ip, IP_PROTO_UDP, packet, udp_len);
}

static bool send_dhcp_discover(void) {
    net.dhcp_xid++;
    net.dhcp_offer = 0;
    net.dhcp_server = 0;
    return send_dhcp_message(1, 0, 0);
}

static bool send_dhcp_request(uint32_t requested_ip, uint32_t server_ip) {
    return send_dhcp_message(3, requested_ip, server_ip);
}

static void handle_dhcp_payload(const uint8_t* udp, uint16_t udp_len) {
    const uint8_t* bootp;
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t bootp_len;
    uint32_t xid;
    uint32_t yiaddr;
    uint8_t msg_type = 0;
    uint32_t server = 0;
    uint32_t router = 0;
    uint32_t mask = 0;
    uint32_t dns = 0;
    size_t pos;

    if (udp_len < 252) {
        return;
    }
    src_port = rd16be(udp + 0);
    dst_port = rd16be(udp + 2);
    if (src_port != DHCP_SERVER_PORT || dst_port != DHCP_CLIENT_PORT) {
        return;
    }
    bootp = udp + 8;
    bootp_len = (uint16_t)(udp_len - 8u);
    xid = rd32be(bootp + 4);
    if (xid != net.dhcp_xid || rd32be(bootp + 236) != DHCP_MAGIC) {
        return;
    }
    yiaddr = rd32be(bootp + 16);
    pos = 240;
    while (pos + 1 < bootp_len) {
        uint8_t opt = bootp[pos++];
        uint8_t len;
        if (opt == 255) {
            break;
        }
        if (opt == 0) {
            continue;
        }
        len = bootp[pos++];
        if (pos + len > bootp_len) {
            break;
        }
        if (opt == 53 && len >= 1) {
            msg_type = bootp[pos];
        } else if (opt == 54 && len >= 4) {
            server = rd32be(bootp + pos);
        } else if (opt == 1 && len >= 4) {
            mask = rd32be(bootp + pos);
        } else if (opt == 3 && len >= 4) {
            router = rd32be(bootp + pos);
        } else if (opt == 6 && len >= 4) {
            dns = rd32be(bootp + pos);
        }
        pos += len;
    }
    if (msg_type == 2 && yiaddr) {
        net.dhcp_offer = yiaddr;
        net.dhcp_server = server;
        (void)send_dhcp_request(yiaddr, server);
    } else if (msg_type == 5 && yiaddr) {
        net.ipv4 = yiaddr;
        if (router) {
            net.gateway = router;
        }
        if (mask) {
            net.netmask = mask;
        }
        if (dns) {
            net.dns_server = dns;
        }
        net.stats.ipv4 = net.ipv4;
        net.stats.gateway = net.gateway;
        net.stats.netmask = net.netmask;
        net.stats.dns_server = net.dns_server;
    }
}

static void http_append(const uint8_t* payload, uint16_t len) {
    size_t room;

    if (!payload || len == 0 || net.http.response_len + 1 >= sizeof(net.http.response)) {
        return;
    }
    room = sizeof(net.http.response) - 1u - net.http.response_len;
    if (len > room) {
        len = (uint16_t)room;
    }
    memcpy(net.http.response + net.http.response_len, payload, len);
    net.http.response_len += len;
    net.http.response[net.http.response_len] = '\0';
}

static void handle_tcp_payload(const uint8_t* ip_payload, uint16_t payload_len, uint32_t src_ip) {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint8_t hdr_len;
    uint8_t flags;
    const uint8_t* data;
    uint16_t data_len;

    if (payload_len < 20) {
        return;
    }
    src_port = rd16be(ip_payload + 0);
    dst_port = rd16be(ip_payload + 2);
    if (net.http.state == HTTP_IDLE || src_ip != net.http.ip || src_port != net.http.port || dst_port != net.http.local_port) {
        return;
    }
    seq = rd32be(ip_payload + 4);
    ack = rd32be(ip_payload + 8);
    hdr_len = (uint8_t)((ip_payload[12] >> 4) * 4u);
    flags = ip_payload[13];
    if (hdr_len < 20 || hdr_len > payload_len) {
        return;
    }
    data = ip_payload + hdr_len;
    data_len = (uint16_t)(payload_len - hdr_len);

    if ((flags & TCP_FLAG_RST) != 0) {
        net.http.state = HTTP_ERROR;
        return;
    }
    if (net.http.state == HTTP_SYN_SENT && (flags & (TCP_FLAG_SYN | TCP_FLAG_ACK)) == (TCP_FLAG_SYN | TCP_FLAG_ACK) && ack == net.http.seq + 1u) {
        net.http.seq++;
        net.http.ack = seq + 1u;
        (void)send_tcp(net.http.ip, net.http.port, net.http.local_port, net.http.seq, net.http.ack, TCP_FLAG_ACK, NULL, 0);
        net.http.state = HTTP_ESTABLISHED;
        return;
    }
    if ((net.http.state == HTTP_REQUEST_SENT || net.http.state == HTTP_ESTABLISHED) && data_len) {
        http_append(data, data_len);
        net.http.ack = seq + data_len;
        (void)send_tcp(net.http.ip, net.http.port, net.http.local_port, net.http.seq, net.http.ack, TCP_FLAG_ACK, NULL, 0);
    }
    if ((flags & TCP_FLAG_FIN) != 0) {
        net.http.ack = seq + data_len + 1u;
        (void)send_tcp(net.http.ip, net.http.port, net.http.local_port, net.http.seq, net.http.ack, TCP_FLAG_ACK, NULL, 0);
        net.http.state = HTTP_DONE;
    }
    (void)ack;
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
    net.dns_server = (10u << 24) | (0u << 16) | (2u << 8) | 3u;
    net.netmask = 0xFFFFFF00u;
    net.next_ip_id = 1;
    net.next_icmp_id = 1;
    net.next_dns_id = 1;
    net.next_tcp_port = 40000;
    net.dhcp_xid = 0x4B363400u;
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
    net.stats.dns_server = net.dns_server;
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

        if (ihl < 20 || ip_len < ihl || (uint16_t)(14u + ip_len) > len ||
            (dst_ip != net.ipv4 && dst_ip != 0xFFFFFFFFu)) {
            return;
        }

        arp_remember(src_ip, frame + 6);
        net.stats.rx_ipv4++;
        if (proto == IP_PROTO_UDP) {
            const uint8_t* udp = frame + 14 + ihl;
            uint16_t udp_len = (uint16_t)(ip_len - ihl);
            net.stats.rx_udp++;
            handle_dns_payload(udp, udp_len);
            handle_dhcp_payload(udp, udp_len);
        } else if (proto == IP_PROTO_ICMP) {
            const uint8_t* icmp = frame + 14 + ihl;
            uint16_t icmp_len = (uint16_t)(ip_len - ihl);
            net.stats.rx_icmp++;
            if (icmp_len >= 8 && icmp[0] == 0) {
                net.stats.ping_replies++;
            }
            if (icmp_len >= 8 && icmp[0] == 8) {
                uint8_t reply[1480];
                memcpy(reply, icmp, icmp_len);
                reply[0] = 0;
                reply[2] = 0;
                reply[3] = 0;
                wr16be(reply + 2, csum16(reply, icmp_len));
                (void)send_ipv4(frame + 6, src_ip, IP_PROTO_ICMP, reply, icmp_len);
            }
        } else if (proto == IP_PROTO_TCP) {
            net.stats.rx_tcp++;
            handle_tcp_payload(frame + 14 + ihl, (uint16_t)(ip_len - ihl), src_ip);
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

bool k64_net_dhcp_discover(void) {
    return send_dhcp_discover();
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
    wr16be(packet + 6, udp_checksum(net.ipv4, dst_ip, packet, udp_len));
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

bool k64_net_resolve_host(const char* host, uint32_t* out_ip, bool* pending) {
    if (pending) {
        *pending = false;
    }
    if (!host || !host[0] || !out_ip) {
        return false;
    }
    if (k64_net_parse_ipv4(host, out_ip)) {
        return true;
    }
    if (dns_cache_lookup(host, out_ip)) {
        return true;
    }
    if (send_dns_query(host)) {
        if (pending) {
            *pending = true;
        }
    }
    return false;
}

bool k64_net_http_get(const char* host, const char* path, uint16_t port, char* out, size_t out_size, const char** state) {
    uint8_t mac[6];
    char request[256];
    size_t len = 0;
    bool pending = false;

    if (state) {
        *state = "idle";
    }
    if (out && out_size) {
        out[0] = '\0';
    }
    if (!host || !host[0]) {
        if (state) {
            *state = "bad host";
        }
        return false;
    }
    if (!path || !path[0]) {
        path = "/";
    }
    if (port == 0) {
        port = 80;
    }
    if (net.http.state == HTTP_DONE && text_eq(net.http.host, host) && text_eq(net.http.path, path) && net.http.port == port) {
        if (out && out_size) {
            copy_text(out, out_size, net.http.response);
        }
        if (state) {
            *state = "done";
        }
        return true;
    }
    if (net.http.state == HTTP_IDLE || net.http.state == HTTP_ERROR ||
        !text_eq(net.http.host, host) || !text_eq(net.http.path, path) || net.http.port != port) {
        memset(&net.http, 0, sizeof(net.http));
        net.http.state = HTTP_RESOLVE;
        copy_text(net.http.host, sizeof(net.http.host), host);
        copy_text(net.http.path, sizeof(net.http.path), path);
        net.http.port = port;
        net.http.local_port = net.next_tcp_port++;
        net.http.seq = 0x4B640000u + net.http.local_port;
    }

    if (net.http.state == HTTP_RESOLVE) {
        if (!k64_net_resolve_host(net.http.host, &net.http.ip, &pending)) {
            if (state) {
                *state = pending ? "resolving dns" : "dns failed";
            }
            return false;
        }
        net.http.state = HTTP_ARP;
    }
    if (net.http.state == HTTP_ARP) {
        if (!resolve_mac(net.http.ip, mac)) {
            if (state) {
                *state = "resolving arp";
            }
            return false;
        }
        if (!send_tcp(net.http.ip, net.http.port, net.http.local_port, net.http.seq, 0, TCP_FLAG_SYN, NULL, 0)) {
            if (state) {
                *state = "tcp send failed";
            }
            return false;
        }
        net.http.state = HTTP_SYN_SENT;
    }
    if (net.http.state == HTTP_SYN_SENT) {
        if (state) {
            *state = "connecting";
        }
        return false;
    }
    if (net.http.state == HTTP_ESTABLISHED) {
        const char* a = "GET ";
        const char* b = " HTTP/1.0\r\nHost: ";
        const char* c = "\r\nUser-Agent: K64-kcurl/1.0\r\nConnection: close\r\n\r\n";
        for (size_t i = 0; a[i] && len + 1 < sizeof(request); ++i) request[len++] = a[i];
        for (size_t i = 0; net.http.path[i] && len + 1 < sizeof(request); ++i) request[len++] = net.http.path[i];
        for (size_t i = 0; b[i] && len + 1 < sizeof(request); ++i) request[len++] = b[i];
        for (size_t i = 0; net.http.host[i] && len + 1 < sizeof(request); ++i) request[len++] = net.http.host[i];
        for (size_t i = 0; c[i] && len + 1 < sizeof(request); ++i) request[len++] = c[i];
        request[len] = '\0';
        if (!send_tcp(net.http.ip, net.http.port, net.http.local_port, net.http.seq, net.http.ack, TCP_FLAG_PSH | TCP_FLAG_ACK, (const uint8_t*)request, (uint16_t)len)) {
            if (state) {
                *state = "request send failed";
            }
            return false;
        }
        net.http.seq += (uint32_t)len;
        net.http.state = HTTP_REQUEST_SENT;
    }
    if (net.http.state == HTTP_REQUEST_SENT) {
        if (net.http.response_len && out && out_size) {
            copy_text(out, out_size, net.http.response);
        }
        if (state) {
            *state = net.http.response_len ? "receiving" : "waiting";
        }
        return false;
    }
    if (net.http.state == HTTP_DONE) {
        if (out && out_size) {
            copy_text(out, out_size, net.http.response);
        }
        if (state) {
            *state = "done";
        }
        return true;
    }
    if (state) {
        *state = "error";
    }
    return false;
}

bool k64_net_status(k64_net_status_t* out) {
    if (!out) {
        return false;
    }
    *out = net.stats;
    out->link_up = net.ready;
    out->ipv4 = net.ipv4;
    out->gateway = net.gateway;
    out->netmask = net.netmask;
    out->dns_server = net.dns_server;
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
