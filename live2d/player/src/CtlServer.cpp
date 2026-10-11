/**
 * CtlServer.cpp — 本机控制端口实现（见 CtlServer.hpp）
 */
#include "CtlServer.hpp"
#include "Platform.h"
#include "JsonLite.h"
#include <random>
#include <atomic>
#include <condition_variable>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET CtlSockT;
#define CTL_CLOSE(s) closesocket(s)
#define CTL_LASTERR() WSAGetLastError()
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int CtlSockT;
#define CTL_CLOSE(s) close(s)
#define CTL_LASTERR() errno
#endif

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace
{
const size_t kMaxLine = 64 * 1024;   // 一行最多 64 KB（时间线 JSON 足够）
const size_t kMaxQueue = 256;        // 待处理请求上限（防刷爆）
const size_t kMaxConns = 32;         // 同时连接上限（UI 2 条 + agent + 调试工具，远远够用）
}

struct CtlServer::Impl
{
    CtlSockT listenFd = (CtlSockT)-1;
    std::atomic<bool> running{ false };
    std::string token;
    std::condition_variable readersDone;
    size_t readers = 0;
    std::thread acceptThread;
    std::mutex mu;
    std::deque<std::pair<int, std::string> > inbox;
    std::map<int, CtlSockT> fds;     // conn → fd
    std::map<int, bool> subscribed;  // conn → 订阅事件
    std::set<int> uiConns;           // UI 进程的订阅（不算 agent）
    int nextConn = 1;

    void ReadLoop(int conn, CtlSockT fd)
    {
        bool authenticated = false;
        std::string acc;
        char buf[4096];

        for (;;)
        {
            const int n = (int)recv(fd, buf, sizeof(buf), 0);

            if (n <= 0)
            {
                break;
            }
            acc.append(buf, (size_t)n);
            size_t pos;
            bool rejected = false;

            while ((pos = acc.find('\n')) != std::string::npos)
            {
                std::string line = acc.substr(0, pos);

                acc.erase(0, pos + 1);
                if (!line.empty() && line[line.size() - 1] == '\r')
                {
                    line.erase(line.size() - 1);
                }
                if (line.empty())
                {
                    continue;
                }
                if (line.size() > kMaxLine) { rejected = true; break; }
                if (!authenticated)
                {
                    jsonlite::Value auth;
                    const bool parsed = jsonlite::parse(line.c_str(), line.size(), auth, NULL);
                    const std::string supplied = auth.get("token").strOr("");
                    unsigned difference = supplied.size() == token.size() ? 0 : 1;
                    for (size_t i = 0; i < token.size(); ++i)
                        difference |= (unsigned char)token[i] ^ (i < supplied.size() ? (unsigned char)supplied[i] : 0);
                    authenticated = parsed && std::string(auth.get("cmd").strOr("")) == "auth" && difference == 0;
                    SendFd(fd, authenticated ? "{\"ok\":true,\"type\":\"auth\"}\n"
                                             : "{\"ok\":false,\"code\":\"unauthorized\"}\n");
                    if (!authenticated) { rejected = true; break; }
                    // An authenticated idle client may keep its connection open.
#ifdef _WIN32
                    DWORD receiveTimeout = 0;
#else
                    timeval receiveTimeout{};
#endif
                    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&receiveTimeout, sizeof(receiveTimeout));
                    continue;
                }
                // 只收 JSON 行：任何不以 '{' 开头的行（"POST / HTTP/1.1"、HTTP 头……）→ 立刻断开整个连接。
                // 否则任意网页都能 fetch('http://127.0.0.1:<端口>', {method:'POST', body:'\n{"cmd":…}\n'})
                // 跨协议打进来（头行解析失败被忽略，body 里的 JSON 行照样执行）——可改 LLM 接口地址把 API Key 送走、
                // 导出/导入记忆、删模型。正规客户端（UI 进程 / agent / pet_client）每行都是 JSON 对象。
                size_t f = 0;
                while (f < line.size() && (line[f] == ' ' || line[f] == '\t')) ++f;
                if (f >= line.size() || line[f] != '{')
                {
                    fprintf(stderr, "[ctl] 连接 %d 发来非 JSON 行（%.24s…）——疑似浏览器跨协议请求，已断开\n", conn,
                            line.c_str());
                    rejected = true;
                    break;
                }
                std::lock_guard<std::mutex> lk(mu);

                if (inbox.size() < kMaxQueue)
                {
                    inbox.push_back(std::make_pair(conn, line));
                }
            }
            if (rejected)
            {
                break;
            }
            if (acc.size() > kMaxLine)
            {
                break; // Disconnect oversized partial lines rather than accepting their suffix.
            }
        }
        std::lock_guard<std::mutex> lk(mu);

        fds.erase(conn);
        --readers;
        readersDone.notify_all();
        subscribed.erase(conn);
        uiConns.erase(conn);
        CTL_CLOSE(fd);
    }

    void AcceptLoop()
    {
        while (running)
        {
            sockaddr_in addr;
#ifdef _WIN32
            int len = sizeof(addr);
#else
            socklen_t len = sizeof(addr);
#endif
            const CtlSockT fd = accept(listenFd, (sockaddr*)&addr, &len);

            if (fd < 0)
            {
                if (!running)
                {
                    break;
                }
                continue;
            }
            // 写超时：主机不读也不会卡住主循环（Windows 的 SO_SNDTIMEO 是 DWORD 毫秒，不是 timeval）
#ifdef _WIN32
            const DWORD tvMs = 2000;
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tvMs, sizeof(tvMs));
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tvMs, sizeof(tvMs));
#else
            timeval tv;
            tv.tv_sec = 2;
            tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
            int conn;
            {
                std::lock_guard<std::mutex> lk(mu);

                if (fds.size() >= kMaxConns)   // 每个连接一个线程：限个数，防被刷爆
                {
                    CTL_CLOSE(fd);
                    continue;
                }
                conn = nextConn++;
                fds[conn] = fd;
                ++readers;
            }
            std::thread(&Impl::ReadLoop, this, conn, fd).detach();
        }
    }

    /** 整行发完返回 true。失败（对端不读 → 写超时 / 已断）时把连接 shutdown：读线程随即收尾，
     *  之后不再往它发——否则一个不读的订阅者会让每次广播都在锁里卡 2s（主循环跟着卡），
     *  而且半行写出去后续行全错位。 */
    bool SendFd(CtlSockT fd, const std::string& s)
    {
        size_t off = 0;

        while (off < s.size())
        {
#ifdef _WIN32
            const int n = send(fd, s.data() + off, (int)(s.size() - off), 0);
#else
            const int n = (int)send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
#endif

            if (n <= 0)
            {
                shutdown(fd, SHUT_RDWR);
                return false;
            }
            off += (size_t)n;
        }
        return true;
    }
};

CtlServer::CtlServer() : _impl(new Impl()) {}

CtlServer::~CtlServer()
{
    Stop();
    delete _impl;
}

bool CtlServer::Start(int port)
{
    const std::string root = plat::HomeDir() + "/.config/l2dpet";
    if (!plat::CreateDir(root, true)) return false;
    const std::string path = root + "/control-token-" + std::to_string(port);
    try
    {
        std::random_device random;
        const char* hex = "0123456789abcdef";
        _impl->token.clear();
        for (int i = 0; i < 32; ++i)
        {
            const unsigned char b = (unsigned char)random();
            _impl->token += hex[b >> 4]; _impl->token += hex[b & 15];
        }
    }
    catch (...) { return false; }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    _impl->listenFd = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (_impl->listenFd == INVALID_SOCKET)
#else
    if (_impl->listenFd < 0)
#endif
    {
        return false;
    }
    int one = 1;

    setsockopt(_impl->listenFd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one));
    sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 只听本机
    if (bind(_impl->listenFd, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(_impl->listenFd, 8) != 0)
    {
        fprintf(stderr, "[ctl] 监听 127.0.0.1:%d 失败（错误码 %d）\n", port, (int)CTL_LASTERR());
        CTL_CLOSE(_impl->listenFd);
        _impl->listenFd = (CtlSockT)-1;
        return false;
    }
    // Write only after binding succeeds: another player on this port must not rotate our token.
    if (!plat::WritePrivateFile(path, _impl->token))
    {
        CTL_CLOSE(_impl->listenFd); _impl->listenFd = (CtlSockT)-1;
        return false;
    }
    _impl->running = true;
    _impl->acceptThread = std::thread(&Impl::AcceptLoop, _impl);
    printf("[ctl] 控制端口 127.0.0.1:%d（JSON Lines，协议见 PROTOCOL-LLM.md）\n", port);
    fflush(stdout);
    return true;
}

void CtlServer::Stop()
{
    if (!_impl->running)
    {
        return;
    }
    _impl->running = false;
    shutdown(_impl->listenFd, SHUT_RDWR);
    CTL_CLOSE(_impl->listenFd);
    if (_impl->acceptThread.joinable())
    {
        _impl->acceptThread.join();
    }
    std::unique_lock<std::mutex> lk(_impl->mu);

    for (std::map<int, CtlSockT>::iterator it = _impl->fds.begin(); it != _impl->fds.end(); ++it)
    {
        shutdown(it->second, SHUT_RDWR);
    }
    _impl->readersDone.wait(lk, [this] { return _impl->readers == 0; });
}

bool CtlServer::Poll(int* conn, std::string* line)
{
    std::lock_guard<std::mutex> lk(_impl->mu);

    if (_impl->inbox.empty())
    {
        return false;
    }
    *conn = _impl->inbox.front().first;
    line->swap(_impl->inbox.front().second);
    _impl->inbox.pop_front();
    return true;
}

void CtlServer::Reply(int conn, const std::string& json)
{
    // 发送在锁内：读线程关 fd 也要拿这把锁，避免往"刚被关掉又被系统复用"的 fd 写
    // （写有 2s 超时，本机场景足够）
    std::lock_guard<std::mutex> lk(_impl->mu);
    std::map<int, CtlSockT>::iterator it = _impl->fds.find(conn);

    if (it != _impl->fds.end())
    {
        _impl->SendFd(it->second, json + "\n");
    }
}

void CtlServer::Subscribe(int conn)
{
    std::lock_guard<std::mutex> lk(_impl->mu);

    _impl->subscribed[conn] = true;
}

void CtlServer::Broadcast(const std::string& json)
{
    std::lock_guard<std::mutex> lk(_impl->mu);

    for (std::map<int, bool>::iterator it = _impl->subscribed.begin(); it != _impl->subscribed.end(); ++it)
    {
        if (!it->second) continue;
        std::map<int, CtlSockT>::iterator f = _impl->fds.find(it->first);

        if (f != _impl->fds.end() && !_impl->SendFd(f->second, json + "\n"))
        {
            it->second = false;   // 发不出去：退订（连接已 shutdown，读线程会清理）
        }
    }
}

int CtlServer::SubscriberCount()
{
    std::lock_guard<std::mutex> lk(_impl->mu);
    int n = 0;

    for (std::map<int, bool>::iterator it = _impl->subscribed.begin(); it != _impl->subscribed.end(); ++it)
    {
        if (it->second && _impl->fds.find(it->first) != _impl->fds.end() && _impl->uiConns.count(it->first) == 0)
        {
            ++n;
        }
    }
    return n;
}

void CtlServer::MarkUi(int conn)
{
    std::lock_guard<std::mutex> lk(_impl->mu);
    _impl->uiConns.insert(conn);
}
