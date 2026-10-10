/**
 * LlmClient.cpp — 见 LlmClient.h
 * 传输统一走 libcurl（http/https 都行；不跟重定向、60s 超时、响应 ≤2MB）。
 * 未编入 libcurl（L2D_WITH_CURL 未定义）时 LlmChat 直接报错——内置 agent 需要带 curl 构建。
 */
#include "LlmClient.h"
#include "JsonLite.h"

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
};

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
    out->apiPath = (path.empty() ? "" : path) + "/chat/completions";
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
    if (jsonMode) body += ",\"response_format\":{\"type\":\"json_object\"}";
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
                  std::string* resp, std::string* err)
{
    static std::once_flag once;   // curl_easy_init 隐式 global_init 不是线程安全的：显式做一次
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* c = curl_easy_init();
    if (c == NULL)
    {
        *err = "curl_easy_init 失败";
        return false;
    }
    std::string url = ep.scheme + "://" + ep.host;
    if ((ep.scheme == "https" && ep.port != 443) || (ep.scheme == "http" && ep.port != 80))
    {
        url += ":" + std::to_string(ep.port);
    }
    url += ep.apiPath;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, CurlWriteCb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);   // 不跟重定向
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);         // 在 agent 线程里用：不许 curl 用信号做 DNS 超时
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, CurlProgressCb);   // 退出时能立即打断
    struct curl_slist* h = NULL;
    h = curl_slist_append(h, "Content-Type: application/json");
    h = curl_slist_append(h, ("Authorization: Bearer " + key).c_str());
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

static bool LlmChatImpl(const LlmSettings& cfg, const std::vector<LlmMsg>& msgs, float temperature,
                        bool jsonMode, std::string* content, std::string* err, LlmUsage* usage);

bool LlmChat(const LlmSettings& cfg, const std::vector<LlmMsg>& msgs, float temperature,
             bool jsonMode, std::string* content, std::string* err, LlmUsage* usage)
{
    // 没真正发请求的情况（退出中 / 睡眠暂停 / 没 Key）不记成一次结果——"已连接"状态只反映真实请求
    const bool real = !g_cancel.load() && !g_paused.load() && !cfg.key.empty();
    const bool ok = LlmChatImpl(cfg, msgs, temperature, jsonMode, content, err, usage);
    if (real && !g_cancel.load()) RecordStatus(ok, ok ? std::string() : *err);
    return ok;
}

static bool LlmChatImpl(const LlmSettings& cfg, const std::vector<LlmMsg>& msgs, float temperature,
                        bool jsonMode, std::string* content, std::string* err, LlmUsage* usage)
{
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
    const std::string body = BuildBody(cfg, msgs, temperature, jsonMode);
    const auto t0 = std::chrono::steady_clock::now();   // 墙钟（clock() 是 CPU 时间，等网络时几乎不走）
    std::string resp;
    if (!CurlHttpPost(ep, body, cfg.key, &resp, err)) return false;
    const auto t1 = std::chrono::steady_clock::now();
    jsonlite::Value v;
    if (!jsonlite::parse(resp.c_str(), resp.size(), v, NULL) || !v.isObj())
    {
        *err = "响应不是合法 JSON";
        return false;
    }
    const jsonlite::Value& choices = v.get("choices");
    if (!choices.isArr() || choices.arr.empty())
    {
        *err = "响应缺少 choices";
        return false;
    }
    const std::string text = choices.arr[0].get("message").get("content").strOr("");
    if (usage != NULL)
    {
        usage->ms += (long long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        const jsonlite::Value& u = v.get("usage");
        usage->tokIn += (long long)u.get("prompt_tokens").numOr(0);
        usage->tokOut += (long long)u.get("completion_tokens").numOr(0);
    }
    *content = text;
    return true;
#endif
}

}   // namespace l2d
