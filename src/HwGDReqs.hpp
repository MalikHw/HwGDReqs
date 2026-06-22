#pragma once

#include <mutex>
#include <string>
#include <thread>

#include <Geode/utils/async.hpp>

struct TwitchAuth {
    std::string accessToken;
    std::string refreshToken;
    std::string userId;
};

class HwGDReqs {
public:
    static HwGDReqs* get() {
        static HwGDReqs instance;
        return &instance;
    }

    void init();
    void startTwitchAuth();

private:
    TwitchAuth m_auth;
    bool m_polling = false;
    std::thread m_pollThread;
    std::mutex m_mutex;

    void setupCustomSetting();
    void loadAuth();
    void saveAuth();
    void showAuthPopup(std::string const& code, std::string const& link);
    arc::Future<> pollForToken(std::string deviceCode, int interval);
    void getTwitchUserId();
    void startChatPolling();
    void connectToEventSub();
    void pollEvents(std::string const& url, std::string const& sessionId);
    void checkMessageForLevelId(std::string const& message, std::string const& username);
    void checkLevel(std::string const& levelId, std::string const& username);
};
