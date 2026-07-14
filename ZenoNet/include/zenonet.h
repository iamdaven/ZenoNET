#ifndef ZENONET_H
#define ZENONET_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#define ZN_VERSION "0.1.0"
#define ZN_MAX_PACKET 4096
#define ZN_MAX_ROOMS 128
#define ZN_MAX_CONNECTIONS 256
#define ZN_BUF_SIZE 8192

typedef enum {
    PKT_CONNECT = 1,
    PKT_DISCONNECT = 2,
    PKT_DATA = 3,
    PKT_ACK = 4,
    PKT_PING = 5,
    PKT_PONG = 6,
    PKT_JOIN_ROOM = 7,
    PKT_LEAVE_ROOM = 8,
    PKT_ROOM_LIST = 9,
    PKT_ERROR = 99
} PacketType;

typedef struct {
    char id[16];
    PacketType type;
    char room[64];
    char target[64];
    uint32_t sequence;
    uint32_t data_len;
    char *data;
} Packet;

typedef struct zn_conn Connection;
typedef struct zn_room Room;
typedef struct zn_server Server;
typedef struct zn_client Client;

typedef void (*conn_callback)(Connection *conn, void *userdata);
typedef void (*data_callback)(Connection *conn, Packet *pkt, void *userdata);
typedef void (*packet_callback)(Packet *pkt, void *userdata);
typedef void (*server_callback)(Server *srv, void *userdata);

Packet *zn_packet_create(PacketType type, const char *data_json);
Packet *zn_packet_create_with_room(PacketType type, const char *data_json, const char *room);
void zn_packet_free(Packet *pkt);
uint8_t *zn_packet_serialize(Packet *pkt, uint32_t *out_len);
Packet *zn_packet_deserialize(const uint8_t *raw, uint32_t len);

Connection *zn_conn_new(int sockfd, const char *addr, uint16_t port);
void zn_conn_free(Connection *conn);
bool zn_conn_send(Connection *conn, Packet *pkt);
bool zn_conn_send_data(Connection *conn, const char *json_data);
bool zn_conn_send_raw(Connection *conn, const uint8_t *data, uint32_t len);
void zn_conn_close(Connection *conn);
bool zn_conn_is_alive(Connection *conn);
void zn_conn_set_name(Connection *conn, const char *name);
const char *zn_conn_get_id(Connection *conn);
void zn_conn_set_userdata(Connection *conn, void *userdata);
void *zn_conn_get_userdata(Connection *conn);

Room *zn_room_create(const char *name, int max_players);
Room *zn_room_create_with_pw(const char *name, int max_players, const char *password);
void zn_room_destroy(Room *room);
bool zn_room_add(Room *room, Connection *conn);
bool zn_room_remove(Room *room, Connection *conn);
void zn_room_broadcast(Room *room, Packet *pkt, Connection *exclude);
void zn_room_broadcast_data(Room *room, const char *json_data, Connection *exclude);
int zn_room_count(Room *room);
const char *zn_room_name(Room *room);

Server *zn_server_create(uint16_t port);
Server *zn_server_create_host(const char *host, uint16_t port);
void zn_server_destroy(Server *srv);
bool zn_server_start(Server *srv);
void zn_server_stop(Server *srv);
void zn_server_broadcast(Server *srv, Packet *pkt, Connection *exclude);
void zn_server_broadcast_data(Server *srv, const char *data, Connection *exclude);
Connection *zn_server_get_conn(Server *srv, const char *id);
int zn_server_conn_count(Server *srv);
void zn_server_on_connect(Server *srv, conn_callback cb, void *userdata);
void zn_server_on_disconnect(Server *srv, conn_callback cb, void *userdata);
void zn_server_on_data(Server *srv, data_callback cb, void *userdata);

Client *zn_client_new(void);
void zn_client_free(Client *cli);
bool zn_client_connect(Client *cli, const char *host, uint16_t port);
bool zn_client_connect_name(Client *cli, const char *host, uint16_t port, const char *name);
void zn_client_disconnect(Client *cli);
bool zn_client_send(Client *cli, Packet *pkt);
bool zn_client_send_data(Client *cli, const char *json_data);
bool zn_client_join_room(Client *cli, const char *room_name);
bool zn_client_leave_room(Client *cli);
bool zn_client_is_connected(Client *cli);
void zn_client_on_connect(Client *cli, conn_callback cb, void *userdata);
void zn_client_on_disconnect(Client *cli, conn_callback cb, void *userdata);
void zn_client_on_data(Client *cli, data_callback cb, void *userdata);
void zn_client_on_message(Client *cli, packet_callback cb, void *userdata);

uint32_t zn_timestamp(void);
void zn_log(const char *fmt, ...);

#ifdef __cplusplus
}

#include <string>
#include <functional>
#include <memory>
#include <vector>
#include <unordered_map>

namespace zenonet {

class Connection;
class Room;
class Server;
class Client;

class Packet {
public:
    Packet(PacketType type = PKT_DATA, const std::string &data = "{}");
    Packet(const Packet &other);
    ~Packet();
    std::string id;
    PacketType type;
    std::string room;
    std::string target;
    uint32_t sequence;
    std::string data_str;
    std::vector<uint8_t> serialize();
    static Packet deserialize(const std::vector<uint8_t> &raw);
    static std::unique_ptr<Packet> deserialize_ptr(const std::vector<uint8_t> &raw);
};

class Connection {
public:
    Connection(int sockfd, const std::string &addr, uint16_t port);
    ~Connection();
    bool send(const std::string &data);
    bool send(Packet &pkt);
    void close();
    bool is_alive() const;
    std::string id;
    std::string name;
    std::string addr;
    uint16_t port;
    void *userdata;
private:
    int sockfd_;
    bool alive_;
    std::vector<uint8_t> buffer_;
    friend class Server;
    friend class Client;
};

typedef std::function<void(Connection&, Packet&)> DataCallback;
typedef std::function<void(Connection&)> ConnCallback;

class Room {
public:
    Room(const std::string &name, int max_players = 0);
    ~Room();
    bool add(Connection &conn);
    bool remove(Connection &conn);
    void broadcast(const std::string &data, Connection *exclude = nullptr);
    int player_count() const;
    std::string name;
    std::string password;
private:
    int max_players_;
    std::unordered_map<std::string, Connection*> connections_;
};

class Server {
public:
    Server(uint16_t port);
    ~Server();
    bool start();
    void stop();
    void broadcast(const std::string &data, Connection *exclude = nullptr);
    Connection *get_connection(const std::string &id);
    int connection_count() const;
    void on_connect(ConnCallback cb);
    void on_disconnect(ConnCallback cb);
    void on_data(DataCallback cb);
    std::unordered_map<std::string, Connection*> connections;
    std::unordered_map<std::string, Room*> rooms;
private:
    int sockfd_;
    bool running_;
    uint16_t port_;
    std::string host_;
    ConnCallback connect_cb_;
    ConnCallback disconnect_cb_;
    DataCallback data_cb_;
    void accept_loop();
    void read_loop(Connection *conn);
};

class Client {
public:
    Client();
    ~Client();
    bool connect(const std::string &host, uint16_t port, const std::string &name = "Player");
    void disconnect();
    bool send(const std::string &data);
    bool send(Packet &pkt);
    bool is_connected() const;
    void on_connect(ConnCallback cb);
    void on_disconnect(ConnCallback cb);
    void on_data(DataCallback cb);
    void on_message(std::function<void(const std::string&)> cb);
private:
    int sockfd_;
    bool connected_;
    std::string host_;
    uint16_t port_;
    std::string name_;
    std::vector<uint8_t> buffer_;
    ConnCallback connect_cb_;
    ConnCallback disconnect_cb_;
    DataCallback data_cb_;
    std::function<void(const std::string&)> msg_cb_;
    void read_loop();
    void ping_loop();
};

}

#endif

#endif