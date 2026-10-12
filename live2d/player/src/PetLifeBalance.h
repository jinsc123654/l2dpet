#pragma once
#include <algorithm>

// Shared numerical policy; rendering, dialogue and state transitions stay in PlayerLife.
namespace petlife {
constexpr float kGentleTouchCost = 0.008f;
constexpr float kGentleAffectionGain = 0.025f;
constexpr float kLonelyFullSeconds = 360.0f;
inline float Clamp(float value) { return std::max(0.0f, std::min(1.0f, value)); }
inline float AffectionGain(float affection, float gain) {
    return gain * std::min(1.0f, (1.0f - affection) * 1.6f) * (affection < 0.25f ? 0.5f : 1.0f);
}
inline float PlayMultiplier(int combo, bool gentle) {
    return std::min(1.0f + 0.25f * (combo - 1), gentle ? 1.25f : 2.0f);
}
struct Irritation {
    int combo = 0;
    float last = -100.0f;
    float Add(float now, float base, bool rude, float affection) {
        combo = now - last <= 5.0f ? std::min(combo + 1, 6) : 1;
        last = now;
        float gain = base + (rude ? 0.08f : 0.045f) * (combo - 1);
        // Friendship softens ordinary pokes, but never removes a disliked boundary.
        if (!rude && affection > 0.7f) gain *= 0.7f;
        return gain;
    }
    void Soothe() { combo = 0; }
    float Decay(float now, float dt, bool sulking) const {
        // Don't cancel the previous poke while the user is still bothering her.
        return now - last <= 5.0f ? 0.0f : dt / (sulking ? 90.0f : 60.0f);
    }
};
}
