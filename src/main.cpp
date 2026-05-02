#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>

using namespace geode::prelude;

// Простая кнопка войсчата в главном меню
class $modify(MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto btn = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_microBtn_001.png"),
            this,
            menu_selector(MenuLayer::onVoiceChat)
        );

        auto menu = this->getChildByID("bottom-menu");
        if (menu) {
            menu->addChild(btn);
            menu->updateLayout();
        }

        return true;
    }

    void onVoiceChat(CCObject*) {
        FLAlertLayer::create(
            "VoiceChat",
            "Voice chat coming soon!",
            "OK"
        )->show();
    }
};
