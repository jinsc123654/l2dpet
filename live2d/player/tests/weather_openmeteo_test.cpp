// 直连天气源（open-meteo）的解析回归：两份真实形状的响应 → 天气事实表。
// 不需要联网；用例取自 2026-10-12 实际响应（滁州）。
// g++ -std=c++14 -ffunction-sections -fdata-sections live2d/player/tests/weather_openmeteo_test.cpp \
//   live2d/player/src/PetWeather.cpp live2d/player/src/Platform.cpp -Wl,--gc-sections -pthread \
//   -o /tmp/weather_openmeteo_test
#include "../src/PetWeather.h"
#include <cassert>
#include <iostream>
#include <string>

static const char* kGeo = R"({"results":[{"id":1814757,"name":"滁州","latitude":32.32194,
  "longitude":118.29778,"elevation":24.0,"feature_code":"PPLA2","country_code":"CN",
  "admin1_id":1806293,"timezone":"Asia/Shanghai","population":782200,"country_id":1814991,
  "country":"中国","admin1":"安徽省"}],"generationtime_ms":0.9})";

static const char* kForecast = R"({"latitude":32.302284,"longitude":118.25455,
  "timezone":"Asia/Shanghai","utc_offset_seconds":28800,"current_units":{"temperature_2m":"°C"},
  "current":{"time":"2026-10-12T17:15","interval":900,"temperature_2m":18.4,
    "relative_humidity_2m":71,"apparent_temperature":19.1,"weather_code":3,
    "wind_speed_10m":4.6,"precipitation":0},
  "daily_units":{"time":"iso8601"},
  "daily":{"time":["2026-10-12","2026-10-13"],
    "weather_code":[3,61],
    "temperature_2m_max":[22.4,20.9],
    "temperature_2m_min":[15.1,14.7],
    "precipitation_probability_max":[10,65]}})";

int main()
{
    std::string facts, err;
    assert(PetWeather::ParseOpenMeteo(kGeo, kForecast, &facts, &err));
    assert(facts.find("open-meteo 直连") != std::string::npos);
    assert(facts.find("滁州") != std::string::npos && facts.find("中国") != std::string::npos);
    assert(facts.find("18.4℃") != std::string::npos);            // 实况温度
    assert(facts.find("19.1℃") != std::string::npos);            // 体感
    assert(facts.find("阴") != std::string::npos);               // WMO 3 = 阴
    assert(facts.find("2026-10-12T17:15") != std::string::npos); // 当地观测时间
    assert(facts.find("今天（2026-10-12）：15.1~22.4℃，阴") != std::string::npos);
    assert(facts.find("降水概率 10%") != std::string::npos);
    assert(facts.find("明天（2026-10-13）：14.7~20.9℃，小雨") != std::string::npos);
    assert(facts.find("降水概率 65%") != std::string::npos);

    // 坐标要用得上（预报是按坐标查的）：地理编码单独解析也要给对
    double lat = 0.0, lon = 0.0;
    std::string display;
    assert(PetWeather::OpenMeteoPlace(kGeo, &lat, &lon, &display, &err));
    assert(lat > 32.3 && lat < 32.4 && lon > 118.2 && lon < 118.3);
    assert(display.find("滁州") == 0);

    // 坏输入都不能当成有效天气（宁可没有数据，也不能编）
    std::string bad;
    assert(!PetWeather::ParseOpenMeteo(R"({"results":[]})", kForecast, &bad, &err));
    assert(!PetWeather::ParseOpenMeteo("不是 JSON", kForecast, &bad, &err));
    assert(!PetWeather::ParseOpenMeteo(kGeo, R"({"current":{}})", &bad, &err));
    assert(!PetWeather::ParseOpenMeteo(kGeo, "null", &bad, &err));

    // 只有实况、没有预报数组时仍可用，但要写明预报缺失
    assert(PetWeather::ParseOpenMeteo(kGeo, R"({"current":{"time":"2026-10-12T17:15",
      "temperature_2m":18.4,"weather_code":0}})", &bad, &err));
    assert(bad.find("晴") != std::string::npos);
    assert(bad.find("预报部分暂缺") != std::string::npos);

    std::cout << "PASS open-meteo 解析：坐标/实况/今明预报/降水概率/WMO 码，坏输入不缓存\n";
}
