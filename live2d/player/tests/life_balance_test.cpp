#include "../src/PetLifeBalance.h"
#include <cassert>
#include <iostream>

int Irritate(float base, bool rude, float affection, float interval) {
    petlife::Irritation pressure;
    float annoy = 0, now = 0;
    for (int hits = 1; hits <= 100; ++hits) {
        // Advance between interactions at a realistic one-second tick rate.
        for (float elapsed = 0; elapsed < interval; elapsed += 1.0f) {
            now += 1.0f;
            annoy = petlife::Clamp(annoy - pressure.Decay(now, 1.0f, false));
        }
        annoy = petlife::Clamp(annoy + pressure.Add(now, base, rude, affection));
        if (annoy >= 0.7f) return hits;
    }
    return 101;
}
int main() {
    // Alternating zones and repeated disliked rubs share the same timed pressure tracker.
    assert(Irritate(0.06f, false, 0.3f, 1) == 5);
    assert(Irritate(0.22f, true, 0.3f, 1) == 3);
    assert(Irritate(0.14f, true, 0.3f, 1) == 4);
    assert(Irritate(0.06f, false, 0.9f, 1) > 5);
    assert(Irritate(0.22f, true, 0.9f, 1) == 3);
    assert(Irritate(0.06f, false, 0.3f, 10) == 101); // Occasional interaction doesn't make her angry.
    petlife::Irritation pressure;
    pressure.Add(1, 0.06f, false, 0.3f);
    assert(pressure.Decay(4, 1, false) == 0);
    assert(pressure.Decay(7, 1, false) > 0);
    pressure.Soothe();
    assert(pressure.Add(2, 0.06f, false, 0.3f) == 0.06f);
    float affection = 0.3f, energy = 0.65f;
    int rubs = 0;
    for (; rubs < 60 && affection <= 0.7f; ++rubs) {
        assert(energy >= 0.3f); // Affection must be reachable before tired interactions penalize it.
        energy = petlife::Clamp(energy - petlife::kGentleTouchCost * petlife::PlayMultiplier(rubs + 1, true));
        affection += petlife::AffectionGain(affection, petlife::kGentleAffectionGain);
    }
    assert(affection > 0.7f && energy > 0.3f);
    const int friendlyRubs = rubs;
    energy = 0.8f;
    int plays = 0;
    while (energy > 0.02f && plays < 100) {
        ++plays;
        energy = petlife::Clamp(energy - 0.015f * petlife::PlayMultiplier(plays, false));
    }
    assert(plays < 35); // Extended rapid play can reach exhausted sleep.
    float lonely = 0.2f;
    int seconds = 0;
    while (lonely <= 0.6f && seconds < 600) {
        ++seconds;
        if (seconds > 45) lonely = petlife::Clamp(lonely + 1.0f / petlife::kLonelyFullSeconds);
    }
    assert(seconds >= 180 && seconds < 200); // Active user elsewhere -> seek in about three minutes.
    assert(petlife::Clamp(lonely - 0.25f) < 0.6f); // Actual company settles loneliness.
    std::cout << "PASS: normal poke 5, disliked poke 3, disliked rub 4; affection high after "
              << friendlyRubs << " gentle rubs; exhaustion after " << plays << " plays; seek after "
              << seconds << " seconds; pressure reset and passive recovery\n";
}
