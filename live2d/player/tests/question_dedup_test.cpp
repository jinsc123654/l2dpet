#include "../src/PetAgent.cpp"
#include <cassert>
#include <iostream>

static jsonlite::Value Parse(const std::string& text)
{
    jsonlite::Value v;
    assert(jsonlite::parse(text.c_str(), text.size(), v, NULL));
    return v;
}

int main()
{
    PetQuestions q;
    q.Load("dedup-test"); // Test runner supplies an isolated home.
    q.MarkAsked("主人平时做什么呀？", {"工作", "上学"});
    assert(q.Fresh(10).empty());
    assert(q.AlreadyAsked("你是工作还是上学呢？"));
    const auto generate = [](const std::string&, const std::string&, std::string* out, std::string*) {
        *out = R"({"items":[{"topic":"工作学习","q":"你是工作还是上学呢？","options":["工作","上学"]},{"topic":"吃","q":"主人你平时爱吃甜的还是咸的？","options":["甜的","咸的"]},{"topic":"作息","q":"主人你平时几点睡觉？","options":["早睡","晚睡"]}]})";
        return true;
    };
    std::string err;
    assert(q.Generate("dedup-test", "Test", "", {}, generate, &err));
    assert(!QuestionsNeedRefresh(q)); // Two unasked questions are enough; no early refill.
    assert(QuestionsNeedRefresh(q, 0)); // Full mode may refill fewer than three remaining questions.
    assert(!QuestionsNeedRefresh(q, 2) && !QuestionsNeedRefresh(q, 3));
    const auto fresh = q.Fresh(10);
    assert(fresh.size() == 2); // Common prefix must not merge unrelated diet/sleep questions.
    assert(!fresh[0]->id.empty() && fresh[0]->id != fresh[1]->id);
    const std::string id = fresh[0]->id;
    const std::string canonical = fresh[0]->q;
    jsonlite::Value seq = Parse("{\"steps\":[{\"speak\":\"换一种口吻\",\"question_id\":\"" + id + "\"}]}");
    RestoreQuestionOptions(&seq, fresh, &q);
    assert(seq.get("steps").arr[0].get("speak").str == canonical);
    assert(seq.get("steps").arr[0].get("options").arr.size() == 2);
    q.MarkAsked(canonical, {"甜的", "咸的"});
    q.MarkAnswered(canonical, "甜的");
    assert(q.Save(&err));
    PetQuestions loaded;
    assert(loaded.Load("dedup-test"));
    assert(loaded.AlreadyAsked("你是工作还是上学呢？"));
    assert(loaded.KnownText().find("甜的") != std::string::npos);
    assert(loaded.Generate("dedup-test", "Test", "", {}, generate, &err));
    assert(loaded.Fresh(10).size() == 1); // History survives regeneration and restart.
    assert(!QuestionsNeedRefresh(loaded)); // Keep even the last remaining unasked question.
    loaded.MarkAsked(loaded.Fresh(1)[0]->q, {"早睡", "晚睡"});
    assert(QuestionsNeedRefresh(loaded)); // Refill only once the complete bank is exhausted.
    seq = Parse(R"({"steps":[{"speak":"你是工作还是上学呢？","options":["工作","上学"],"emotion":"curious"},{"speak":"你现在在干什么？","options":["甲","乙"]}]})");
    RestoreQuestionOptions(&seq, loaded.Fresh(3), &loaded);
    assert(!seq.get("steps").arr[0].has("speak"));
    assert(seq.get("steps").arr[0].has("emotion"));
    assert(!seq.get("steps").arr[1].has("speak"));
    seq = Parse(R"({"steps":[{"speak":"你平时做什么我都记得，我会陪着你。"}]})");
    RestoreQuestionOptions(&seq, {}, &loaded);
    assert(seq.get("steps").arr[0].has("speak")); // Normal statements are not questions.
    seq = Parse(R"({"steps":[{"speak":"你平时做什么呀"}]})");
    RestoreQuestionOptions(&seq, {}, &loaded);
    assert(!seq.get("steps").arr[0].has("speak")); // Missing punctuation must not bypass question guards.
    std::cout << "PASS stable IDs, canonical answers, unanswered/answered history, regeneration and display guard\n";
}
