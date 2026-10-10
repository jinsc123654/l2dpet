// petlink.h — l2dpet-ui 与桌宠进程之间的连接（桌宠协议 JSON Lines over TCP，见 PROTOCOL-LLM.md）
//
// 一个后台线程：连 127.0.0.1:<port>；每 ~0.4s 发 {"cmd":"panel_state"} 拉一次面板状态，
// 内容变了才回调 onChange（UI 线程据此 requestUpdate 重新 compose）；界面上的操作经 send() 排队发出。
// 断线自动重连（每秒一次）。POSIX socket / Winsock 双实现，三平台通用。
// 另有一条事件连接：{"cmd":"subscribe","role":"ui"} 收桌宠推来的 ui_* 事件（右键菜单/气泡/打开设置），毫秒级。
#pragma once

#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class PetLink
{
public:
    ~PetLink() { stop(); }

    /** 启动后台线程；onChange 在后台线程里调用（只能做 requestUpdate 这类线程安全的事） */
    void start(int port, std::function<void()> onChange);
    void stop();

    /** 排一条协议命令（一行 JSON，不带换行） */
    void send(const std::string& json);

    /** 最新的 panel_state 回复原文；connected = 当前是否连着桌宠 */
    std::string snapshot(bool* connected) const;

    /** 取一条桌宠推来的事件（JSON 一行）；没有返回 false */
    bool popEvent(std::string* json);

private:
    void loop();
    void eventLoop();
    bool connectOnce();
    void closeSock();
    bool sendLine(const std::string& line);
    bool readLine(std::string* line, int timeoutMs);

    int _port = 23333;
    std::function<void()> _onChange;
    std::thread _thread;
    std::thread _evThread;
    std::deque<std::string> _events;
    std::atomic<bool> _running{ false };
    mutable std::mutex _mu;
    std::deque<std::string> _queue;
    std::string _state;
    bool _connected = false;
    long long _sock = -1;
    std::string _buf;
};
