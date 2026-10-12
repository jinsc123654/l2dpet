#include "../src/PetCompanion.h"
#include <cassert>
#include <iostream>

int main()
{
    PetCompanion pet;
    auto start = pet.Receive("started", "Codex", "Implement the feature", "turn1", 0, 30, true, false, "zh");
    assert(start.shown && !start.speech.empty());
    auto repeated = pet.Receive("started", "Codex", "Implement the feature", "turn1", 1, 30, true, false, "zh");
    assert(!repeated.shown && repeated.reason == "duplicate");
    auto changedDuplicate = pet.Receive("started", "Codex", "Changed summary", "turn1", 2, 30, true, false, "zh");
    assert(!changedDuplicate.shown && changedDuplicate.reason == "duplicate");
    auto early = pet.Receive("progress", "Codex", "Found a clue", "step1", 10, 30, true, false, "zh");
    assert(!early.shown && early.reason == "rate_limited");
    auto progress = pet.Receive("progress", "Codex", "Found a clue", "step1", 31, 30, true, false, "zh");
    assert(progress.shown);
    auto failure = pet.Receive("failed", "Codex", "Build exited with code 2", "step2", 32, 30, true, false, "zh");
    assert(failure.shown); // errors bypass the ordinary progress interval
    assert(pet.Events().back().message == "Build exited with code 2");
    auto waiting = pet.Receive("waiting", "Codex", "Choose a target", "step3", 33, 30, true, true, "zh");
    assert(!waiting.shown && waiting.reason == "awaiting_choice");
    assert(pet.Events().back().message == "Choose a target"); // original retained without replacing choices
    auto disabled = pet.Receive("completed", "Codex", "result", "turn2", 34, 30, false, false, "zh");
    assert(!disabled.shown && disabled.reason == "disabled");
    auto invalid = pet.Receive("unknown", "Codex", "result", "turn2", 35, 30, true, false, "zh");
    assert(!invalid.shown && invalid.reason == "invalid_kind");
    auto english = pet.Receive("completed", "Codex", "Tests still fail", "turn3", 36, 30, true, false, "en");
    assert(english.shown && english.speech.find("success") == std::string::npos);
    assert(pet.Events().back().message == "Tests still fail"); // completion does not invent success
    auto japanese = pet.Receive("interrupted", "Codex", "", "turn4", 37, 30, true, false, "ja");
    assert(japanese.shown && !japanese.speech.empty());
    auto newTurn = pet.Receive("started", "Codex", "New work", "turn5", 38, 30, true, false, "zh");
    assert(newTurn.shown);
    for (int i = 0; i < 80; ++i)
        pet.Receive("completed", "Codex", "message " + std::to_string(i), "turn" + std::to_string(i + 10), 50 + i, 30, true, false, "zh");
    assert(pet.Events().size() == 60);
    assert(pet.Events().front().message == "message 20");
    assert(pet.Events().back().message == "message 79");
    std::cout << "PASS companion deduplication, throttling, priorities, choices, disable switch, truthful completion, languages and bounded history\n";
}
