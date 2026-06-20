#include "HwGDReqs.hpp"
#include "constants.hpp"

#include <Geode/Geode.hpp>
#include <Geode/utils/web.hpp>

using namespace geode::prelude;

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
            .header("Client-ID", TWITCH_CLIENT_ID)
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
