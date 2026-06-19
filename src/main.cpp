#include <Geode/Geode.hpp>
#include <Geode/loader/SettingV3.hpp>
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

class HwGDReqs {
public:
    static HwGDReqs* get() {
        static HwGDReqs instance;
        return &instance;
    }

    void init() {
        loadAuth();
        startChatPolling();
        setupCustomSetting();
    }

private:
    TwitchAuth m_auth;
    bool m_polling = false;
    std::thread m_pollThread;
    std::mutex m_mutex;

    void setupCustomSetting() {
        Mod::get()->addCustomSetting<SettingV3>("auth", "custom", [this](auto) {
            startTwitchAuth();
            return nullptr;
        }, nullptr);
    }

    void loadAuth() {
        auto authJson = Mod::get()->getSavedValue<matjson::Value>("twitch-auth", matjson::Object());
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

    void saveAuth() {
        matjson::Object obj;
        obj["access_token"] = m_auth.accessToken;
        obj["refresh_token"] = m_auth.refreshToken;
        obj["user_id"] = m_auth.userId;
        Mod::get()->setSavedValue("twitch-auth", obj);
    }

    void startTwitchAuth() {
        web::WebRequest().post("https://id.twitch.tv/oauth2/device")
            .bodyParam("client_id", "YOUR_TWITCH_CLIENT_ID")
            .bodyParam("scopes", "chat:read")
            .send()
            .then([this](web::WebResponse const& resp) {
                if (!resp.ok()) {
                    log::error("Failed to start device auth: {}", resp.string());
                    return;
                }
                auto json = resp.json();
                auto deviceCode = json["device_code"].asString();
                auto userCode = json["user_code"].asString();
                auto verificationUri = json["verification_uri"].asString();
                auto interval = json["interval"].asInt();

                showAuthPopup(userCode, verificationUri);
                
                pollForToken(deviceCode, interval);
            })
            .expect([](std::string const& err) {
                log::error("Device auth error: {}", err);
            });
    }

    void showAuthPopup(std::string const& code, std::string const& link) {
        Loader::get()->queueInMainThread([code, link]() {
            auto alert = FLAlertLayer::create(
                "Twitch Login",
                fmt::format("Write {} in {}", code, link),
                "Open Link",
                "Cancel"
            );
            alert->setCallback([link](FLAlertLayer*, bool btn2) {
                if (!btn2) {
                    utils::web::openLinkInBrowser(link);
                }
            });
            alert->show();
        });
    }

    void pollForToken(std::string const& deviceCode, int interval) {
        std::thread([this, deviceCode, interval]() {
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(interval));
                
                web::WebRequest().post("https://id.twitch.tv/oauth2/token")
                    .bodyParam("client_id", "YOUR_TWITCH_CLIENT_ID")
                    .bodyParam("device_code", deviceCode)
                    .bodyParam("grant_type", "urn:ietf:params:oauth:grant-type:device_code")
                    .send()
                    .then([this](web::WebResponse const& resp) {
                        if (resp.ok()) {
                            auto json = resp.json();
                            m_auth.accessToken = json["access_token"].asString();
                            m_auth.refreshToken = json["refresh_token"].asString();
                            saveAuth();
                            getTwitchUserId();
                            throw std::runtime_error("Success");
                        } else {
                            auto json = resp.json();
                            if (json.contains("error") && json["error"].asString() != "authorization_pending") {
                                throw std::runtime_error("Auth failed");
                            }
                        }
                    })
                    .expect([](std::string const& err) {
                        if (err != "Success") {
                            log::error("Token poll error: {}", err);
                        }
                    });
            }
        }).detach();
    }

    void getTwitchUserId() {
        web::WebRequest().get("https://api.twitch.tv/helix/users")
            .header("Client-ID", "YOUR_TWITCH_CLIENT_ID")
            .header("Authorization", fmt::format("Bearer {}", m_auth.accessToken))
            .send()
            .then([this](web::WebResponse const& resp) {
                if (!resp.ok()) {
                    log::error("Failed to get user ID: {}", resp.string());
                    return;
                }
                auto json = resp.json();
                m_auth.userId = json["data"][0]["id"].asString();
                saveAuth();
            })
            .expect([](std::string const& err) {
                log::error("Get user ID error: {}", err);
            });
    }

    void startChatPolling() {
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

    void connectToEventSub() {
        web::WebRequest().post("https://api.twitch.tv/helix/eventsub/subscriptions")
            .header("Client-ID", "YOUR_TWITCH_CLIENT_ID")
            .header("Authorization", fmt::format("Bearer {}", m_auth.accessToken))
            .header("Content-Type", "application/json")
            .body(R"({"type":"channel.chat.message","version":"1","condition":{"broadcaster_user_id":")" + m_auth.userId + R"(","user_id":")" + m_auth.userId + R"("},"transport":{"method":"websocket","session_id":""}})")
            .send()
            .then([this](web::WebResponse const& resp) {
                log::info("EventSub response: {}", resp.string());
            })
            .expect([](std::string const& err) {
                log::error("EventSub error: {}", err);
            });
    }

    void pollEvents(std::string const& url, std::string const& sessionId) {
        // For simplicity, we'll just use a basic approach here - in a real implementation you'd use WebSocket
        // This is a placeholder to show the structure
    }

    void checkMessageForLevelId(std::string const& message, std::string const& username) {
        std::regex regex(R"(\b(\d{6,10})\b)");
        std::smatch match;
        if (std::regex_search(message, match, regex)) {
            auto levelId = match[1].str();
            checkLevel(levelId, username);
        }
    }

    void checkLevel(std::string const& levelId, std::string const& username) {
        web::WebRequest().get(fmt::format("https://www.boomlings.com/database/getGJLevels21.php?gameVersion=22&binaryVersion=42&gdw=0&type=0&str={}&diff=-&len=-&page=0&total=0&uncompleted=0&onlyCompleted=0&featured=0&original=0&twoPlayer=0&coins=0&epic=0&secret=Wmfd2893gb7", levelId))
            .send()
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
};

$on_mod(Loaded) {
    HwGDReqs::get()->init();
}
