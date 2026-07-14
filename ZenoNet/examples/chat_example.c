#include "../include/zenonet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void on_connect_c(Connection *conn, void *userdata) {
    printf("[+] connected: %s\n", zn_conn_get_id(conn));
    zn_conn_send_data(conn, "{\"msg\":\"welcome to ZenoChat!\"}");
}

void on_disconnect_c(Connection *conn, void *userdata) {
    printf("[-] left: %s\n", zn_conn_get_id(conn));
}

void on_data_c(Connection *conn, Packet *pkt, void *userdata) {
    Server *srv = (Server *)userdata;
    char *msg_start = strstr(pkt->data, "\"msg\":\"");
    if (msg_start) {
        msg_start += 7;
        char *msg_end = strchr(msg_start, '"');
        if (msg_end) {
            *msg_end = '\0';
            const char *cid = zn_conn_get_id(conn);
            printf("[%.5s] %s\n", cid, msg_start);
            char buf[256];
            snprintf(buf, sizeof(buf), "{\"msg\":\"%.5s: %s\"}", cid, msg_start);
            zn_server_broadcast_data(srv, buf, conn);
        }
    }
}

int main(int argc, char **argv) {
    srand((unsigned int)time(NULL));
    uint16_t port = 7777;
    if (argc > 1) port = (uint16_t)atoi(argv[1]);
    Server *srv = zn_server_create(port);
    zn_server_on_connect(srv, on_connect_c, srv);
    zn_server_on_disconnect(srv, on_disconnect_c, srv);
    zn_server_on_data(srv, on_data_c, srv);
    printf("[*] chat on port %d\n", port);
    zn_server_start(srv);
    printf("[*] done\n");
    zn_server_destroy(srv);
    return 0;
}