#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
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
static std::atomic<bool> g_micMuted(true);
static float g_othersVolume = 1.0f;

#ifdef GEODE_IS_ANDROID
static SLObjectItf g_engineObj = nullptr;
static SLEngineItf g_engine = nullptr;
static SLObjectItf g_recorderObj = nullptr;
static SLRecordItf g_recorder = nullptr;
static SLAndroidSimpleBufferQueueItf g_recorderQueue = nullptr;
static std::vector<short> g_audioBuffer(FRAMES_PER_BUFFER);
static SLObjectItf g_outputMixObj = nullptr;
static SLObjectItf g_playerObj = nullptr;
static SLPlayItf g_player = nullptr;
static SLAndroidSimpleBufferQueueItf g_playerQueue = nullptr;
static std::vector<short> g_playBuffer(FRAMES_PER_BUFFER);

void sendAudioFrame(const std::vector<short>& data) {
    if (g_socket < 0 || g_micMuted) return;
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
    if (g_recording && g_socket >= 0)
        sendAudioFrame(g_audioBuffer);
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
        if (n >= (int)(offset + payloadLen))
            playAudioFrame(buf.data() + offset, payloadLen);
    }
}

bool startRecording() {
    SLresult result;
    result = slCreateEngine(&g_engineObj, 0, nullptr, 0, nullptr, nullptr);
    if (result != SL_RESULT_SUCCESS) return false;
    (*g_engineObj)->Realize(g_engineObj, SL_BOOLEAN_FALSE);
    (*g_engineObj)->GetInterface(g_engineObj, SL_IID_ENGINE, &g_engine);
    (*g_engine)->CreateOutputMix(g_engine, &g_outputMixObj, 0, nullptr, nullptr);
    (*g_outputMixObj)->Realize(g_outputMixObj, SL_BOOLEAN_FALSE);

    SLDataLocator_IODevice loc = {SL_DATALOCATOR_IODEVICE, SL_IODEVICE_AUDIOINPUT, SL_DEFAULTDEVICEID_AUDIOINPUT, nullptr};
    SLDataSource recSrc = {&loc, nullptr};
    SLDataLocator_AndroidSimpleBufferQueue recBqLoc = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM fmt = {SL_DATAFORMAT_PCM, 1, SL_SAMPLINGRATE_16, SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN};
    SLDataSink recSink = {&recBqLoc, &fmt};
    const SLInterfaceID recIds[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean recReq[] = {SL_BOOLEAN_TRUE};

    result = (*g_engine)->CreateAudioRecorder(g_engine, &g_recorderObj, &recSrc, &recSink, 1, recIds, recReq);
    if (result != SL_RESULT_SUCCESS) { log::error("mic failed: {}", result); return false; }
    (*g_recorderObj)->Realize(g_recorderObj, SL_BOOLEAN_FALSE);
    (*g_recorderObj)->GetInterface(g_recorderObj, SL_IID_RECORD, &g_recorder);
    (*g_recorderObj)->GetInterface(g_recorderObj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &g_recorderQueue);
    (*g_recorderQueue)->RegisterCallback(g_recorderQueue, recordCallback, nullptr);
    (*g_recorderQueue)->Enqueue(g_recorderQueue, g_audioBuffer.data(), g_audioBuffer.size() * 2);
    (*g_recorder)->SetRecordState(g_recorder, SL_RECORDSTATE_RECORDING);
    g_recording = true;

    SLDataLocator_AndroidSimpleBufferQueue playBqLoc = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataSource playSrc = {&playBqLoc, &fmt};
    SLDataLocator_OutputMix outLoc = {SL_DATALOCATOR_OUTPUTMIX, g_outputMixObj};
    SLDataSink playSink = {&outLoc, nullptr};
    const SLInterfaceID playIds[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean playReq[] = {SL_BOOLEAN_TRUE};

    result = (*g_engine)->CreateAudioPlayer(g_engine, &g_playerObj, &playSrc, &playSink, 1, playIds, playReq);
    if (result != SL_RESULT_SUCCESS) return false;
    (*g_playerObj)->Realize(g_playerObj, SL_BOOLEAN_FALSE);
    (*g_playerObj)->GetInterface(g_playerObj, SL_IID_PLAY, &g_player);
    (*g_playerObj)->GetInterface(g_playerObj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &g_playerQueue);
    (*g_player)->SetPlayState(g_player, SL_PLAYSTATE_PLAYING);
    g_playing = true;
    return true;
}

void stopRecording() {
    g_recording = false;
    g_playing = false;
    if (g_recorder) (*g_recorder)->SetRecordState(g_recorder, SL_RECORDSTATE_STOPPED);
    if (g_player) (*g_player)->SetPlayState(g_player, SL_PLAYSTATE_STOPPED);
    if (g_playerObj) { (*g_playerObj)->Destroy(g_playerObj); g_playerObj = nullptr; }
    if (g_recorderObj) { (*g_recorderObj)->Destroy(g_recorderObj); g_recorderObj = nullptr; }
    if (g_outputMixObj) { (*g_outputMixObj)->Destroy(g_outputMixObj); g_outputMixObj = nullptr; }
    if (g_engineObj) { (*g_engineObj)->Destroy(g_engineObj); g_engineObj = nullptr; }
    g_recorder = nullptr; g_player = nullptr; g_engine = nullptr;
    g_recorderQueue = nullptr; g_playerQueue = nullptr;
}
#endif

bool connectToServer() {
    struct addrinfo hints{}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(SERVER_HOST, std::to_string(SERVER_PORT).c_str(), &hints, &res) != 0) return false;
    g_socket = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (g_socket < 0) { freeaddrinfo(res); return false; }
    if (connect(g_socket, res->ai_addr, res->ai_addrlen) < 0) { close(g_socket); freeaddrinfo(res); return false; }
    std::string handshake =
        "GET / HTTP/1.1\r\nHost: " SERVER_HOST "\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
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
    g_micMuted = true;
}

// ===== ГОЛОВНЕ МЕНЮ ВОЙСЧАТУ =====
class VoiceChatLayer : public CCLayer {
public:
    CCLabelBMFont* m_statusLabel = nullptr;
    CCNode* m_connectingNode = nullptr;
    CCNode* m_settingsNode = nullptr;
    int m_dotCount = 0;

    static VoiceChatLayer* create() {
        auto ret = new VoiceChatLayer();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }

    bool init() {
        if (!CCLayer::init()) return false;

        auto winSize = CCDirector::get()->getWinSize();

        // Темний фон
        auto bg = CCLayerColor::create({0, 0, 0, 200});
        this->addChild(bg, -1);

        // Заголовок
        auto title = CCLabelBMFont::create("VoiceChat", "goldFont.fnt");
        title->setScale(0.9f);
        title->setPosition({winSize.width / 2, winSize.height - 30});
        this->addChild(title);

        // Кнопка назад
        auto backBtn = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png"),
            this,
            menu_selector(VoiceChatLayer::onBack)
        );
        auto backMenu = CCMenu::create();
        backMenu->setPosition({25, winSize.height - 25});
        backMenu->addChild(backBtn);
        this->addChild(backMenu);

        // Нода для "Connecting..."
        m_connectingNode = CCNode::create();
        m_connectingNode->setPosition({winSize.width / 2, winSize.height / 2});
        this->addChild(m_connectingNode);

        m_statusLabel = CCLabelBMFont::create("Connecting.", "bigFont.fnt");
        m_statusLabel->setScale(0.6f);
        m_statusLabel->setColor({255, 255, 255});
        m_connectingNode->addChild(m_statusLabel);

        // Нода для налаштувань (прихована поки)
        m_settingsNode = CCNode::create();
        m_settingsNode->setVisible(false);
        this->addChild(m_settingsNode);

        this->buildSettingsUI();

        // Таймер для анімації крапок
        this->schedule(schedule_selector(VoiceChatLayer::updateDots), 0.5f);

        // Починаємо підключення
        this->startConnect();

        // Закрити при кліку на фон
        this->setTouchEnabled(true);

        return true;
    }

    void buildSettingsUI() {
        auto winSize = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_settingsNode->addChild(menu);

        // Статус підключення
        auto statusDot = CCLabelBMFont::create("● Connected", "bigFont.fnt");
        statusDot->setScale(0.5f);
        statusDot->setColor({0, 255, 100});
        statusDot->setPosition({winSize.width / 2, winSize.height / 2 + 60});
        m_settingsNode->addChild(statusDot);

        // Мікрофон кнопка
        auto micSpr = ButtonSprite::create(
            "Mic: OFF", "bigFont.fnt", "GJ_button_06.png", 0.8f
        );
        micSpr->setTag(200);
        auto micBtn = CCMenuItemSpriteExtra::create(
            micSpr, this,
            menu_selector(VoiceChatLayer::onToggleMic)
        );
        micBtn->setPosition({winSize.width / 2, winSize.height / 2 + 10});
        micBtn->setTag(201);
        menu->addChild(micBtn);

        // Гучність інших
        auto volLabel = CCLabelBMFont::create("Others Volume:", "bigFont.fnt");
        volLabel->setScale(0.4f);
        volLabel->setColor({200, 200, 200});
        volLabel->setPosition({winSize.width / 2, winSize.height / 2 - 35});
        m_settingsNode->addChild(volLabel);

        // Кнопки гучності
        auto volDown = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png"),
            this,
            menu_selector(VoiceChatLayer::onVolumeDown)
        );
        volDown->setPosition({winSize.width / 2 - 60, winSize.height / 2 - 60});

        auto volUp = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png"),
            this,
            menu_selector(VoiceChatLayer::onVolumeUp)
        );
        volUp->getNormalImage()->setFlipX(true);
        volUp->setPosition({winSize.width / 2 + 60, winSize.height / 2 - 60});

        auto volText = CCLabelBMFont::create("100%", "bigFont.fnt");
        volText->setScale(0.5f);
        volText->setTag(202);
        volText->setPosition({winSize.width / 2, winSize.height / 2 - 60});
        m_settingsNode->addChild(volText);

        menu->addChild(volDown);
        menu->addChild(volUp);

        // Кнопка відключитись
        auto discSpr = ButtonSprite::create(
            "Disconnect", "bigFont.fnt", "GJ_button_06.png", 0.8f
        );
        auto discBtn = CCMenuItemSpriteExtra::create(
            discSpr, this,
            menu_selector(VoiceChatLayer::onDisconnect)
        );
        discBtn->setPosition({winSize.width / 2, winSize.height / 2 - 100});
        menu->addChild(discBtn);
    }

    void startConnect() {
        g_state = 1;
        std::thread([this]() {
            bool ok = connectToServer();
            Loader::get()->queueInMainThread([this, ok]() {
                if (ok) {
                    g_state = 2;
#ifdef GEODE_IS_ANDROID
                    startRecording();
#endif
                    this->unschedule(schedule_selector(VoiceChatLayer::updateDots));
                    m_connectingNode->setVisible(false);
                    m_settingsNode->setVisible(true);
                } else {
                    g_state = 0;
                    this->unschedule(schedule_selector(VoiceChatLayer::updateDots));
                    m_statusLabel->setString("No internet connection!");
                    m_statusLabel->setColor({255, 80, 80});
                    m_statusLabel->setScale(0.45f);
                }
            });
        }).detach();
    }

    void updateDots(float) {
        m_dotCount = (m_dotCount % 3) + 1;
        std::string dots(m_dotCount, '.');
        m_statusLabel->setString(("Connecting" + dots).c_str());
    }

    void onToggleMic(CCObject*) {
        g_micMuted = !g_micMuted;
        auto winSize = CCDirector::get()->getWinSize();
        if (auto btn = m_settingsNode->getChildByTag(200)) {
            static_cast<ButtonSprite*>(btn)->updateBGImage(
                g_micMuted ? "GJ_button_06.png" : "GJ_button_01.png"
            );
            static_cast<ButtonSprite*>(btn)->setString(
                g_micMuted ? "Mic: OFF" : "Mic: ON"
            );
        }
    }

    void onVolumeDown(CCObject*) {
        g_othersVolume = std::max(0.0f, g_othersVolume - 0.1f);
        updateVolumeLabel();
    }

    void onVolumeUp(CCObject*) {
        g_othersVolume = std::min(2.0f, g_othersVolume + 0.1f);
        updateVolumeLabel();
    }

    void updateVolumeLabel() {
        if (auto lbl = static_cast<CCLabelBMFont*>(m_settingsNode->getChildByTag(202))) {
            int pct = (int)(g_othersVolume * 100);
            lbl->setString((std::to_string(pct) + "%").c_str());
        }
    }

    void onDisconnect(CCObject*) {
        createQuickPopup(
            "Disconnect",
            "Disconnect from VoiceChat?",
            "Cancel", "Yes",
            [this](auto, bool confirm) {
                if (confirm) {
                    disconnectFromServer();
                    this->onBack(nullptr);
                }
            }
        );
    }

    void onBack(CCObject*) {
        CCDirector::get()->popScene();
    }
};

// ===== КНОПКА МІК НА ПАУЗІ =====
class $modify(VCPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (g_state != 2) return;

        auto winSize = CCDirector::get()->getWinSize();
        auto label = CCLabelBMFont::create(
            g_micMuted ? "Mic OFF" : "Mic ON", "bigFont.fnt"
        );
        label->setScale(0.45f);
        label->setColor(g_micMuted ? ccColor3B{255, 80, 80} : ccColor3B{0, 255, 100});
        label->setID("vc-mic-label");

        auto btn = CCMenuItemSpriteExtra::create(
            label, this,
            menu_selector(VCPauseLayer::onToggleMic)
        );
        auto menu = CCMenu::create();
        menu->setPosition({winSize.width - 55, winSize.height - 25});
        menu->addChild(btn);
        this->addChild(menu, 100);
    }

    void onToggleMic(CCObject*) {
        g_micMuted = !g_micMuted;
        if (auto lbl = static_cast<CCLabelBMFont*>(this->getChildByID("vc-mic-label"))) {
            lbl->setString(g_micMuted ? "Mic OFF" : "Mic ON");
            lbl->setColor(g_micMuted ? ccColor3B{255, 80, 80} : ccColor3B{0, 255, 100});
        }
    }
};

// ===== КНОПКА В ГОЛОВНОМУ МЕНЮ =====
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
        if (g_state == 2) {
            // Вже підключений — показуємо налаштування
            auto scene = CCScene::create();
            auto layer = VoiceChatLayer::create();
            // Відразу показуємо налаштування без connecting
            layer->m_connectingNode->setVisible(false);
            layer->m_settingsNode->setVisible(true);
            layer->unschedule(schedule_selector(VoiceChatLayer::updateDots));
            scene->addChild(layer);
            CCDirector::get()->pushScene(scene);
        } else {
            // Нове підключення
            auto scene = CCScene::create();
            scene->addChild(VoiceChatLayer::create());
            CCDirector::get()->pushScene(scene);
        }
    }
};
