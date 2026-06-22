#include "HwGDReqs.hpp"

#include <Geode/Geode.hpp>

using namespace geode::prelude;

void HwGDReqs::init() {
    loadAuth();
    

    m_schedulerNode = TwitchSchedulerNode::create();
    m_schedulerNode->retain();
    m_schedulerNode->onReschedule = [this]() { pollToken(); };
    if (auto scene = CCDirector::get()->getRunningScene()) {
        scene->addChild(m_schedulerNode);
    }
    
    startChatPolling();
    setupCustomSetting();
}

$on_mod(Loaded) {
    HwGDReqs::get()->init();
}
