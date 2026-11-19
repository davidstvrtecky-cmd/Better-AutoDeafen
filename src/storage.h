#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>

#include <Geode/Geode.hpp>
using namespace geode::prelude;

namespace storage {

    inline std::mutex storageMutex;

    // Dedicated persistent location for Discord OAuth keys:
    // %APPDATA%\BetterAutoDeafen\keys.json (fallback: Geode mod save dir)
    inline std::filesystem::path getCachePath() {
        PWSTR appDataPath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath)) && appDataPath != nullptr) {
            std::filesystem::path p = std::filesystem::path(appDataPath) / L"BetterAutoDeafen" / L"keys.json";
            CoTaskMemFree(appDataPath);
            return p;
        }
        if (appDataPath != nullptr) {
            CoTaskMemFree(appDataPath);
        }
        return Mod::get()->getSaveDir() / "keys.json";
    }

    inline std::filesystem::path getLegacyCachePath(const wchar_t* folderName) {
        PWSTR appDataPath = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath)) && appDataPath != nullptr) {
            std::filesystem::path p = std::filesystem::path(appDataPath) / folderName / L"keys.json";
            CoTaskMemFree(appDataPath);
            return p;
        }
        if (appDataPath != nullptr) {
            CoTaskMemFree(appDataPath);
        }
        return {};
    }

    inline std::string getCachePathDisplay() {
        return getCachePath().string();
    }

    inline void openCacheFolder() {
        const auto dir = getCachePath().parent_path();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // Atomically writes keys to %APPDATA%\BetterAutoDeafen\keys.json AND immediately flushes Geode's saved.json.
    inline void saveKeys() {
        std::lock_guard lock(storageMutex);

        auto root = matjson::Value();
        root["client_id"] = CLIENT_ID;
        root["client_secret"] = CLIENT_SECRET;
        root["access_token"] = DISCORD_ACCESS_TOKEN;
        root["refresh_token"] = DISCORD_REFRESH_TOKEN;
        root["token_expiry"] = TOKEN_EXPIRY;

        const auto cachePath = getCachePath();
        std::error_code ec;
        std::filesystem::create_directories(cachePath.parent_path(), ec);

        auto tmpPath = cachePath;
        tmpPath += L".tmp";

        {
            std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
            if (out.is_open()) {
                std::string serialized = root.dump();
                out.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
                out.flush();
                out.close();

                if (!MoveFileExW(
                    tmpPath.c_str(),
                    cachePath.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
                )) {
                    std::filesystem::rename(tmpPath, cachePath, ec);
                }
                log::info("Saved Better AutoDeafen keys to {}", cachePath.string());
            } else {
                log::warn("Failed to open key cache for writing: {}", tmpPath.string());
            }
        }

        // Mirror to Geode's own save container and flush immediately to disk
        Mod::get()->setSavedValue<std::string>("CLIENT_ID", CLIENT_ID);
        Mod::get()->setSavedValue<std::string>("CLIENT_SECRET", CLIENT_SECRET);
        Mod::get()->setSavedValue<std::string>("DISCORD_ACCESS_TOKEN", DISCORD_ACCESS_TOKEN);
        Mod::get()->setSavedValue<std::string>("DISCORD_REFRESH_TOKEN", DISCORD_REFRESH_TOKEN);
        Mod::get()->setSavedValue<long long>("TOKEN_EXPIRY", TOKEN_EXPIRY);
        (void)Mod::get()->saveData();
    }

    inline void clearKeys() {
        std::lock_guard lock(storageMutex);

        CLIENT_ID.clear();
        CLIENT_SECRET.clear();
        DISCORD_ACCESS_TOKEN.clear();
        DISCORD_REFRESH_TOKEN.clear();
        TOKEN_EXPIRY = 0;

        const auto cachePath = getCachePath();
        std::error_code ec;
        std::filesystem::remove(cachePath, ec);

        Mod::get()->setSavedValue<std::string>("CLIENT_ID", "");
        Mod::get()->setSavedValue<std::string>("CLIENT_SECRET", "");
        Mod::get()->setSavedValue<std::string>("DISCORD_ACCESS_TOKEN", "");
        Mod::get()->setSavedValue<std::string>("DISCORD_REFRESH_TOKEN", "");
        Mod::get()->setSavedValue<long long>("TOKEN_EXPIRY", 0);
        (void)Mod::get()->saveData();

        log::info("Cleared cached Better AutoDeafen keys");
    }

    // Loads keys from %APPDATA%\BetterAutoDeafen\keys.json first, migrating legacy cache or Geode saved.json if present.
    inline void loadKeys() {
        bool migratedFromGeode = false;
        bool cacheFileExisted = false;

        {
            std::lock_guard lock(storageMutex);
            const auto cachePath = getCachePath();
            std::error_code ec;
            cacheFileExisted = std::filesystem::exists(cachePath, ec);

            auto readFromFile = [&](const std::filesystem::path& p) {
                std::ifstream in(p, std::ios::binary);
                if (!in.is_open()) return false;
                std::ostringstream ss;
                ss << in.rdbuf();
                if (auto parsed = matjson::parse(ss.str())) {
                    auto& root = parsed.unwrap();
                    if (CLIENT_ID.empty()) {
                        if (auto v = root.get("client_id")) CLIENT_ID = v.unwrap().asString().unwrapOr("");
                    }
                    if (CLIENT_SECRET.empty()) {
                        if (auto v = root.get("client_secret")) CLIENT_SECRET = v.unwrap().asString().unwrapOr("");
                    }
                    if (DISCORD_ACCESS_TOKEN.empty()) {
                        if (auto v = root.get("access_token")) DISCORD_ACCESS_TOKEN = v.unwrap().asString().unwrapOr("");
                    }
                    if (DISCORD_REFRESH_TOKEN.empty()) {
                        if (auto v = root.get("refresh_token")) DISCORD_REFRESH_TOKEN = v.unwrap().asString().unwrapOr("");
                    }
                    if (TOKEN_EXPIRY == 0) {
                        if (auto v = root.get("token_expiry")) TOKEN_EXPIRY = v.unwrap().asInt().unwrapOr(0);
                    }
                    log::info("Loaded Better AutoDeafen keys from {}", p.string());
                    return true;
                }
                log::warn("Could not parse key cache {}", p.string());
                return false;
            };

            if (cacheFileExisted) {
                readFromFile(cachePath);
            } else {
                for (const wchar_t* legacyFolder : { L"AutoDeaphen", L"AutoDeafen" }) {
                    const auto legacyPath = getLegacyCachePath(legacyFolder);
                    if (!legacyPath.empty() && std::filesystem::exists(legacyPath, ec)) {
                        if (readFromFile(legacyPath)) {
                            migratedFromGeode = true;
                            break;
                        }
                    }
                }
            }

            // Fallback / migrate any missing values from Geode's saved.json
            if (CLIENT_ID.empty() && Mod::get()->hasSavedValue("CLIENT_ID")) {
                CLIENT_ID = Mod::get()->getSavedValue<std::string>("CLIENT_ID");
                if (!CLIENT_ID.empty()) migratedFromGeode = true;
            }
            if (CLIENT_SECRET.empty() && Mod::get()->hasSavedValue("CLIENT_SECRET")) {
                CLIENT_SECRET = Mod::get()->getSavedValue<std::string>("CLIENT_SECRET");
                if (!CLIENT_SECRET.empty()) migratedFromGeode = true;
            }
            if (DISCORD_ACCESS_TOKEN.empty() && Mod::get()->hasSavedValue("DISCORD_ACCESS_TOKEN")) {
                DISCORD_ACCESS_TOKEN = Mod::get()->getSavedValue<std::string>("DISCORD_ACCESS_TOKEN");
                if (!DISCORD_ACCESS_TOKEN.empty()) migratedFromGeode = true;
            }
            if (DISCORD_REFRESH_TOKEN.empty() && Mod::get()->hasSavedValue("DISCORD_REFRESH_TOKEN")) {
                DISCORD_REFRESH_TOKEN = Mod::get()->getSavedValue<std::string>("DISCORD_REFRESH_TOKEN");
                if (!DISCORD_REFRESH_TOKEN.empty()) migratedFromGeode = true;
            }
            if (TOKEN_EXPIRY == 0 && Mod::get()->hasSavedValue("TOKEN_EXPIRY")) {
                TOKEN_EXPIRY = Mod::get()->getSavedValue<long long>("TOKEN_EXPIRY");
                if (TOKEN_EXPIRY != 0) migratedFromGeode = true;
            }
        }

        if (migratedFromGeode || (!cacheFileExisted && (!CLIENT_ID.empty() || !DISCORD_ACCESS_TOKEN.empty()))) {
            saveKeys();
        }
    }

}
