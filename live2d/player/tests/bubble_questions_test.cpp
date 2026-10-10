// Standalone regression test (no Cubism SDK or LLM key required):
// g++ -std=c++11 -ffunction-sections -fdata-sections live2d/player/tests/bubble_questions_test.cpp -Wl,--gc-sections -pthread -o /tmp/bubble_questions_test
#include "../src/PetAgent.cpp"
#include <cassert>
#include <iostream>

static jsonlite::Value Parse(const std::string& input)
{
    jsonlite::Value value;
    assert(jsonlite::parse(input.c_str(), input.size(), value, NULL));
    return value;
}

int main()
{
    PetQuestions::Item posture, travel;
    posture.q = "写代码的时候，你呀是坐得直直的还是缩成一团呀？";
    posture.options = {"坐直直的", "缩成一团", "趴着敲呢"};
    travel.q = "以后要是出门玩，你呀想去海边还是山里呀？";
    travel.options = {"去海边", "进山里", "哪儿都行"};
    const std::vector<const PetQuestions::Item*> candidates = {&posture, &travel};

    // Screenshot: model repeats the candidate with a prefix/comma but omits options.
    jsonlite::Value seq = Parse(R"({"steps":[{"at":0,"speak":"唔……写代码的时候，你呀是坐得直直的，还是缩成一团呀？"}]})");
    RestoreQuestionOptions(&seq, candidates);
    assert(seq.get("steps").arr[0].get("options").arr.size() == 3);
    assert(seq.get("steps").arr[0].get("options").arr[1].str == "缩成一团");
    std::string cut;
    assert(ThrottleSeq(&seq, 1000, 0, true, &cut));

    // Numbered selection also supports paraphrasing and repairs malformed options.
    seq = Parse(R"({"steps":[{"speak":"想去哪儿玩呀？","question_id":2,"options":"去海边/进山里"}]})");
    RestoreQuestionOptions(&seq, candidates);
    assert(seq.get("steps").arr[0].get("options").arr[0].str == "去海边");
    assert(!seq.get("steps").arr[0].has("question_id"));

    // Keep valid model-authored choices and their immediate actions.
    seq = Parse(R"({"steps":[{"speak":"想去哪儿？","question_id":2,"options":[{"label":"海边","do":{"emotion":"happy"}},"山里"]}]})");
    RestoreQuestionOptions(&seq, candidates);
    assert(seq.get("steps").arr[0].get("options").arr[0].get("do").get("emotion").str == "happy");

    // Do not attach unrelated choices to weather chit-chat or unknown identifiers.
    seq = Parse(R"({"steps":[{"speak":"南京今天热不热呀？","question_id":99}]})");
    RestoreQuestionOptions(&seq, candidates);
    assert(!seq.get("steps").arr[0].has("options"));
    assert(!seq.get("steps").arr[0].has("question_id"));
    seq = Parse(R"({"steps":[{"speak":"随口聊天","question_id":1}]})");
    RestoreQuestionOptions(&seq, {});
    assert(!seq.get("steps").arr[0].has("options"));

    // Cooldown / disabled questions must not leave an unanswerable question bubble.
    for (int disabled = 0; disabled < 2; ++disabled)
    {
        seq = Parse(R"({"steps":[{"at":0,"speak":"选哪个？","options":["甲","乙"],"emotion":"curious"}]})");
        cut.clear();
        assert(!ThrottleSeq(&seq, 1000, disabled ? 0 : 900, !disabled, &cut));
        assert(!seq.get("steps").arr[0].has("speak"));
        assert(seq.get("steps").arr[0].has("emotion"));
    }

    // Only one question; later speech is removed, but non-speech animation stays.
    seq = Parse(R"({"steps":[{"at":0,"speak":"开场"},{"at":2,"speak":"选哪个？","options":["甲","乙"]},{"at":4,"speak":"覆盖问题","emotion":"happy"},{"at":6,"speak":"第二题？","options":["丙","丁"],"look":"viewer"}]})");
    cut.clear();
    assert(ThrottleSeq(&seq, 1000, 0, true, &cut));
    assert(seq.get("steps").arr.size() == 4);
    assert(seq.get("steps").arr[0].has("speak"));
    assert(seq.get("steps").arr[1].has("options"));
    assert(!seq.get("steps").arr[2].has("speak"));
    assert(seq.get("steps").arr[2].has("emotion"));
    assert(!seq.get("steps").arr[3].has("speak"));
    assert(!seq.get("steps").arr[3].has("options"));
    assert(seq.get("steps").arr[3].has("look"));
    std::cout << "Bubble question regression tests passed\n";
}
