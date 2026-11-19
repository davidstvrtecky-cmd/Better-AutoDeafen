#pragma once

#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>

#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Popup.hpp>

#include "oauth.h"

extern std::atomic<bool> undeafenOnExitCached;

inline std::string formatPercentageString(float value) {
    auto formatted = std::to_string(std::round(value * 100000.f) / 100000.f);
    formatted.erase(formatted.find_last_not_of('0') + 1);

    if (!formatted.empty() && formatted.back() == '.') {
        formatted.pop_back();
    }

    return formatted;
}

inline std::string maskSecret(const std::string& secret) {
    if (secret.empty()) return "...";
    if (secret.size() <= 4) return "Saved (****)";
    return "Saved (****" + secret.substr(secret.size() - 4) + ")";
}

class AuthLayer : public geode::Popup {
public:
    void onClose(CCObject* a) override {
        Popup::onClose(a);
        currentlyInMenu = false;
    }

    static AuthLayer* create() {
        auto ret = new AuthLayer;
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

protected:
    CCLabelBMFont* clientId = nullptr;
    CCLabelBMFont* clientSecret = nullptr;

    void openTutorial(CCObject*) {
        ShellExecuteA(nullptr, "open", "https://lynxdeer.com/autodeafen_setup.html", nullptr, nullptr, SW_SHOWNORMAL);
        log::info("opened docs");
    }

    void pasteClientId(CCObject*) {
        std::string paste = helpers::getClipboardText();
        if (paste.empty()) return;

        CLIENT_ID = paste;
        storage::saveKeys();

        clientId->setString(CLIENT_ID.c_str());
    }

    void pasteClientSecret(CCObject*) {
        std::string paste = helpers::getClipboardText();
        if (paste.empty()) return;

        CLIENT_SECRET = paste;
        storage::saveKeys();

        clientSecret->setString(maskSecret(CLIENT_SECRET).c_str());
    }

    void done(CCObject* c) {
        log::info("starting oauth flow");
        storage::saveKeys();
        oauth::startServer();
        ShellExecuteA(
            nullptr,
            "open",
            ("https://discord.com/oauth2/authorize?client_id=" + CLIENT_ID + "&response_type=code&redirect_uri=http%3A%2F%2Flocalhost%3A8000&scope=rpc.voice.write+rpc").c_str(),
            nullptr,
            nullptr,
            SW_SHOWNORMAL
        );
        AuthLayer::onClose(c);
    }

    bool init() override {
        if (!geode::Popup::init(330, 255)) return false;
        currentlyInMenu = true;

        CCPoint topMiddle = ccp(m_size.width / 2.f, m_size.height);

        auto topLabel = CCLabelBMFont::create("Setup", "goldFont.fnt");
        topLabel->setAnchorPoint({0.5f, 0.5f});
        topLabel->setScale(0.95f);
        topLabel->setPosition(topMiddle + ccp(0, -14));

        std::string idDisplay = CLIENT_ID.empty() ? "..." : CLIENT_ID;
        std::string secretDisplay = maskSecret(CLIENT_SECRET);

        clientId = CCLabelBMFont::create(idDisplay.c_str(), "chatFont-uhd.fnt");
        clientId->setAnchorPoint({0.5f, 0.5f});
        clientId->setScale(0.5f);
        clientId->setPosition(topMiddle + ccp(0, -72));

        clientSecret = CCLabelBMFont::create(secretDisplay.c_str(), "chatFont-uhd.fnt");
        clientSecret->setAnchorPoint({0.5f, 0.5f});
        clientSecret->setScale(0.5f);
        clientSecret->setPosition(topMiddle + ccp(0, -130));

        auto tutorialButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Open Tutorial"),
            this,
            menu_selector(AuthLayer::openTutorial)
        );
        tutorialButton->setScale(0.85f);
        tutorialButton->setPosition(topMiddle + ccp(0, -45));

        auto pasteClientIdButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Paste Client ID"),
            this,
            menu_selector(AuthLayer::pasteClientId)
        );
        pasteClientIdButton->setScale(0.85f);
        pasteClientIdButton->setPosition(topMiddle + ccp(0, -100));

        auto pasteClientSecretButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Paste Client Secret"),
            this,
            menu_selector(AuthLayer::pasteClientSecret)
        );
        pasteClientSecretButton->setScale(0.85f);
        pasteClientSecretButton->setPosition(topMiddle + ccp(0, -158));

        auto doneButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Next"),
            this,
            menu_selector(AuthLayer::done)
        );
        doneButton->setScale(0.9f);
        doneButton->setPosition(topMiddle + ccp(0, -206));

        auto cacheLabel = CCLabelBMFont::create("Keys cached in %APPDATA%\\BetterAutoDeafen\\keys.json", "chatFont-uhd.fnt");
        cacheLabel->setAnchorPoint({0.5f, 0.5f});
        cacheLabel->setScale(0.4f);
        cacheLabel->setOpacity(180);
        cacheLabel->setPosition(topMiddle + ccp(0, -238));

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});

        menu->addChild(tutorialButton);
        menu->addChild(pasteClientIdButton);
        menu->addChild(pasteClientSecretButton);
        menu->addChild(doneButton);

        m_mainLayer->addChild(topLabel);
        m_mainLayer->addChild(clientId);
        m_mainLayer->addChild(clientSecret);
        m_mainLayer->addChild(cacheLabel);
        m_mainLayer->addChild(menu);

        return true;
    }
};

namespace gui {

    inline void openPastingSetup() {
        AuthLayer::create()->show();
    }

    inline void openSetupPopup() {
        geode::createQuickPopup(
            "Better AutoDeafen",
            "<cj>Better AutoDeafen requires some setting-up.</c> <cg>Press 'Open' to open a tutorial link on how to set up Better AutoDeafen.</c>",
            "Later", "Open",
            [](auto, bool btn2) {
                if (btn2) {
                    log::info("opening docs");
                    openPastingSetup();
                    ShellExecuteA(nullptr, "open", "https://lynxdeer.com/autodeafen_setup.html", nullptr, nullptr, SW_SHOWNORMAL);
                    log::info("opened docs");
                }
            }
        );
    }

}

class AdvancedLayer : public geode::Popup {
public:
    TextInput* pbOffsetInput = nullptr;
    TextInput* delayInput = nullptr;

    static inline std::atomic<bool> testInFlight = false;

    static AdvancedLayer* create() {
        auto ret = new AdvancedLayer;
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    void toggleBoolSetting(std::string_view key) {
        bool next = !Mod::get()->getSettingValue<bool>(key);
        Mod::get()->setSettingValue<bool>(key, next);
        if (key == "undeafen_on_exit") {
            undeafenOnExitCached = next;
        }
        (void)Mod::get()->saveData();
    }

    void onToggleStartPos(CCObject*) { toggleBoolSetting("disable_on_startpos"); }
    void onTogglePause(CCObject*) { toggleBoolSetting("undeafen_on_pause"); }
    void onToggleMuteOnly(CCObject*) { toggleBoolSetting("mute_only"); }
    void onToggleIndicator(CCObject*) { toggleBoolSetting("show_indicator"); }
    void onToggleExit(CCObject*) { toggleBoolSetting("undeafen_on_exit"); }
    void onToggleNearPB(CCObject*) { toggleBoolSetting("near_pb_mode"); }
    void onTogglePractice(CCObject*) { toggleBoolSetting("practice"); }
    void onToggleDefaultEnabled(CCObject*) { toggleBoolSetting("default_enabled"); }

    void onTestDeafen(CCObject*) {
        if (!ipc::authenticated) {
            FLAlertLayer::create(
                "Better AutoDeafen",
                "Not connected to Discord yet! Make sure Discord is open and click <cg>Reconnect</c> first.",
                "OK"
            )->show();
            return;
        }
        if (testInFlight.exchange(true)) return;

        bool muteOnly = Mod::get()->getSettingValue<bool>("mute_only");
        std::thread([muteOnly]() {
            ipc::setVoiceState(true, muteOnly, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            ipc::setVoiceState(false, muteOnly, true);
            testInFlight = false;
        }).detach();
    }

    void onOpenCache(CCObject*) {
        storage::openCacheFolder();
    }

    void onClearKeys(CCObject*) {
        geode::createQuickPopup(
            "Clear Cached Keys?",
            "This will wipe your saved Discord Client ID, Secret, and tokens from <cj>%APPDATA%\\BetterAutoDeafen\\keys.json</c>.",
            "Cancel", "Clear",
            [](auto, bool btn2) {
                if (btn2) {
                    ipc::disconnect();
                    storage::clearKeys();
                    FLAlertLayer::create(
                        "Better AutoDeafen",
                        "Cached keys cleared! Press <cg>Re-Setup</c> whenever you want to link a Discord account.",
                        "OK"
                    )->show();
                }
            }
        );
    }

    void onClose(CCObject* a) override {
        if (pbOffsetInput != nullptr && !pbOffsetInput->getString().empty()) {
            float val = geode::utils::numFromString<float>(pbOffsetInput->getString())
                .ok()
                .value_or(Mod::get()->getSettingValue<float>("pb_offset"));
            val = std::clamp(val, 0.0f, 100.0f);
            Mod::get()->setSettingValue<float>("pb_offset", val);
        }
        if (delayInput != nullptr && !delayInput->getString().empty()) {
            float val = geode::utils::numFromString<float>(delayInput->getString())
                .ok()
                .value_or(Mod::get()->getSettingValue<float>("undeafen_delay"));
            val = std::clamp(val, 0.0f, 10.0f);
            Mod::get()->setSettingValue<float>("undeafen_delay", val);
        }
        (void)Mod::get()->saveData();
        Popup::onClose(a);
    }

protected:
    bool init() override {
        if (!geode::Popup::init(430, 305)) return false;

        this->setKeyboardEnabled(true);

        CCPoint topLeft = ccp(0, m_size.height);
        CCPoint topMiddle = ccp(m_size.width / 2.f, m_size.height);

        auto title = CCLabelBMFont::create("Advanced Settings", "goldFont.fnt");
        title->setAnchorPoint({0.5f, 0.5f});
        title->setScale(0.85f);
        title->setPosition(topMiddle + ccp(0, -18));
        m_mainLayer->addChild(title);

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});

        auto addToggleRow = [&](float xLabel, float xToggle, float yOffset, const char* text, const char* settingKey, SEL_MenuHandler handler) {
            auto lbl = CCLabelBMFont::create(text, "bigFont.fnt");
            lbl->setAnchorPoint({0.f, 0.5f});
            lbl->setScale(0.40f);
            lbl->setPosition(topLeft + ccp(xLabel, yOffset));
            m_mainLayer->addChild(lbl);

            auto toggler = CCMenuItemToggler::create(
                CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"),
                CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"),
                this,
                handler
            );
            toggler->setPosition(topLeft + ccp(xToggle, yOffset));
            toggler->setScale(0.68f);
            toggler->setClickable(true);
            toggler->toggle(Mod::get()->getSettingValue<bool>(settingKey));
            menu->addChild(toggler);
        };

        auto addFloatRow = [&](float xLabel, float xInput, float yOffset, const char* text, const char* placeholder, const char* settingKey) -> TextInput* {
            auto lbl = CCLabelBMFont::create(text, "bigFont.fnt");
            lbl->setAnchorPoint({0.f, 0.5f});
            lbl->setScale(0.40f);
            lbl->setPosition(topLeft + ccp(xLabel, yOffset));
            m_mainLayer->addChild(lbl);

            auto input = TextInput::create(64.f, placeholder);
            input->setCommonFilter(geode::CommonFilter::Float);
            input->setWidth(60.f);
            input->setScale(0.75f);
            input->setMaxCharCount(6);
            input->setPosition(topLeft + ccp(xInput, yOffset));
            input->setString(formatPercentageString(Mod::get()->getSettingValue<float>(settingKey)));
            menu->addChild(input);
            return input;
        };

        // Left column
        addToggleRow(24.f, 190.f, -62.f,  "Disable StartPos", "disable_on_startpos", menu_selector(AdvancedLayer::onToggleStartPos));
        addToggleRow(24.f, 190.f, -98.f,  "Undeafen Pause",   "undeafen_on_pause",   menu_selector(AdvancedLayer::onTogglePause));
        addToggleRow(24.f, 190.f, -134.f, "Mute-Only Mode",   "mute_only",           menu_selector(AdvancedLayer::onToggleMuteOnly));
        addToggleRow(24.f, 190.f, -170.f, "Status Indicator", "show_indicator",      menu_selector(AdvancedLayer::onToggleIndicator));
        addToggleRow(24.f, 190.f, -206.f, "Undeafen on Exit", "undeafen_on_exit",    menu_selector(AdvancedLayer::onToggleExit));

        // Right column
        addToggleRow(224.f, 385.f, -62.f,  "Near PB Mode",   "near_pb_mode",    menu_selector(AdvancedLayer::onToggleNearPB));
        pbOffsetInput = addFloatRow(224.f, 385.f, -98.f,  "PB Offset (%)",  "%", "pb_offset");
        delayInput    = addFloatRow(224.f, 385.f, -134.f, "End Delay (s)",  "s", "undeafen_delay");
        addToggleRow(224.f, 385.f, -170.f, "Practice Mode",  "practice",        menu_selector(AdvancedLayer::onTogglePractice));
        addToggleRow(224.f, 385.f, -206.f, "Global Default", "default_enabled", menu_selector(AdvancedLayer::onToggleDefaultEnabled));

        // Bottom utility buttons
        auto testBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Test Deafen"),
            this,
            menu_selector(AdvancedLayer::onTestDeafen)
        );
        testBtn->setScale(0.72f);
        testBtn->setPosition(topLeft + ccp(82.f, -262.f));

        auto cacheBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Open Cache"),
            this,
            menu_selector(AdvancedLayer::onOpenCache)
        );
        cacheBtn->setScale(0.72f);
        cacheBtn->setPosition(topLeft + ccp(215.f, -262.f));

        auto clearBtn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Clear Keys"),
            this,
            menu_selector(AdvancedLayer::onClearKeys)
        );
        clearBtn->setScale(0.72f);
        clearBtn->setPosition(topLeft + ccp(348.f, -262.f));

        menu->addChild(testBtn);
        menu->addChild(cacheBtn);
        menu->addChild(clearBtn);

        m_mainLayer->addChild(menu);

        return true;
    }
};

class ConfigLayer : public geode::Popup {
public:
    TextInput* percentageInput = nullptr;

    void runSetup(CCObject*) {
        gui::openSetupPopup();
    }

    void openAdvanced(CCObject*) {
        AdvancedLayer::create()->show();
    }

    void runReconnect(CCObject* a) {
        helpers::ensureConnected(2);
        onClose(a);
    }

    void toggleEnabled(CCObject*) {
        DEAFEN_ENABLED = !DEAFEN_ENABLED;
    }

    void onClose(CCObject* a) override {
        Popup::onClose(a);
        if (percentageInput != nullptr && !percentageInput->getString().empty()) {
            DEAFEN_PERCENTAGE = geode::utils::numFromString<float>(percentageInput->getString())
                .ok()
                .value_or(Mod::get()->getSettingValue<float>("default_percentage"));
        }
        currentlyInMenu = false;
    }

    static ConfigLayer* create() {
        auto ret = new ConfigLayer;
        if (ret && ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

protected:
    bool init() override {
        if (!geode::Popup::init(330, 225)) return false;

        currentlyInMenu = true;
        this->setKeyboardEnabled(true);

        CCPoint topLeftCorner = ccp(0, m_size.height);
        CCPoint topMiddle = ccp(m_size.width / 2.f, m_size.height);

        auto topLabel = CCLabelBMFont::create("Better AutoDeafen", "goldFont.fnt");
        topLabel->setAnchorPoint({0.5f, 0.5f});
        topLabel->setScale(0.85f);
        topLabel->setPosition(topMiddle + ccp(0, -16));

        CCLabelBMFont* enabledLabel = nullptr;
        CCLabelBMFont* percentageLabel = nullptr;
        CCLabelBMFont* nearPbNote = nullptr;
        CCLabelBMFont* reauthLabel = nullptr;
        CCMenuItemToggler* enabledButton = nullptr;
        CCMenuItemSpriteExtra* reconnectButton = nullptr;

        const bool isAuthed = ipc::authenticated.load();

        if (isAuthed) {
            enabledLabel = CCLabelBMFont::create("Enabled", "bigFont.fnt");
            enabledLabel->setAnchorPoint({0, 0.5f});
            enabledLabel->setScale(0.68f);
            enabledLabel->setPosition(topLeftCorner + ccp(55, -66));

            enabledButton = CCMenuItemToggler::create(
                CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"),
                CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"),
                this,
                menu_selector(ConfigLayer::toggleEnabled)
            );

            enabledButton->setPosition(topLeftCorner + ccp(245, -66));
            enabledButton->setScale(0.85f);
            enabledButton->setClickable(true);
            enabledButton->toggle(DEAFEN_ENABLED);

            percentageInput = TextInput::create(100.f, "%");
            percentageInput->setCommonFilter(geode::CommonFilter::Float);
            percentageInput->setWidth(80);
            percentageInput->setPosition(topLeftCorner + ccp(245, -108));
            percentageInput->setScale(0.85f);
            percentageInput->setMaxCharCount(7);
            percentageInput->setEnabled(true);
            percentageInput->setString(formatPercentageString(DEAFEN_PERCENTAGE));

            percentageLabel = CCLabelBMFont::create("Percent", "bigFont.fnt");
            percentageLabel->setAnchorPoint({0, 0.5f});
            percentageLabel->setScale(0.68f);
            percentageLabel->setPosition(topLeftCorner + ccp(55, -108));

            if (Mod::get()->getSettingValue<bool>("near_pb_mode")) {
                nearPbNote = CCLabelBMFont::create("Near PB Mode ON (auto % active)", "chatFont-uhd.fnt");
                nearPbNote->setAnchorPoint({0.5f, 0.5f});
                nearPbNote->setScale(0.48f);
                nearPbNote->setColor({150, 255, 150});
                nearPbNote->setPosition(topMiddle + ccp(0, -138));
            }
        } else {
            if (resDidntReturnOk) {
                reauthLabel = CCLabelBMFont::create(
                    ("Discord OAuth returned an error\nResponse Code = " + std::to_string(responseCode) +
                     "\nError = '" + responseError + "'\n\nTry Reconnect, or press Re-Setup.").c_str(),
                    "bigFont.fnt"
                );
            } else if (!webRequestDone) {
                reauthLabel = CCLabelBMFont::create(
                    "Refreshing Discord permission...\nThis should only take a second.\nMake sure Discord is open, then press Reconnect.",
                    "bigFont.fnt"
                );
            } else {
                reauthLabel = CCLabelBMFont::create(
                    "Not connected to Discord yet.\nYour keys are still saved!\nMake sure Discord is open, then press Reconnect.",
                    "bigFont.fnt"
                );
            }
            reauthLabel->setAnchorPoint({0.5f, 0.5f});
            reauthLabel->setScale(0.36f);
            reauthLabel->setAlignment(kCCTextAlignmentCenter);
            reauthLabel->setPosition(topMiddle + ccp(0, -90));

            reconnectButton = CCMenuItemSpriteExtra::create(
                ButtonSprite::create("Reconnect"),
                this,
                menu_selector(ConfigLayer::runReconnect)
            );
            reconnectButton->setScale(0.72f);
            reconnectButton->setPosition(topMiddle + ccp(-102, -176));
        }

        auto advancedButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Advanced"),
            this,
            menu_selector(ConfigLayer::openAdvanced)
        );
        advancedButton->setScale(isAuthed ? 0.82f : 0.72f);
        advancedButton->setPosition(isAuthed ? (topMiddle + ccp(-72, -176)) : (topMiddle + ccp(0, -176)));

        auto reAuthButton = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Re-Setup"),
            this,
            menu_selector(ConfigLayer::runSetup)
        );
        reAuthButton->setScale(isAuthed ? 0.82f : 0.72f);
        reAuthButton->setPosition(isAuthed ? (topMiddle + ccp(72, -176)) : (topMiddle + ccp(102, -176)));

        auto menu = CCMenu::create();
        menu->setPosition({0, 0});

        if (isAuthed) {
            menu->addChild(enabledButton);
            menu->addChild(percentageInput);
        } else {
            menu->addChild(reconnectButton);
        }

        menu->addChild(advancedButton);
        menu->addChild(reAuthButton);
        m_mainLayer->addChild(topLabel);

        if (isAuthed) {
            m_mainLayer->addChild(enabledLabel);
            m_mainLayer->addChild(percentageLabel);
            if (nearPbNote) m_mainLayer->addChild(nearPbNote);
        } else {
            m_mainLayer->addChild(reauthLabel);
        }

        m_mainLayer->addChild(menu);

        return true;
    }
};

inline void openModPopup() {
    ConfigLayer::create()->show();
}
