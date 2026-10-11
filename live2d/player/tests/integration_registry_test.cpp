#include "../src/PetIntegration.h"
#include <cassert>
#include <iostream>

int main()
{
    PetIntegration registry;
    assert(!registry.Register("../escape", "bad"));
    assert(!registry.Register("Bad-ID", "bad"));
    assert(registry.Register("assistant-a", "助手 A"));
    assert(registry.Register("assistant-b", "助手 B"));
    assert(registry.SetEnabled("assistant-a", false));
    PetIntegration restarted;
    assert(!restarted.Enabled("assistant-a"));
    assert(restarted.Enabled("assistant-b"));
    assert(plat::WritePrivateFile(PetIntegration::Root() + "/clients/assistant-a/registration.json",
        "{\"schema_version\":1,\"id\":\"assistant-a\",\"name\":\"更新的 A\",\"enabled\":true}"));
    assert(restarted.Registered("assistant-a"));
    assert(!restarted.Enabled("assistant-a")); // Assistant updates cannot reset the user's choice.
    assert(restarted.Name("assistant-a") == "更新的 A");
    assert(!restarted.SetEnabled("unknown", false));
    assert(plat::WritePrivateFile(PetIntegration::Root() + "/clients/assistant-b/registration.json", "{}"));
    assert(!restarted.Registered("assistant-b"));
    assert(restarted.ClientsJson().find("assistant-b") == std::string::npos);
    assert(restarted.SetEnabled("assistant-a", true));
    std::cout << "PASS IDs, registrations, per-client isolation, persistent mute and malformed records\n";
}
