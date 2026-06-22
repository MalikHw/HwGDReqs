#include "HwGDReqs.hpp"
#include "constants.hpp"

#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/web.hpp>

using namespace geode::prelude;

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

void HwGDReqs::pollToken() {
    log::info("[HwGDReqs] Polling for token...");
    auto body = fmt::format("client_id={}&device_code={}&grant_type={}", 
        TWITCH_CLIENT_ID, m_deviceCode, "urn:ietf:params:oauth:grant-type:device_code");

    m_tokenTask.spawn("hwd-poll-token",
        web::WebRequest()
            .header("Content-Type", "application/x-www-form-urlencoded")
            .bodyString(body)
            .post("https://id.twitch.tv/oauth2/token"),
        [this](web::WebResponse resp) {
            if (!resp.ok()) {
                log::debug("[HwGDReqs] poll response: {}", resp.code());
                reschedule();
                return;
            }
            auto jsonRes = resp.json();
            if (!jsonRes) {
                log::debug("[HwGDReqs] poll json error");
                reschedule();
                return;
            }
            auto json = jsonRes.unwrap();
            if (json.contains("access_token")) {
                auto accessTokenRes = json["access_token"].asString();
                if (accessTokenRes) {
                    m_auth.accessToken = accessTokenRes.unwrap();
                    if (json.contains("refresh_token")) {
                        auto refreshTokenRes = json["refresh_token"].asString();
                        if (refreshTokenRes)
                            m_auth.refreshToken = refreshTokenRes.unwrap();
                    }
                    saveAuth();
                    FLAlertLayer::create("Twitch Login", "Logged in!", "OK")->show();
                    getTwitchUserId();
                }
            } else {
                log::debug("[HwGDReqs] waiting for authorization...");
                reschedule();
            }
        }
    );
}

void HwGDReqs::reschedule() {
    log::info("[HwGDReqs] Rescheduling poll in {} seconds", m_pollInterval);
    if (!m_schedulerNode->getParent()) {
        log::warn("[HwGDReqs] Scheduler not in scene, re-adding...");
        if (auto scene = CCDirector::get()->getRunningScene()) {
            scene->addChild(m_schedulerNode);
        } else {
            log::error("[HwGDReqs] No running scene!");
            return;
        }
    }

    m_schedulerNode->scheduleOnce(
        schedule_selector(TwitchSchedulerNode::rescheduleTick),
        (float)m_pollInterval
    );
}

void HwGDReqs::startTwitchAuth() {
    auto body = fmt::format("client_id={}&scope={}", TWITCH_CLIENT_ID, "chat:read");

    m_deviceTask.spawn("hwd-device",
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
            m_deviceCode = deviceCodeRes.unwrap();
            auto userCode = userCodeRes.unwrap();
            auto verificationUri = verificationUriRes.unwrap();
            m_pollInterval = intervalRes.unwrap();

            showAuthPopup(userCode, verificationUri);
            if (!m_schedulerNode) {
                m_schedulerNode = TwitchSchedulerNode::create();
                m_schedulerNode->retain();
                m_schedulerNode->onReschedule = [this]() { pollToken(); };
            }
            if (auto scene = CCDirector::get()->getRunningScene()) {
                if (!m_schedulerNode->getParent()) {
                    scene->addChild(m_schedulerNode);
                }
            }
            
            pollToken();
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
    (void)Mod::get()->registerCustomSettingType("twitch-auth", &TwitchAuthSettingV3::parse);
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

void HwGDReqs::getTwitchUserId() {
    async::spawn(
        web::WebRequest()
            .header("Client-ID", TWITCH_CLIENT_ID)
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
