#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace ipc {

    enum class AuthResult {
        Success,
        DiscordNotRunning,
        TokenRejected,
        PipeError
    };

    inline std::atomic<bool> authenticated = false;

    inline HANDLE discordPipe = INVALID_HANDLE_VALUE;
    inline std::mutex pipeMutex;

    // Discord exposes up to 10 possible local IPC named pipes
    inline HANDLE connectToDiscord() {
        for (int i = 0; i < 10; i++) {
            std::wstring pipe = L"\\\\?\\pipe\\discord-ipc-" + std::to_wstring(i);
            HANDLE hPipe = CreateFileW(
                pipe.c_str(),
                GENERIC_READ | GENERIC_WRITE,
                0,
                nullptr,
                OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED,
                nullptr
            );
            if (hPipe != INVALID_HANDLE_VALUE) return hPipe;
        }
        return INVALID_HANDLE_VALUE;
    }

    inline bool writeAll(HANDLE pipe, const void* data, DWORD size) {
        OVERLAPPED overlapped = {};
        overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;

        DWORD written = 0;
        BOOL ok = WriteFile(pipe, data, size, &written, &overlapped);
        if (!ok) {
            if (GetLastError() != ERROR_IO_PENDING) {
                CloseHandle(overlapped.hEvent);
                return false;
            }
            if (WaitForSingleObject(overlapped.hEvent, 5000) != WAIT_OBJECT_0) {
                CancelIo(pipe);
                CloseHandle(overlapped.hEvent);
                return false;
            }
            if (!GetOverlappedResult(pipe, &overlapped, &written, FALSE)) {
                CloseHandle(overlapped.hEvent);
                return false;
            }
        }

        CloseHandle(overlapped.hEvent);
        return written == size;
    }

    inline bool sendFrame(int opcode, const std::string& json) {
        std::lock_guard lock(pipeMutex);
        if (discordPipe == INVALID_HANDLE_VALUE) return false;

        int32_t op = opcode;
        auto len = static_cast<int32_t>(json.size());

        if (!writeAll(discordPipe, &op, 4)) return false;
        if (!writeAll(discordPipe, &len, 4)) return false;
        if (!writeAll(discordPipe, json.data(), static_cast<DWORD>(len))) return false;
        return true;
    }

    inline bool readFrame(HANDLE pipe, std::string& outPayload, DWORD timeoutMs = 5000) {
        int32_t opcode = 0, length = 0;
        DWORD read = 0;
        OVERLAPPED overlapped = {};
        overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) return false;

        auto cleanup = [&](bool result) {
            CloseHandle(overlapped.hEvent);
            return result;
        };

        if (!ReadFile(pipe, &opcode, 4, &read, &overlapped)) {
            if (GetLastError() != ERROR_IO_PENDING) return cleanup(false);
            if (WaitForSingleObject(overlapped.hEvent, timeoutMs) != WAIT_OBJECT_0) {
                CancelIo(pipe);
                return cleanup(false);
            }
        }
        if (!GetOverlappedResult(pipe, &overlapped, &read, FALSE) || read != 4) return cleanup(false);

        ResetEvent(overlapped.hEvent);
        if (!ReadFile(pipe, &length, 4, &read, &overlapped)) {
            if (GetLastError() != ERROR_IO_PENDING) return cleanup(false);
            if (WaitForSingleObject(overlapped.hEvent, timeoutMs) != WAIT_OBJECT_0) {
                CancelIo(pipe);
                return cleanup(false);
            }
        }
        if (!GetOverlappedResult(pipe, &overlapped, &read, FALSE) || read != 4) return cleanup(false);
        if (length < 0 || length > 1024 * 1024) return cleanup(false);

        outPayload.assign(static_cast<size_t>(length), '\0');
        if (length > 0) {
            ResetEvent(overlapped.hEvent);
            if (!ReadFile(pipe, outPayload.data(), static_cast<DWORD>(length), &read, &overlapped)) {
                if (GetLastError() != ERROR_IO_PENDING) return cleanup(false);
                if (WaitForSingleObject(overlapped.hEvent, timeoutMs) != WAIT_OBJECT_0) {
                    CancelIo(pipe);
                    return cleanup(false);
                }
            }
            if (!GetOverlappedResult(pipe, &overlapped, &read, FALSE) || read != static_cast<DWORD>(length)) {
                return cleanup(false);
            }
        }

        return cleanup(true);
    }

    // Continuously drains unsolicited frames from Discord so the pipe never stalls
    inline void drainLoop(HANDLE pipe) {
        while (pipe != INVALID_HANDLE_VALUE && authenticated) {
            int32_t opcode = 0, length = 0;
            DWORD read = 0;
            OVERLAPPED overlapped = {};
            overlapped.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
            if (!overlapped.hEvent) break;

            if (!ReadFile(pipe, &opcode, 4, &read, &overlapped)) {
                if (GetLastError() != ERROR_IO_PENDING) {
                    CloseHandle(overlapped.hEvent);
                    break;
                }
            }
            DWORD waitResult = WaitForSingleObject(overlapped.hEvent, 100);
            if (waitResult == WAIT_TIMEOUT) {
                CancelIo(pipe);
                CloseHandle(overlapped.hEvent);
                continue;
            }
            if (!GetOverlappedResult(pipe, &overlapped, &read, FALSE) || read != 4) {
                CloseHandle(overlapped.hEvent);
                break;
            }

            ResetEvent(overlapped.hEvent);
            if (!ReadFile(pipe, &length, 4, &read, &overlapped)) {
                if (GetLastError() != ERROR_IO_PENDING) {
                    CloseHandle(overlapped.hEvent);
                    break;
                }
            }
            if (WaitForSingleObject(overlapped.hEvent, 1000) != WAIT_OBJECT_0 ||
                !GetOverlappedResult(pipe, &overlapped, &read, FALSE) || read != 4 ||
                length < 0 || length > 1024 * 1024) {
                CloseHandle(overlapped.hEvent);
                break;
            }

            if (length > 0) {
                std::string payload(static_cast<size_t>(length), '\0');
                ResetEvent(overlapped.hEvent);
                if (!ReadFile(pipe, payload.data(), static_cast<DWORD>(length), &read, &overlapped)) {
                    if (GetLastError() != ERROR_IO_PENDING) {
                        CloseHandle(overlapped.hEvent);
                        break;
                    }
                }
                if (WaitForSingleObject(overlapped.hEvent, 1000) != WAIT_OBJECT_0 ||
                    !GetOverlappedResult(pipe, &overlapped, &read, FALSE) || read != static_cast<DWORD>(length)) {
                    CloseHandle(overlapped.hEvent);
                    break;
                }
            }

            CloseHandle(overlapped.hEvent);
        }

        log::info("Discord IPC pipe closed/invalidated");
        authenticated = false;
        {
            std::lock_guard lock(pipeMutex);
            if (discordPipe == pipe) {
                discordPipe = INVALID_HANDLE_VALUE;
            }
        }
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
        }
    }

    inline AuthResult initializeDiscordAuth() {
        if (CLIENT_ID.empty() || DISCORD_ACCESS_TOKEN.empty()) {
            return AuthResult::TokenRejected;
        }

        // Disconnect any existing handle cleanly
        authenticated = false;
        {
            std::lock_guard lock(pipeMutex);
            if (discordPipe != INVALID_HANDLE_VALUE) {
                CloseHandle(discordPipe);
                discordPipe = INVALID_HANDLE_VALUE;
            }
        }

        HANDLE pipe = connectToDiscord();
        if (pipe == INVALID_HANDLE_VALUE) {
            return AuthResult::DiscordNotRunning;
        }

        {
            std::lock_guard lock(pipeMutex);
            discordPipe = pipe;
        }

        auto fail = [&](AuthResult res) {
            authenticated = false;
            {
                std::lock_guard lock(pipeMutex);
                if (discordPipe == pipe) {
                    discordPipe = INVALID_HANDLE_VALUE;
                }
            }
            CloseHandle(pipe);
            return res;
        };

        std::string handshakeResp;
        if (!sendFrame(0, R"({"v": 1, "client_id": ")" + CLIENT_ID + "\"}")) {
            return fail(AuthResult::PipeError);
        }
        if (!readFrame(pipe, handshakeResp)) {
            return fail(AuthResult::PipeError);
        }

        std::string authResp;
        if (!sendFrame(1, R"({"cmd": "AUTHENTICATE", "args": {"access_token": ")" + DISCORD_ACCESS_TOKEN + R"("},"nonce": "auth"})")) {
            return fail(AuthResult::PipeError);
        }
        if (!readFrame(pipe, authResp)) {
            return fail(AuthResult::PipeError);
        }

        if (authResp.find("\"ERROR\"") != std::string::npos) {
            log::warn("Discord IPC rejected cached access token: {}", authResp);
            return fail(AuthResult::TokenRejected);
        }

        authenticated = true;
        log::info("Successfully authenticated with Discord IPC");

        std::thread(drainLoop, pipe).detach();
        return AuthResult::Success;
    }

    inline void setVoiceState(bool active, bool muteOnly = false, bool sync = false) {
        if (!authenticated || discordPipe == INVALID_HANDLE_VALUE) return;

        std::string field = muteOnly ? "mute" : "deaf";
        std::string payload = R"({"cmd": "SET_VOICE_SETTINGS", "args": { ")" + field + R"(": )" +
                              std::string(active ? "true" : "false") + R"( }, "nonce": "deafen" })";

        if (sync) {
            sendFrame(1, payload);
        } else {
            std::thread([payload = std::move(payload)] {
                sendFrame(1, payload);
            }).detach();
        }
    }

    inline void deafen(bool deafen) {
        setVoiceState(deafen, false, false);
    }

    inline void disconnect() {
        authenticated = false;
        std::lock_guard lock(pipeMutex);
        if (discordPipe != INVALID_HANDLE_VALUE) {
            CloseHandle(discordPipe);
            discordPipe = INVALID_HANDLE_VALUE;
        }
    }

}
