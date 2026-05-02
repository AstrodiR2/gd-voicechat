#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <thread>
#include <atomic>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

using namespace geode::prelude;

#define SERVER_HOST "gd-voicechat-server-production.up.railway.app"
#define SERVER_PORT 443

static std::atomic<bool> g_connected(false);
static int g_socket = -1;

bool connectToServer() {
    #ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
    #endif

    struct addrinfo hints{}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(SERVER_HOST, std::to_string(SERVER_PORT).c_str(), &hints, &res) != 0)
        return false;

    g_socket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (g_socket < 0) {
        freeaddrinfo(res);
        return false;
    }

    if (connect(g_socket, res->ai_addr, res->ai_addrlen) < 0) {
        #ifdef _WIN32
        closesocket(g_socket);
        #else
        close(g_socket);
        #endif
        freeaddrinfo(res);
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

    char buf[512] = {};
    recv(g_socket, buf, sizeof(buf) - 1, 0);

    freeaddrinfo(res);
    g_connected = true;
    return true;
}

void disconnectFromServer() {
    if (g_socket >= 0) {
        #ifdef _WIN32
        closesocket(g_socket);
        #else
        close(g_socket);
        #endif
        g_socket = -1;
    }
    g_connected = false;
}

class $modify(VCMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto btn = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_deleteSoundBtn_001.png"),
            this,
            menu_selector(VCMenuLayer::onVoiceChat)
        );
        btn->setID("voicechat-btn");

        auto menu = this->getChildByID("bottom-menu");
        if (menu) {
            static_cast<CCMenu*>(menu)->addChild(btn);
            static_cast<CCMenu*>(menu)->updateLayout();
        }

        return true;
    }

    void onVoiceChat(CCObject*) {
        if (!g_connected) {
            std::thread([]() {
                if (connectToServer()) {
                    log::info("VoiceChat: connected to server!");
                } else {
                    log::error("VoiceChat: failed to connect!");
                }
            }).detach();
            FLAlertLayer::create(
                "VoiceChat",
                "Connecting to server...",
                "OK"
            )->show();
        } else {
            disconnectFromServer();
            FLAlertLayer::create(
                "VoiceChat",
                "Disconnected!",
                "OK"
            )->show();
        }
    }
};
