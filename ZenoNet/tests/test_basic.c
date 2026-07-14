#include "../include/zenonet.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#define SLEEP(x) Sleep(x)
#else
#include <unistd.h>
#include <pthread.h>
#define SLEEP(x) usleep((x)*1000)
#endif

int tests_passed = 0;
int tests_failed = 0;

void test(const char *name, int condition) {
    if (condition) {
        printf("  PASS: %s\n", name);
        tests_passed++;
    } else {
        printf("  FAIL: %s\n", name);
        tests_failed++;
    }
}

void test_packet_create() {
    Packet *p = zn_packet_create(PKT_DATA, "{\"hello\":\"world\"}");
    test("packet create not null", p != NULL);
    test("packet type correct", p->type == PKT_DATA);
    test("packet has data", p->data != NULL);
    test("packet has id", strlen(p->id) > 0);
    test("packet data contains hello", strstr(p->data, "hello") != NULL);
    zn_packet_free(p);
}

void test_packet_serialize_deserialize() {
    Packet *p1 = zn_packet_create(PKT_DATA, "{\"msg\":\"test\"}");
    uint32_t len;
    uint8_t *raw = zn_packet_serialize(p1, &len);
    test("serialize returns data", raw != NULL);
    test("serialized length > 0", len > 0);

    Packet *p2 = zn_packet_deserialize(raw, len);
    test("deserialize not null", p2 != NULL);
    test("deserialized type matches", p2->type == p1->type);
    test("deserialized id matches", strcmp(p2->id, p1->id) == 0);
    test("deserialized data matches", strcmp(p2->data, p1->data) == 0);

    free(raw);
    zn_packet_free(p1);
    zn_packet_free(p2);
}

void test_packet_with_room() {
    Packet *p = zn_packet_create_with_room(PKT_JOIN_ROOM, "{}", "lobby");
    test("room packet not null", p != NULL);
    test("room name set", strcmp(p->room, "lobby") == 0);
    test("type is JOIN_ROOM", p->type == PKT_JOIN_ROOM);
    zn_packet_free(p);
}

void test_room_basic() {
    Room *r = zn_room_create("test_room", 4);
    test("room created", r != NULL);
    test("room name correct", strcmp(zn_room_name(r), "test_room") == 0);
    test("room empty", zn_room_count(r) == 0);
    zn_room_destroy(r);
}

void test_room_add_remove() {
    Room *r = zn_room_create("test", 2);
    Connection *c1 = zn_conn_new(0, "127.0.0.1", 5001);
    Connection *c2 = zn_conn_new(0, "127.0.0.1", 5002);

    test("add first player", zn_room_add(r, c1));
    test("room has 1 player", zn_room_count(r) == 1);
    test("add second player", zn_room_add(r, c2));
    test("room has 2 players", zn_room_count(r) == 2);
    test("remove first player", zn_room_remove(r, c1));
    test("room has 1 player after remove", zn_room_count(r) == 1);

    zn_conn_free(c1);
    zn_conn_free(c2);
    zn_room_destroy(r);
}

void test_room_max_players() {
    Room *r = zn_room_create("small", 1);
    Connection *c1 = zn_conn_new(0, "127.0.0.1", 6001);
    Connection *c2 = zn_conn_new(0, "127.0.0.1", 6002);

    test("add to small room", zn_room_add(r, c1));
    test("reject when full", !zn_room_add(r, c2));

    zn_conn_free(c1);
    zn_conn_free(c2);
    zn_room_destroy(r);
}

void test_room_with_password() {
    Room *r = zn_room_create_with_pw("secure", 10, "hunter2");
    test("password room created", r != NULL);
    zn_room_destroy(r);
}

void test_connection_basics() {
    Connection *c = zn_conn_new(42, "192.168.1.1", 8888);
    test("connection not null", c != NULL);
    test("connection alive", zn_conn_is_alive(c));
    test("connection id not empty", strlen(zn_conn_get_id(c)) > 0);
    test("connection id contains addr", strstr(zn_conn_get_id(c), "192.168.1.1") != NULL);

    zn_conn_set_name(c, "test_player");
    zn_conn_set_userdata(c, (void*)0xDEADBEEF);
    test("userdata set", zn_conn_get_userdata(c) == (void*)0xDEADBEEF);

    zn_conn_close(c);
    test("connection not alive after close", !zn_conn_is_alive(c));
    zn_conn_free(c);
}

void test_client_basics() {
    Client *cli = zn_client_new();
    test("client not null", cli != NULL);
    test("client not connected", !zn_client_is_connected(cli));
    zn_client_free(cli);
}

void test_timestamp() {
    uint32_t ts = zn_timestamp();
    test("timestamp > 0", ts > 0);
    // this is a dumb test but whatever
    test("timestamp is reasonable", ts > 1000000);
}

int main() {
    srand((unsigned int)time(NULL));
    printf("\n=== ZenoNet C Tests ===\n\n");

    printf("[packet tests]\n");
    test_packet_create();
    test_packet_serialize_deserialize();
    test_packet_with_room();

    printf("\n[room tests]\n");
    test_room_basic();
    test_room_add_remove();
    test_room_max_players();
    test_room_with_password();

    printf("\n[connection tests]\n");
    test_connection_basics();

    printf("\n[client tests]\n");
    test_client_basics();

    printf("\n[utility tests]\n");
    test_timestamp();

    printf("\n=== Results ===\n");
    printf("  Passed: %d\n", tests_passed);
    printf("  Failed: %d\n", tests_failed);
    printf("  Total:  %d\n", tests_passed + tests_failed);

    return tests_failed > 0 ? 1 : 0;
}

// TODO: add actual network tests
// need to figure out how to test server/client without blocking forever
// maybe someday