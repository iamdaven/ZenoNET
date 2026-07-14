#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif

#include "../include/zenonet.h"

static void gen_id(char *buf, int len) {
    const char *chars = "abcdef0123456789";
    for (int i = 0; i < len - 1; i++) {
        buf[i] = chars[rand() % 16];
    }
    buf[len - 1] = '\0';
}

Packet *zn_packet_create(PacketType type, const char *data_json) {
    Packet *pkt = (Packet *)calloc(1, sizeof(Packet));
    if (!pkt) return NULL;
    
    gen_id(pkt->id, sizeof(pkt->id));
    pkt->type = type;
    pkt->sequence = 0;
    pkt->room[0] = '\0';
    pkt->target[0] = '\0';
    
    if (data_json) {
        pkt->data_len = (uint32_t)strlen(data_json) + 1;
        pkt->data = (char *)malloc(pkt->data_len);
        if (pkt->data) {
            memcpy(pkt->data, data_json, pkt->data_len);
        }
    } else {
        pkt->data = strdup("{}");
        pkt->data_len = 3;
    }
    
    return pkt;
}

Packet *zn_packet_create_with_room(PacketType type, const char *data_json, const char *room) {
    Packet *pkt = zn_packet_create(type, data_json);
    if (pkt && room) {
        strncpy(pkt->room, room, sizeof(pkt->room) - 1);
    }
    return pkt;
}

void zn_packet_free(Packet *pkt) {
    if (pkt) {
        if (pkt->data) free(pkt->data);
        free(pkt);
    }
}

uint8_t *zn_packet_serialize(Packet *pkt, uint32_t *out_len) {
    char header_buf[512];
    int hlen = snprintf(header_buf, sizeof(header_buf), 
        "{\"id\":\"%s\",\"type\":%d,\"room\":\"%s\",\"target\":\"%s\",\"seq\":%u}",
        pkt->id, (int)pkt->type, pkt->room, pkt->target, pkt->sequence);
    
    if (hlen < 0 || hlen >= (int)sizeof(header_buf)) {
        return NULL;
    }
    
    uint16_t header_len = (uint16_t)hlen;
    uint32_t total = sizeof(uint32_t) + sizeof(uint16_t) + header_len + pkt->data_len;
    
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return NULL;
    
    uint32_t net_total = total;
    memcpy(buf, &net_total, 4);
    
    uint16_t net_hlen = header_len;
    memcpy(buf + 4, &net_hlen, 2);
    
    memcpy(buf + 6, header_buf, header_len);
    memcpy(buf + 6 + header_len, pkt->data, pkt->data_len);
    
    if (out_len) *out_len = total;
    return buf;
}

Packet *zn_packet_deserialize(const uint8_t *raw, uint32_t len) {
    if (len < 6) return NULL;
    
    uint16_t header_len;
    memcpy(&header_len, raw + 4, 2);
    
    if (6 + header_len + 1 > len) return NULL;
    
    Packet *pkt = (Packet *)calloc(1, sizeof(Packet));
    if (!pkt) return NULL;
    
    // super jank json parser
    char header_buf[512];
    uint16_t copy_len = header_len < (uint16_t)(sizeof(header_buf) - 1) ? header_len : (uint16_t)(sizeof(header_buf) - 1);
    memcpy(header_buf, raw + 6, copy_len);
    header_buf[copy_len] = '\0';
    
    char *id_start = strstr(header_buf, "\"id\":\"");
    if (id_start) {
        id_start += 6;
        char *id_end = strchr(id_start, '"');
        if (id_end && (id_end - id_start) < 16) {
            memcpy(pkt->id, id_start, id_end - id_start);
        }
    }
    
    char *type_start = strstr(header_buf, "\"type\":");
    if (type_start) {
        pkt->type = (PacketType)atoi(type_start + 7);
    }
    
    char *room_start = strstr(header_buf, "\"room\":\"");
    if (room_start) {
        room_start += 8;
        char *room_end = strchr(room_start, '"');
        if (room_end && (room_end - room_start) < 64) {
            memcpy(pkt->room, room_start, room_end - room_start);
        }
    }
    
    char *target_start = strstr(header_buf, "\"target\":\"");
    if (target_start) {
        target_start += 10;
        char *target_end = strchr(target_start, '"');
        if (target_end && (target_end - target_start) < 64) {
            memcpy(pkt->target, target_start, target_end - target_start);
        }
    }
    
    char *seq_start = strstr(header_buf, "\"seq\":");
    if (seq_start) {
        pkt->sequence = (uint32_t)atoi(seq_start + 6);
    }
    
    uint32_t data_len = len - 6 - header_len;
    if (data_len > 0 && data_len < ZN_MAX_PACKET) {
        pkt->data = (char *)malloc(data_len + 1);
        if (pkt->data) {
            memcpy(pkt->data, raw + 6 + header_len, data_len);
            pkt->data[data_len] = '\0';
            pkt->data_len = data_len + 1;
        }
    } else {
        pkt->data = strdup("{}");
        pkt->data_len = 3;
    }
    
    return pkt;
}