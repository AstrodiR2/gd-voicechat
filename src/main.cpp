#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

#ifdef GEODE_IS_ANDROID
#include <aaudio/AAudio.h>
#endif

using namespace geode::prelude;

#define SERVER_HOST "tramway.proxy.rlwy.net"
#define SERVER_PORT 16282
#define FRAMES_PER_BUFFER 1024

static std::atomic<int> g_state(0);
static int g_socket = -1;
static std::atomic<bool> g_recording(false);

#ifdef GEODE_IS_ANDROID
static AAudioStream* g_audioStream = nullptr;
static std::vector<int16_t> g_audioBuffer(FRAMES_PER_BUFFER);

void sendAudioFrame(const int16_t* data, int frames) {
    if (g_socket < 0) return;
    size_t len = frames * 2;
    std::vector<uint8_t> frame;
    frame.push_back(0x82);
    if (len < 126) {
        frame.push_back(0x80 | (uint8_t)len);
    } else {
        frame.push_back(0x80 | 126);
        frame.push_back((len >> 8) & 0xFF);
        frame.push_back(len & 0xFF);
    }
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    frame.insert(frame.end(), mask, mask + 4);
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; i++)
        frame.push_back(raw[i] ^ mask[i % 4]);
    send(g_socket, frame.data(), frame.size(), 0);
}

aaudio_data_callback_result_t audioCallback(
    AAudioStream* stream,
    void* userData,
    void* audioData,
    int32_t numFrames
) {
    if (g_recording && g_socket >= 0) {
        sendAudioFrame(reinterpret_cast<int16_t*>(audioData), numFrames);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

bool startRecording() {
    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result;

    result = AAudio_createStreamBuilder(&builder);
    if (result != AAUDIO_OK) {
        log::error("VoiceChat: createStreamBuilder failed: {}", result);
        return false;
    }

    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
    AAudioStreamBuilder_setSampleRate(builder, 16000);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setFramesPerDataCallback(builder, FRAMES_PER_BUFFER);
    AAudioStreamBuilder_setDataCallback(builder, audioCallback, nullptr);
    AAudioStreamBuilder_setInputPreset(builder, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);

    result = AAudioStreamBuilder_openStream(builder, &g_audioStream);
    AAudioStreamBuilder_delete(builder);

    if (result != AAUDIO_OK) {
        log::error("VoiceChat: openStream failed: {}", result);
        return false;
    }

    result = AAudioStream_requestStart(g_audioStream);
    if (result != AAUDIO_OK) {
        log::error("VoiceChat: requestStart failed: {}", result);
        AAudioStream_close(g_audioStream);
        g_audioStream = nullptr;
        return false;
    }

    g_recording = true;
    log::info("VoiceChat: AAudio recording started!");
    return true;
}

void stopRecording() {
    g_recording = false;
    if (g_audioStream) {
        AAudioStream_requestStop(g_audioStream);
        AAudioStream_close(g_audioStream);
        g_audioStream = nullptr;
    }
}
#endif

bool connectToServer() {
    struct addrinfo hints{}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(SERVER_HOST, std::to_string(SERVER_PORT).c_str(), &hints, &res) != 0)
        return false;

    g_socket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (g_socket < 0) { freeaddrinfo(res); return false; }

    if (connect(g_socket, res->ai_addr, res->ai_addrlen) < 0) {
        close(g_socket); freeaddrinfo(res); return false;
    }

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
    return true;
}

void disconnectFromServer() {
#ifdef GEODE_IS_ANDROID
    stopRecording();
#endif
    if (g_socket >= 0) { close(g_socket); g_socket = -1; }
    g_state = 0;
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
        if (g_state == 1) {
            FLAlertLayer::create("VoiceChat", "Still connecting...", "OK")->show();
            return;
        }
        if (g_state == 0) {
            g_state = 1;
            std::thread([]() {
                if (connectToServer()) {
                    g_state = 2;
#ifdef GEODE_IS_ANDROID
                    startRecording();
#endif
                    log::info("VoiceChat: connected!");
                } else {
                    g_state = 0;
                    log::error("VoiceChat: failed to connect!");
                }
            }).detach();
            FLAlertLayer::create("VoiceChat", "Connecting...", "OK")->show();
        } else if (g_state == 2) {
            disconnectFromServer();
            FLAlertLayer::create("VoiceChat", "Disconnected!", "OK")->show();
        }
    }
};
