#pragma once
#include <functional>
#include <string>

// 缓存"联网查来的天气事实"：优先免 key 的直连源（open-meteo），不行再退回模型的联网搜索；
// 不接入需要注册/密钥的第三方天气 API。（直连源在国内外链路都实测可达；见 docs/weather-search.md）
class PetWeather
{
public:
    /** 取数函数：成功时 *facts = 给 LLM 参考的天气事实表（缓存的就是它）；失败填 error */
    typedef std::function<bool(const std::string&, std::string*, std::string*)> FetchFn;
    static constexpr double kCacheSeconds = 5 * 3600;
    bool NeedsRefresh(const std::string& location, double now) const;
    bool Refresh(const std::string& location, double now, const FetchFn& fetch, std::string* err);
    std::string Context(const std::string& location, double now) const;
    bool Load(const std::string& path);
    bool Save(const std::string& path) const;
    /** 解析模型联网搜索的响应（web_search_tool_result + 搜索之后的回答）→ 事实表 */
    static bool ParseSearchResult(const std::string& response, std::string* result, std::string* err);
    /** 解析直连源（open-meteo）的两份响应（地理编码 + 预报）→ 事实表。纯函数，可离线测试。 */
    static bool ParseOpenMeteo(const std::string& geoJson, const std::string& forecastJson,
                               std::string* facts, std::string* err);
    /** 地理编码响应 → 坐标与显示名（取数方先拿坐标才能查预报；纯函数） */
    static bool OpenMeteoPlace(const std::string& geoJson, double* lat, double* lon,
                               std::string* display, std::string* err);
    bool Fresh(const std::string& location, double now) const;
private:
    std::string _location, _result, _attemptLocation;
    double _fetchedAt = 0, _attemptAt = -1e30;
};
