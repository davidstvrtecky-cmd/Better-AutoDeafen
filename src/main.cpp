#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <thread>

std::string CLIENT_ID = "";
std::string CLIENT_SECRET = "";

std::string DISCORD_ACCESS_TOKEN = "";
std::string DISCORD_REFRESH_TOKEN = "";
long long TOKEN_EXPIRY = 0;

std::string CURRENT_LEVEL = "";
bool DEAFEN_ENABLED = false;
float DEAFEN_PERCENTAGE = 50.0f;
bool deafenedThisAttempt = false;
bool deafenedWasMuteOnly = false;
bool pausedWhileDeafened = false;
bool hasDied = false;
std::atomic<uint64_t> deafenEpoch = 0;
std::atomic<bool> undeafenOnExitCached = true;
std::string lastIndicatorText = "";

bool currentlyInMenu = false;

bool webRequestDone = false;
bool resDidntReturnOk = false;
int responseCode = 0;
std::string responseError = "";

#include <Geode/Geode.hpp>
using namespace geode::prelude;

#include "storage.h"
#include "ipc.h"
#include "helpers.h"
#include "gui.h"

#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <cocos2d.h>

inline bool isPlayingFromStartPos(PlayLayer* pl) {
    return pl && (pl->m_isTestMode || pl->m_startPosObject != nullptr);
}

inline float getEffectiveDeafenPercent(PlayLayer* pl) {
    if (Mod::get()->getSettingValue<bool>("near_pb_mode") && pl && pl->m_level) {
        int pb = pl->m_level->m_normalPercent.value();
        if (pb > 0 && pb < 100) {
            float offset = std::clamp(Mod::get()->getSettingValue<float>("pb_offset"), 0.0f, 100.0f);
            return std::max(0.0f, static_cast<float>(pb) - offset);
        }
    }
    return DEAFEN_PERCENTAGE;
}

inline void triggerDeafen() {
    if (deafenedThisAttempt) return;
    bool muteOnly = Mod::get()->getSettingValue<bool>("mute_only");
    deafenedWasMuteOnly = muteOnly;
    deafenedThisAttempt = true;
    pausedWhileDeafened = false;
    ++deafenEpoch;
    ipc::setVoiceState(true, muteOnly, false);
}

inline void triggerUndeafen(bool useDelay = false, bool sync = false) {
    if (!deafenedThisAttempt && !pausedWhileDeafened) {
        hasDied = false;
        return;
    }

    bool wasMuteOnly = deafenedWasMuteOnly;
    bool wasActive = deafenedThisAttempt;
    deafenedThisAttempt = false;
    pausedWhileDeafened = false;
    hasDied = false;

    if (!wasActive) return;

    float delaySec = useDelay
        ? std::clamp(Mod::get()->getSettingValue<float>("undeafen_delay"), 0.0f, 10.0f)
        : 0.0f;

    if (!sync && delaySec > 0.01f) {
        uint64_t expectedEpoch = deafenEpoch.load();
        int delayMs = static_cast<int>(delaySec * 1000.0f);
        std::thread([expectedEpoch, delayMs, wasMuteOnly]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
            if (deafenEpoch.load() == expectedEpoch) {
                ipc::setVoiceState(false, wasMuteOnly, true);
            }
        }).detach();
    } else {
        ipc::setVoiceState(false, wasMuteOnly, sync);
    }
}

inline void updateStatusIndicator(PlayLayer* pl) {
    if (!pl) return;
    auto node = pl->getChildByID("blenderanimat.better-autodeafen/status-indicator");
    auto label = static_cast<CCLabelBMFont*>(node);
    if (!label) return;

    if (!Mod::get()->getSettingValue<bool>("show_indicator")) {
        label->setVisible(false);
        return;
    }
    label->setVisible(true);

    std::string text;
    ccColor3B color = {200, 200, 200};

    if (!ipc::authenticated) {
        text = "AutoDeafen: Disconnected";
        color = {255, 165, 90};
    } else if (Mod::get()->getSettingValue<bool>("disable_on_startpos") && isPlayingFromStartPos(pl)) {
        text = "AutoDeafen: OFF (StartPos)";
        color = {180, 180, 180};
    } else if (!DEAFEN_ENABLED) {
        text = "AutoDeafen: OFF";
        color = {180, 180, 180};
    } else if (deafenedThisAttempt) {
        text = deafenedWasMuteOnly ? "MUTED" : "DEAFENED";
        color = {255, 90, 90};
    } else if (pausedWhileDeafened) {
        text = "AutoDeafen: PAUSED";
        color = {255, 220, 110};
    } else {
        float target = getEffectiveDeafenPercent(pl);
        bool muteOnly = Mod::get()->getSettingValue<bool>("mute_only");
        text = (muteOnly ? "AutoMute: " : "AutoDeafen: ") + formatPercentageString(target) + "%";
        color = {140, 255, 160};
    }

    if (text != lastIndicatorText) {
        lastIndicatorText = text;
        label->setString(text.c_str());
        label->setColor(color);
    }
}

$on_mod(Loaded) {
    storage::loadKeys();
    undeafenOnExitCached = Mod::get()->getSettingValue<bool>("undeafen_on_exit");

    std::atexit([]() {
        if (undeafenOnExitCached.load() && deafenedThisAttempt) {
            ipc::setVoiceState(false, deafenedWasMuteOnly, true);
            deafenedThisAttempt = false;
        }
    });

    bool hasClientCreds = !CLIENT_ID.empty() && !CLIENT_SECRET.empty();
    bool hasAnyToken = !DISCORD_ACCESS_TOKEN.empty() || !DISCORD_REFRESH_TOKEN.empty();

    if (hasClientCreds && hasAnyToken) {
        log::info("Loaded cached Better AutoDeafen credentials from {}", storage::getCachePathDisplay());

        bool needsRefresh = DISCORD_ACCESS_TOKEN.empty() ||
                            TOKEN_EXPIRY == 0 ||
                            (helpers::currentTime() + 60 >= TOKEN_EXPIRY);

        if (needsRefresh) {
            if (!DISCORD_REFRESH_TOKEN.empty()) {
                log::info("Cached Discord token expired or missing; refreshing automatically");
                helpers::sendRefreshRequest();
            }
        } else {
            webRequestDone = true;
            // Retry briefly in background in case Discord is still starting after a PC restart
            helpers::initIPC(true, 6);
        }
    }
};

class $modify(PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        deafenedThisAttempt = false;
        pausedWhileDeafened = false;
        hasDied = false;
        lastIndicatorText.clear();

        int id = m_level->m_levelID.value();
        short levelType = helpers::getLevelType(level);
        if (levelType == 1) id = m_level->m_M_ID;

        CURRENT_LEVEL = std::to_string(id) + "-" + std::to_string(levelType);

        bool defaultEnabled = Mod::get()->getSettingValue<bool>("default_enabled");
        float defaultDeafenPercentage = Mod::get()->getSettingValue<float>("default_percentage");

        if (Mod::get()->hasSavedValue(CURRENT_LEVEL)) {
            auto value = Mod::get()->getSavedValue<matjson::Value>(CURRENT_LEVEL);
            DEAFEN_ENABLED = value["e"].asBool().ok().value_or(defaultEnabled);
            DEAFEN_PERCENTAGE = value["p"].as<float>().ok().value_or(defaultDeafenPercentage);
        } else {
            DEAFEN_ENABLED = defaultEnabled;
            DEAFEN_PERCENTAGE = defaultDeafenPercentage;
        }

        // Create subtle on-screen status indicator in the top-left corner
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        auto indicator = CCLabelBMFont::create("", "chatFont.fnt");
        indicator->setID("blenderanimat.better-autodeafen/status-indicator");
        indicator->setAnchorPoint({0.f, 1.f});
        indicator->setScale(0.55f);
        indicator->setOpacity(175);
        indicator->setPosition({8.f, winSize.height - 6.f});
        this->addChild(indicator, 999);
        updateStatusIndicator(this);

        // If Discord was opened after Geometry Dash, silently reconnect when entering a level
        if (!ipc::authenticated) {
            helpers::ensureConnected(1);
        }

        return true;
    }

    void postUpdate(float p0) {
        PlayLayer::postUpdate(p0);
        updateStatusIndicator(this);

        // Automatically turn off when playing from a StartPos (if enabled in Advanced settings)
        if (Mod::get()->getSettingValue<bool>("disable_on_startpos") && isPlayingFromStartPos(this)) {
            triggerUndeafen(false, false);
            return;
        }
        if (this->m_isPracticeMode && !Mod::get()->getSettingValue<bool>("practice")) { return; }
        if (!DEAFEN_ENABLED) return;
        if (this->m_hasCompletedLevel) {
            if (deafenedThisAttempt) {
                triggerUndeafen(true, false);
            }
            return;
        }

        float targetPercent = getEffectiveDeafenPercent(this);
        if (getCurrentPercent() >= targetPercent && !deafenedThisAttempt) {
            triggerDeafen();
        }
    }

    void resetLevel() {
        triggerUndeafen(false, false);
        PlayLayer::resetLevel();
    }

    void fullReset() {
        triggerUndeafen(false, false);
        PlayLayer::fullReset();
    }

    void resetLevelFromStart() {
        triggerUndeafen(false, false);
        PlayLayer::resetLevelFromStart();
    }

    void onQuit() {
        triggerUndeafen(false, false);
        PlayLayer::onQuit();
    }

    void onExit() {
        triggerUndeafen(false, false);
        PlayLayer::onExit();
    }
};

class $modify(PlayerObject) {
    void playerDestroyed(bool p0) {
        auto playLayer = PlayLayer::get();
        if (playLayer && playLayer->m_level && this == playLayer->m_player1 && !playLayer->m_level->isPlatformer()) {
            if (deafenedThisAttempt && !hasDied) {
                hasDied = true;
                triggerUndeafen(true, false);
            }
        }
        PlayerObject::playerDestroyed(p0);
    }
};

class $modify(MyPauseLayer, PauseLayer) {
    void onQuit(cocos2d::CCObject* sender) {
        PauseLayer::onQuit(sender);
        triggerUndeafen(false, false);

        bool defaultEnabled = Mod::get()->getSettingValue<bool>("default_enabled");
        float defaultDeafenPercentage = Mod::get()->getSettingValue<float>("default_percentage");

        if (!(DEAFEN_ENABLED == defaultEnabled && defaultDeafenPercentage == DEAFEN_PERCENTAGE)) {
            auto json = matjson::Value();
            json["e"] = DEAFEN_ENABLED;
            json["p"] = DEAFEN_PERCENTAGE;

            Mod::get()->setSavedValue(CURRENT_LEVEL, json);
            (void)Mod::get()->saveData();
        }
    }

    void onResume(cocos2d::CCObject* sender) {
        PauseLayer::onResume(sender);
        if (pausedWhileDeafened) {
            pausedWhileDeafened = false;
            auto pl = PlayLayer::get();
            bool blockedByStartPos = Mod::get()->getSettingValue<bool>("disable_on_startpos") && isPlayingFromStartPos(pl);
            if (pl && DEAFEN_ENABLED && !blockedByStartPos && !pl->m_hasCompletedLevel) {
                triggerDeafen();
            }
        }
    }

    void onAutoDeafenMenuClick(CCObject* target) {
        bool hasCachedAuth = !DISCORD_ACCESS_TOKEN.empty() ||
                             (!DISCORD_REFRESH_TOKEN.empty() && !CLIENT_ID.empty() && !CLIENT_SECRET.empty());
        if (hasCachedAuth) {
            if (!ipc::authenticated) {
                helpers::ensureConnected(2);
            }
            openModPopup();
        } else {
            gui::openSetupPopup();
        }
    }

    void customSetup() {
        PauseLayer::customSetup();

        // Optional: undeafen while paused and re-deafen on resume
        if (Mod::get()->getSettingValue<bool>("undeafen_on_pause") && deafenedThisAttempt) {
            ipc::setVoiceState(false, deafenedWasMuteOnly, false);
            deafenedThisAttempt = false;
            pausedWhileDeafened = true;
        }

        auto menu = this->getChildByID("right-button-menu");

        auto sprite = CCSprite::createWithSpriteFrameName("GJ_musicOffBtn_001.png");
        auto btn = CCMenuItemSpriteExtra::create(sprite, sprite, this, menu_selector(MyPauseLayer::onAutoDeafenMenuClick));
        menu->addChild(btn);
        menu->updateLayout();
    }
};
