/**
 * LlmClient.cpp — 见 LlmClient.h
 * 传输统一走 libcurl（http/https 都行；不跟重定向、60s 超时、响应 ≤2MB）。
 * 未编入 libcurl（L2D_WITH_CURL 未定义）时 LlmChat 直接报错——内置 agent 需要带 curl 构建。
 */
#include "LlmClient.h"
#include "JsonLite.h"
#include "Platform.h"
#include "PetWeather.h"   // 直连天气源的解析/校验（纯函数）；见 OpenMeteoWeather

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>

#include <atomic>
#include <mutex>
#include <chrono>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#if defined(L2D_WITH_CURL)
#include <curl/curl.h>
#endif

namespace l2d
{

namespace
{

std::atomic<bool> g_cancel{ false };
std::atomic<bool> g_paused{ false };
std::mutex g_statusMu;
LlmStatus g_status;
LlmTokenStats g_tokens;

void RecordStatus(bool ok, const std::string& err)
{
    std::lock_guard<std::mutex> lk(g_statusMu);
    g_status.state = ok ? 1 : 2;
    g_status.err = ok ? std::string() : err.substr(0, 300);
    g_status.at = (long long)time(NULL);
}

std::string HomeConfig(const char* rel)
{
    const char* home = getenv("HOME");
#ifdef _WIN32
    if (home == NULL) home = getenv("USERPROFILE");
#endif
    return std::string(home != NULL ? home : "/tmp") + rel;
}

std::string ReadFileText(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (f == NULL) return "";
    std::string text;
    char buf[4096];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, r);
    fclose(f);
    return text;
}

std::string JsonEscape(const std::string& s)
{
    // 先过 UTF-8 合法性：截断的多字节序列/孤立续字节丢弃（记忆 md 按字节截断会产生）
    std::string out;
    out.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size();)
    {
        const unsigned char c = (unsigned char)s[i];
        size_t need = c < 0x80 ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : ((c & 0xF8) == 0xF0 ? 4 : 0)));
        if (need == 0 || i + need > s.size())
        {
            ++i;   // 孤立续字节 / 截断的头字节：丢弃
            continue;
        }
        bool valid = true;
        for (size_t j = 1; j < need; ++j)
        {
            if (((unsigned char)s[i + j] & 0xC0) != 0x80) { valid = false; break; }
        }
        if (!valid)
        {
            ++i;
            continue;
        }
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04x", c);
                out += b;
            }
            else
            {
                out.append(s, i, need);
            }
            break;
        }
        i += need;
    }
    return out;
}

/** 解析出的地址是否"内网/本机/保留段"（fake-ip 例外见内注释）*/
bool AddrBlocked(const sockaddr_storage& sa, std::string* why)
{
    char ip[INET6_ADDRSTRLEN] = { 0 };
    if (sa.ss_family == AF_INET)
    {
        const sockaddr_in* a = (const sockaddr_in*)&sa;
        inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
        const unsigned host = (unsigned)ntohl(a->sin_addr.s_addr);
        // 198.18.0.0/15 = Clash/mihomo fake-ip：本机代理环境下 DNS 给公网域名返回这个段，
        // 实际经 TUN 代理出去——它就是这台机器的出口，不算内网目标
        const bool fakeIp = (host & 0xFFFE0000u) == 0xC6120000u;
        const unsigned net8 = host >> 24, net16 = host >> 16, net24 = host >> 8;
        if (!fakeIp && (net8 == 0 || net8 == 10 || net8 == 127 || net8 >= 224
                        || (net16 >= 0xAC10 && net16 <= 0xAC1F) || net16 == 0xC0A8 || net24 == 0xA9FE))
        {
            *why = std::string("内网或本机地址 ") + ip;
            return true;
        }
        return false;
    }
    if (sa.ss_family == AF_INET6)
    {
        const sockaddr_in6* a = (const sockaddr_in6*)&sa;
        inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof(ip));
        const unsigned char* b = a->sin6_addr.s6_addr;
        bool allZero = true;
        for (int i = 0; i < 16; ++i) allZero = allZero && b[i] == 0;
        const bool loopback = allZero && b[15] == 1;                    // ::1
        const bool linkLocal = b[0] == 0xFE && (b[1] & 0xC0) == 0x80;   // fe80::/10
        const bool uniqueLocal = (b[0] & 0xFE) == 0xFC;                 // fc00::/7
        const bool multicast = b[0] == 0xFF;                            // ff00::/8
        if (allZero || loopback || linkLocal || uniqueLocal || multicast)
        {
            *why = std::string("内网或本机地址 ") + ip;
            return true;
        }
        return false;
    }
    return false;
}

struct Endpoint
{
    std::string scheme, host, apiPath;
    int port = 80;
    bool anthropic = false;   // 按接口地址识别出的协议：Anthropic Messages（见 AnthropicByUrl）
};

/** 接口地址带 "anthropic" 段（各家网关都这么写，如 https://host/anthropic）或就是官方域名
 *  = Anthropic Messages 协议；其余一律按 OpenAI 兼容 */
bool AnthropicByUrl(const std::string& host, const std::string& path)
{
    if (host == "api.anthropic.com") return true;
    std::string low = path;
    for (size_t i = 0; i < low.size(); ++i)
    {
        if (low[i] >= 'A' && low[i] <= 'Z') low[i] = (char)(low[i] + 32);
    }
    return low.find("anthropic") != std::string::npos;
}

/** Anthropic 端点的路径："" → /v1/messages；"/anthropic" → /anthropic/v1/messages；
 *  "/v1" → /v1/messages；用户已经把 /messages 写全了就原样用 */
std::string AnthropicPath(const std::string& path)
{
    const std::string tail = "/messages";
    if (path.size() >= tail.size() && path.compare(path.size() - tail.size(), tail.size(), tail) == 0)
    {
        return path;
    }
    if (path.size() >= 3 && path.compare(path.size() - 3, 3, "/v1") == 0)
    {
        return path + "/messages";
    }
    return path + "/v1/messages";
}

/** 完整请求地址（试连接回报"实际拼成了什么"也用它） */
std::string FullUrl(const Endpoint& ep)
{
    std::string url = ep.scheme + "://" + ep.host;
    if ((ep.scheme == "https" && ep.port != 443) || (ep.scheme == "http" && ep.port != 80))
    {
        url += ":" + std::to_string(ep.port);
    }
    return url + ep.apiPath;
}

bool ParseEndpoint(const std::string& baseUrl, Endpoint* out, std::string* err)
{
    std::string base = baseUrl;
    while (!base.empty() && base[base.size() - 1] == '/') base.erase(base.size() - 1);
    if (base.find_first_of("\r\n\t?#") != std::string::npos || base.find('@') != std::string::npos)
    {
        *err = "LLM_BASE_URL 只允许干净的 http/https 地址";
        return false;
    }
    size_t sep = base.find("://");
    if (sep == std::string::npos)
    {
        *err = "LLM_BASE_URL 缺少 scheme（http:// 或 https://）";
        return false;
    }
    out->scheme = base.substr(0, sep);
    if (out->scheme != "http" && out->scheme != "https")
    {
        *err = "LLM_BASE_URL 只允许 http/https";
        return false;
    }
    const std::string rest = base.substr(sep + 3);
    const size_t slash = rest.find('/');
    const std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "" : rest.substr(slash);
    if (hostport.empty())
    {
        *err = "LLM_BASE_URL 缺少主机名";
        return false;
    }
    if (hostport.find('[') != std::string::npos)
    {
        *err = "LLM_BASE_URL 暂不支持 IPv6 字面量";
        return false;
    }
    out->port = out->scheme == "https" ? 443 : 80;
    const size_t colon = hostport.rfind(':');
    if (colon != std::string::npos)
    {
        out->port = atoi(hostport.substr(colon + 1).c_str());
        if (out->port <= 0 || out->port > 65535)
        {
            *err = "LLM_BASE_URL 端口不合法";
            return false;
        }
        out->host = hostport.substr(0, colon);
    }
    else
    {
        out->host = hostport;
    }
    while (!path.empty() && path[path.size() - 1] == '/') path.erase(path.size() - 1);
    // 协议按地址识别：带 /anthropic 段（各家网关都这么写）或官方域名 = Anthropic Messages；
    // 其余一律 OpenAI 兼容。以前不论哪种都只拼 /chat/completions，于是 Anthropic 端点一律 404
    // ——"接口地址看着没填错，就是不能用"，只能靠试出来的错误里那串 404 HTML 去猜。
    out->anthropic = AnthropicByUrl(out->host, path);
    out->apiPath = out->anthropic ? AnthropicPath(path)
                                  : (path.empty() ? "" : path) + "/chat/completions";
    if (out->apiPath.empty() || out->apiPath[0] != '/') out->apiPath = "/" + out->apiPath;
    return true;
}

bool PrivateAllowed()
{
    const char* e = getenv("LLM_ALLOW_PRIVATE");
    return e != NULL && strcmp(e, "1") == 0;
}

bool CheckEndpointPublic(const Endpoint& ep, std::string* err)
{
    if (PrivateAllowed()) return true;
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", ep.port);
    addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = NULL;
    if (getaddrinfo(ep.host.c_str(), portStr, &hints, &res) != 0 || res == NULL)
    {
        *err = "LLM_BASE_URL 主机名解析失败";
        return false;
    }
    for (addrinfo* ai = res; ai != NULL; ai = ai->ai_next)
    {
        sockaddr_storage sa;
        memcpy(&sa, ai->ai_addr, ai->ai_addrlen > (socklen_t)sizeof(sa) ? sizeof(sa) : ai->ai_addrlen);
        if (AddrBlocked(sa, err))
        {
            freeaddrinfo(res);
            *err += "；本地端点请显式设 LLM_ALLOW_PRIVATE=1";
            return false;
        }
    }
    freeaddrinfo(res);
    return true;
}

std::string BuildBody(const LlmSettings& cfg, const std::vector<LlmMsg>& msgs, float temperature, bool jsonMode)
{
    std::string body = "{\"model\":\"" + JsonEscape(cfg.model) + "\",\"messages\":[";
    for (size_t i = 0; i < msgs.size(); ++i)
    {
        if (i) body += ",";
        body += "{\"role\":\"" + msgs[i].role + "\",\"content\":\"" + JsonEscape(msgs[i].content) + "\"}";
    }
    char temp[32];
    snprintf(temp, sizeof(temp), "%.2f", temperature);
    body += std::string("],\"temperature\":") + temp;
    if (cfg.maxReplyTokens > 0) body += ",\"max_tokens\":" + std::to_string(cfg.maxReplyTokens);
    if (jsonMode) body += ",\"response_format\":{\"type\":\"json_object\"}";
    if (cfg.noThinking) body += ",\"reasoning_effort\":\"none\"";   // 思考型模型照格式写作不用想
    body += "}";
    return body;
}

/** Anthropic Messages 的请求体：system 提到顶层、相邻同角色合并（Anthropic 要求 user/assistant 交替），
 *  max_tokens 必填（OpenAI 那边"不填=用供应商默认"，这里没有这个选项，给个宽松值） */
std::string BuildBodyAnthropic(const LlmSettings& cfg, const std::vector<LlmMsg>& msgs, float temperature)
{
    std::string system;
    std::vector<LlmMsg> turns;
    for (size_t i = 0; i < msgs.size(); ++i)
    {
        if (msgs[i].role == "system")
        {
            if (!system.empty()) system += "\n\n";
            system += msgs[i].content;
            continue;
        }
        if (!turns.empty() && turns.back().role == msgs[i].role)
        {
            turns.back().content += "\n\n" + msgs[i].content;
        }
        else
        {
            turns.push_back(msgs[i]);
        }
    }
    if (turns.empty())
    {
        LlmMsg only;
        only.role = "user";
        only.content = "你好";
        turns.push_back(only);
    }
    std::string body = "{\"model\":\"" + JsonEscape(cfg.model) + "\",\"messages\":[";
    for (size_t i = 0; i < turns.size(); ++i)
    {
        if (i) body += ",";
        body += "{\"role\":\"" + turns[i].role + "\",\"content\":\"" + JsonEscape(turns[i].content) + "\"}";
    }
    body += "],\"max_tokens\":" + std::to_string(cfg.maxReplyTokens > 0 ? cfg.maxReplyTokens : 4096);
    float temp = temperature < 0.0f ? 0.0f : temperature;
    if (temp > 1.0f) temp = 1.0f;   // Anthropic 的 temperature 上限就是 1.0
    char tb[32];
    snprintf(tb, sizeof(tb), "%.2f", temp);
    body += std::string(",\"temperature\":") + tb;
    if (!system.empty()) body += ",\"system\":\"" + JsonEscape(system) + "\"";
    if (cfg.noThinking) body += ",\"thinking\":{\"type\":\"disabled\"}";   // 长生成不用长思考链
    body += "}";
    return body;
}

#if defined(L2D_WITH_CURL)

size_t CurlWriteCb(char* ptr, size_t size, size_t nmemb, void* ud)
{
    std::string* out = (std::string*)ud;
    if (out->size() + size * nmemb > (2u << 20)) return 0;   // 响应最多 2MB
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

int CurlProgressCb(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    return g_cancel.load() ? 1 : 0;   // 非 0 = 中止传输（CURLE_ABORTED_BY_CALLBACK）
}

bool CurlHttpPost(const Endpoint& ep, const std::string& body, const std::string& key,
                  std::string* resp, std::string* err, bool anthropic = false,
                  long timeoutSec = 60L, long connectSec = 15L)
{
    static std::once_flag once;   // curl_easy_init 隐式 global_init 不是线程安全的：显式做一次
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* c = curl_easy_init();
    if (c == NULL)
    {
        *err = "curl_easy_init 失败";
        return false;
    }
    const std::string url = FullUrl(ep);
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, CurlWriteCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, connectSec);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);   // 不跟重定向
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);         // 在 agent 线程里用：不许 curl 用信号做 DNS 超时
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, CurlProgressCb);   // 退出时能立即打断
    struct curl_slist* h = NULL;
    h = curl_slist_append(h, "Content-Type: application/json");
    h = curl_slist_append(h, ("Authorization: Bearer " + key).c_str());
    if (anthropic)
    {
        // Anthropic Messages 用 x-api-key；Authorization 一起带上（有的网关只看它，官方会忽略多余头）
        h = curl_slist_append(h, ("x-api-key: " + key).c_str());
        h = curl_slist_append(h, "anthropic-version: 2023-06-01");
    }
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    const CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(h);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK)
    {
        *err = std::string("网络错误：") + curl_easy_strerror(rc);
        return false;
    }
    if (status != 200)
    {
        *err = "LLM 端点返回 HTTP " + std::to_string(status) + "：" + resp->substr(0, 200);
        return false;
    }
    return true;
}

#endif   // L2D_WITH_CURL

}   // namespace

bool LlmLoadProviders(std::string* active, std::vector<LlmProvider>* providers)
{
    providers->clear();
    active->clear();
    jsonlite::Value v;
    const std::string text = ReadFileText(HomeConfig("/.config/l2dpet/llm.json"));
    if (text.empty() || !jsonlite::parse(text.c_str(), text.size(), v, NULL) || !v.isObj())
    {
        return false;
    }
    // v2：{v:2, active, providers:[{id,name,base_url,api_key,model,models}]}
    const jsonlite::Value& arr = v.get("providers");
    if (arr.isArr())
    {
        for (size_t i = 0; i < arr.arr.size(); ++i)
        {
            const jsonlite::Value& p = arr.arr[i];
            if (!p.isObj()) continue;
            LlmProvider one;
            one.id = p.get("id").strOr("");
            one.name = p.get("name").strOr("");
            one.baseUrl = p.get("base_url").strOr("");
            one.apiKey = p.get("api_key").strOr("");
            one.model = p.get("model").strOr("");
            const jsonlite::Value& ms = p.get("models");
            if (ms.isArr())
            {
                for (size_t k = 0; k < ms.arr.size(); ++k)
                {
                    const std::string m = ms.arr[k].strOr("");
                    if (!m.empty()) one.models.push_back(m);
                }
            }
            if (one.model.empty() && !one.models.empty()) one.model = one.models[0];
            if (one.id.empty()) one.id = "p" + std::to_string(i + 1);
            providers->push_back(one);
        }
        *active = v.get("active").strOr("");
        if (active->empty() && !providers->empty()) *active = providers->front().id;
        return !providers->empty();
    }
    // v1 兼容：{api_key, model, base_url} → 单供应商（id "default"）
    if (v.has("api_key") || v.has("base_url") || v.has("model"))
    {
        LlmProvider one;
        one.id = "default";
        one.name = "默认";
        one.baseUrl = v.get("base_url").strOr("");
        one.apiKey = v.get("api_key").strOr("");
        one.model = v.get("model").strOr("");
        if (!one.model.empty()) one.models.push_back(one.model);
        if (one.model.empty()) one.model = "deepseek-chat";
        if (one.models.empty()) one.models.push_back(one.model);
        if (one.baseUrl.empty()) one.baseUrl = "https://api.deepseek.com";
        providers->push_back(one);
        *active = one.id;
        return true;
    }
    return false;
}

LlmSettings LlmLoadSettings()
{
    LlmSettings s;
    std::string activeId;
    std::vector<LlmProvider> providers;
    if (LlmLoadProviders(&activeId, &providers))
    {
        // 取 active 指定的供应商；找不到就退回第一个（配置被手改坏时不至于没得用）
        const LlmProvider* use = &providers.front();
        for (size_t i = 0; i < providers.size(); ++i)
        {
            if (providers[i].id == activeId) use = &providers[i];
        }
        s.key = use->apiKey;
        s.model = use->model;
        s.baseUrl = use->baseUrl;
        s.providerName = use->name;
    }
    if (const char* e = getenv("LLM_API_KEY"))
    {
        s.key = e;
        s.keySource = "env";
    }
    else if (!s.key.empty())
    {
        s.keySource = "file";
    }
    if (const char* e = getenv("LLM_MODEL")) s.model = e;
    if (const char* e = getenv("LLM_BASE_URL")) s.baseUrl = e;
    if (s.model.empty()) s.model = "deepseek-chat";
    if (s.baseUrl.empty()) s.baseUrl = "https://api.deepseek.com";
    return s;
}

LlmStatus LlmLastStatus()
{
    std::lock_guard<std::mutex> lk(g_statusMu);
    return g_status;
}

void LlmSetPaused(bool paused)
{
    g_paused.store(paused);
}

void LlmCancelAll()
{
    g_cancel.store(true);
}

bool LlmHasHttps()
{
#if defined(L2D_WITH_CURL)
    return true;
#else
    return false;
#endif
}

bool LlmWeatherSearch(const LlmSettings& cfg, const std::string& location,
                      std::string* response, std::string* err)
{
    response->clear();
    if (g_cancel.load() || g_paused.load()) { *err = "已暂停或取消"; return false; }
    if (cfg.key.empty()) { *err = "没有 API Key"; return false; }
#if !defined(L2D_WITH_CURL)
    (void)location;
    *err = "此构建未编入 libcurl，无法联网搜索";
    return false;
#else
    Endpoint ep;
    if (!ParseEndpoint(cfg.baseUrl, &ep, err)) return false;
    // 只有用户选择官方 DeepSeek 时才将同一供应商密钥用于它的原生搜索接口。
    if (ep.scheme != "https" || ep.host != "api.deepseek.com" || ep.port != 443)
    { *err = "当前供应商尚未接入原生联网搜索（现支持官方 DeepSeek）"; return false; }
    if (!CheckEndpointPublic(ep, err)) return false;
    ep.apiPath = "/anthropic/v1/messages";
    const time_t currentTime = time(NULL);
    struct tm local;
    plat::LocalTime(currentTime, &local);
    char localTime[80];
    strftime(localTime, sizeof(localTime), "%Y-%m-%d %H:%M:%S %z", &local);
    const std::string query = "必须使用 web_search 联网查询城市「" + location
        + "」的当前天气和今天/明天预报。本机当前日期时间和 UTC 偏移为 " + localTime
        + "；请按目标城市所在时区确认今天/明天的日期，不要把 UTC 日期当成当地日期。"
          "优先使用气象机构来源，核对来源城市和日期，不要把不同日期/不一致来源的数据拼成一份实况。"
          "用中文简要总结天气、温度、降雨和风，标明城市、当地日期、观测/预报时间与来源。"
          "只写搜索证实的数据；没有查到就明确说明，不要靠知识或猜测补全。";
    const std::string body = "{\"model\":\"" + JsonEscape(cfg.model)
        + "\",\"max_tokens\":2048,\"messages\":[{\"role\":\"user\",\"content\":"
        + "\"" + JsonEscape(query) + "\"}],\"tools\":[{\"type\":\"web_search_20250305\",\"name\":\"web_search\",\"max_uses\":3}]}";
    return CurlHttpPost(ep, body, cfg.key, response, err, true);
#endif
}

LlmTokenStats LlmGetTokenStats()
{
    std::lock_guard<std::mutex> lk(g_statusMu);
    return g_tokens;
}

namespace {
/** 城市名只作为查询参数：百分号编码后接入，只留 RFC3986 的 unreserved */
std::string EncodeQueryValue(const std::string& text)
{
    const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text)
    {
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
            || c == '-' || c == '.' || c == '_' || c == '~';
        if (keep) out += (char)c;
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
/** 直连天气源的两个固定地址（主机与路径写死在函数里；城市名编码后作查询参数，坐标是数字） */
std::string WeatherGeocodeUrl(const std::string& location)
{
    std::string url = "https://geocoding-api.open-meteo.com/v1/search?name=";
    url += EncodeQueryValue(location);
    url += "&count=1&language=zh&format=json";
    return url;
}
std::string WeatherForecastUrl(double lat, double lon)
{
    std::string url = "https://api.open-meteo.com/v1/forecast?latitude=";
    url += std::to_string(lat);
    url += "&longitude=";
    url += std::to_string(lon);
    url += "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,"
           "wind_speed_10m,precipitation"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
           "&timezone=auto&forecast_days=2";
    return url;
}
}   // namespace

bool OpenMeteoWeather(const std::string& location, std::string* facts, std::string* err)
{
    facts->clear();
    if (location.empty())
    {
        *err = "还没设置天气城市";
        return false;
    }
#if !defined(L2D_WITH_CURL)
    *err = "此构建未编入 libcurl，无法直连天气源";
    return false;
#else
    std::string geoJson;
    const std::string geoUrl = WeatherGeocodeUrl(location);
    if (!HttpGet(geoUrl, &geoJson, err)) return false;
    double lat = 0.0, lon = 0.0;
    std::string display;
    if (!PetWeather::OpenMeteoPlace(geoJson, &lat, &lon, &display, err)) return false;
    std::string forecastJson;
    const std::string forecastUrl = WeatherForecastUrl(lat, lon);
    if (!HttpGet(forecastUrl, &forecastJson, err)) return false;
    return PetWeather::ParseOpenMeteo(geoJson, forecastJson, facts, err);
#endif
}

bool HttpGet(const std::string& url, std::string* body, std::string* err, long timeoutSec)
{
    body->clear();
#if !defined(L2D_WITH_CURL)
    (void)url;
    (void)timeoutSec;
    *err = "此构建未编入 libcurl，无法联网取数据";
    return false;
#else
    // 只取 scheme/host/port 做公网校验（path 里带查询串没关系，查询串由调用方编码好）
    const size_t sep = url.find("://");
    if (sep == std::string::npos)
    {
        *err = "地址缺少 scheme（http:// 或 https://）";
        return false;
    }
    Endpoint ep;
    ep.scheme = url.substr(0, sep);
    if (ep.scheme != "http" && ep.scheme != "https")
    {
        *err = "只允许 http/https 地址";
        return false;
    }
    std::string rest = url.substr(sep + 3);
    const size_t cut = rest.find_first_of("/?");
    const std::string hostport = cut == std::string::npos ? rest : rest.substr(0, cut);
    if (hostport.empty())
    {
        *err = "地址缺少主机名";
        return false;
    }
    ep.port = ep.scheme == "https" ? 443 : 80;
    const size_t colon = hostport.rfind(':');
    if (colon != std::string::npos)
    {
        ep.port = atoi(hostport.substr(colon + 1).c_str());
        if (ep.port <= 0 || ep.port > 65535)
        {
            *err = "端口不合法";
            return false;
        }
        ep.host = hostport.substr(0, colon);
    }
    else
    {
        ep.host = hostport;
    }
    if (!CheckEndpointPublic(ep, err)) return false;

    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* c = curl_easy_init();
    if (c == NULL)
    {
        *err = "curl_easy_init 失败";
        return false;
    }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, CurlWriteCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, timeoutSec);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, timeoutSec < 10 ? timeoutSec : 10L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, CurlProgressCb);
    struct curl_slist* h = NULL;
    h = curl_slist_append(h, "Accept: application/json");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    const CURLcode rc = curl_easy_perform(c);
    curl_slist_free_all(h);
    long status = 0;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK)
    {
        *err = std::string("网络错误：") + curl_easy_strerror(rc);
        return false;
    }
    if (status != 200)
    {
        *err = "接口返回 HTTP " + std::to_string(status) + "：" + body->substr(0, 120);
        return false;
    }
    return true;
#endif
}

static bool LlmChatImpl(const LlmSettings& cfg, const std::vector<LlmMsg>& msgs, float temperature,
                        bool jsonMode, std::string* content, std::string* err, LlmUsage* usage)
{
    if (g_cancel.load())
    {
        *err = "已取消（正在退出）";
        return false;
    }
#if !defined(L2D_WITH_CURL)
    (void)msgs;
    (void)temperature;
    (void)jsonMode;
    (void)content;
    (void)usage;
    *err = "此构建未编入 libcurl，内置 agent 不可用（重新配置 CMake 找到 libcurl）";
    return false;
#else
    Endpoint ep;
    if (!ParseEndpoint(cfg.baseUrl, &ep, err)) return false;
    if (!CheckEndpointPublic(ep, err)) return false;
    const std::string body = ep.anthropic ? BuildBodyAnthropic(cfg, msgs, temperature)
                                          : BuildBody(cfg, msgs, temperature, jsonMode);
    const auto t0 = std::chrono::steady_clock::now();   // 墙钟（clock() 是 CPU 时间，等网络时几乎不走）
    std::string resp;
    // 超时按调用方给的上限（10~300 秒）：长生成（性格/题库/台词库）要放宽，否则思考型模型容易假超时
    long timeoutSec = cfg.timeoutSec < 10 ? 10 : (cfg.timeoutSec > 300 ? 300 : cfg.timeoutSec);
    bool posted = CurlHttpPost(ep, body, cfg.key, &resp, err, ep.anthropic, timeoutSec,
                               timeoutSec < 15 ? timeoutSec : 15L);
    if (!posted && cfg.noThinking && err->find("HTTP 4") != std::string::npos)
    {
        // 老供应商可能不认关思考的字段（reasoning_effort / thinking）：被 4xx 拒了就摘掉重试一次
        LlmSettings plain = cfg;
        plain.noThinking = false;
        const std::string retryBody = ep.anthropic ? BuildBodyAnthropic(plain, msgs, temperature)
                                                   : BuildBody(plain, msgs, temperature, jsonMode);
        resp.clear();
        posted = CurlHttpPost(ep, retryBody, cfg.key, &resp, err, ep.anthropic, timeoutSec,
                              timeoutSec < 15 ? timeoutSec : 15L);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const long long elapsedMs = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    if (!posted)
    {
        // 失败也要把真实耗时交给调用方：日志里"用时 X 秒（超时）"比一刀切的 0.0 秒有用得多
        if (usage != NULL) usage->ms += elapsedMs;
        return false;
    }
    if (getenv("L2D_LLM_DEBUG") != NULL)   // 排障用：请求地址 + 响应头 200 字（L2D_LLM_DEBUG=1 打开）
    {
        fprintf(stderr, "[llm-debug] POST %s → %zu 字节响应：%.200s\n", FullUrl(ep).c_str(), resp.size(), resp.c_str());
    }
    jsonlite::Value v;
    if (!jsonlite::parse(resp.c_str(), resp.size(), v, NULL) || !v.isObj())
    {
        *err = "响应不是合法 JSON";
        return false;
    }
    LlmUsage measured;
    measured.ms = elapsedMs;
    measured.tokIn = (long long)(ep.anthropic ? v.get("usage").get("input_tokens").numOr(0)
                                              : v.get("usage").get("prompt_tokens").numOr(0));
    measured.tokOut = (long long)(ep.anthropic ? v.get("usage").get("output_tokens").numOr(0)
                                               : v.get("usage").get("completion_tokens").numOr(0));
    {
        std::lock_guard<std::mutex> lk(g_statusMu);
        g_tokens.last = measured;
        g_tokens.total.ms += measured.ms;
        g_tokens.total.tokIn += measured.tokIn;
        g_tokens.total.tokOut += measured.tokOut;
    }
    if (usage != NULL)
    {
        usage->ms += measured.ms;
        usage->tokIn += measured.tokIn;
        usage->tokOut += measured.tokOut;
    }
    std::string text;
    bool truncated = false;
    if (ep.anthropic)
    {
        const jsonlite::Value& blocks = v.get("content");
        if (!blocks.isArr())
        {
            *err = "响应缺少 content";
            return false;
        }
        // 只取 text 块：思考型模型（如 mimo-v2.6、Claude 扩展思考）会先给一段 thinking，不能当台词。
        // 注意 strOr("") 返回 const char*，比字符串必须先用 std::string 接住（直接和字面量 == 是指针比较）
        for (size_t i = 0; i < blocks.arr.size(); ++i)
        {
            const std::string type = blocks.arr[i].get("type").strOr("");
            if (type == "text") text += blocks.arr[i].get("text").strOr("");
        }
        const std::string stop = v.get("stop_reason").strOr("");
        truncated = stop == "max_tokens";
    }
    else
    {
        const jsonlite::Value& choices = v.get("choices");
        if (!choices.isArr() || choices.arr.empty())
        {
            *err = "响应缺少 choices";
            return false;
        }
        text = choices.arr[0].get("message").get("content").strOr("");
        const std::string finish = choices.arr[0].get("finish_reason").strOr("");   // 同上：先接成 std::string
        truncated = finish == "length";
    }
    if (truncated)
    {
        *err = "回复达到模型输出上限；请降低 Token 节省档位后重试";
        return false; // Never retry truncated JSON as if it were malformed output.
    }
    *content = text;
    return true;
#endif
}

bool LlmChat(const LlmSettings& cfg, const std::vector<LlmMsg>& msgs, float temperature,
             bool jsonMode, std::string* content, std::string* err, LlmUsage* usage)
{
    // 没真正发请求的情况（退出中 / 睡眠暂停 / 没 Key）不记成一次结果——"已连接"状态只反映真实请求
    if (g_cancel.load())
    {
        *err = "已取消（正在退出）";
        return false;
    }
    if (g_paused.load())
    {
        *err = "她在睡觉（LLM 暂停，点她一下才恢复）";
        return false;
    }
    if (cfg.key.empty())
    {
        *err = "没有 API Key（在桌宠「状态与设置 → 对话」里填，或设环境变量 LLM_API_KEY）";
        return false;
    }
    const bool ok = LlmChatImpl(cfg, msgs, temperature, jsonMode, content, err, usage);
    if (!g_cancel.load()) RecordStatus(ok, ok ? std::string() : *err);
    return ok;
}

bool LlmTest(const LlmSettings& cfg, LlmTestResult* out)
{
    out->ok = false;
    out->protocol = "openai";
    out->url.clear();
    out->ms = 0;
    out->sample.clear();
    out->error.clear();
    if (cfg.baseUrl.empty())
    {
        out->error = "还没填接口地址（例如 https://api.deepseek.com）";
        return false;
    }
    if (cfg.model.empty())
    {
        out->error = "还没选模型：在下面的「模型列表」里点「选用」";
        return false;
    }
    if (cfg.key.empty())
    {
        out->error = "还没填 API Key";
        return false;
    }
#if !defined(L2D_WITH_CURL)
    out->error = "此构建未编入 libcurl，无法测试连接";
    return false;
#else
    Endpoint ep;
    std::string err;
    if (!ParseEndpoint(cfg.baseUrl, &ep, &err)) { out->error = err; return false; }
    out->protocol = ep.anthropic ? "anthropic" : "openai";
    out->url = FullUrl(ep);   // 地址填错时，面板上直接看到"实际拼成了什么"
    if (!CheckEndpointPublic(ep, &err)) { out->error = err; return false; }
    std::vector<LlmMsg> msgs(1);
    msgs[0].role = "user";
    msgs[0].content = "只回复两个字：好的";   // 最小请求：一句话、几乎不花 token
    LlmSettings probe = cfg;
    probe.maxReplyTokens = 64;
    probe.timeoutSec = 30;   // 试连接不用等太久
    const auto t0 = std::chrono::steady_clock::now();
    std::string content;
    const bool ok = LlmChatImpl(probe, msgs, 0.0f, false, &content, &err, NULL);
    out->ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    RecordStatus(ok, ok ? std::string() : err);
    if (!ok)
    {
        out->error = err;
        return false;
    }
    std::string sample = content;
    const size_t nl = sample.find('\n');
    if (nl != std::string::npos) sample = sample.substr(0, nl);
    // 按 UTF-8 字符截断（按字节截会把汉字切成半个，回给面板就是非法 UTF-8）
    if (sample.size() > 60) sample = plat::Utf8Head(sample, 60) + "…";
    out->sample = sample.empty() ? "（模型回了个空字符串）" : sample;
    out->ok = true;
    return true;
#endif
}

}   // namespace l2d
