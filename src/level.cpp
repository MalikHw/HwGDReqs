#include "HwGDReqs.hpp"

#include <Geode/Geode.hpp>
#include <Geode/utils/web.hpp>
#include <regex>

using namespace geode::prelude;

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
