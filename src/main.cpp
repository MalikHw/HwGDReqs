#include "HwGDReqs.hpp"

#include <Geode/Geode.hpp>

using namespace geode::prelude;

void HwGDReqs::init() {
    loadAuth();
    startChatPolling();
    setupCustomSetting();
}

$on_mod(Loaded) {
    HwGDReqs::get()->init();
}
