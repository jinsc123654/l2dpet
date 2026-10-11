#include "../src/PetPersona.h"
#include <cassert>
#include <iostream>

int main()
{
    PetPersona persona;
    persona.Load("persona-guidance-test"); // Initialize the per-model storage path.
    const std::string hint = "嘴硬心软，少聊天气；口癖是\"哼哼\"。\n喜欢音乐。";
    std::string err;
    auto generate = [&](const std::string&, const std::string& user, std::string* out, std::string*) {
        assert(user.find(hint) != std::string::npos);
        assert(user.find("exp:happy") != std::string::npos);
        *out = R"({"name":"芙芙","persona":"嘴硬心软，喜欢音乐。","speech":"哼哼","fav_expr":["exp:happy","exp:invented"]})";
        return true;
    };
    assert(persona.Generate("persona-guidance-test", "pet", "capabilities", {"exp:happy"}, generate, &err, hint));
    assert(persona.Get().userGuidance == hint);
    assert(persona.Get().favExpr.size() == 1);
    assert(persona.Save(&err));
    assert(persona.SetNames("自定义名字", "小名", "哥哥", &err));
    PetPersona restored;
    assert(restored.Load("persona-guidance-test"));
    assert(restored.Get().userGuidance == hint);
    auto freeGenerate = [](const std::string&, const std::string& user, std::string* out, std::string*) {
        assert(user.find("主人希望的性格方向") == std::string::npos);
        *out = R"({"name":"自由名字","persona":"温柔好奇。"})";
        return true;
    };
    assert(restored.Generate("persona-guidance-test", "pet", "capabilities", {}, freeGenerate, &err));
    assert(restored.Get().userGuidance.empty());
    assert(restored.Get().userName == "自定义名字");
    assert(restored.Get().userNick == "小名");
    assert(restored.Get().userCall == "哥哥");
    assert(restored.Save(&err));
    assert(persona.Load("persona-guidance-test"));
    assert(persona.Get().userGuidance.empty());
    std::cout << "PASS guidance prompt, expressions, persistence, clearing and name preservation\n";
}
