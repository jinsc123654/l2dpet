// g++ -std=c++14 live2d/player/tests/weather_search_test.cpp live2d/player/src/PetWeather.cpp live2d/player/src/Platform.cpp -pthread -o /tmp/weather_search_test
#include "../src/PetWeather.h"
#include <cassert>
#include <cstdio>
#include <iostream>

int main()
{
    const std::string native = R"({"content":[{"type":"text","text":"我来查询天气"},{"type":"server_tool_use","name":"web_search"},{"type":"web_search_tool_result","content":[{"type":"web_search_result","url":"https://example.org/weather","title":"南京天气"}]},{"type":"text","text":"南京，今日多云，23℃。"}]})";
    std::string parsed, err;
    assert(PetWeather::ParseSearchResult(native, &parsed, &err));
    assert(parsed.find("23℃") != std::string::npos);
    assert(parsed.find("https://example.org/weather") != std::string::npos);
    assert(parsed.find("我来查询") == std::string::npos);
    assert(!PetWeather::ParseSearchResult(R"({"content":[{"type":"text","text":"晴天 30℃"}]})", &parsed, &err));
    assert(!PetWeather::ParseSearchResult(R"({"content":[{"type":"web_search_tool_result","content":{"type":"web_search_tool_result_error","error_code":"unavailable"}},{"type":"text","text":"晴天"}]})", &parsed, &err));
    assert(!PetWeather::ParseSearchResult("not json", &parsed, &err));
    assert(!PetWeather::ParseSearchResult(R"({"stop_reason":"max_tokens","content":[]})", &parsed, &err));

    PetWeather cache;
    int calls = 0;
    auto search = [&](const std::string& loc, std::string* response, std::string*) {
        assert(loc == "南京" || loc == "上海"); ++calls; *response = native; return true;
    };
    const double now = 100000;
    assert(!cache.NeedsRefresh("", now));
    assert(cache.NeedsRefresh("南京", now));
    assert(cache.Refresh("南京", now, search, &err));
    assert(calls == 1);
    assert(!cache.NeedsRefresh("南京", now + 17999));
    assert(!cache.Refresh("南京", now + 17999, search, &err));
    assert(calls == 1);
    assert(cache.Context("南京", now+100).find("23℃") != std::string::npos);
    assert(cache.Context("上海", now+100).find("23℃") == std::string::npos);
    assert(cache.NeedsRefresh("南京", now + 18000));
    assert(cache.Context("南京", now+18000).find("23℃") == std::string::npos);
    assert(cache.NeedsRefresh("南京", now-1)); // 系统时间倒退，不误用未来缓存

    const std::string path = "/tmp/l2dpet-weather-search-regression.json";
    assert(cache.Save(path));
    PetWeather restart;
    assert(restart.Load(path));
    assert(!restart.NeedsRefresh("南京", now+100));
    assert(restart.NeedsRefresh("南京", now+18000));
    assert(restart.Context("南京", now+100).find("23℃") != std::string::npos);
    std::remove(path.c_str());

    assert(cache.NeedsRefresh("上海", now+100));
    auto failure = [&](const std::string&, std::string*, std::string* error) { ++calls; *error="网络不可用"; return false; };
    assert(!cache.Refresh("上海", now+100, failure, &err));
    assert(!cache.NeedsRefresh("上海", now+399));
    assert(cache.NeedsRefresh("上海", now+400));
    assert(cache.Context("上海", now+101).find("23℃") == std::string::npos);
    assert(cache.Context("南京", now+101).find("23℃") != std::string::npos);
    assert(cache.Refresh("南京", now+18000, search, &err));
    assert(!cache.NeedsRefresh("南京", now+18001));
    std::cout << "PASS: native search validation, 5h boundary, restart, city changes, retry and stale-data exclusion\n";
}
