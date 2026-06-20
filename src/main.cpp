#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/utils/coro.hpp>
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

};



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
    auto clientId = std::string("hq65d75rdxry2cfjgemvydqp2vfr84");
    auto body = fmt::format("client_id={}&scope={}", clientId, "chat:read");

    async::spawn(
        web::WebRequest().header("Content-Type", "application/x-www-form-urlencoded").bodyString(body).post("https://id.twitch.tv/oauth2/device"),
        [this](web::WebResponse resp){
            if (!resp.ok()) {
                log::error("Failed to start device auth: {}", resp.string().unwrapOr(""));
                return;
            }
            auto jsonRes = resp.json();
            if (!jsonRes) {
                log::error("Failed to parse JSON");
                return;
            }
            auto json = jsonRes.unwrap();
            auto deviceCodeRes = json["device_code"].asString();
            auto userCodeRes = json["user_code"].asString();
            auto verificationUriRes = json["verification_uri"].asString();
            auto intervalRes = json["interval"].asInt();
            if (!deviceCodeRes || !userCodeRes || !verificationUriRes || !intervalRes) {
                log::error("Missing required fields in device auth response");
                return;
            }
            auto deviceCode = deviceCodeRes.unwrap();
            auto userCode = userCodeRes.unwrap();
            auto verificationUri = verificationUriRes.unwrap();
            auto interval = intervalRes.unwrap();

            showAuthPopup(userCode, verificationUri);
            pollForToken(deviceCode, interval);
        }
    );
}

class TwitchAuthSettingV3 : public SettingV3 {
public:
    static Result<std::shared_ptr<SettingV3>> parse(std::string const& key, std::string const& modID, matjson::Value const& json) {
        auto ret = std::make_shared<TwitchAuthSettingV3>();
        auto root = checkJson(json, "TwitchAuthSettingV3");
        ret->init(key, modID, root);
        ret->parseNameAndDescription(root);
        ret->parseEnableIf(root);
        root.checkUnknownKeys();
        return root.ok(std::static_pointer_cast<SettingV3>(ret));
    }

    bool load(matjson::Value const& json) override {
        return true;
    }
    bool save(matjson::Value& json) const override {
        return true;
    }
    SettingNodeV3* createNode(float width) override;
    bool isDefaultValue() const override { return true; }
    void reset() override {}
};

class TwitchAuthSettingNode : public SettingNodeV3 {
public:
    static TwitchAuthSettingNode* create(std::shared_ptr<SettingV3> setting, float width) {
        auto ret = new TwitchAuthSettingNode();
        if (ret->init(std::move(setting), width)) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    bool init(std::shared_ptr<SettingV3> setting, float width) {
        if (!SettingNodeV3::init(setting, width)) return false;

        auto menu = this->getButtonMenu();
        if (!menu) {
            menu = CCMenu::create();
            this->addChild(menu);
        }

        auto btn = ButtonSprite::create("Login", "bigFont.fnt", "GJ_button_01.png");
        btn->setScale(0.7f);
        auto item = CCMenuItemSpriteExtra::create(btn, this, menu_selector(TwitchAuthSettingNode::onLogin));
        menu->addChild(item);

        this->setContentSize(cocos2d::CCSizeMake(width, 50.f));
        return true;
    }

    void onLogin(CCObject*) {
        HwGDReqs::get()->startTwitchAuth();
    }

    void onCommit() override {}
    void onResetToDefault() override {}
    bool hasUncommittedChanges() const override { return false; }
    bool hasNonDefaultValue() const override { return false; }
};

SettingNodeV3* TwitchAuthSettingV3::createNode(float width) {
    return TwitchAuthSettingNode::create(std::static_pointer_cast<SettingV3>(shared_from_this()), width);
}

void HwGDReqs::setupCustomSetting() {
    Mod::get()->registerCustomSettingType("twitch-auth", &TwitchAuthSettingV3::parse);
}

void HwGDReqs::loadAuth() {
    auto authJson = Mod::get()->getSavedValue<matjson::Value>("twitch-auth", matjson::Value());
    if (authJson.contains("access_token")) {
        auto res = authJson["access_token"].asString();
        if (res) m_auth.accessToken = res.unwrap();
    }
    if (authJson.contains("refresh_token")) {
        auto res = authJson["refresh_token"].asString();
        if (res) m_auth.refreshToken = res.unwrap();
    }
    if (authJson.contains("user_id")) {
        auto res = authJson["user_id"].asString();
        if (res) m_auth.userId = res.unwrap();
    }
}

void HwGDReqs::saveAuth() {
    matjson::Value obj = matjson::Value();
    obj["access_token"] = m_auth.accessToken;
    obj["refresh_token"] = m_auth.refreshToken;
    obj["user_id"] = m_auth.userId;
    Mod::get()->setSavedValue("twitch-auth", obj);
}


void HwGDReqs::pollForToken(std::string const& deviceCode, int interval) {
    auto task = $async(deviceCode, interval) {
        auto clientId = std::string("hq65d75rdxry2cfjgemvydqp2vfr84");
        auto self = HwGDReqs::get();
        
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
                auto accessTokenRes = json["access_token"].asString();
                if (accessTokenRes) {
                    auto accessToken = accessTokenRes.unwrap();
                    std::string refreshToken;
                    if (json.contains("refresh_token")) {
                        auto refreshTokenRes = json["refresh_token"].asString();
                        if (refreshTokenRes)
                            refreshToken = refreshTokenRes.unwrap();
                    }
                    Loader::get()->queueInMainThread([self, accessToken, refreshToken]() {
                        self->m_auth.accessToken = accessToken;
                        self->m_auth.refreshToken = refreshToken;
                        self->saveAuth();
                        self->getTwitchUserId();
                    });
                    co_return;
                }
            } else {
                auto jsonRes = resp.json();
                if (jsonRes) {
                    auto json = jsonRes.unwrap();
                    auto errorRes = json["error"].asString();
                    if (errorRes) {
                        auto error = errorRes.unwrap();
                        if (error != "authorization_pending") {
                            log::error("Auth failed: {}", error);
                            co_return;
                        }
                    }
                }
            }
            co_await arc::sleep(std::chrono::seconds(interval));
        }
    };
}

void HwGDReqs::getTwitchUserId() {
    async::spawn(
        web::WebRequest()
            .header("Client-ID", "hq65d75rdxry2cfjgemvydqp2vfr84")
            .header("Authorization", fmt::format("Bearer {}", m_auth.accessToken))
            .get("https://api.twitch.tv/helix/users"),
        [this](web::WebResponse resp) {
            if (!resp.ok()) {
                log::error("Failed to get user ID: {}", resp.string().unwrapOr(""));
                return;
            }
            auto jsonRes = resp.json();
            if (!jsonRes) return;
            auto json = jsonRes.unwrap();
            if (json.contains("data") && json["data"].isArray() && json["data"].size() > 0) {
                auto idRes = json["data"][0]["id"].asString();
                if (idRes) {
                    m_auth.userId = idRes.unwrap();
                    saveAuth();
                }
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
    std::string body = R"({"type":"channel.chat.message","version":"1","condition":{"broadcaster_user_id":")" + m_auth.userId + R"(","user_id":")" + m_auth.userId + R"("},"transport":{"method":"websocket","session_id":""}})";
    async::spawn(
        web::WebRequest()
            .header("Client-ID", "hq65d75rdxry2cfjgemvydqp2vfr84")
            .header("Authorization", fmt::format("Bearer {}", m_auth.accessToken))
            .header("Content-Type", "application/json")
            .bodyString(body)
            .post("https://api.twitch.tv/helix/eventsub/subscriptions"),
        [](web::WebResponse resp) {
            log::info("EventSub response: {}", resp.string().unwrapOr(""));
        }
    );
}

void HwGDReqs::pollEvents(std::string const& url, std::string const& sessionId) {
    // implementation soon
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
    auto url = fmt::format("https://www.boomlings.com/database/getGJLevels21.php?gameVersion=22&binaryVersion=42&gdw=0&type=0&str={}&diff=-&len=-&page=0&total=0&uncompleted=0&onlyCompleted=0&featured=0&original=0&twoPlayer=0&coins=0&epic=0&secret=Wmfd2893gb7", levelId);
    async::spawn(
        web::WebRequest().get(url),
        [this, levelId, username](web::WebResponse resp) {
            auto respStrRes = resp.string();
            if (!resp.ok() || !respStrRes || respStrRes.unwrap().empty() || respStrRes.unwrap() == "-1") {
                return;
            }
            auto response = respStrRes.unwrap();
            if (response.find("#") != std::string::npos) {
                auto levelData = response.substr(0, response.find("#"));
                auto parts = utils::string::split(levelData, ":");
                if (parts.size() > 2) {
                    auto levelName = parts[1];
                    auto creatorName = parts.size() > 5 ? parts[5] : "Unknown";
                    log::info("Found {}: {} by {} from {}", levelId, levelName, creatorName, username);
                }
            }
        }
    );
}

$on_mod(Loaded) {
    HwGDReqs::get()->init();
}