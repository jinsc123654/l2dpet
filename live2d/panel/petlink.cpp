// petlink.cpp — 见 petlink.h
#include "petlink.h"

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include "../player/src/JsonLite.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SockT = SOCKET;
static void CloseSocket(SockT s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SockT = int;
static void CloseSocket(SockT s) { close(s); }
#endif

static bool Authenticate(SockT sock, int port)
{
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (std::getenv("USERPROFILE")) home = std::getenv("USERPROFILE");
#endif
    if (home == nullptr) return false;
    std::ifstream file(std::filesystem::u8path(std::string(home) + "/.config/l2dpet/control-token-" + std::to_string(port)));
    std::string token;
    file >> token;
    if (token.size() != 64 || token.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
    const std::string auth = "{\"cmd\":\"auth\",\"token\":\"" + token + "\"}\n";
    size_t sent = 0;
    while (sent < auth.size())
    {
#ifdef _WIN32
        const int n = ::send(sock, auth.data() + sent, (int)(auth.size() - sent), 0);
#else
        const int n = (int)::send(sock, auth.data() + sent, auth.size() - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        sent += n;
    }
    std::string line;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (line.size() < 256 && std::chrono::steady_clock::now() < deadline)
    {
#ifdef _WIN32
        WSAPOLLFD pfd{sock, POLLRDNORM, 0};
        if (WSAPoll(&pfd, 1, 100) <= 0) continue;
#else
        pollfd pfd{sock, POLLIN, 0};
        if (poll(&pfd, 1, 100) <= 0) continue;
#endif
        char byte;
        if (recv(sock, &byte, 1, 0) != 1) return false;
        if (byte == '\n')
        {
            jsonlite::Value reply;
            return jsonlite::parse(line.c_str(), line.size(), reply, nullptr) && reply.get("ok").isBool() && reply.get("ok").b;
        }
        line += byte;
    }
    return false;
}

void PetLink::start(int port, std::function<void()> onChange)
{
    if (_running.exchange(true))
    {
        return;
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    _port = port;
    _onChange = std::move(onChange);
    _thread = std::thread([this] { loop(); });
    _evThread = std::thread([this] { eventLoop(); });
}

void PetLink::stop()
{
    if (!_running.exchange(false))
    {
        return;
    }
    if (_thread.joinable())
    {
        _thread.join();
    }
    if (_evThread.joinable())
    {
        _evThread.join();
    }
    closeSock();
}

void PetLink::send(const std::string& json)
{
    std::lock_guard<std::mutex> lk(_mu);
    if (_queue.size() < 64)
    {
        _queue.push_back(json);
    }
}

bool PetLink::popEvent(std::string* json)
{
    std::lock_guard<std::mutex> lk(_mu);
    if (_events.empty()) return false;
    *json = _events.front();
    _events.pop_front();
    return true;
}

std::string PetLink::snapshot(bool* connected) const
{
    std::lock_guard<std::mutex> lk(_mu);
    if (connected != nullptr)
    {
        *connected = _connected;
    }
    return _state;
}

void PetLink::closeSock()
{
    if (_sock >= 0)
    {
        CloseSocket((SockT)_sock);
        _sock = -1;
    }
    _buf.clear();
}

bool PetLink::connectOnce()
{
    SockT s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
    if (s == INVALID_SOCKET) return false;
#else
    if (s < 0) return false;
#endif
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 只连本机
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0)
    {
        CloseSocket(s);
        return false;
    }
    if (!Authenticate(s, _port)) { CloseSocket(s); return false; }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
    _sock = (long long)s;
    _buf.clear();
    return true;
}

bool PetLink::sendLine(const std::string& line)
{
    const std::string data = line + "\n";
    size_t off = 0;
    while (off < data.size())
    {
#ifdef _WIN32
        const int n = ::send((SockT)_sock, data.data() + off, (int)(data.size() - off), 0);
#else
        const ssize_t n = ::send((SockT)_sock, data.data() + off, data.size() - off, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

bool PetLink::readLine(std::string* line, int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;)
    {
        const size_t nl = _buf.find('\n');
        if (nl != std::string::npos)
        {
            *line = _buf.substr(0, nl);
            _buf.erase(0, nl + 1);
            return true;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) return false;
#ifdef _WIN32
        WSAPOLLFD pfd{ (SockT)_sock, POLLRDNORM, 0 };
        if (WSAPoll(&pfd, 1, (int)left) <= 0) return false;
#else
        pollfd pfd{ (SockT)_sock, POLLIN, 0 };
        if (poll(&pfd, 1, (int)left) <= 0) return false;
#endif
        char tmp[16384];
        const int n = (int)recv((SockT)_sock, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        _buf.append(tmp, (size_t)n);
        if (_buf.size() > (8u << 20)) return false;   // 防失控
    }
}

void PetLink::loop()
{
    using namespace std::chrono;
    auto lastPoll = steady_clock::now() - seconds(10);
    while (_running)
    {
        if (_sock < 0)
        {
            if (!connectOnce())
            {
                bool changed = false;
                {
                    std::lock_guard<std::mutex> lk(_mu);
                    changed = _connected;
                    _connected = false;
                }
                if (changed && _onChange) _onChange();
                for (int i = 0; i < 10 && _running; ++i) std::this_thread::sleep_for(milliseconds(100));
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(_mu);
                _connected = true;
            }
            lastPoll = steady_clock::now() - seconds(10);
        }
        // 1) 发界面排队的命令（回执读掉；面板以下一次 panel_state 为准）
        std::deque<std::string> q;
        {
            std::lock_guard<std::mutex> lk(_mu);
            q.swap(_queue);
        }
        bool ok = true;
        for (const std::string& c : q)
        {
            std::string reply;
            if (!sendLine(c) || !readLine(&reply, 3000))
            {
                ok = false;
                break;
            }
        }
        // 2) 拉面板状态（有命令刚发出去就立刻拉一次，界面跟手）
        if (ok && (!q.empty() || steady_clock::now() - lastPoll > milliseconds(400)))
        {
            lastPoll = steady_clock::now();
            std::string reply;
            if (sendLine("{\"cmd\":\"panel_state\"}") && readLine(&reply, 3000))
            {
                bool changed = false;
                {
                    std::lock_guard<std::mutex> lk(_mu);
                    if (reply != _state)
                    {
                        _state = reply;
                        changed = true;
                    }
                }
                if (changed && _onChange) _onChange();
            }
            else
            {
                ok = false;
            }
        }
        if (!ok)
        {
            closeSock();
            continue;
        }
        std::this_thread::sleep_for(milliseconds(40));
    }
}

// 事件连接：独立 socket，订阅后一直读；断了每秒重连
void PetLink::eventLoop()
{
    using namespace std::chrono;
    while (_running)
    {
        SockT s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef _WIN32
        if (s == INVALID_SOCKET)
#else
        if (s < 0)
#endif
        {
            for (int i = 0; i < 10 && _running; ++i) std::this_thread::sleep_for(milliseconds(100));
            continue;
        }
        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((unsigned short)_port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, (sockaddr*)&addr, sizeof(addr)) != 0)
        {
            CloseSocket(s);
            for (int i = 0; i < 10 && _running; ++i) std::this_thread::sleep_for(milliseconds(100));
            continue;
        }
        if (!Authenticate(s, _port)) { CloseSocket(s); std::this_thread::sleep_for(milliseconds(1000)); continue; }
        const std::string sub = "{\"cmd\":\"subscribe\",\"role\":\"ui\"}\n";
#ifdef _WIN32
        ::send(s, sub.data(), (int)sub.size(), 0);
#else
        ::send(s, sub.data(), sub.size(), MSG_NOSIGNAL);   // 桌宠刚好退出时不能被 SIGPIPE 杀掉
#endif
        std::string buf;
        bool first = true;   // 第一行是 subscribe 的回执
        while (_running)
        {
#ifdef _WIN32
            WSAPOLLFD pfd{ s, POLLRDNORM, 0 };
            const int pr = WSAPoll(&pfd, 1, 200);
#else
            pollfd pfd{ s, POLLIN, 0 };
            const int pr = poll(&pfd, 1, 200);
#endif
            if (pr == 0) continue;
            if (pr < 0) break;
            char tmp[16384];
            const int n = (int)recv(s, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            buf.append(tmp, (size_t)n);
            size_t nl;
            bool any = false;
            while ((nl = buf.find('\n')) != std::string::npos)
            {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (first) { first = false; continue; }
                // 面板要处理的事件：ui_*（菜单/气泡/设置窗）+ 模型页的 model_import / model_delete / probe / persona
                // （其余事件如 touch/mood/click/hover 面板不关心，挡在这里省得排队与重绘）
                if (line.find("\"event\":\"ui_") == std::string::npos
                    && line.find("\"event\":\"model_import\"") == std::string::npos
                    && line.find("\"event\":\"model_delete\"") == std::string::npos
                    && line.find("\"event\":\"probe\"") == std::string::npos
                    && line.find("\"event\":\"persona\"") == std::string::npos
                    && line.find("\"event\":\"mem_export\"") == std::string::npos
                    && line.find("\"event\":\"mem_import\"") == std::string::npos
                    && line.find("\"event\":\"mem_compress_done\"") == std::string::npos) continue;
                std::lock_guard<std::mutex> lk(_mu);
                if (_events.size() < 256) _events.push_back(line);
                any = true;
            }
            if (any && _onChange) _onChange();
        }
        CloseSocket(s);
    }
}
