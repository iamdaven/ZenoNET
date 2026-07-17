#include "../include/zenonet.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <thread>
#include <chrono>
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace zenonet {

static int cpp_winsock_refs = 0;
#ifdef _WIN32
static void cpp_ensure_winsock(void) {
    if (cpp_winsock_refs++ == 0) { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); }
}
#else
static void cpp_ensure_winsock(void) {}
#endif

Packet::Packet(PacketType type, const std::string &data)
    : type(type), data_str(data), sequence(0) {
    static const char chars[] = "abcdef0123456789";
    id.resize(8);
    for (int i = 0; i < 8; i++) id[i] = chars[rand() % 16];
}

Packet::Packet(const Packet &other) {
    id = other.id; type = other.type; room = other.room;
    target = other.target; sequence = other.sequence; data_str = other.data_str;
}

Packet::~Packet() {}

std::vector<uint8_t> Packet::serialize() {
    std::string header = "{\"id\":\"" + id + "\",\"type\":" + std::to_string((int)type) +
        ",\"room\":\"" + room + "\",\"target\":\"" + target +
        "\",\"seq\":" + std::to_string(sequence) + "}";
    uint16_t hlen = (uint16_t)header.size();
    uint32_t total = 4 + 2 + hlen + (uint32_t)data_str.size() + 1;
    std::vector<uint8_t> buf(total);
    memcpy(&buf[0], &total, 4); memcpy(&buf[4], &hlen, 2);
    memcpy(&buf[6], header.data(), hlen);
    memcpy(&buf[6 + hlen], data_str.data(), data_str.size() + 1);
    return buf;
}

Packet Packet::deserialize(const std::vector<uint8_t> &raw) {
    if (raw.size() < 6) return Packet(PKT_ERROR);
    uint16_t hlen; memcpy(&hlen, &raw[4], 2);
    if (6 + hlen > raw.size()) return Packet(PKT_ERROR);
    std::string header((const char *)&raw[6], hlen);
    Packet pkt(PKT_DATA);
    auto fv = [&](const std::string &k) -> std::string {
        auto p = header.find("\"" + k + "\":\"");
        if (p != std::string::npos) { p += k.size() + 5; auto e = header.find('"', p); if (e != std::string::npos) return header.substr(p, e - p); }
        p = header.find("\"" + k + "\":");
        if (p != std::string::npos) { p += k.size() + 3; auto e = header.find_first_of(",}", p); if (e != std::string::npos) return header.substr(p, e - p); }
        return "";
    };
    pkt.id = fv("id"); if (pkt.id.empty()) pkt.id = "anon";
    std::string ts = fv("type"); if (!ts.empty()) pkt.type = (PacketType)std::stoi(ts);
    pkt.room = fv("room"); pkt.target = fv("target");
    std::string ss = fv("seq"); if (!ss.empty()) pkt.sequence = (uint32_t)std::stoul(ss);
    uint32_t ds = 6 + hlen;
    if (ds < raw.size()) pkt.data_str = std::string((const char *)&raw[ds]);
    return pkt;
}

std::unique_ptr<Packet> Packet::deserialize_ptr(const std::vector<uint8_t> &raw) {
    return std::make_unique<Packet>(deserialize(raw));
}

Connection::Connection(int sockfd, const std::string &addr, uint16_t port)
    : sockfd_(sockfd), addr(addr), port(port), userdata(nullptr), alive_(true) {
    static int c = 0; id = addr + ":" + std::to_string(port) + "-" + std::to_string(c++);
}
Connection::~Connection() { close(); }
bool Connection::send(const std::string &data) { Packet pkt(PKT_DATA, data); return send(pkt); }

bool Connection::send(Packet &pkt) {
    if (!alive_) return false;
    auto raw = pkt.serialize(); int sent = 0;
    while (sent < (int)raw.size()) {
        int n = ::send(sockfd_, (const char *)(raw.data() + sent), (int)(raw.size() - sent), 0);
        if (n <= 0) { alive_ = false; return false; } sent += n;
    }
    return true;
}

void Connection::close() { alive_ = false; #ifdef _WIN32 closesocket(sockfd_); #else ::close(sockfd_); #endif }
bool Connection::is_alive() const { return alive_; }

Room::Room(const std::string &name, int max_players) : name(name), max_players_(max_players) {}
Room::~Room() {}
bool Room::add(Connection &conn) {
    if (max_players_ > 0 && (int)connections_.size() >= max_players_) return false;
    if (connections_.find(conn.id) != connections_.end()) return false;
    connections_[conn.id] = &conn; return true;
}
bool Room::remove(Connection &conn) {
    auto it = connections_.find(conn.id);
    if (it != connections_.end()) { connections_.erase(it); return true; } return false;
}
void Room::broadcast(const std::string &data, Connection *exclude) {
    for (auto &[id, conn] : connections_) if (conn != exclude) conn->send(data);
}
int Room::player_count() const { return (int)connections_.size(); }

Server::Server(uint16_t port) : port_(port), sockfd_(-1), running_(false), host_("0.0.0.0") { cpp_ensure_winsock(); }
Server::~Server() { stop(); for (auto &[i,c] : connections) delete c; for (auto &[n,r] : rooms) delete r; }

bool Server::start() {
    sockfd_ = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0) return false;
    int opt = 1; setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof(opt));
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(port_); addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sockfd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) return false;
    listen(sockfd_, 128); running_ = true;
    printf("[*] C++ server on port %d\n", port_);
    std::thread(&Server::accept_loop, this).detach();
    return true;
}

void Server::stop() {
    running_ = false;
    if (sockfd_ >= 0) { #ifdef _WIN32 closesocket(sockfd_); #else ::close(sockfd_); #endif sockfd_ = -1; }
}

void Server::broadcast(const std::string &data, Connection *exclude) {
    for (auto &[id, conn] : connections) if (conn != exclude) conn->send(data);
}
Connection *Server::get_connection(const std::string &id) {
    auto it = connections.find(id); return it != connections.end() ? it->second : nullptr;
}
int Server::connection_count() const { return (int)connections.size(); }
void Server::on_connect(ConnCallback cb) { connect_cb_ = cb; }
void Server::on_disconnect(ConnCallback cb) { disconnect_cb_ = cb; }
void Server::on_data(DataCallback cb) { data_cb_ = cb; }

static void sock_setnonblock(int fd) {
    #ifdef _WIN32
    u_long mode = 1; ioctlsocket(fd, FIONBIO, &mode);
    #else
    int flags = fcntl(fd, F_GETFL, 0); fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    #endif
}

void Server::accept_loop() {
    fd_set readfds; struct timeval tv;
    while (running_) {
        FD_ZERO(&readfds); FD_SET(sockfd_, &readfds);
        tv.tv_sec = 0; tv.tv_usec = 500000;
        if (select(sockfd_ + 1, &readfds, NULL, NULL, &tv) <= 0) continue;
        struct sockaddr_in client_addr; socklen_t addr_len = sizeof(client_addr);
        int client_fd = (int)accept(sockfd_, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd < 0) continue;
        char addr_str[INET_ADDRSTRLEN]; inet_ntop(AF_INET, &client_addr.sin_addr, addr_str, sizeof(addr_str));
        sock_setnonblock(client_fd);
        Connection *conn = new Connection(client_fd, addr_str, ntohs(client_addr.sin_port));
        connections[conn->id] = conn;
        printf("[+] C++ conn: %s\n", conn->id.c_str());
        if (connect_cb_) connect_cb_(*conn);
        std::thread(&Server::read_loop, this, conn).detach();
    }
}

void Server::read_loop(Connection *conn) {
    uint8_t buf[4096];
    while (conn->is_alive() && running_) {
        int n = recv(conn->sockfd_, (char *)buf, sizeof(buf), 0);
        if (n <= 0) {
            if (disconnect_cb_) disconnect_cb_(*conn);
            connections.erase(conn->id); delete conn; break;
        }
        if (n >= 6) {
            uint32_t msg_len; memcpy(&msg_len, buf, 4);
            if (msg_len <= (uint32_t)n) {
                std::vector<uint8_t> pd(buf, buf + msg_len);
                auto pkt = Packet::deserialize(pd);
                if (pkt.type == PKT_PING) { Packet pong(PKT_PONG); conn->send(pong); }
                else if (data_cb_) data_cb_(*conn, pkt);
            }
        }
    }
}

Client::Client() : sockfd_(-1), connected_(false), port_(0) { cpp_ensure_winsock(); }
Client::~Client() { disconnect(); }

bool Client::connect(const std::string &host, uint16_t port, const std::string &name) {
    host_ = host; port_ = port; name_ = name;
    sockfd_ = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0) return false;
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host.c_str());
    if (::connect(sockfd_, (struct sockaddr *)&addr, sizeof(addr)) < 0) return false;
    connected_ = true;
    printf("[*] C++ client connected\n");
    if (connect_cb_) connect_cb_(*this);
    std::thread(&Client::read_loop, this).detach();
    std::thread(&Client::ping_loop, this).detach();
    return true;
}

void Client::disconnect() {
    connected_ = false;
    if (sockfd_ >= 0) { #ifdef _WIN32 closesocket(sockfd_); #else ::close(sockfd_); #endif sockfd_ = -1; }
    if (disconnect_cb_) disconnect_cb_(*this);
}

bool Client::send(const std::string &data) { Packet pkt(PKT_DATA, data); return send(pkt); }
bool Client::send(Packet &pkt) {
    if (!connected_) return false;
    auto raw = pkt.serialize(); int sent = 0;
    while (sent < (int)raw.size()) {
        int n = ::send(sockfd_, (const char *)(raw.data() + sent), (int)(raw.size() - sent), 0);
        if (n <= 0) { connected_ = false; return false; } sent += n;
    }
    return true;
}
bool Client::is_connected() const { return connected_; }
void Client::on_connect(ConnCallback cb) { connect_cb_ = cb; }
void Client::on_disconnect(ConnCallback cb) { disconnect_cb_ = cb; }
void Client::on_data(DataCallback cb) { data_cb_ = cb; }
void Client::on_message(std::function<void(const std::string&)> cb) { msg_cb_ = cb; }

void Client::read_loop() {
    uint8_t buf[4096];
    while (connected_) {
        int n = recv(sockfd_, (char *)buf, sizeof(buf), 0);
        if (n <= 0) { connected_ = false; if (disconnect_cb_) disconnect_cb_(*this); break; }
        if (n >= 6) {
            uint32_t msg_len; memcpy(&msg_len, buf, 4);
            if (msg_len <= (uint32_t)n) {
                std::vector<uint8_t> pd(buf, buf + msg_len);
                auto pkt = Packet::deserialize(pd);
                if (pkt.type == PKT_DATA) { if (msg_cb_) msg_cb_(pkt.data_str); if (data_cb_) data_cb_(*this, pkt); }
            }
        }
    }
}

void Client::ping_loop() {
    while (connected_) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (connected_) { Packet ping(PKT_PING); send(ping); }
    }
}

}