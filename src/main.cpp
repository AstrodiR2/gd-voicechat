#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <thread>
#include <atomic>
#include <vector>

#ifdef GEODE_IS_ANDROID
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#else
#include <AL/al.h>
#include <AL/alc.h>
#endif

// WebSocket через системний сокет
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

using namespace geode::prelude;

#define SERVER_HOST "gd-voicechat-server-production.up.railway.app"
#define SERVER_PORT 8080
#define SAMPLE_RATE 16000
#define BUFFER_SIZE 1024

static std::atomic<bool> g_connected(false);
static std::atomic<bool> g_talking(false);
static int g_socket = -1;

// Простий WebSocket handshake
bool connectToServer() {
    struct addrinfo hints{}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(SERVER_HOST, std::to_string(SERVER_PORT).c_str(), &hints, &res) != 0)
        return false;

    g_socket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (g_socket < 0) return false;

    if (connect(g_socket, res->ai_addr, res->ai_addrlen) < 0) {
        close(g_socket);
        return false;
    }

    // WebSocket handshake
    std::string handshake =
        "GET / HTTP/1.1\r\n"
        "Host: " SERVER_HOST "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";

    send(g_socket, handshake.c_str(), handshake.size(), 0);

    char buf[512];
    recv(g_socket, buf, sizeof(buf), 0);

    freeaddrinfo(res);
    g_connected = true;
    return true;
}

void sendAudio(const std::vector<uint8_t>& data) {
    if (!g_connected || g_socket < 0) return;

    // WebSocket frame (binary)
    std::vector<uint8_t> frame;
    frame.push_back(0x82); // FIN + binary opcode

    if (data.size() < 126) {
        frame.push_back(0x80 | data.size()); // masked
    } else {
        frame.push_back(0x80 | 126);
        frame.push_back((data.size() >> 8) & 0xFF);
        frame.push_back(data.size() & 0xFF);
    }

    // Маска
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    frame.insert(frame.end(), mask, mask + 4);

    for (size_t i = 0; i < data.size(); i++)
        frame.push_back(data[i] ^ mask[i % 4]);

    send(g_socket, frame.data(), frame.size(), 0);
}

// Кнопка войсчату в меню
class $modify(MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto spr = CCSprite::createWithSpriteFrameName("GJ_deleteSoundBtn_001.png");
        if (!spr) spr = CCSprite::create();
        spr->setScale(0.9f);

        auto btn = CCMenuItemSpriteExtra::create(
            spr, this,
            menu_selector(MenuLayer::onVoiceChat)
        );
        btn->setID("voicechat-btn");

        auto menu = this->getChildByID("bottom-menu");
        if (menu) {
            menu->addChild(btn);
            menu->updateLayout();
        }

        return true;
    }

    void onVoiceChat(CCObject*) {
        if (!g_connected) {
            std::thread([]() {
                if (connectToServer()) {
                    log::info("VoiceChat connected!");
                } else {
                    log::error("VoiceChat connection failed!");
                }
            }).detach();
            FLAlertLayer::create("VoiceChat", "Connecting to server...", "OK")->show();
        } else {
            close(g_socket);
            g_socket = -1;
            g_connected = false;
            FLAlertLayer::create("VoiceChat", "Disconnected!", "OK")->show();
        }
    }
};
