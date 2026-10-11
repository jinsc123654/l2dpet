# 模型联网查天气

天气由模型调用供应商的原生联网搜索工具查询，桌宠不调用独立天气 API。

当前实现支持官方 DeepSeek：使用已配置的供应商密钥和模型，通过
`https://api.deepseek.com/anthropic/v1/messages` 启用 `web_search_20250305`。
普通对话继续使用原来的 Chat Completions 接口，不改用户的模型设置。
DeepSeek 会按其 Anthropic 接口规则映射不支持的模型名称。

- 在设置中填写天气城市。打开天气闲聊后，醒着时首次查询，并每隔 5 小时更新。
- 点“聊天气”也会触发查询；有效缓存期内直接复用。
- 查询结果注入后续对话上下文，天气台词依据该结果生成。
- 切换城市立即重查，不会把旧城市的数据用于新城市。
- 缓存在 `~/.config/l2dpet/weather-search.json`，重启也可复用。
- 查询失败后至少等待 5 分钟重试；超过 5 小时的结果不再提供给模型。
- 必须收到实际 `web_search_tool_result`、来源链接和搜索后的回答才视为成功。
  普通模型直接生成的“天气回答”不会被缓存为查询结果。
- 未设置城市、联网失败或供应商未接入原生搜索时，模型会得到无有效天气的提示。
- 睡眠暂停期间不查询；唤醒后按缓存有效期更新。
- 自动天气关心独立按模型启动计时：首次 5 分钟，成功提交有台词的天气段后每隔 1 小时。忙着回应或等待选项时延后，睡眠期间不打扰，也不补播多条。模型切换重新计时。
- 自动天气需要开启天气功能、有效城市与新鲜查询缓存；查询或生成失败不算已经说过，5 分钟后重试。关闭普通闲时小剧场不会关闭单独启用的天气关心。手动“聊聊天气”仍立即触发。

联网搜索会产生供应商模型调用费用。其他供应商需按各自的原生搜索接口单独接入，
不能假定任意 OpenAI 兼容对话接口都具备搜索功能。

参考：
- [DeepSeek 原生 Web Search](https://api-docs.deepseek.com/quick_start/agent_integrations/claude_code/)
- [DeepSeek Anthropic 接口](https://api-docs.deepseek.com/guides/anthropic_api/)

## 回归测试

无需联网的缓存/响应校验测试：

```sh
g++ -std=c++14 live2d/player/tests/weather_search_test.cpp \
  live2d/player/src/PetWeather.cpp live2d/player/src/Platform.cpp \
  -pthread -o /tmp/weather_search_test
/tmp/weather_search_test
```

覆盖原生搜索结果校验、5 小时边界、重启恢复、换城市、查询失败退避、过期数据排除。
