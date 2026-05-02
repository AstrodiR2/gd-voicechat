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
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#endif

using namespace geode::prelude;

#define SERVER_HOST "tramway.proxy.rlwy.net"
#define SERVER_PORT 16282
#define SAMPLE_RATE 16000
#define FRAMES_PER_BUFFER 1024

static std::atomic<int> g_state(0);
static int g_socket = -1;
static std::atomic<bool> g_recording(false);

#ifdef GEODE_IS_ANDROID
static SLObjectItf g_engineObj = nullptr;
static SLEngineItf g_engine = nullptr;
static SLObjectItf g_recorderObj = nullptr;
static SLRecordItf g_recorder = nullptr;
static SLAndroidSimpleBufferQueueItf g_recorderQueue = nullptr;

static std::vector<short> g_audioBuffer(FRAMES_PER_BUFFER);

void sendAudioFrame(const std::vector<short>& data) {
    if (g_socket < 0) return;

    // WebSocket binary frame
    std::vector<uint8_t> frame;
    size_t len = data.size() * 2;
    frame.push_back(0x82);
    if (len < 126) {
        frame.push_back(0x80 | len);
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

bool startRecording() {
    slCreateEngine(&g_engineObj, 0, nullptr, 0, nullptr, nullptr);
    (*g_engineObj)->Realize(g_engineObj, SL_BOOLEAN_FALSE);
    (*g_engineObj)->GetInterface(g_engineObj, SL_IID_ENGINE, &g_engine);

    SLDataLocator_IODevice loc = {
        SL_DATALOCATOR_IODEVICE,
        SL_IODEVICE_AUDIOINPUT,
        SL_DEFAULTDEVICEID_AUDIOINPUT,
        nullptr
    };
    SLDataSource src = {&loc, nullptr};

    SLDataLocator_AndroidSimpleBufferQueue bqLoc = {
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
    SLDataSink sink = {&bqLoc, &fmt};

    const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean req[] = {SL_BOOLEAN_TRUE};

    (*g_engine)->CreateAudioRecorder(g_engine, &g_recorderObj, &src, &sink, 1, ids, req);
    (*g_recorderObj)->Realize(g_recorderObj, SL_BOOLEAN_FALSE);
    (*g_recorderObj)->GetInterface(g_recorderObj, SL_IID_RECORD, &g_recorder);
    (*g_recorderObj)->GetInterface(g_recorderObj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &g_recorderQueue);

    (*g_recorderQueue)->RegisterCallback(g_recorderQueue, recordCallback, nullptr);
    (*g_recorderQueue)->Enqueue(g_recorderQueue, g_audioBuffer.data(), g_audioBuffer.size() * 2);
    (*g_recorder)->SetRecordState(g_recorder, SL_RECORDSTATE_RECORDING);

    g_recording = true;
    return true;
}

void stopRecording() {
    g_recording = false;
    if (g_recorder)
        (*g_recorder)->SetRecordState(g_recorder, SL_RECORDSTATE_STOPPED);
    if (g_recorderObj) {
        (*g_recorderObj)->Destroy(g_recorderObj);
        g_recorderObj = nullptr;
    }
    if (g_engineObj) {
        (*g_engineObj)->Destroy(g_engineObj);
        g_engineObj = nullptr;
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
                    log::info("VoiceChat: connected + recording!");
                } else {
                    g_state = 0;
                    log::error("VoiceChat: failed!");
                }
            }).detach();
            FLAlertLayer::create("VoiceChat", "Connecting...", "OK")->show();
        } else if (g_state == 2) {
            disconnectFromServer();
            FLAlertLayer::create("VoiceChat", "Disconnected!", "OK")->show();
        }
    }
};
