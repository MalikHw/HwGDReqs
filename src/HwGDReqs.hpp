#pragma once

#include <string>

#include <Geode/Geode.hpp>
#include <Geode/utils/async.hpp>

struct TwitchAuth {
    std::string accessToken;
    std::string refreshToken;
    std::string userId;
};

class TwitchSchedulerNode : public cocos2d::CCNode {
public:
    std::function<void()> onPoll;
    std::function<void()> onReschedule;
    static TwitchSchedulerNode* create() {
        auto ret = new TwitchSchedulerNode();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
    void pollTick(float) {if (onPoll) onPoll();}
    void rescheduleTick(float) {
        this->unschedule(schedule_selector(TwitchSchedulerNode::rescheduleTick));
        if (onReschedule) onReschedule();
    }
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
    std::string m_deviceCode;
    int m_pollInterval = 5;
    geode::async::TaskHolder<geode::web::WebResponse> m_deviceTask;
    geode::async::TaskHolder<geode::web::WebResponse> m_tokenTask;
    geode::async::TaskHolder<geode::web::WebResponse> m_userIdTask;
    TwitchSchedulerNode* m_schedulerNode = nullptr;

    void setupCustomSetting();
    void loadAuth();
    void saveAuth();
    void showAuthPopup(std::string const& code, std::string const& link);
    void pollToken();
    void reschedule();
    void getTwitchUserId();
    void startChatPolling();
    void connectToEventSub();
    void pollEvents(std::string const& url, std::string const& sessionId);
    void checkMessageForLevelId(std::string const& message, std::string const& username);
    void checkLevel(std::string const& levelId, std::string const& username);
};
