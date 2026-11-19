#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

#include "oauth.h"

#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include <Geode/Geode.hpp>
#include <Geode/loader/Event.hpp>
#include <Geode/utils/web.hpp>

using namespace geode::async;
using namespace geode::utils;

extern std::string CLIENT_ID;
extern std::string CLIENT_SECRET;

namespace helpers {
    extern std::function<void(geode::utils::web::WebResponse)> webHandler;
}

void oauth::serverThread() {
    struct ServerGuard {
        ~ServerGuard() { oauth::serverRunning = false; }
    } guard;

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return;
    }

    SOCKET lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock == INVALID_SOCKET) {
        WSACleanup();
        return;
    }

    int opt = 1;
    setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8000);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(lsock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
        listen(lsock, 1) == SOCKET_ERROR) {
        geode::prelude::log::warn("AutoDeafen OAuth server failed to bind port 8000 (WSA error {})", WSAGetLastError());
        closesocket(lsock);
        WSACleanup();
        return;
    }

    // Wait up to 3 minutes for the OAuth redirect so we don't leak port 8000 if cancelled
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(lsock, &readfds);
    timeval tv{};
    tv.tv_sec = 180;
    tv.tv_usec = 0;

    int sel = select(0, &readfds, nullptr, nullptr, &tv);
    if (sel <= 0) {
        closesocket(lsock);
        WSACleanup();
        return;
    }

    SOCKET csock = accept(lsock, nullptr, nullptr);
    if (csock == INVALID_SOCKET) {
        closesocket(lsock);
        WSACleanup();
        return;
    }

    int timeout = 10000;
    setsockopt(csock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    char buffer[4096] = {};
    if (recv(csock, buffer, 4095, 0) <= 0) {
        closesocket(csock);
        closesocket(lsock);
        WSACleanup();
        return;
    }

    std::string request(buffer);

    const char* response =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
        "<h2 style='font-family:sans-serif'>There may be an error?</h2>"
        "<p style='font-family:sans-serif'>There's no oauth code, but also no error from discord. Something went wrong.</p>";

    size_t pos = request.find("GET /?code=");
    size_t posBad = request.find("GET /?error=");
    if (pos != std::string::npos) {
        auto start = pos + 11;
        auto end = request.find_first_of(" &", start);
        std::string oauth_code = (end == std::string::npos) ? request.substr(start) : request.substr(start, end - start);

        std::string params =
            "client_id=" + CLIENT_ID +
            "&client_secret=" + CLIENT_SECRET +
            "&grant_type=authorization_code"
            "&code=" + oauth_code +
            "&redirect_uri=http://localhost:8000";

        geode::Loader::get()->queueInMainThread([params = std::move(params)]() {
            static TaskHolder<web::WebResponse> listener;

            auto req = web::WebRequest();
            req.header("Content-Type", "application/x-www-form-urlencoded");
            req.body(std::vector<uint8_t>(params.begin(), params.end()));

            geode::prelude::log::info("Sending Discord OAuth authorization_code exchange");
            listener.spawn(
                req.post("https://discord.com/api/oauth2/token"),
                helpers::webHandler
            );
        });

        response =
            "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
            "<h2 style='font-family:sans-serif'>All set!</h2>"
            "<p style='font-family:sans-serif'>Your keys have been cached permanently. You can close this tab and go back to Geometry Dash!</p>";
    } else if (posBad != std::string::npos) {
        response =
            "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
            "<h2 style='font-family:sans-serif'>Discord Returned an OAuth error</h2>"
            "<p style='font-family:sans-serif'>Check this page's url for a (somewhat) more detailed description. Try the troubleshooting steps on the tutorial site.</p>";
    }

    send(csock, response, static_cast<int>(std::strlen(response)), 0);

    closesocket(csock);
    closesocket(lsock);
    WSACleanup();
}
