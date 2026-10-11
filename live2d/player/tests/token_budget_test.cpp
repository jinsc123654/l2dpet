#include "../src/TokenBudget.h"
#include "../src/LlmClient.cpp"
#include <cassert>
#include <limits>
#include <iostream>

int main()
{
    using namespace l2d;
    assert(TokenTier(-5) == 0 && TokenTier(99) == 3);
    assert(TokenTier(std::numeric_limits<double>::quiet_NaN()) == 1);
    assert(TokenTier(1.6) == 2);
    const int history[] = {6, 4, 2, 0};
    for (int tier = 0; tier < 4; ++tier)
    {
        const TokenBudget b = BudgetForTier(tier);
        assert(b.historyMessages == history[tier]);
        assert(b.historyMessages % 2 == 0); // Keep complete user/assistant pairs.
        LlmSettings cfg;
        cfg.maxReplyTokens = b.replyTokens;
        LlmMsg question;
        question.role = "user";
        question.content = "当前问题";
        const std::string body = BuildBody(cfg, {question}, 0.9f, true);
        jsonlite::Value parsed;
        assert(jsonlite::parse(body.c_str(), body.size(), parsed, NULL));
        assert(parsed.get("messages").arr.size() == 1);
        if (tier == 0) assert(!parsed.get("max_tokens").isNum());
        else assert(parsed.get("max_tokens").num == b.replyTokens);
    }
    assert(BudgetForTier(1).idleMultiplier == 1.5);
    assert(BudgetForTier(2).idleMultiplier == 3.0);
    assert(!BudgetForTier(3).idleEnabled);
    // A fresh maintenance configuration must not inherit a dialogue output cap.
    LlmSettings maintenance;
    assert(BuildBody(maintenance, {}, 0.3f, true).find("max_tokens") == std::string::npos);
    std::cout << "PASS token tiers, complete history pairs and independent maintenance budgets\n";
}
