#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>

#ifdef GEODE_IS_ANDROID
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#endif

using namespace geode::prelude;

#define SERVER_HOST "tramway.proxy.rlwy.net"
#define SERVER_PORT 16282
#define FRAMES_PER_BUFFER 1024

static std::atomic<int> g_state(0);
static int g_socket = -1;
static std::atomic<bool> g_recording(false);
static std::atomic<bool> g_playing(false);

#ifdef GEODE_IS_ANDROID
static SLObjectItf g_engineObj = nullptr;
static SLEngineItf g_engine = nullptr;

// Recording
static SLObjectItf g_recorderObj = nullptr;
static SLRecordItf g_recorder = nullptr;
static SLAndroidSimpleBufferQueueItf g_recorderQueue = nullptr;
static std::vector<short> g_audioBuffer(FRAMES_PER_BUFFER);

// Playback
static SLObjectItf g_outputMixObj = nullptr;
static SLObjectItf g_playerObj = nullptr;
static SLPlayItf g_player = nullptr;
static SLAndroidSimpleBufferQueueItf g_playerQueue = nullptr;
static std::vector<short> g_playBuffer(FRAMES_PER_BUFFER);

void sendAudioFrame(const std::vector<short>& data) {
    if (g_socket < 0) return;
    std::vector<uint8_t> frame;
    size_t len = data.size() * 2;
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
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(data.data());
    for (size_t i = 0; i < len; i++)
        frame.push_back(raw[i] ^ mask[i % 4]);
    send(g_socket, frame.data(), frame.size(), 0);
}

void recordCallback(SLAndroidSimpleBufferQueueItf bq, void* context) {
    if (g_recording && g_socket >= 0) {
        sendAudioFrame(g_audioBuffer);
    }
    (*bq)->Enqueue(bq, g_audioBuffer.data(), g_audioBuffer.size() * 2);
}

void playAudioFrame(const uint8_t* data, size_t len) {
    if (!g_playing || !g_playerQueue) return;
    size_t copyLen = std::min(len, g_playBuffer.size() * 2);
    memcpy(g_playBuffer.data(), data, copyLen);
    (*g_playerQueue)->Enqueue(g_playerQueue, g_playBuffer.data(), copyLen);
}

void receiveLoop() {
    std::vector<uint8_t> buf(4096);
    while (g_state == 2 && g_socket >= 0) {
        int n = recv(g_socket, buf.data(), buf.size(), 0);
        if (n <= 0) break;
        if (n < 2) continue;

        uint8_t opcode = buf[0] & 0x0F;
        if (opcode != 0x2) continue;

        size_t payloadLen = buf[1] & 0x7F;
        size_t offset = 2;
        if (payloadLen == 126) {
            if (n < 4) continue;
            payloadLen = (buf[2] << 8) | buf[3];
            offset = 4;
        }

        if (n >= (int)(offset + payloadLen)) {
            playAudioFrame(buf.data() + offset, payloadLen);
        }
    }
}

bool startRecording() {
    SLresult result;

    result = slCreateEngine(&g_engineObj, 0, nullptr, 0, nullptr, nullptr);
    if (result != SL_RESULT_SUCCESS) {
        log::error("VoiceChat: slCreateEngine failed");
        return false;
    }

    result = (*g_engineObj)->Realize(g_engineObj, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) {
        (*g_engineObj)->Destroy(g_engineObj);
        g_engineObj = nullptr;
        return false;
    }

    result = (*g_engineObj)->GetInterface(g_engineObj, SL_IID_ENGINE, &g_engine);
    if (result != SL_RESULT_SUCCESS) {
        (*g_engineObj)->Destroy(g_engineObj);
        g_engineObj = nullptr;
        return false;
    }

    // Output mix для плеєра
    (*g_engine)->CreateOutputMix(g_engine, &g_outputMixObj, 0, nullptr, nullptr);
    (*g_outputMixObj)->Realize(g_outputMixObj, SL_BOOLEAN_FALSE);

    // Recorder
    SLDataLocator_IODevice loc = {
        SL_DATALOCATOR_IODEVICE,
        SL_IODEVICE_AUDIOINPUT,
        SL_DEFAULTDEVICEID_AUDIOINPUT,
        nullptr
    };
    SLDataSource recSrc = {&loc, nullptr};

    SLDataLocator_AndroidSimpleBufferQueue recBqLoc = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2
    };
    SLDataFormat_PCM fmt = {
        SL_DATAFORMAT_PCM, 1,
        SL_SAMPLINGRATE_16,
        SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_SPEAKER_FRONT_CENTER,
        SL_BYTEORDER_LITTLEENDIAN
    };
    SLDataSink recSink = {&recBqLoc, &fmt};

    const SLInterfaceID recIds[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean recReq[] = {SL_BOOLEAN_TRUE};

    result = (*g_engine)->CreateAudioRecorder(
        g_engine, &g_recorderObj, &recSrc, &recSink, 1, recIds, recReq
    );
    if (result != SL_RESULT_SUCCESS) {
        log::error("VoiceChat: CreateAudioRecorder failed: {}", result);
        (*g_engineObj)->Destroy(g_engineObj);
        g_engineObj = nullptr;
        return false;
    }

    result = (*g_recorderObj)->Realize(g_recorderObj, SL_BOOLEAN_FALSE);
    if (result != SL_RESULT_SUCCESS) {
        (*g_recorderObj)->Destroy(g_recorderObj);
        g_recorderObj = nullptr;
        (*g_engineObj)->Destroy(g_engineObj);
        g_engineObj = nullptr;
        return false;
    }

    (*g_recorderObj)->GetInterface(g_recorderObj, SL_IID_RECORD, &g_recorder);
    (*g_recorderObj)->GetInterface(g_recorderObj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &g_recorderQueue);
    (*g_recorderQueue)->RegisterCallback(g_recorderQueue, recordCallback, nullptr);
    (*g_recorderQueue)->Enqueue(g_recorderQueue, g_audioBuffer.data(), g_audioBuffer.size() * 2);
    (*g_recorder)->SetRecordState(g_recorder, SL_RECORDSTATE_RECORDING);
    g_recording = true;

    // Player
    SLDataLocator_AndroidSimpleBufferQueue playBqLoc = {
        SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2
    };
    SLDataSource playSrc = {&playBqLoc, &fmt};
    SLDataLocator_OutputMix outLoc = {SL_DATALOCATOR_OUTPUTMIX, g_outputMixObj};
    SLDataSink playSink = {&outLoc, nullptr};

    const SLInterfaceID playIds[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean playReq[] = {SL_BOOLEAN_TRUE};

    result = (*g_engine)->CreateAudioPlayer(
        g_engine, &g_playerObj, &playSrc, &playSink, 1, playIds, playReq
    );
    if (result != SL_RESULT_SUCCESS) {
        log::error("VoiceChat: CreateAudioPlayer failed: {}", result);
        return false;
    }

    (*g_playerObj)->Realize(g_playerObj, SL_BOOLEAN_FALSE);
    (*g_playerObj)->GetInterface(g_playerObj, SL_IID_PLAY, &g_player);
    (*g_playerObj)->GetInterface(g_playerObj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &g_playerQueue);
    (*g_player)->SetPlayState(g_player, SL_PLAYSTATE_PLAYING);
    g_playing = true;

    log::info("VoiceChat: recording + playback started!");
    return true;
}

void stopRecording() {
    g_recording = false;
    g_playing = false;
    if (g_recorder)
        (*g_recorder)->SetRecordState(g_recorder, SL_RECORDSTATE_STOPPED);
    if (g_player)
        (*g_player)->SetPlayState(g_player, SL_PLAYSTATE_STOPPED);
    if (g_playerObj) {
        (*g_playerObj)->Destroy(g_playerObj);
        g_playerObj = nullptr;
    }
    if (g_recorderObj) {
        (*g_recorderObj)->Destroy(g_recorderObj);
        g_recorderObj = nullptr;
    }
    if (g_outputMixObj) {
        (*g_outputMixObj)->Destroy(g_outputMixObj);
        g_outputMixObj = nullptr;
    }
    if (g_engineObj) {
        (*g_engineObj)->Destroy(g_engineObj);
        g_engineObj = nullptr;
    }
    g_recorder = nullptr;
    g_player = nullptr;
    g_engine = nullptr;
    g_recorderQueue = nullptr;
    g_playerQueue = nullptr;
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

#ifdef GEODE_IS_ANDROID
    std::thread(receiveLoop).detach();
#endif

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
                    if (!startRecording()) {
                        log::error("VoiceChat: mic failed!");
                    }
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
