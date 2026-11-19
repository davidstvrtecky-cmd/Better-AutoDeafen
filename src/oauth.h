#pragma once

#include <atomic>
#include <thread>

namespace oauth {

    inline std::atomic<bool> serverRunning = false;

    void serverThread();

    inline void startServer() {
        if (serverRunning.exchange(true)) {
            return;
        }
        std::thread(serverThread).detach();
    }

}
