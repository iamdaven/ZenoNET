#include "../include/zenonet.h"
#include <iostream>
#include <thread>
#include <chrono>
#include <random>
#include <unordered_map>

struct Player { float x = 0; float y = 0; float hp = 100; std::string name; };
std::unordered_map<std::string, Player> game_players;
std::mt19937 rng(std::random_device{}());

std::string j(const std::string &t, const std::string &e) {
    return "{\"type\":\"" + t + "\"," + e + "}";
}

int main() {
    zenonet::Server srv(8888);

    srv.on_connect([](zenonet::Connection &conn) {
        Player p;
        p.x = std::uniform_real_distribution<float>(0, 500)(rng);
        p.y = std::uniform_real_distribution<float>(0, 500)(rng);
        p.name = conn.id.substr(0, 5);
        game_players[conn.id] = p;
        std::cout << "[game] " << conn.id << " spawned\n";

        std::string join = j("player_join", "\"id\":\"" + conn.id + "\",\"x\":" +
            std::to_string(p.x) + ",\"y\":" + std::to_string(p.y) + ",\"name\":\"" + p.name + "\"");

        for (auto &[pid, pl] : game_players) if (pid != conn.id) {
            conn.send(join);
            break;
        }
        for (auto &[pid, pl] : game_players) if (pid != conn.id) {
            conn.send(j("player_join", "\"id\":\"" + pid + "\",\"x\":" +
                std::to_string(pl.x) + ",\"y\":" + std::to_string(pl.y) + ",\"name\":\"" + pl.name + "\""));
            break;
        }
    });

    srv.on_disconnect([&](zenonet::Connection &conn) {
        game_players.erase(conn.id);
        srv.broadcast(j("player_leave", "\"id\":\"" + conn.id + "\""), &conn);
        std::cout << "[game] " << conn.id << " left\n";
    });

    srv.on_data([&](zenonet::Connection &conn, zenonet::Packet &pkt) {
        auto it = game_players.find(conn.id);
        if (it == game_players.end()) return;
        Player &p = it->second;

        if (pkt.data_str.find("\"type\":\"move\"") != std::string::npos) {
            auto xp = pkt.data_str.find("\"x\":"), yp = pkt.data_str.find("\"y\":");
            if (xp != std::string::npos && yp != std::string::npos) {
                try {
                    p.x = std::stof(pkt.data_str.substr(xp + 4));
                    p.y = std::stof(pkt.data_str.substr(yp + 4));
                } catch (...) {}
            }
            srv.broadcast(j("move", "\"id\":\"" + conn.id + "\",\"x\":" +
                std::to_string(p.x) + ",\"y\":" + std::to_string(p.y)), &conn);
        }

        if (pkt.data_str.find("\"type\":\"attack\"") != std::string::npos) {
            auto tp = pkt.data_str.find("\"target\":\"");
            if (tp != std::string::npos) {
                std::string tid = pkt.data_str.substr(tp + 10);
                auto e = tid.find('"');
                if (e != std::string::npos) {
                    tid = tid.substr(0, e);
                    auto tgt = game_players.find(tid);
                    if (tgt != game_players.end()) {
                        int dmg = std::uniform_int_distribution<int>(5, 15)(rng);
                        tgt->second.hp -= dmg;
                        srv.broadcast(j("attack", "\"attacker\":\"" + conn.id +
                            "\",\"target\":\"" + tid + "\",\"damage\":" +
                            std::to_string(dmg) + ",\"hp\":" + std::to_string(tgt->second.hp)));
                        if (tgt->second.hp <= 0) {
                            srv.broadcast(j("death", "\"id\":\"" + tid +
                                "\",\"killer\":\"" + conn.id + "\""));
                            game_players.erase(tid);
                        }
                    }
                }
            }
        }
    });

    if (!srv.start()) { std::cerr << "[!] failed\n"; return 1; }
    std::cout << "[*] game server on 8888\n";
    while (true) std::this_thread::sleep_for(std::chrono::milliseconds(50));
}