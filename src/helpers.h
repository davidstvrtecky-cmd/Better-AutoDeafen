#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <Geode/Geode.hpp>
#include <Geode/loader/Event.hpp>
#include <Geode/utils/web.hpp>
using namespace geode::prelude;

#include "storage.h"
#include "ipc.h"

namespace helpers {

    inline std::atomic<bool> ipcInitInFlight = false;
    inline std::atomic<bool> refreshInFlight = false;

    inline std::string trim(std::string s) {
        const char* ws = " \t\r\n";
        auto start = s.find_first_not_of(ws);
        if (start == std::string::npos) return {};
        auto end = s.find_last_not_of(ws);
        return s.substr(start, end - start + 1);
    }

    inline std::string getClipboardText() {
        if (!OpenClipboard(nullptr)) return {};
        auto h = GetClipboardData(CF_UNICODETEXT);
        auto p = h ? static_cast<LPCWSTR>(GlobalLock(h)) : nullptr;

        std::string out;
        if (p) {
            int n = WideCharToMultiByte(CP_UTF8, 0, p, -1, nullptr, 0, nullptr, nullptr);
            out.resize(n ? n - 1 : 0);
            if (n) WideCharToMultiByte(CP_UTF8, 0, p, -1, out.data(), n, nullptr, nullptr);
            GlobalUnlock(h);
        }

        CloseClipboard();
        return trim(out);
    }

    inline short getLevelType(GJGameLevel* level) {
        if (level->m_levelType != GJLevelType::Saved) return 1;
        if (level->m_dailyID > 0) return 2;
        if (level->m_gauntletLevel) return 3;
        return 0;
    }

    inline long long currentTime() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    inline void sendRefreshRequest();
    inline void initIPC(bool allowRefreshOnReject = true, int maxAttempts = 6);

    inline std::function<void(geode::utils::web::WebResponse)> webHandler = [](geode::utils::web::WebResponse res) {
        refreshInFlight = false;
        webRequestDone = true;

        if (!res.ok()) {
            log::warn("Discord OAuth request failed: HTTP {} ({})", res.code(), res.error());
            resDidntReturnOk = true;
            responseCode = res.code();
            responseError = res.error();
            return;
        }

        auto jsonRes = res.json();
        if (!jsonRes) {
            log::warn("Discord OAuth returned invalid JSON");
            resDidntReturnOk = true;
            responseCode = res.code();
            responseError = "Invalid JSON response from Discord";
            return;
        }

        auto& json = jsonRes.unwrap();
        std::string newAccess = "";
        std::string newRefresh = "";
        long long expiresIn = 0;

        if (auto v = json.get("access_token")) {
            newAccess = v.unwrap().asString().unwrapOr("");
        }
        if (auto v = json.get("refresh_token")) {
            newRefresh = v.unwrap().asString().unwrapOr("");
        }
        if (auto v = json.get("expires_in")) {
            expiresIn = v.unwrap().asInt().unwrapOr(0);
        }

        if (newAccess.empty()) {
            log::warn("Discord OAuth response did not include access_token");
            resDidntReturnOk = true;
            responseCode = res.code();
            responseError = "Discord response missing access_token";
            return;
        }

        resDidntReturnOk = false;
        DISCORD_ACCESS_TOKEN = newAccess;
        if (!newRefresh.empty()) {
            DISCORD_REFRESH_TOKEN = newRefresh;
        }
        TOKEN_EXPIRY = currentTime() + (expiresIn > 0 ? expiresIn : 604800LL);

        // Immediately persist to %APPDATA%\AutoDeafen\keys.json and flush Geode saved.json
        storage::saveKeys();

        log::info("Updated Discord OAuth tokens (expires at {})", TOKEN_EXPIRY);
        initIPC(false, 4);
    };

    inline void sendRefreshRequest() {
        if (CLIENT_ID.empty() || CLIENT_SECRET.empty() || DISCORD_REFRESH_TOKEN.empty()) {
            log::warn("Cannot refresh Discord token: missing client_id, client_secret, or refresh_token");
            webRequestDone = true;
            return;
        }

        if (refreshInFlight.exchange(true)) {
            return;
        }

        webRequestDone = false;
        resDidntReturnOk = false;

        static TaskHolder<web::WebResponse> listener;

        // Fixed: Discord requires `refresh_token=` (NOT `code=`) for grant_type=refresh_token
        std::string params =
            "client_id=" + CLIENT_ID +
            "&client_secret=" + CLIENT_SECRET +
            "&grant_type=refresh_token"
            "&refresh_token=" + DISCORD_REFRESH_TOKEN;

        auto req = web::WebRequest();
        req.header("Content-Type", "application/x-www-form-urlencoded");
        req.body(std::vector<uint8_t>(params.begin(), params.end()));

        log::info("Sending Discord OAuth refresh_token request");
        listener.spawn(
            req.post("https://discord.com/api/oauth2/token"),
            webHandler
        );
    }

    inline void initIPC(bool allowRefreshOnReject, int maxAttempts) {
        if (ipcInitInFlight.exchange(true)) {
            return;
        }

        log::info("Attempting to initialize Discord IPC (maxAttempts={})", maxAttempts);
        std::thread([allowRefreshOnReject, maxAttempts]() {
            for (int attempt = 0; attempt < maxAttempts; ++attempt) {
                auto result = ipc::initializeDiscordAuth();
                if (result == ipc::AuthResult::Success) {
                    ipcInitInFlight = false;
                    return;
                }

                if (result == ipc::AuthResult::TokenRejected) {
                    ipcInitInFlight = false;
                    if (allowRefreshOnReject && !DISCORD_REFRESH_TOKEN.empty() && !CLIENT_ID.empty() && !CLIENT_SECRET.empty()) {
                        log::info("Cached token rejected by Discord; refreshing via refresh_token");
                        Loader::get()->queueInMainThread([]() {
                            helpers::sendRefreshRequest();
                        });
                    }
                    return;
                }

                if (attempt + 1 < maxAttempts) {
                    int delaySec = (result == ipc::AuthResult::DiscordNotRunning) ? 3 : 2;
                    std::this_thread::sleep_for(std::chrono::seconds(delaySec));
                }
            }

            ipcInitInFlight = false;
        }).detach();
    }

    // Ensures we are connected to Discord using cached credentials without forcing re-setup
    inline void ensureConnected(int maxAttempts = 2) {
        if (ipc::authenticated) return;
        if (CLIENT_ID.empty() || CLIENT_SECRET.empty()) return;

        bool needsRefresh = DISCORD_ACCESS_TOKEN.empty() || TOKEN_EXPIRY == 0 || (currentTime() + 60 >= TOKEN_EXPIRY);
        if (needsRefresh) {
            if (!DISCORD_REFRESH_TOKEN.empty()) {
                sendRefreshRequest();
            }
        } else {
            webRequestDone = true;
            initIPC(true, maxAttempts);
        }
    }

}
