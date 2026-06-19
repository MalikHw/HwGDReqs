#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/loader/SettingNodeV3.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/web.hpp>
#include <regex>
#include <thread>
#include <mutex>

using namespace geode::prelude;

struct TwitchAuth {
    std::string accessToken;
    std::string refreshToken;
    std::string userId;
};

class HwGDReqs;

class TwitchAuthSetting : public SettingNodeV3 {
public:
    static TwitchAuthSetting* create(SettingV3* setting) {
        auto ret = new TwitchAuthSetting();
        if (ret->init(setting)) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    bool init(SettingV3* setting) override;
    void onLogin(CCObject*);
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
    void pollForToken(std::string const& deviceCode, int interval);
    void getTwitchUserId();
    void startChatPolling();
    void connectToEventSub();
    void pollEvents(std::string const& url, std::string const& sessionId);
    void checkMessageForLevelId(std::string const& message, std::string const& username);
    void checkLevel(std::string const& levelId, std::string const& username);

    class AuthPopup;
};

// Simplified: use geode::createQuickPopup in showAuthPopup instead of custom FLAlertLayer/Protocol

bool TwitchAuthSetting::init(SettingV3* setting) {
    if (!SettingNodeV3::init(setting)) return false;
    
    auto menu = CCMenu::create();
    
    auto btn = ButtonSprite::create("Login", "bigFont.fnt", "GJ_button_01.png");
    btn->setScale(0.7f);
    auto item = CCMenuItemSpriteExtra::create(
        btn, this, menu_selector(TwitchAuthSetting::onLogin)
    );
    menu->addChild(item);
    menu->setLayout(RowLayout::create());
    
    this->setContentHeight(50.f);
    this->addChild(menu);
    
    return true;
}

void TwitchAuthSetting::onLogin(CCObject*) {
    HwGDReqs::get()->startTwitchAuth();
}

void HwGDReqs::init() {
    loadAuth();
    startChatPolling();
    setupCustomSetting();
}

void HwGDReqs::showAuthPopup(std::string const& code, std::string const& link) {
    Loader::get()->queueInMainThread([code, link]() {
        geode::createQuickPopup(
            "Twitch Login",
            fmt::format("Write {} in {}", code, link),
            "Cancel", "Open Link",
            [link](auto, bool btn2){
                if (btn2) {
                    utils::web::openLinkInBrowser(link);
                }
            }
        );
    });
}

void HwGDReqs::startTwitchAuth() {
    // Twitch device auth: POST form and handle response on main thread using async
    auto clientId = std::string("YOUR_TWITCH_CLIENT_ID");
    auto body = fmt::format("client_id={}&scope={}", clientId, "chat:read");

    async::spawn(
        web::WebRequest().header("Content-Type", "application/x-www-form-urlencoded").bodyString(body).post("https://id.twitch.tv/oauth2/device"),
        [this](web::WebResponse resp){
            if (!resp.ok()) {
                log::error("Failed to start device auth: {}", resp.string());
                return;
            }
            auto jsonRes = resp.json();
            if (!jsonRes) {
                log::error("Failed to parse JSON");
                return;
            }
            auto json = jsonRes.unwrap();
            auto deviceCode = json["device_code"].asString();
            auto userCode = json["user_code"].asString();
            auto verificationUri = json["verification_uri"].asString();
            auto interval = json["interval"].asInt();

            showAuthPopup(userCode, verificationUri);
            pollForToken(deviceCode, interval);
        }
    );
}

void HwGDReqs::setupCustomSetting() {
    // Register a custom setting type for v3 settings
    Mod::get()->addCustomSettingType("twitch-auth", [](SettingV3* setting) {
        return TwitchAuthSetting::create(setting);
    });
}

void HwGDReqs::loadAuth() {
    auto authJson = Mod::get()->getSavedValue<matjson::Value>("twitch-auth", matjson::makeObject());
    if (authJson.contains("access_token")) {
        m_auth.accessToken = authJson["access_token"].asString();
    }
    if (authJson.contains("refresh_token")) {
        m_auth.refreshToken = authJson["refresh_token"].asString();
    }
    if (authJson.contains("user_id")) {
        m_auth.userId = authJson["user_id"].asString();
    }
}

void HwGDReqs::saveAuth() {
    matjson::Value obj = matjson::makeObject();
    obj["access_token"] = m_auth.accessToken;
    obj["refresh_token"] = m_auth.refreshToken;
    obj["user_id"] = m_auth.userId;
    Mod::get()->setSavedValue("twitch-auth", obj);
}

void HwGDReqs::showAuthPopup(std::string const& code, std::string const& link) {
    Loader::get()->queueInMainThread([code, link]() {
        AuthPopup::create(code, link)->show();
    });
}

void HwGDReqs::pollForToken(std::string const& deviceCode, int interval) {
    // Use async coroutine to poll without blocking threads
    async::spawn([this, deviceCode, interval] -> arc::Future<> {
        auto clientId = std::string("YOUR_TWITCH_CLIENT_ID");
        while (true) {
            auto body = fmt::format("client_id={}&device_code={}&grant_type={}", clientId, deviceCode, "urn:ietf:params:oauth:grant-type:device_code");
            auto resp = co_await web::WebRequest()
                .header("Content-Type", "application/x-www-form-urlencoded")
                .bodyString(body)
                .post("https://id.twitch.tv/oauth2/token");

            if (resp.ok()) {
                auto jsonRes = resp.json();
                if (!jsonRes) {
                    co_return;
                }
                auto json = jsonRes.unwrap();
                if (json.contains("access_token")) {
                    m_auth.accessToken = json["access_token"].asString();
                    if (json.contains("refresh_token"))
                        m_auth.refreshToken = json["refresh_token"].asString();
                    saveAuth();
                    getTwitchUserId();
                    co_return;
                }
            } else {
                auto jsonRes = resp.json();
                if (jsonRes) {
                    auto json = jsonRes.unwrap();
                    if (json.contains("error") && json["error"].asString() != "authorization_pending") {
                        log::error("Auth failed: {}", json["error"].asString());
                        co_return;
                    }
                }
            }

            co_await arc::sleep(asp::Duration::fromSecs(interval));
        }
    });
}

void HwGDReqs::getTwitchUserId() {
    async::spawn(
        web::WebRequest()
            .header("Client-ID", "YOUR_TWITCH_CLIENT_ID")
            .header("Authorization", fmt::format("Bearer {}", m_auth.accessToken))
            .get("https://api.twitch.tv/helix/users"),
        [this](web::WebResponse resp) {
            if (!resp.ok()) {
                log::error("Failed to get user ID: {}", resp.string());
                return;
            }
            auto jsonRes = resp.json();
            if (!jsonRes) return;
            auto json = jsonRes.unwrap();
            if (json.contains("data") && json["data"].isArray() && json["data"].size() > 0) {
                m_auth.userId = json["data"][0]["id"].asString();
                saveAuth();
            }
        }
    );
}

void HwGDReqs::startChatPolling() {
    if (m_polling || m_auth.accessToken.empty() || m_auth.userId.empty()) return;
    
    m_polling = true;
    m_pollThread = std::thread([this]() {
        std::string serverUrl;
        std::string sessionId;
        
        while (m_polling) {
            if (serverUrl.empty()) {
                connectToEventSub();
            } else {
                pollEvents(serverUrl, sessionId);
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });
}

void HwGDReqs::connectToEventSub() {
    // Send subscription request
    auto body = fmt::format(R"({"type":"channel.chat.message","version":"1","condition":{"broadcaster_user_id":"{}","user_id":"{}"},"transport":{"method":"websocket","session_id":""}})", m_auth.userId, m_auth.userId);
    async::spawn(
        web::WebRequest()
            .header("Client-ID", "YOUR_TWITCH_CLIENT_ID")
            .header("Authorization", fmt::format("Bearer {}", m_auth.accessToken))
            .header("Content-Type", "application/json")
            .bodyString(body)
            .post("https://api.twitch.tv/helix/eventsub/subscriptions"),
        [](web::WebResponse resp) {
            log::info("EventSub response: {}", resp.string());
        }
    );
}

void HwGDReqs::pollEvents(std::string const& url, std::string const& sessionId) {
    // For simplicity, we'll just use a basic approach here - in a real implementation you'd use WebSocket
    // This is a placeholder to show the structure
}

void HwGDReqs::checkMessageForLevelId(std::string const& message, std::string const& username) {
    std::regex regex(R"(\b(\d{6,10})\b)");
    std::smatch match;
    if (std::regex_search(message, match, regex)) {
        auto levelId = match[1].str();
        checkLevel(levelId, username);
    }
}

void HwGDReqs::checkLevel(std::string const& levelId, std::string const& username) {
    auto req = web::WebRequest();
    req.get(fmt::format("https://www.boomlings.com/database/getGJLevels21.php?gameVersion=22&binaryVersion=42&gdw=0&type=0&str={}&diff=-&len=-&page=0&total=0&uncompleted=0&onlyCompleted=0&featured=0&original=0&twoPlayer=0&coins=0&epic=0&secret=Wmfd2893gb7", levelId));
    req.send()
        .then([this, levelId, username](web::WebResponse const& resp) {
            if (!resp.ok() || resp.string().empty() || resp.string() == "-1") {
                return;
            }
            auto response = resp.string();
            if (response.find("#") != std::string::npos) {
                auto levelData = response.substr(0, response.find("#"));
                auto parts = utils::string::split(levelData, ":");
                if (parts.size() > 2) {
                    auto levelName = parts[1];
                    auto creatorName = parts.size() > 5 ? parts[5] : "Unknown";
                    log::info("Found {}: {} by {} from {}", levelId, levelName, creatorName, username);
                }
            }
        })
        .expect([](std::string const& err) {
            log::error("Check level error: {}", err);
        });
}

$on_mod(Loaded) {
    HwGDReqs::get()->init();
}
