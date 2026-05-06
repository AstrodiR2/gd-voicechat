#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
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

// ===== DRAGGABLE MIC BUTTON (floating, drag vs tap) =====
class DraggableMicBtn : public CCNode {
public:
    CCPoint m_touchStart;
    CCPoint m_nodeStart;
    bool m_dragging = false;
    bool m_moved = false;
    CCNode* m_iconMuted = nullptr;
    CCNode* m_iconActive = nullptr;
    CCLayerColor* m_bg = nullptr;

    static DraggableMicBtn* create() {
        auto ret = new DraggableMicBtn();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }

    bool init() {
        if (!CCNode::init()) return false;

        // Круглый фон
        m_bg = CCLayerColor::create({50, 50, 50, 200}, 60, 60);
        m_bg->setPosition({-30, -30});
        this->addChild(m_bg, 0);

        // Icon muted (перечеркнутый микрофон) — простой крест текстом
        auto mutedLbl = CCLabelBMFont::create("x", "bigFont.fnt");
        mutedLbl->setScale(0.7f);
        mutedLbl->setColor({255, 80, 80});
        mutedLbl->setPosition({0, 0});
        mutedLbl->setID("mic-muted-icon");
        this->addChild(mutedLbl, 1);
        m_iconMuted = mutedLbl;

        // Icon active
        auto activeLbl = CCLabelBMFont::create("o", "bigFont.fnt");
        activeLbl->setScale(0.7f);
        activeLbl->setColor({0, 255, 100});
        activeLbl->setPosition({0, 0});
        activeLbl->setID("mic-active-icon");
        activeLbl->setVisible(false);
        this->addChild(activeLbl, 1);
        m_iconActive = activeLbl;

        this->updateIcon();
        this->setTouchEnabled(true);
        CCTouchDispatcher::get()->addTargetedDelegate(this, 0, true);
        return true;
    }

    void updateIcon() {
        m_iconMuted->setVisible(g_micMuted);
        m_iconActive->setVisible(!g_micMuted);
        if (g_micMuted)
            m_bg->setColor({80, 30, 30});
        else
            m_bg->setColor({30, 80, 30});
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        auto loc = this->convertToNodeSpace(touch->getLocation());
        if (loc.x < -35 || loc.x > 35 || loc.y < -35 || loc.y > 35) return false;
        m_touchStart = touch->getLocation();
        m_nodeStart = this->getPosition();
        m_dragging = true;
        m_moved = false;
        return true;
    }

    void ccTouchMoved(CCTouch* touch, CCEvent*) override {
        if (!m_dragging) return;
        auto delta = touch->getLocation() - m_touchStart;
        if (std::abs(delta.x) > 5 || std::abs(delta.y) > 5) m_moved = true;
        if (m_moved) this->setPosition(m_nodeStart + delta);
    }

    void ccTouchEnded(CCTouch* touch, CCEvent*) override {
        if (!m_dragging) return;
        m_dragging = false;
        if (!m_moved) {
            // Toggle mic
            g_micMuted = !g_micMuted;
            this->updateIcon();
        }
    }

    void onExit() override {
        CCTouchDispatcher::get()->removeDelegate(this);
        CCNode::onExit();
    }
};

// ===== SLIDER VOLUME NODE =====
// Простой слайдер через CCControlSlider или ручной через touch
class VCSlider : public CCNode {
public:
    CCLayerColor* m_track = nullptr;
    CCLayerColor* m_fill = nullptr;
    CCLayerColor* m_thumb = nullptr;
    CCLabelBMFont* m_pctLabel = nullptr;
    float m_width = 200.f;
    bool m_dragging = false;

    static VCSlider* create(float width) {
        auto ret = new VCSlider();
        ret->m_width = width;
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }

    bool init() {
        if (!CCNode::init()) return false;

        float h = 8.f;
        // Track bg
        m_track = CCLayerColor::create({60, 60, 60, 255}, m_width, h);
        m_track->setPosition({-m_width / 2, -h / 2});
        this->addChild(m_track, 0);

        // Fill
        m_fill = CCLayerColor::create({0, 200, 80, 255}, m_width * g_othersVolume / 2.f, h);
        m_fill->setPosition({-m_width / 2, -h / 2});
        this->addChild(m_fill, 1);

        // Thumb circle
        m_thumb = CCLayerColor::create({255, 255, 255, 255}, 18, 18);
        m_thumb->setPosition({-9 + (m_width * g_othersVolume / 2.f) - m_width / 2, -9});
        this->addChild(m_thumb, 2);

        // Percent label
        m_pctLabel = CCLabelBMFont::create("100%", "bigFont.fnt");
        m_pctLabel->setScale(0.4f);
        m_pctLabel->setColor({220, 220, 220});
        m_pctLabel->setPosition({m_width / 2 + 30, 0});
        this->addChild(m_pctLabel, 3);

        this->updateVisuals();
        CCTouchDispatcher::get()->addTargetedDelegate(this, 0, true);
        return true;
    }

    void updateVisuals() {
        float v = g_othersVolume / 2.f; // 0..1 (max 200%)
        float fillW = m_width * v;
        m_fill->setContentSize({fillW, 8});
        m_thumb->setPositionX(-9 + fillW - m_width / 2);
        int pct = (int)(g_othersVolume * 100);
        m_pctLabel->setString((std::to_string(pct) + "%").c_str());
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        auto loc = this->convertToNodeSpace(touch->getLocation());
        if (loc.x < -m_width / 2 - 15 || loc.x > m_width / 2 + 15 || loc.y < -20 || loc.y > 20) return false;
        m_dragging = true;
        this->handleTouch(loc);
        return true;
    }

    void ccTouchMoved(CCTouch* touch, CCEvent*) override {
        if (!m_dragging) return;
        auto loc = this->convertToNodeSpace(touch->getLocation());
        this->handleTouch(loc);
    }

    void ccTouchEnded(CCTouch* touch, CCEvent*) override {
        m_dragging = false;
    }

    void handleTouch(CCPoint loc) {
        float t = (loc.x + m_width / 2) / m_width;
        t = std::max(0.f, std::min(1.f, t));
        g_othersVolume = t * 2.f; // 0..200%
        this->updateVisuals();
    }

    void onExit() override {
        CCTouchDispatcher::get()->removeDelegate(this);
        CCNode::onExit();
    }
};

// ===== ГОЛОВНЕ МЕНЮ ВОЙСЧАТУ =====
class VoiceChatLayer : public CCLayer {
public:
    // Стейти UI: 0=connect screen, 1=connecting, 2=settings
    int m_uiState = 0;
    int m_dotCount = 0;

    // Ноди для кожного стейту
    CCNode* m_connectNode = nullptr;   // скрін 2 стиль — кнопка Connect
    CCNode* m_connectingNode = nullptr; // скрін 2 стиль — Connecting...
    CCNode* m_settingsNode = nullptr;  // скрін 3 стиль — налаштування

    CCLabelBMFont* m_connectingLabel = nullptr;

    static VoiceChatLayer* create(bool alreadyConnected = false) {
        auto ret = new VoiceChatLayer();
        ret->m_uiState = alreadyConnected ? 2 : 0;
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }

    bool init() {
        if (!CCLayer::init()) return false;
        auto winSize = CCDirector::get()->getWinSize();

        // === ФОН — як в GD (іконки бігають) ===
        // Використовуємо стандартний GD фон MenuGameLayer або просто синій тайловий
        auto bg = CCSprite::create("GJ_gradientBG.png");
        if (bg) {
            bg->setScaleX(winSize.width / bg->getContentSize().width);
            bg->setScaleY(winSize.height / bg->getContentSize().height);
            bg->setPosition({winSize.width / 2, winSize.height / 2});
            bg->setColor({40, 80, 180});
            this->addChild(bg, -2);
        } else {
            auto bgColor = CCLayerColor::create({40, 80, 180, 255});
            this->addChild(bgColor, -2);
        }

        // Кнопка назад (стандартна GD)
        auto backBtn = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png"),
            this,
            menu_selector(VoiceChatLayer::onBack)
        );
        auto backMenu = CCMenu::create();
        backMenu->setPosition({25, winSize.height - 25});
        backMenu->addChild(backBtn);
        this->addChild(backMenu, 10);

        this->buildConnectNode();
        this->buildConnectingNode();
        this->buildSettingsNode();

        // Показуємо потрібний стейт
        m_connectNode->setVisible(m_uiState == 0);
        m_connectingNode->setVisible(m_uiState == 1);
        m_settingsNode->setVisible(m_uiState == 2);

        return true;
    }

    // ======================================================
    // CONNECT NODE — стиль як Globed (скрін 2): панель з кнопкою
    // ======================================================
    void buildConnectNode() {
        auto winSize = CCDirector::get()->getWinSize();
        m_connectNode = CCNode::create();
        this->addChild(m_connectNode, 5);

        // Панель (GJ_square07.png або GJ_square01.png)
        auto panel = CCScale9Sprite::create("GJ_square07.png");
        if (!panel) panel = CCScale9Sprite::create("GJ_square01.png");
        panel->setContentSize({300, 200});
        panel->setPosition({winSize.width / 2, winSize.height / 2});
        m_connectNode->addChild(panel);

        // Заголовок всередині панелі
        auto title = CCLabelBMFont::create("VoiceChat", "goldFont.fnt");
        title->setScale(0.85f);
        title->setPosition({winSize.width / 2, winSize.height / 2 + 65});
        m_connectNode->addChild(title);

        // Кнопка Connect — зелена як в Globed
        auto connectSpr = ButtonSprite::create(
            "Connect", "bigFont.fnt", "GJ_button_01.png", 1.0f
        );
        auto connectBtn = CCMenuItemSpriteExtra::create(
            connectSpr, this,
            menu_selector(VoiceChatLayer::onConnect)
        );

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        connectBtn->setPosition({winSize.width / 2, winSize.height / 2});
        menu->addChild(connectBtn);
        m_connectNode->addChild(menu);
    }

    // ======================================================
    // CONNECTING NODE — та ж панель, але з анімацією тексту
    // ======================================================
    void buildConnectingNode() {
        auto winSize = CCDirector::get()->getWinSize();
        m_connectingNode = CCNode::create();
        this->addChild(m_connectingNode, 5);

        auto panel = CCScale9Sprite::create("GJ_square07.png");
        if (!panel) panel = CCScale9Sprite::create("GJ_square01.png");
        panel->setContentSize({300, 200});
        panel->setPosition({winSize.width / 2, winSize.height / 2});
        m_connectingNode->addChild(panel);

        auto title = CCLabelBMFont::create("VoiceChat", "goldFont.fnt");
        title->setScale(0.85f);
        title->setPosition({winSize.width / 2, winSize.height / 2 + 65});
        m_connectingNode->addChild(title);

        m_connectingLabel = CCLabelBMFont::create("Connecting.", "bigFont.fnt");
        m_connectingLabel->setScale(0.55f);
        m_connectingLabel->setColor({255, 255, 255});
        m_connectingLabel->setPosition({winSize.width / 2, winSize.height / 2});
        m_connectingNode->addChild(m_connectingLabel);
    }

    // ======================================================
    // SETTINGS NODE — стиль як Global Room (скрін 3): велика панель
    // ======================================================
    void buildSettingsNode() {
        auto winSize = CCDirector::get()->getWinSize();
        m_settingsNode = CCNode::create();
        this->addChild(m_settingsNode, 5);

        // Велика панель як Global Room
        auto panel = CCScale9Sprite::create("GJ_square07.png");
        if (!panel) panel = CCScale9Sprite::create("GJ_square01.png");
        panel->setContentSize({360, 300});
        panel->setPosition({winSize.width / 2, winSize.height / 2});
        m_settingsNode->addChild(panel);

        // Заголовок у верхній частині панелі з жовтою рамкою (як "Global Room")
        auto headerBg = CCScale9Sprite::create("GJ_square02.png");
        if (headerBg) {
            headerBg->setContentSize({340, 40});
            headerBg->setPosition({winSize.width / 2, winSize.height / 2 + 115});
            m_settingsNode->addChild(headerBg, 1);
        }

        auto title = CCLabelBMFont::create("VoiceChat", "goldFont.fnt");
        title->setScale(0.7f);
        title->setPosition({winSize.width / 2, winSize.height / 2 + 115});
        m_settingsNode->addChild(title, 2);

        // Роздільник
        auto line = CCLayerColor::create({255, 255, 255, 40}, 330, 2);
        line->setPosition({winSize.width / 2 - 165, winSize.height / 2 + 90});
        m_settingsNode->addChild(line, 1);

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_settingsNode->addChild(menu, 2);

        // --- СЛАЙДЕР ГУЧНОСТІ ---
        auto volTitle = CCLabelBMFont::create("Others Volume", "bigFont.fnt");
        volTitle->setScale(0.45f);
        volTitle->setColor({200, 200, 200});
        volTitle->setPosition({winSize.width / 2, winSize.height / 2 + 60});
        m_settingsNode->addChild(volTitle, 2);

        auto slider = VCSlider::create(200.f);
        slider->setPosition({winSize.width / 2 - 15, winSize.height / 2 + 30});
        m_settingsNode->addChild(slider, 3);

        // --- КНОПКА DISCONNECT ---
        auto discSpr = ButtonSprite::create(
            "Disconnect", "bigFont.fnt", "GJ_button_06.png", 0.9f
        );
        auto discBtn = CCMenuItemSpriteExtra::create(
            discSpr, this,
            menu_selector(VoiceChatLayer::onDisconnect)
        );
        discBtn->setPosition({winSize.width / 2, winSize.height / 2 - 60});
        menu->addChild(discBtn);

        // --- Плаваюча кнопка мік (додається окремо поверх усього) ---
        // Буде додана після того як settingsNode стає видимим
    }

    void showSettingsAndAddMicBtn() {
        auto winSize = CCDirector::get()->getWinSize();
        m_settingsNode->setVisible(true);

        // Floating draggable mic button
        auto micBtn = DraggableMicBtn::create();
        micBtn->setPosition({winSize.width - 50, 50});
        micBtn->setID("floating-mic-btn");
        this->addChild(micBtn, 20);
    }

    void onConnect(CCObject*) {
        // Перехід до connecting з анімацією (стандартна GD — fade/scale)
        m_connectNode->setVisible(false);
        m_connectingNode->setVisible(true);
        this->schedule(schedule_selector(VoiceChatLayer::updateDots), 0.5f);

        g_state = 1;
        std::thread([this]() {
            bool ok = connectToServer();
            Loader::get()->queueInMainThread([this, ok]() {
                this->unschedule(schedule_selector(VoiceChatLayer::updateDots));
                m_connectingNode->setVisible(false);
                if (ok) {
                    g_state = 2;
#ifdef GEODE_IS_ANDROID
                    startRecording();
#endif
                    this->showSettingsAndAddMicBtn();
                } else {
                    g_state = 0;
                    m_connectingLabel->setString("No connection!");
                    m_connectingLabel->setColor({255, 80, 80});
                    m_connectingNode->setVisible(true);
                    // Через 2 сек повертаємо Connect кнопку
                    this->runAction(CCSequence::create(
                        CCDelayTime::create(2.0f),
                        CCCallFunc::create(this, callfunc_selector(VoiceChatLayer::showConnectAgain)),
                        nullptr
                    ));
                }
            });
        }).detach();
    }

    void showConnectAgain() {
        m_connectingNode->setVisible(false);
        m_connectingLabel->setString("Connecting.");
        m_connectingLabel->setColor({255, 255, 255});
        m_connectNode->setVisible(true);
    }

    void updateDots(float) {
        m_dotCount = (m_dotCount % 3) + 1;
        std::string dots(m_dotCount, '.');
        m_connectingLabel->setString(("Connecting" + dots).c_str());
    }

    void onDisconnect(CCObject*) {
        createQuickPopup(
            "Disconnect",
            "Disconnect from VoiceChat?",
            "Cancel", "Yes",
            [this](auto, bool confirm) {
                if (confirm) {
                    disconnectFromServer();
                    // Видаляємо плаваючу кнопку мік
                    if (auto mb = this->getChildByID("floating-mic-btn")) mb->removeFromParent();
                    m_settingsNode->setVisible(false);
                    m_connectNode->setVisible(true);
                }
            }
        );
    }

    void onBack(CCObject*) {
        // Стандартна GD анімація — popSceneWithTransition
        CCDirector::get()->popSceneWithTransition(0.5f, PopTransition::kPopTransitionFade);
    }
};

// ===== КНОПКА МІК НА ПАУЗІ =====
// Залишаємо компактною, але стилізуємо
class $modify(VCPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (g_state != 2) return;

        auto winSize = CCDirector::get()->getWinSize();

        // Маленька кругла кнопка мік у кутку
        auto bg = CCScale9Sprite::create("GJ_square07.png");
        if (!bg) bg = CCScale9Sprite::create("GJ_square01.png");
        bg->setContentSize({70, 30});

        auto label = CCLabelBMFont::create(
            g_micMuted ? "Mic OFF" : "Mic ON", "bigFont.fnt"
        );
        label->setScale(0.4f);
        label->setColor(g_micMuted ? ccColor3B{255, 80, 80} : ccColor3B{0, 255, 100});
        label->setID("vc-mic-label");

        auto btn = CCMenuItemSpriteExtra::create(
            label, this,
            menu_selector(VCPauseLayer::onToggleMic)
        );
        auto menu = CCMenu::create();
        menu->setPosition({winSize.width - 45, winSize.height - 20});
        menu->addChild(btn);
        this->addChild(menu, 100);
    }

    void onToggleMic(CCObject*) {
        g_micMuted = !g_micMuted;
        auto children = this->getChildren();
        // Оновлюємо лейбл
        CCObject* obj;
        CCARRAY_FOREACH(children, obj) {
            auto node = static_cast<CCNode*>(obj);
            if (auto lbl = static_cast<CCLabelBMFont*>(node->getChildByID("vc-mic-label"))) {
                lbl->setString(g_micMuted ? "Mic OFF" : "Mic ON");
                lbl->setColor(g_micMuted ? ccColor3B{255, 80, 80} : ccColor3B{0, 255, 100});
            }
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
        auto layer = VoiceChatLayer::create(g_state == 2);
        auto scene = CCScene::create();
        scene->addChild(layer);
        // Стандартна GD анімація переходу
        CCDirector::get()->pushScene(CCTransitionFade::create(0.5f, scene));
    }
};
