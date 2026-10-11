#pragma once
#include <cmath>

namespace l2d {
struct TokenBudget
{
    int historyMessages;
    int replyTokens; // 0 preserves the provider's default.
    int steps;
    double idleMultiplier;
    bool idleEnabled;
};

inline int TokenTier(double value)
{
    if (!std::isfinite(value)) return 1;
    return value <= 0 ? 0 : value >= 3 ? 3 : (int)std::lround(value);
}

inline TokenBudget BudgetForTier(double value)
{
    const TokenBudget tiers[] = {
        {6, 0, 6, 1.0, true},
        {4, 1536, 3, 1.5, true},
        {2, 1024, 2, 3.0, true},
        {0, 768, 1, 1.0, false}
    };
    return tiers[TokenTier(value)];
}
}
