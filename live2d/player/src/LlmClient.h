/**
 * LlmClient.h — 内置 agent 的 LLM 接入（OpenAI 兼容 /chat/completions，非流式）
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

/** 接入参数（env > llm.json > 默认）。 */
LlmSettings LlmLoadSettings();

/** 本构建是否支持 https（编入了 libcurl）。 */
bool LlmHasHttps();

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

}   // namespace l2d
