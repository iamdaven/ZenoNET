#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../include/zenonet.h"

struct zn_room {
    char name[64];
    char password[64];
    int max_players;
    bool has_password;
    Connection **connections;
    int conn_count;
    int conn_cap;
};

Room *zn_room_create(const char *name, int max_players) {
    Room *room = (Room *)calloc(1, sizeof(Room));
    if (!room) return NULL;
    if (name) strncpy(room->name, name, sizeof(room->name) - 1);
    room->max_players = max_players > 0 ? max_players : 0;
    room->has_password = false;
    room->password[0] = '\0';
    room->conn_count = 0;
    room->conn_cap = 16;
    room->connections = (Connection **)calloc(room->conn_cap, sizeof(Connection *));
    if (!room->connections) { free(room); return NULL; }
    return room;
}

Room *zn_room_create_with_pw(const char *name, int max_players, const char *password) {
    Room *room = zn_room_create(name, max_players);
    if (room && password) {
        strncpy(room->password, password, sizeof(room->password) - 1);
        room->has_password = true;
    }
    return room;
}

void zn_room_destroy(Room *room) {
    if (room) { if (room->connections) free(room->connections); free(room); }
}

bool zn_room_add(Room *room, Connection *conn) {
    if (!room || !conn) return false;
    if (room->max_players > 0 && room->conn_count >= room->max_players) return false;
    for (int i = 0; i < room->conn_count; i++) {
        if (room->connections[i] == conn) return false;
    }
    if (room->conn_count >= room->conn_cap) {
        room->conn_cap *= 2;
        Connection **new_arr = (Connection **)realloc(room->connections, room->conn_cap * sizeof(Connection *));
        if (!new_arr) return false;
        room->connections = new_arr;
    }
    room->connections[room->conn_count++] = conn;
    return true;
}

bool zn_room_remove(Room *room, Connection *conn) {
    if (!room || !conn) return false;
    for (int i = 0; i < room->conn_count; i++) {
        if (room->connections[i] == conn) {
            room->connections[i] = room->connections[room->conn_count - 1];
            room->conn_count--;
            return true;
        }
    }
    return false;
}

void zn_room_broadcast(Room *room, Packet *pkt, Connection *exclude) {
    if (!room || !pkt) return;
    for (int i = 0; i < room->conn_count; i++) {
        if (room->connections[i] != exclude) zn_conn_send(room->connections[i], pkt);
    }
}

void zn_room_broadcast_data(Room *room, const char *json_data, Connection *exclude) {
    Packet *pkt = zn_packet_create(PKT_DATA, json_data);
    if (pkt) { zn_room_broadcast(room, pkt, exclude); zn_packet_free(pkt); }
}

int zn_room_count(Room *room) { return room ? room->conn_count : 0; }
const char *zn_room_name(Room *room) { return room ? room->name : ""; }