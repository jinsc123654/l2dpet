#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include "../src/PetJokes.h"
#include "../src/PetMemory.h"
#include "../src/PetPersona.h"
#include "../src/PetQuestions.h"
#include "../src/PetWeather.h"
#include "../src/LlmClient.h"
#define private public
#include "../src/PetAgent.h"
#undef private
#include "../src/PetAgent.cpp"
#include <cassert>
#include <iostream>

int main()
{
    PetAgent agent;
    l2d::LlmMsg old;
    old.content = "旧角色台词";
    agent._history.push_back(old);
    agent._lastQText = "旧角色的问题";
    agent._lastQOpts = {"旧选项"};
    agent._awaitingUntil = 123;
    agent.ResetConversation("性格更新测试");
    assert(agent._history.empty() && agent._lastQText.empty() && agent._lastQOpts.empty());
    assert(agent._awaitingUntil == 0);
    PetAgent::RpcPtr call;
    assert(agent.PopPending(&call));
    assert(call->line.find("reset") != std::string::npos); // Clear the old displayed choice too.
    agent._deferPersona = true;
    agent._deferPersonaGuidance = "只属于旧模型";
    agent._deferCompress = true;
    agent._backlog[0]["head"] = 3;
    agent._lastHover = "old-zone";
    agent.ClearRoleContext("切换模型测试");
    assert(!agent._deferPersona && agent._deferPersonaGuidance.empty() && !agent._deferCompress);
    assert(agent._backlog[0].empty() && agent._lastHover.empty());
    std::cout << "PASS old dialog, question, bubble reset and deferred role task isolation\n";
}
