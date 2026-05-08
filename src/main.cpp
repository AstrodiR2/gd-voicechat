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

// ===== DRAGGABLE MIC BUTTON =====
class DraggableMicBtn : public CCNode, public CCTouchDelegate {
public:
    CCNode* m_expandMenu = nullptr;
    CCSprite* m_micIcon = nullptr;
    bool m_dragging = false;
    bool m_expanded = false;
    CCPoint m_touchStart;
    CCPoint m_nodeStart;
    float m_dragThreshold = 10.f;

    static DraggableMicBtn* create() {
        auto ret = new DraggableMicBtn();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }

    bool init() {
        if (!CCNode::init()) return false;

        // Коло фон
        auto circle = CCSprite::createWithSpriteFrameName("GJ_button_01.png");
        if (!circle) circle = CCSprite::create();
        circle->setScale(1.2f);
        this->addChild(circle, 0);

        // Іконка мікрофону
        m_micIcon = CCSprite::createWithSpriteFrameName("GJ_deleteSoundBtn_001.png");
        if (!m_micIcon) m_micIcon = CCSprite::create();
        m_micIcon->setScale(0.7f);
        this->addChild(m_micIcon, 1);

        this->updateMicIcon();

        // Приховане меню з кнопками
        m_expandMenu = CCNode::create();
        m_expandMenu->setVisible(false);
        this->addChild(m_expandMenu, 2);

        // Кнопка вкл/викл мік
        auto micToggleMenu = CCMenu::create();
        micToggleMenu->setPosition({0, 50});

        auto micLbl = CCLabelBMFont::create(
            g_micMuted ? "Mic: OFF" : "Mic: ON", "bigFont.fnt"
        );
        micLbl->setScale(0.4f);
        micLbl->setTag(300);

        auto micToggleBtn = CCMenuItemSpriteExtra::create(
            micLbl, this,
            menu_selector(DraggableMicBtn::onToggleMic)
        );
        micToggleMenu->addChild(micToggleBtn);
        m_expandMenu->addChild(micToggleMenu);

        CCTouchDispatcher::get()->addTargetedDelegate(this, 0, true);
        return true;
    }

    void updateMicIcon() {
        if (m_micIcon) {
            m_micIcon->setColor(g_micMuted ? ccColor3B{255, 80, 80} : ccColor3B{0, 255, 100});
        }
    }

    void onToggleMic(CCObject*) {
        g_micMuted = !g_micMuted;
        updateMicIcon();
        if (auto lbl = static_cast<CCLabelBMFont*>(m_expandMenu->getChildByTag(300))) {
            lbl->setString(g_micMuted ? "Mic: OFF" : "Mic: ON");
        }
        m_expandMenu->setVisible(false);
        m_expanded = false;
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        auto loc = touch->getLocation();
        auto pos = this->getParent()->convertToNodeSpace(loc);
        auto myPos = this->getPosition();
        float dist = ccpDistance(pos, myPos);
        if (dist > 30.f) return false;

        m_dragging = false;
        m_touchStart = loc;
        m_nodeStart = myPos;
        return true;
    }

    void ccTouchMoved(CCTouch* touch, CCEvent*) override {
        auto loc = touch->getLocation();
        float dist = ccpDistance(loc, m_touchStart);
        if (dist > m_dragThreshold) {
            m_dragging = true;
            m_expanded = false;
            m_expandMenu->setVisible(false);
        }
        if (m_dragging) {
            auto delta = loc - m_touchStart;
            this->setPosition(m_nodeStart + delta);
        }
    }

    void ccTouchEnded(CCTouch* touch, CCEvent*) override {
        if (!m_dragging) {
            m_expanded = !m_expanded;
            m_expandMenu->setVisible(m_expanded);
        }
        m_dragging = false;
    }

    void ccTouchCancelled(CCTouch* touch, CCEvent* event) override {
        ccTouchEnded(touch, event);
    }

    ~DraggableMicBtn() {
        CCTouchDispatcher::get()->removeDelegate(this);
    }
};

// ===== VOLUME SLIDER =====
class VolumeSlider : public CCNode, public CCTouchDelegate {
public:
    CCSprite* m_track = nullptr;
    CCSprite* m_thumb = nullptr;
    CCLabelBMFont* m_label = nullptr;
    float m_width = 150.f;
    bool m_dragging = false;

    static VolumeSlider* create(float width = 150.f) {
        auto ret = new VolumeSlider();
        ret->m_width = width;
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }

    bool init() {
        if (!CCNode::init()) return false;

        // Track
        auto trackBg = CCScale9Sprite::create("square02_small.png");
        trackBg->setContentSize({m_width, 8});
        trackBg->setColor({60, 60, 60});
        trackBg->setPosition({m_width / 2, 0});
        this->addChild(trackBg);

        m_track = CCSprite::createWithSpriteFrameName("GJ_progressBar_001.png");
        if (!m_track) {
            auto fill = CCScale9Sprite::create("square02_small.png");
            fill->setColor({0, 200, 100});
            fill->setContentSize({m_width * g_othersVolume / 2.f, 8});
            fill->setAnchorPoint({0, 0.5f});
            fill->setPosition({0, 0});
            fill->setTag(401);
            this->addChild(fill);
        }

        // Thumb
        m_thumb = CCSprite::createWithSpriteFrameName("GJ_button_01.png");
        if (!m_thumb) m_thumb = CCSprite::create();
        m_thumb->setScale(0.4f);
        m_thumb->setPosition({m_width * g_othersVolume / 2.f, 0});
        this->addChild(m_thumb, 1);

        // Label
        m_label = CCLabelBMFont::create("100%", "bigFont.fnt");
        m_label->setScale(0.35f);
        m_label->setPosition({m_width + 30, 0});
        this->addChild(m_label);

        this->updateLabel();
        CCTouchDispatcher::get()->addTargetedDelegate(this, 0, true);
        return true;
    }

    void updateLabel() {
        int pct = (int)(g_othersVolume * 100);
        m_label->setString((std::to_string(pct) + "%").c_str());
        float x = (g_othersVolume / 2.f) * m_width;
        m_thumb->setPositionX(x);
        if (auto fill = static_cast<CCScale9Sprite*>(this->getChildByTag(401))) {
            fill->setContentSize({x, 8});
        }
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        auto loc = this->convertToNodeSpace(touch->getLocation());
        if (loc.x >= -10 && loc.x <= m_width + 10 && fabsf(loc.y) < 20) {
            m_dragging = true;
            this->updateFromTouch(loc.x);
            return true;
        }
        return false;
    }

    void ccTouchMoved(CCTouch* touch, CCEvent*) override {
        if (m_dragging) {
            auto loc = this->convertToNodeSpace(touch->getLocation());
            this->updateFromTouch(loc.x);
        }
    }

    void ccTouchEnded(CCTouch*, CCEvent*) override { m_dragging = false; }
    void ccTouchCancelled(CCTouch*, CCEvent*) override { m_dragging = false; }

    void updateFromTouch(float x) {
        x = std::max(0.f, std::min(x, m_width));
        g_othersVolume = (x / m_width) * 2.f;
        updateLabel();
    }

    ~VolumeSlider() {
        CCTouchDispatcher::get()->removeDelegate(this);
    }
};

// ===== MAIN VOICECHAT LAYER =====
class VoiceChatLayer : public CCLayer {
public:
    CCLabelBMFont* m_connectingLabel = nullptr;
    CCNode* m_connectingNode = nullptr;
    CCNode* m_settingsNode = nullptr;
    int m_dotCount = 0;
    DraggableMicBtn* m_micBtn = nullptr;

    static VoiceChatLayer* create() {
        auto ret = new VoiceChatLayer();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }

    bool init() {
        if (!CCLayer::init()) return false;

        auto winSize = CCDirector::get()->getWinSize();

        // GD стиль фон — градієнт
        auto bg = CCLayerGradient::create(
            ccc4(5, 15, 40, 255),
            ccc4(10, 30, 80, 255)
        );
        this->addChild(bg, -2);

        // Декоративна рамка
        auto frame = CCScale9Sprite::create("GJ_square01.png");
        frame->setContentSize({winSize.width - 20, winSize.height - 20});
        frame->setPosition({winSize.width / 2, winSize.height / 2});
        frame->setOpacity(180);
        this->addChild(frame, -1);

        // Заголовок в стилі GD
        auto titleBg = CCScale9Sprite::create("GJ_square02.png");
        titleBg->setContentSize({220, 40});
        titleBg->setPosition({winSize.width / 2, winSize.height - 25});
        titleBg->setOpacity(200);
        this->addChild(titleBg);

        auto title = CCLabelBMFont::create("VoiceChat", "goldFont.fnt");
        title->setScale(0.7f);
        title->setPosition({winSize.width / 2, winSize.height - 25});
        this->addChild(title, 1);

        // Кнопка назад
        auto backSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        auto backBtn = CCMenuItemSpriteExtra::create(
            backSpr, this,
            menu_selector(VoiceChatLayer::onBack)
        );
        auto backMenu = CCMenu::create();
        backMenu->setPosition({25, winSize.height - 25});
        backMenu->addChild(backBtn);
        this->addChild(backMenu, 2);

        // ===== CONNECTING NODE =====
        m_connectingNode = CCNode::create();
        this->addChild(m_connectingNode, 1);

        // Connecting панель
        auto connBg = CCScale9Sprite::create("GJ_square02.png");
        connBg->setContentSize({250, 100});
        connBg->setPosition({winSize.width / 2, winSize.height / 2});
        connBg->setOpacity(200);
        m_connectingNode->addChild(connBg);

        m_connectingLabel = CCLabelBMFont::create("Connecting.", "bigFont.fnt");
        m_connectingLabel->setScale(0.55f);
        m_connectingLabel->setColor({255, 255, 255});
        m_connectingLabel->setPosition({winSize.width / 2, winSize.height / 2 + 10});
        m_connectingNode->addChild(m_connectingLabel);

        auto connSubLabel = CCLabelBMFont::create("Connecting to server...", "chatFont.fnt");
        connSubLabel->setScale(0.6f);
        connSubLabel->setColor({150, 180, 255});
        connSubLabel->setPosition({winSize.width / 2, winSize.height / 2 - 20});
        m_connectingNode->addChild(connSubLabel);

        // ===== SETTINGS NODE =====
        m_settingsNode = CCNode::create();
        m_settingsNode->setVisible(false);
        this->addChild(m_settingsNode, 1);

        this->buildSettings();

        // Анімація крапок
        this->schedule(schedule_selector(VoiceChatLayer::updateDots), 0.5f);

        // Починаємо підключення
        if (g_state != 2) {
            this->startConnect();
        } else {
            m_connectingNode->setVisible(false);
            m_settingsNode->setVisible(true);
            this->unschedule(schedule_selector(VoiceChatLayer::updateDots));
        }

        return true;
    }

    void buildSettings() {
        auto winSize = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_settingsNode->addChild(menu);

        // Панель налаштувань
        auto settingsBg = CCScale9Sprite::create("GJ_square02.png");
        settingsBg->setContentSize({winSize.width - 40, winSize.height - 80});
        settingsBg->setPosition({winSize.width / 2, winSize.height / 2 - 10});
        settingsBg->setOpacity(180);
        m_settingsNode->addChild(settingsBg);

        // Статус
        auto statusBg = CCScale9Sprite::create("GJ_square01.png");
        statusBg->setContentSize({200, 30});
        statusBg->setPosition({winSize.width / 2, winSize.height - 65});
        statusBg->setOpacity(150);
        m_settingsNode->addChild(statusBg);

        auto statusLabel = CCLabelBMFont::create("● Connected", "bigFont.fnt");
        statusLabel->setScale(0.4f);
        statusLabel->setColor({0, 255, 100});
        statusLabel->setPosition({winSize.width / 2, winSize.height - 65});
        m_settingsNode->addChild(statusLabel);

        // Гучність інших
        auto volTitleBg = CCScale9Sprite::create("GJ_square01.png");
        volTitleBg->setContentSize({200, 25});
        volTitleBg->setPosition({winSize.width / 2, winSize.height / 2 + 40});
        volTitleBg->setOpacity(120);
        m_settingsNode->addChild(volTitleBg);

        auto volLabel = CCLabelBMFont::create("Others Volume", "bigFont.fnt");
        volLabel->setScale(0.38f);
        volLabel->setColor({200, 220, 255});
        volLabel->setPosition({winSize.width / 2, winSize.height / 2 + 40});
        m_settingsNode->addChild(volLabel);

        // Слайдер гучності
        auto slider = VolumeSlider::create(160.f);
        slider->setPosition({winSize.width / 2 - 80, winSize.height / 2 + 10});
        m_settingsNode->addChild(slider);

        // Мікрофон кнопка
        auto micBg = CCScale9Sprite::create("GJ_square01.png");
        micBg->setContentSize({200, 25});
        micBg->setPosition({winSize.width / 2, winSize.height / 2 - 30});
        micBg->setOpacity(120);
        m_settingsNode->addChild(micBg);

        auto micLabel = CCLabelBMFont::create("Microphone", "bigFont.fnt");
        micLabel->setScale(0.38f);
        micLabel->setColor({200, 220, 255});
        micLabel->setPosition({winSize.width / 2, winSize.height / 2 - 30});
        m_settingsNode->addChild(micLabel);

        auto micSpr = ButtonSprite::create(
            g_micMuted ? "OFF" : "ON",
            "bigFont.fnt",
            g_micMuted ? "GJ_button_06.png" : "GJ_button_01.png",
            0.7f
        );
        micSpr->setTag(500);
        auto micBtn = CCMenuItemSpriteExtra::create(
            micSpr, this,
            menu_selector(VoiceChatLayer::onToggleMic)
        );
        micBtn->setPosition({winSize.width / 2, winSize.height / 2 - 55});
        menu->addChild(micBtn);

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
                    m_connectingLabel->setString("No internet!");
                    m_connectingLabel->setColor({255, 80, 80});
                }
            });
        }).detach();
    }

    void updateDots(float) {
        m_dotCount = (m_dotCount % 3) + 1;
        std::string dots(m_dotCount, '.');
        m_connectingLabel->setString(("Connecting" + dots).c_str());
    }

    void onToggleMic(CCObject*) {
        g_micMuted = !g_micMuted;
        if (auto spr = static_cast<ButtonSprite*>(m_settingsNode->getChildByTag(500))) {
            spr->updateBGImage(g_micMuted ? "GJ_button_06.png" : "GJ_button_01.png");
            spr->setString(g_micMuted ? "OFF" : "ON");
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

// ===== PAUSE LAYER MIC BUTTON =====
class $modify(VCPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (g_state != 2) return;

        auto winSize = CCDirector::get()->getWinSize();

        auto micBtn = DraggableMicBtn::create();
        micBtn->setPosition({winSize.width - 40, winSize.height - 40});
        this->addChild(micBtn, 100);
    }
};

// ===== MAIN MENU BUTTON =====
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
        auto scene = CCScene::create();
        scene->addChild(VoiceChatLayer::create());
        CCDirector::get()->pushScene(CCTransitionFade::create(0.3f, scene));
    }
};
