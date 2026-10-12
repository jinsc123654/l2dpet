/**
 * LlmClient.h — 内置 agent 的 LLM 接入（OpenAI 兼容 /chat/completions，非流式；
 * 接口地址带 /anthropic 段或就是 api.anthropic.com 时自动改用 Anthropic Messages /v1/messages）
 *
 * 传输：编入 libcurl 时 http/https 都走 curl；否则退回内置明文 HTTP/1.1（只支持 http://，
 * 供本地 Ollama/LM Studio 用——交叉编译不拖 TLS 的兜底路径）。
 * 配置：环境变量 LLM_API_KEY/LLM_MODEL/LLM_BASE_URL > 桌宠设置写的 ~/.config/l2dpet/llm.json > 默认。
 * SSRF 校验：端点解析出的 IP 必须是公网（回环/私网/链路本地/保留段拒绝；LLM_ALLOW_PRIVATE=1
 * 显式放行本地端点；198.18.0.0/15 是 Clash fake-ip = 本机代理出口，不算内网目标）。
 */
#pragma once

#include <string>
#include <vector>

namespace l2d
{

struct LlmSettings
{
    std::string key;
    std::string model = "deepseek-chat";
    std::string baseUrl = "https://api.deepseek.com";
    std::string keySource = "none";   // env / file / none（面板显示用，不含 key 本身）
    std::string providerName;         // 生效的供应商名（v2 配置；env 覆盖时为空）
    int maxReplyTokens = 0;          // Only bounded dialogue requests set this; maintenance retains its budget.
};

/** 一个 LLM 供应商（v2 配置；一个供应商可挂多个模型，当前用一个） */
struct LlmProvider
{
    std::string id;                   // 稳定标识（面板生成；保存时按 id 保留密钥）
    std::string name;                 // 显示名
    std::string baseUrl;
    std::string apiKey;
    std::string model;                // 该供应商当前选中的模型
    std::vector<std::string> models;  // 可选模型列表
};

/** 读 llm.json（v2；v1 自动迁移成单供应商）。active = 生效的供应商 id */
bool LlmLoadProviders(std::string* active, std::vector<LlmProvider>* providers);

struct LlmMsg
{
    std::string role;
    std::string content;
};

struct LlmUsage
{
    long long ms = 0;
    long long tokIn = 0;
    long long tokOut = 0;
};

struct LlmTokenStats
{
    LlmUsage last;
    LlmUsage total;
};
LlmTokenStats LlmGetTokenStats(); // Process lifetime, includes maintenance and retries; provider-reported usage.

/** 接入参数（env > llm.json > 默认）。 */
LlmSettings LlmLoadSettings();

/** 本构建是否支持 https（编入了 libcurl）。 */
bool LlmHasHttps();

/** 通过供应商原生联网搜索工具让模型查询天气（当前支持官方 DeepSeek）。 */
bool LlmWeatherSearch(const LlmSettings& cfg, const std::string& location,
                      std::string* response, std::string* err);

/** 直连 open-meteo（免 key、免注册）取天气：城市名 → 坐标 → 实况 + 两天预报，
 *  成功时 *facts = 给 LLM 参考的事实表（解析与校验在 PetWeather，纯函数）。
 *  国内外链路都实测可达；不需要任何模型/密钥。 */
bool OpenMeteoWeather(const std::string& location, std::string* facts, std::string* err);

/** 简单 GET（天气这类免 key 数据源用）：只允许 http/https 的公网地址，和 LLM 端点同一套
 *  SSRF 校验；响应上限 2MB，不跟重定向。未编入 libcurl 的构建返回错误。 */
bool HttpGet(const std::string& url, std::string* body, std::string* err, long timeoutSec = 12);

/** 让进行中的/之后的 LlmChat 立即失败返回（退出时用：别让一个 60s 的请求把关窗拖住）。进程级。 */
void LlmCancelAll();

/** 睡眠暂停：paused=true 时 LlmChat 直接失败（"她在睡觉"），不发任何请求。兜底所有调用路径。 */
void LlmSetPaused(bool paused);

/** 最近一次真正发出的 LLM 请求结果（暂停/取消/没 Key 时不算一次请求，不改它）。
 *  "已连接"只看这个：调用成功过且最近一次是成功的才算连上（光有 agent 进程不算）。 */
struct LlmStatus
{
    int state = 0;        // 0 = 还没调过；1 = 最近一次成功；2 = 最近一次失败
    std::string err;      // state==2 时的原因
    long long at = 0;     // 时间（unix 秒）
};
LlmStatus LlmLastStatus();

/** 一次对话调用。成功时 *content = choices[0].message.content。 */
bool LlmChat(const LlmSettings& cfg, const std::vector<LlmMsg>& msgs, float temperature,
             bool jsonMode, std::string* content, std::string* err, LlmUsage* usage);

/** 试连接的结果（面板「对话 → 尝试连接」显示用） */
struct LlmTestResult
{
    bool ok = false;
    std::string protocol;   // openai / anthropic（按接口地址自动识别）
    std::string url;        // 实际请求的完整地址——地址填错时一眼能看出拼成了什么
    long long ms = 0;
    std::string sample;     // 成功时：模型回的那句话
    std::string error;      // 失败时：原因（含 HTTP 状态码与响应片段）
};

/** 试连接：用与真实调用**完全相同**的地址解析/协议/解析代码发一句最小的话。
 *  面板上手动触发，所以不受"她睡着了"暂停影响；结果同时按真实请求记进连接状态。 */
bool LlmTest(const LlmSettings& cfg, LlmTestResult* out);

}   // namespace l2d
