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

// ===== DRAGGABLE MIC BUTTON (плаваюча кнопка мікрофону) =====
// Показується поверх усього коли підключено (g_state == 2)
// Тап - показує іконки вкл/викл мікро, довгий дотик/перетягування - переміщує
class DragMicButton : public CCNode, public CCTouchDelegate {
public:
    CCSprite* m_circle = nullptr;
    CCSprite* m_micIcon = nullptr;   // іконка мікро (увімкнено)
    CCSprite* m_muteIcon = nullptr;  // іконка мікро (вимкнено)

    CCPoint m_touchStart;
    CCPoint m_nodeStartPos;
    bool m_wasDragged = false;
    bool m_touchDown = false;

    static DragMicButton* create() {
        auto ret = new DragMicButton();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }

    bool init() {
        if (!CCNode::init()) return false;

        // Коло-фон
        m_circle = CCSprite::createWithSpriteFrameName("GJ_button_02.png");
        if (!m_circle) m_circle = CCSprite::create("GJ_button_02.png");
        m_circle->setScale(1.1f);
        this->addChild(m_circle);

        // Іконка мікро (увімкнено) - використовуємо спрайт з GD
        m_micIcon = CCSprite::createWithSpriteFrameName("GJ_deleteSoundBtn_001.png");
        if (!m_micIcon) m_micIcon = CCSprite::create();
        m_micIcon->setScale(0.6f);
        this->addChild(m_micIcon, 1);

        // Іконка вимкненого мікро - просто перехрещена версія (малюємо X поверх)
        m_muteIcon = CCSprite::createWithSpriteFrameName("GJ_deleteSoundBtn_001.png");
        if (!m_muteIcon) m_muteIcon = CCSprite::create();
        m_muteIcon->setScale(0.6f);
        m_muteIcon->setColor({255, 60, 60});
        this->addChild(m_muteIcon, 1);

        this->updateMicVisual();

        this->setContentSize({50, 50});
        this->setAnchorPoint({0.5f, 0.5f});

        return true;
    }

    void updateMicVisual() {
        if (g_micMuted) {
            m_circle->setColor({180, 50, 50});
            m_micIcon->setVisible(false);
            m_muteIcon->setVisible(true);
        } else {
            m_circle->setColor({50, 180, 80});
            m_micIcon->setVisible(true);
            m_muteIcon->setVisible(false);
        }
    }

    void onEnter() override {
        CCNode::onEnter();
        CCTouchDispatcher::get()->addTargetedDelegate(this, -500, true);
    }

    void onExit() override {
        CCNode::onExit();
        CCTouchDispatcher::get()->removeDelegate(this);
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        CCPoint loc = this->getParent()->convertToNodeSpace(touch->getLocation());
        CCRect rect = CCRect(
            this->getPositionX() - 30,
            this->getPositionY() - 30,
            60, 60
        );
        if (!rect.containsPoint(loc)) return false;

        m_touchStart = touch->getLocation();
        m_nodeStartPos = this->getPosition();
        m_wasDragged = false;
        m_touchDown = true;
        return true;
    }

    void ccTouchMoved(CCTouch* touch, CCEvent*) override {
        if (!m_touchDown) return;
        CCPoint delta = touch->getLocation() - m_touchStart;
        float dist = sqrtf(delta.x * delta.x + delta.y * delta.y);
        if (dist > 10.0f) {
            m_wasDragged = true;
        }
        if (m_wasDragged) {
            CCPoint newPos = m_nodeStartPos + delta;
            auto winSize = CCDirector::get()->getWinSize();
            newPos.x = std::max(30.0f, std::min(winSize.width - 30.0f, newPos.x));
            newPos.y = std::max(30.0f, std::min(winSize.height - 30.0f, newPos.y));
            this->setPosition(newPos);
        }
    }

    void ccTouchEnded(CCTouch* touch, CCEvent*) override {
        if (!m_touchDown) return;
        m_touchDown = false;
        if (!m_wasDragged) {
            // Тап — перемкнути мікрофон
            g_micMuted = !g_micMuted;
            this->updateMicVisual();

            // Анімація пульсу при тапі
            auto scaleUp = CCScaleTo::create(0.1f, 1.2f);
            auto scaleDown = CCScaleTo::create(0.1f, 1.0f);
            this->runAction(CCSequence::create(scaleUp, scaleDown, nullptr));
        }
    }

    void ccTouchCancelled(CCTouch* touch, CCEvent*) override {
        m_touchDown = false;
    }
};

// ===== ГОЛОВНА СЦЕНА ВОЙСЧАТУ (стиль як Globed) =====
class VoiceChatLayer : public CCLayer {
public:
    // Стан UI
    CCNode* m_connectNode = nullptr;     // Показується коли не підключено (кнопка Connect)
    CCNode* m_connectingNode = nullptr;  // Показується під час підключення
    CCNode* m_settingsNode = nullptr;    // Показується після підключення

    CCLabelBMFont* m_connectingLabel = nullptr;
    int m_dotCount = 0;

    // Слайдер гучності
    CCSliderThumb* m_volSlider = nullptr;
    CCLabelBMFont* m_volLabel = nullptr;

    static VoiceChatLayer* create() {
        auto ret = new VoiceChatLayer();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }

    bool init() {
        if (!CCLayer::init()) return false;

        auto winSize = CCDirector::get()->getWinSize();

        // ===== ФОН (стиль Globed — синє поле з квадратами як в GD menu) =====
        // Основний колір фону як в меню GD
        auto bg = CCSprite::create("GJ_gradientBG.png");
        if (bg) {
            bg->setScaleX(winSize.width / bg->getContentSize().width);
            bg->setScaleY(winSize.height / bg->getContentSize().height);
            bg->setPosition({winSize.width / 2, winSize.height / 2});
            bg->setColor({40, 80, 160}); // синій відтінок як у GD меню
            this->addChild(bg, -2);
        } else {
            auto bgLayer = CCLayerColor::create({32, 72, 148, 255});
            this->addChild(bgLayer, -2);
        }

        // Декоративні кутові квадрати (стиль Globed)
        // Верхній лівий
        auto cornerTL = CCSprite::create("GJ_square07.png");
        if (cornerTL) {
            cornerTL->setPosition({0, winSize.height});
            cornerTL->setAnchorPoint({0, 1});
            cornerTL->setOpacity(50);
            this->addChild(cornerTL, -1);
        }
        // Нижній правий
        auto cornerBR = CCSprite::create("GJ_square07.png");
        if (cornerBR) {
            cornerBR->setPosition({winSize.width, 0});
            cornerBR->setAnchorPoint({1, 0});
            cornerBR->setOpacity(50);
            this->addChild(cornerBR, -1);
        }

        // ===== ПАНЕЛЬ (стиль Globed — коричневий прямокутник по центру) =====
        auto panel = CCScale9Sprite::create("GJ_square01.png");
        panel->setContentSize({340, 280});
        panel->setPosition({winSize.width / 2, winSize.height / 2});
        this->addChild(panel, 0);

        // ===== ЗАГОЛОВОК =====
        auto title = CCLabelBMFont::create("VoiceChat", "goldFont.fnt");
        title->setScale(0.85f);
        title->setPosition({winSize.width / 2, winSize.height / 2 + 115});
        this->addChild(title, 1);

        // ===== КНОПКА НАЗАД =====
        auto backSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png");
        auto backBtn = CCMenuItemSpriteExtra::create(
            backSpr, this, menu_selector(VoiceChatLayer::onBack)
        );
        auto backMenu = CCMenu::create();
        backMenu->setPosition({25, winSize.height - 25});
        backMenu->addChild(backBtn);
        this->addChild(backMenu, 1);

        // ===== НОДА CONNECT (початковий екран — тільки кнопка Connect) =====
        m_connectNode = CCNode::create();
        m_connectNode->setPosition({winSize.width / 2, winSize.height / 2});
        this->addChild(m_connectNode, 1);

        // Текст сервера
        auto serverLabel = CCLabelBMFont::create("VoiceChat Server", "bigFont.fnt");
        serverLabel->setScale(0.5f);
        serverLabel->setColor({255, 220, 100});
        serverLabel->setPosition({0, 40});
        m_connectNode->addChild(serverLabel);

        // Кнопка Connect (зелена, як у Globed)
        auto connectSpr = ButtonSprite::create(
            "Connect", "bigFont.fnt", "GJ_button_01.png", 0.9f
        );
        auto connectBtn = CCMenuItemSpriteExtra::create(
            connectSpr, this, menu_selector(VoiceChatLayer::onConnect)
        );
        connectBtn->setPosition({0, 0});

        // Маленькі іконки знизу (стиль Globed)
        auto iconMenu = CCMenu::create();
        iconMenu->setPosition({0, -60});

        auto connectMenu = CCMenu::create();
        connectMenu->setPosition({0, 0});
        connectMenu->addChild(connectBtn);
        m_connectNode->addChild(connectMenu);
        m_connectNode->addChild(iconMenu);

        // ===== НОДА CONNECTING (анімація підключення) =====
        m_connectingNode = CCNode::create();
        m_connectingNode->setPosition({winSize.width / 2, winSize.height / 2});
        m_connectingNode->setVisible(false);
        this->addChild(m_connectingNode, 1);

        m_connectingLabel = CCLabelBMFont::create("Connecting.", "bigFont.fnt");
        m_connectingLabel->setScale(0.55f);
        m_connectingLabel->setColor({200, 220, 255});
        m_connectingLabel->setPosition({0, 0});
        m_connectingNode->addChild(m_connectingLabel);

        // ===== НОДА SETTINGS (після підключення — стиль Globed) =====
        m_settingsNode = CCNode::create();
        m_settingsNode->setPosition({winSize.width / 2, winSize.height / 2});
        m_settingsNode->setVisible(false);
        this->addChild(m_settingsNode, 1);

        this->buildSettingsUI();

        // Якщо вже підключені — одразу показуємо налаштування
        if (g_state == 2) {
            m_connectNode->setVisible(false);
            m_settingsNode->setVisible(true);
        }

        return true;
    }

    void buildSettingsUI() {
        // ===== Статус підключення =====
        auto statusDot = CCLabelBMFont::create("● Connected", "bigFont.fnt");
        statusDot->setScale(0.45f);
        statusDot->setColor({0, 255, 100});
        statusDot->setPosition({0, 95});
        m_settingsNode->addChild(statusDot);

        // ===== Розділювач =====
        auto sep = CCSprite::createWithSpriteFrameName("floorLine_001.png");
        if (sep) {
            sep->setScaleX(1.8f);
            sep->setOpacity(80);
            sep->setPosition({0, 75});
            m_settingsNode->addChild(sep);
        }

        // ===== Мікрофон секція =====
        auto micLabel = CCLabelBMFont::create("Microphone", "bigFont.fnt");
        micLabel->setScale(0.4f);
        micLabel->setColor({180, 200, 255});
        micLabel->setPosition({0, 55});
        m_settingsNode->addChild(micLabel);

        // Кнопка мікрофону (велика, округла, Globed стиль)
        auto micBtnSpr = ButtonSprite::create(
            g_micMuted ? "Mic: OFF" : "Mic: ON",
            "bigFont.fnt",
            g_micMuted ? "GJ_button_06.png" : "GJ_button_01.png",
            0.75f
        );
        micBtnSpr->setTag(200);

        auto micMenu = CCMenu::create();
        micMenu->setPosition({0, 0});
        auto micBtn = CCMenuItemSpriteExtra::create(
            micBtnSpr, this, menu_selector(VoiceChatLayer::onToggleMic)
        );
        micBtn->setTag(201);
        micBtn->setPosition({0, 25});
        micMenu->addChild(micBtn);
        m_settingsNode->addChild(micMenu);

        // ===== Гучність — слайдер =====
        auto volTitleLabel = CCLabelBMFont::create("Others Volume", "bigFont.fnt");
        volTitleLabel->setScale(0.4f);
        volTitleLabel->setColor({180, 200, 255});
        volTitleLabel->setPosition({0, -15});
        m_settingsNode->addChild(volTitleLabel);

        // Слайдер Geode/GD
        // GD має вбудований CCSlider — використовуємо його
        auto sliderTrack = CCSprite::create("sliderBar.png");
        if (!sliderTrack) sliderTrack = CCSprite::createWithSpriteFrameName("sliderBar.png");

        // Використовуємо Slider з Geode
        // Slider складається з: трек + повзунок (thumb)
        auto sliderBg = CCScale9Sprite::create("square02_small.png");
        if (!sliderBg) sliderBg = CCScale9Sprite::create("GJ_square07.png");
        if (sliderBg) {
            sliderBg->setContentSize({200, 8});
            sliderBg->setPosition({-20, -40});
            sliderBg->setOpacity(100);
            m_settingsNode->addChild(sliderBg);
        }

        // Заповнена частина слайдера
        auto sliderFill = CCScale9Sprite::create("square02_small.png");
        if (!sliderFill) sliderFill = CCScale9Sprite::create("GJ_square07.png");
        if (sliderFill) {
            sliderFill->setColor({80, 200, 120});
            sliderFill->setContentSize({(float)(g_othersVolume * 100.0f) * 2.0f, 8});
            sliderFill->setPosition({-20 - 100 + (float)(g_othersVolume * 100.0f), -40});
            sliderFill->setAnchorPoint({0, 0.5f});
            sliderFill->setTag(300);
            m_settingsNode->addChild(sliderFill, 1);
        }

        // Повзунок (thumb)
        auto thumbSpr = CCSprite::createWithSpriteFrameName("slidergroove2.png");
        if (!thumbSpr) thumbSpr = CCSprite::create("slidergroove2.png");

        // Thumb — кнопка-повзунок
        if (thumbSpr) {
            thumbSpr->setScale(0.6f);
        } else {
            // fallback — звичайний круглий спрайт
            thumbSpr = CCSprite::createWithSpriteFrameName("GJ_button_01.png");
            if (!thumbSpr) thumbSpr = CCSprite::create();
            thumbSpr->setScale(0.3f);
        }

        auto thumbBtn = CCMenuItemSpriteExtra::create(
            thumbSpr, this, menu_selector(VoiceChatLayer::onVolumeDummy)
        );
        thumbBtn->setTag(301);
        // Позиція = -120 + volume*200 (від 0 до 1)
        thumbBtn->setPosition({-120.0f + g_othersVolume * 200.0f, -40});

        auto thumbMenu = CCMenu::create();
        thumbMenu->setPosition({0, 0});
        thumbMenu->addChild(thumbBtn);
        m_settingsNode->addChild(thumbMenu, 2);

        // Підпис відсотків
        m_volLabel = CCLabelBMFont::create("100%", "bigFont.fnt");
        m_volLabel->setScale(0.45f);
        m_volLabel->setTag(202);
        m_volLabel->setPosition({95, -40});
        m_settingsNode->addChild(m_volLabel, 2);
        this->updateVolumeLabel();

        // Розділювач
        auto sep2 = CCSprite::createWithSpriteFrameName("floorLine_001.png");
        if (sep2) {
            sep2->setScaleX(1.8f);
            sep2->setOpacity(80);
            sep2->setPosition({0, -58});
            m_settingsNode->addChild(sep2);
        }

        // ===== Кнопка Disconnect =====
        auto discSpr = ButtonSprite::create(
            "Disconnect", "bigFont.fnt", "GJ_button_06.png", 0.75f
        );
        auto discMenu = CCMenu::create();
        discMenu->setPosition({0, 0});
        auto discBtn = CCMenuItemSpriteExtra::create(
            discSpr, this, menu_selector(VoiceChatLayer::onDisconnect)
        );
        discBtn->setPosition({0, -82});
        discMenu->addChild(discBtn);
        m_settingsNode->addChild(discMenu);

        // Додаємо touch listener для слайдера
        this->setTouchEnabled(true);
        this->setTouchMode(ccTouchesMode::kCCTouchesOneByOne);
    }

    // Dummy для thumb (реальний drag — через ccTouchBegan)
    void onVolumeDummy(CCObject*) {}

    // ===== TOUCH для слайдера гучності =====
    bool m_draggingSlider = false;
    CCPoint m_sliderOrigin; // в координатах вікна

    void registerWithTouchDispatcher() override {
        CCTouchDispatcher::get()->addTargetedDelegate(this, -100, true);
    }

    bool ccTouchBegan(CCTouch* touch, CCEvent*) override {
        if (!m_settingsNode->isVisible()) return false;
        // Зона слайдера: від -120 до +80 по X, Y ≈ -40 відносно settingsNode
        CCPoint loc = m_settingsNode->convertToNodeSpace(touch->getLocation());
        // Зона touch слайдера: x від -120 до 80, y від -55 до -25
        if (loc.x >= -120 && loc.x <= 80 && loc.y >= -55 && loc.y <= -25) {
            m_draggingSlider = true;
            this->updateSliderFromTouch(loc.x);
            return true;
        }
        return false;
    }

    void ccTouchMoved(CCTouch* touch, CCEvent*) override {
        if (!m_draggingSlider) return;
        CCPoint loc = m_settingsNode->convertToNodeSpace(touch->getLocation());
        this->updateSliderFromTouch(loc.x);
    }

    void ccTouchEnded(CCTouch*, CCEvent*) override {
        m_draggingSlider = false;
    }

    void ccTouchCancelled(CCTouch*, CCEvent*) override {
        m_draggingSlider = false;
    }

    void updateSliderFromTouch(float x) {
        // x від -120 до 80 → vol від 0.0 до 1.0
        float norm = (x - (-120.0f)) / 200.0f;
        norm = std::max(0.0f, std::min(1.0f, norm));
        g_othersVolume = norm * 2.0f; // макс 200%

        // Оновити thumb позицію
        if (auto thumbMenu = m_settingsNode->getChildByTag(0)) {}
        // Шукаємо thumb через дітей settingsNode
        // thumb знаходиться в меню — спробуємо через getChildByTag на меню
        // Простіше — перебудуємо позицію thumb безпосередньо
        // Thumb = меню з тегом 0 (не тегований), тому використаємо userObject або просто scale9
        // Замість цього оновимо через schedule
        Loader::get()->queueInMainThread([this, norm]() {
            this->updateVolumeLabel();
            // Оновлюємо fill
            if (auto fill = static_cast<CCScale9Sprite*>(m_settingsNode->getChildByTag(300))) {
                fill->setContentSize({norm * 200.0f, 8});
                fill->setPosition({-120.0f + norm * 100.0f, -40});
            }
        });
    }

    // ===== ПІДКЛЮЧЕННЯ =====
    void onConnect(CCObject*) {
        m_connectNode->setVisible(false);
        m_connectingNode->setVisible(true);
        m_dotCount = 0;
        this->schedule(schedule_selector(VoiceChatLayer::updateDots), 0.5f);

        g_state = 1;
        std::thread([this]() {
            bool ok = connectToServer();
            Loader::get()->queueInMainThread([this, ok]() {
                this->unschedule(schedule_selector(VoiceChatLayer::updateDots));
                if (ok) {
                    g_state = 2;
#ifdef GEODE_IS_ANDROID
                    startRecording();
#endif
                    // Анімація появи налаштувань (стиль GD — fade + scale)
                    m_connectingNode->setVisible(false);
                    m_settingsNode->setVisible(true);
                    m_settingsNode->setScale(0.8f);
                    m_settingsNode->setOpacity(0);
                    auto fadeIn = CCFadeIn::create(0.2f);
                    auto scaleUp = CCScaleTo::create(0.2f, 1.0f);
                    m_settingsNode->runAction(CCSpawn::create(fadeIn, scaleUp, nullptr));
                } else {
                    g_state = 0;
                    m_connectingNode->setVisible(false);
                    m_connectNode->setVisible(true);
                    m_connectingLabel->setString("No internet connection!");
                    m_connectingLabel->setColor({255, 80, 80});

                    // Показуємо помилку тимчасово і повертаємось до Connect
                    m_connectNode->setVisible(false);
                    m_connectingNode->setVisible(true);
                    this->scheduleOnce([this](float) {
                        m_connectingNode->setVisible(false);
                        m_connectNode->setVisible(true);
                        m_connectingLabel->setString("Connecting.");
                        m_connectingLabel->setColor({200, 220, 255});
                    }, 2.5f, "reset_connect_error");
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
        if (auto btnSpr = static_cast<ButtonSprite*>(m_settingsNode->getChildByTag(200))) {
            btnSpr->updateBGImage(g_micMuted ? "GJ_button_06.png" : "GJ_button_01.png");
            btnSpr->setString(g_micMuted ? "Mic: OFF" : "Mic: ON");
        }
    }

    void updateVolumeLabel() {
        if (m_volLabel) {
            int pct = (int)(g_othersVolume * 100.0f);
            m_volLabel->setString((std::to_string(pct) + "%").c_str());
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
        // Анімація переходу (стандартна GD)
        CCDirector::get()->popSceneWithTransition(0.5f, PopTransition::kPopTransitionFade);
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
        label->setScale(0.42f);
        label->setColor(g_micMuted ? ccColor3B{255, 80, 80} : ccColor3B{0, 255, 100});
        label->setID("vc-mic-label");

        auto btn = CCMenuItemSpriteExtra::create(
            label, this,
            menu_selector(VCPauseLayer::onToggleMicPause)
        );
        auto menu = CCMenu::create();
        menu->setPosition({winSize.width - 55, winSize.height - 22});
        menu->addChild(btn);
        this->addChild(menu, 100);
    }

    void onToggleMicPause(CCObject*) {
        g_micMuted = !g_micMuted;
        if (auto lbl = static_cast<CCLabelBMFont*>(this->getChildByID("vc-mic-label"))) {
            lbl->setString(g_micMuted ? "Mic OFF" : "Mic ON");
            lbl->setColor(g_micMuted ? ccColor3B{255, 80, 80} : ccColor3B{0, 255, 100});
        }
    }
};

// ===== КНОПКА В ГОЛОВНОМУ МЕНЮ + ПЛАВАЮЧА КНОПКА МІК =====
class $modify(VCMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        // Кнопка відкрити VoiceChat меню
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

        // Якщо підключені — показуємо плаваючу кнопку мікрофону
        if (g_state == 2) {
            this->addDragMicButton();
        }

        return true;
    }

    void addDragMicButton() {
        auto winSize = CCDirector::get()->getWinSize();
        auto dragBtn = DragMicButton::create();
        dragBtn->setID("drag-mic-btn");
        // Стартова позиція — правий нижній кут
        dragBtn->setPosition({winSize.width - 50, 80});
        dragBtn->setZOrder(200);
        this->addChild(dragBtn);
    }

    void onVoiceChat(CCObject*) {
        // Анімація переходу як в GD (fade)
        auto scene = CCScene::create();
        scene->addChild(VoiceChatLayer::create());
        CCDirector::get()->pushScene(CCTransitionFade::create(0.4f, scene));
    }
};
