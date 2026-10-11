#pragma once
#include "Platform.h"
#include "JsonLite.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

// Files are declarations only. Never execute a registered command or script.
class PetIntegration
{
public:
    static std::string Root() { return plat::HomeDir() + "/.config/l2dpet/integration"; }
    static bool ValidId(const std::string& id)
    {
        if (id.empty() || id.size() > 64 || id == "." || id == "..") return false;
        for (unsigned char c : id)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
        return true;
    }
    static std::string Quote(const std::string& s)
    {
        std::string out = "\"";
        for (unsigned char c : s)
        {
            if (c == '"' || c == '\\') { out += '\\'; out += c; }
            else if (c < 32) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
            else out += c;
        }
        return out + "\"";
    }
    void Ensure()
    {
        plat::CreateDir(Root() + "/clients", true);
        if (!_prepared)
        {
            _prepared = true;
            const auto exe = plat::SelfExePath();
            const auto dir = exe.substr(0, exe.find_last_of("/\\"));
            const auto adapter = dir + "/tools/pet_bridge.py";
            if (plat::IsFile(adapter)) plat::FileCopy(adapter, Root() + "/pet_bridge.py");
            const auto doc = dir + "/EXTERNAL-INTEGRATION.md";
            if (plat::IsFile(doc)) plat::FileCopy(doc, Root() + "/README.md");
        }
    }
    static std::string LegacyId(const std::string& source)
    {
        std::string id;
        for (unsigned char c : source)
        {
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') id += c;
        }
        return id.empty() ? "legacy" : id.substr(0, 64);
    }
    bool Register(const std::string& id, const std::string& name)
    {
        if (!ValidId(id)) return false;
        Ensure();
        const auto dir = Root() + "/clients/" + id;
        if (plat::IsSymlink(dir)) return false;
        if (!plat::CreateDir(dir, true)) return false;
        const auto path = dir + "/registration.json";
        if (plat::Exists(path)) return Registered(id);
        return plat::WritePrivateFile(path, "{\"schema_version\":1,\"id\":" + Quote(id)
            + ",\"name\":" + Quote(plat::Utf8Head(name, 80)) + "}");
    }
    bool Registered(const std::string& id) const
    {
        if (!ValidId(id)) return false;
        const auto dir = Root() + "/clients/" + id;
        const auto path = dir + "/registration.json";
        if (plat::IsSymlink(dir) || plat::IsSymlink(path)) return false;
        const auto text = plat::ReadTextFile(path);
        jsonlite::Value v;
        return text.size() <= 65536 && jsonlite::parse(text.c_str(), text.size(), v, NULL)
            && v.isObj() && std::string(v.get("id").strOr("")) == id
            && v.get("schema_version").numOr(0) == 1;
    }
    bool Enabled(const std::string& id) const
    {
        const auto text = plat::ReadTextFile(Root() + "/preferences/" + id + ".json");
        jsonlite::Value v;
        if (!jsonlite::parse(text.c_str(), text.size(), v, NULL)) return true;
        return !v.get("enabled").isBool() || v.get("enabled").b;
    }
    bool SetEnabled(const std::string& id, bool enabled)
    {
        if (!Registered(id)) return false;
        if (!plat::CreateDir(Root() + "/preferences", true)) return false;
        return plat::WritePrivateFile(Root() + "/preferences/" + id + ".json",
            std::string("{\"enabled\":") + (enabled ? "true}" : "false}"));
    }
    std::string Name(const std::string& id) const
    {
        const auto text = plat::ReadTextFile(Root() + "/clients/" + id + "/registration.json");
        jsonlite::Value v;
        if (!jsonlite::parse(text.c_str(), text.size(), v, NULL)) return id;
        return plat::Utf8Head(v.get("name").strOr(id.c_str()), 80);
    }
    std::string ClientsJson() const
    {
        std::vector<std::string> ids;
        plat::ListDir(Root() + "/clients", &ids);
        std::sort(ids.begin(), ids.end());
        std::string out = "[";
        int n = 0;
        for (const auto& id : ids)
        {
            if (!Registered(id) || n >= 128) continue;
            if (n++) out += ",";
            out += "{\"id\":" + Quote(id) + ",\"name\":" + Quote(Name(id))
                + ",\"enabled\":" + (Enabled(id) ? "true" : "false")
                + ",\"directory\":" + Quote(Root() + "/clients/" + id) + "}";
        }
        return out + "]";
    }
    void Publish(const std::string& descriptor)
    {
        Ensure();
        if (plat::ReadTextFile(Root() + "/integration.json") != descriptor)
            plat::WritePrivateFile(Root() + "/integration.json", descriptor);
    }
    int activePort = 0;
private:
    bool _prepared = false;
};
