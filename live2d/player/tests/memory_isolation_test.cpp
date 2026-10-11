#include "../src/PetMemory.h"
#include "../src/Platform.h"
#include <cassert>
#include <iostream>

int main()
{
    PetMemory a("isolation-a", "A"), b("isolation-b", "B");
    const auto zone = [](const std::string& s) { return s; };
    std::string err;
    a.Note("主人喜欢音乐", 2);
    auto valid = [](const std::string&, std::string* owner, std::string* us) {
        *owner = "## 关于主人\n- 喜欢音乐";
        *us = "## 最近发生\n- 10-11 主人告诉我喜欢音乐\n\n## 更早\n（空）";
        return true;
    };
    assert(a.Consolidate(valid, zone, false, &err));
    assert(a.Owner() == b.Owner()); // Owner facts are deliberately shared.
    assert(b.Us().find("喜欢音乐") == std::string::npos);
    a.Note("新偏好", 2);
    const auto previous = a.Us();
    auto oversized = [&](const std::string& p, std::string* owner, std::string* us) {
        valid(p, owner, us);
        *us += std::string(6000, 'x');
        return true;
    };
    assert(!a.Consolidate(oversized, zone, false, &err));
    assert(a.Us() == previous && a.PendingWeight() > 0);
    const auto ownerPath = plat::HomeDir() + "/.config/l2dpet/memory/owner.md";
    const std::string restored = "## 关于主人\n- 新恢复的偏好\n";
    auto concurrentRestore = [&](const std::string& p, std::string* owner, std::string* us) {
        valid(p, owner, us);
        assert(plat::WritePrivateFile(ownerPath, restored));
        return true;
    };
    assert(!a.Consolidate(concurrentRestore, zone, false, &err));
    assert(a.Owner() == "## 关于主人\n- 新恢复的偏好");
    assert(a.Us() == previous && a.PendingWeight() > 0);
    std::cout << "PASS shared owner, isolated roles, oversized results and stale write rejection\n";
}
