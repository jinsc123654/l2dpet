#pragma once
#include <functional>
#include <string>

// 缓存模型原生联网搜索的天气回答；不接入独立天气 API。
class PetWeather
{
public:
    typedef std::function<bool(const std::string&, std::string*, std::string*)> SearchFn;
    static constexpr double kCacheSeconds = 5 * 3600;
    bool NeedsRefresh(const std::string& location, double now) const;
    bool Refresh(const std::string& location, double now, const SearchFn& search, std::string* err);
    std::string Context(const std::string& location, double now) const;
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
    static bool ParseSearchResult(const std::string& response, std::string* result, std::string* err);
private:
    bool Fresh(const std::string& location, double now) const;
    std::string _location, _result, _attemptLocation;
    double _fetchedAt = 0, _attemptAt = -1e30;
};
