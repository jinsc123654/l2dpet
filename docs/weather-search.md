# 联网查天气

天气取数有两条路，按顺序试、谁先成功用谁；两条都失败就"不聊天气、只关心/提问"，绝不编造。

1. **直连 open-meteo**（默认；免 key、免注册、不花 token）：城市名 → 地理编码 → 实况 + 两天预报。
   桌宠把它解析成"天气事实表"（数据源 / 城市 / 观测时间 / 实况 / 今明预报）。
   国内移动网络与海外链路都实测可达（约 1 秒）；超时或失败就换下一条。
2. **模型联网搜索**（兜底；需要官方 DeepSeek 供应商）：用已配置的密钥和模型走
   `https://api.deepseek.com/anthropic/v1/messages` 的 `web_search_20250305`。
   必须收到实际 `web_search_tool_result`、来源链接和搜索之后的回答才视为成功；
   普通模型直接生成的"天气回答"不会被缓存成查询结果。

事实表交给**当前生效的供应商**（mimo / DeepSeek 都行）写成一句关心的话，
所以换对话供应商不影响能不能查天气。

- 在设置中填写天气城市。打开天气闲聊后，醒着时首次查询，并每隔 5 小时更新。
- 点"聊天气"也会触发查询；有效缓存期内直接复用。
- 查询结果注入后续对话上下文，天气台词依据该结果生成。
- 切换城市立即重查，不会把旧城市的数据用于新城市。
- 缓存在 `~/.config/l2dpet/weather-search.json`（0600），重启也可复用；文件里第一行写明数据源。
- 查询失败后至少等待 5 分钟重试；超过 5 小时的结果不再提供给模型。
- 未设置城市或两条路都失败时，模型会得到"无有效天气"的提示：只关心/提问，不报天气。
- 睡眠暂停期间不查询；唤醒后按缓存有效期更新。
- 自动天气关心独立按模型启动计时：首次 5 分钟，成功提交有台词的天气段后每隔 1 小时。
  忙着回应或等待选项时延后，睡眠期间不打扰，也不补播多条。模型切换重新计时。
- 自动天气需要开启天气功能、有效城市与新鲜查询缓存；查询或生成失败不算已经说过，5 分钟后重试。
  关闭普通闲时小剧场不会关闭单独启用的天气关心。手动"聊聊天气"仍立即触发。
- 直连源不需要密钥、不产生模型调用费用；模型搜索兜底会产生供应商模型调用费用。

## 国内用户

直连源 open-meteo 是境外主机：2026-10-12 在国内移动网络实测「地理编码 0.96 秒 / 预报 1.01 秒」，
可达且不慢；但跨运营商、跨地区可能偶发变慢或失败，所以设计上做了这些兜底：

- 直连超时 12 秒，失败不重试同一条，直接换成模型联网搜索（有 DeepSeek 供应商时）；
- 两条都失败也只影响"这 5 小时里的天气闲聊"，其它功能不受影响，5 分钟后自动重试；
- 缓存是本地文件，之前查到的结果在有效期内照用。

如果需要一个完全不依赖境外主机的方案，可以加一个"天气 API 密钥"配置项走和风天气
（官方、国内直连、免费额度足够）。目前**没有**接国内免 key 源：中国天气网的公开接口
（2026-10-12 实测）搜索接口返回空、预报接口已经变成 HTML 页面，只剩按城市数字码查实况可用，
需要自带城市码映射表而且没有预报，性价比低。

## 参考

- [open-meteo](https://open-meteo.com/)（免 key：地理编码 + 预报；WMO 天气码）
- [DeepSeek 原生 Web Search](https://api-docs.deepseek.com/quick_start/agent_integrations/claude_code/)
- [DeepSeek Anthropic 接口](https://api-docs.deepseek.com/guides/anthropic_api/)

## 回归测试

无需联网的解析/缓存校验测试：

```sh
g++ -std=c++14 live2d/player/tests/weather_search_test.cpp \
  live2d/player/src/PetWeather.cpp live2d/player/src/Platform.cpp \
  -pthread -o /tmp/weather_search_test && /tmp/weather_search_test

g++ -std=c++14 live2d/player/tests/weather_openmeteo_test.cpp \
  live2d/player/src/PetWeather.cpp live2d/player/src/Platform.cpp \
  -pthread -o /tmp/weather_openmeteo_test && /tmp/weather_openmeteo_test
```
