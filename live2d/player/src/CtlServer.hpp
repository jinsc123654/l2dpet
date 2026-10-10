/**
 * CtlServer.hpp — 本机控制端口：TCP JSON Lines（协议见仓库根 PROTOCOL-LLM.md）
 *
 * 线程模型：接收线程 + 每连接一个读线程，只负责"收行 → 进队列"；
 * 解析与执行在**主循环**里做（Poll 取行、Reply 回写），播放器状态因此无需加锁。
 * 只监听 127.0.0.1。
 */
#pragma once

#include <set>
#include <string>

class CtlServer
{
public:
    CtlServer();
    ~CtlServer();

    /** 开始监听 127.0.0.1:port；失败返回 false */
    bool Start(int port);
    void Stop();

    /** 主循环调用：取一行请求（conn = 连接号，回复用）；没有返回 false */
    bool Poll(int* conn, std::string* line);

    /** 回复（自动补 '\n'）；连接已断则静默丢弃 */
    void Reply(int conn, const std::string& json);

    /** 把连接标成"订阅事件"；Broadcast 推给所有订阅者 */
    void Subscribe(int conn);
    void Broadcast(const std::string& json);

    /** 当前订阅事件的连接数（≈ 连着的 LLM agent 数） */
    int SubscriberCount();

    /** 把订阅连接标成 UI 进程（不算进"LLM agent 已连接"的计数）*/
    void MarkUi(int conn);

private:
    struct Impl;
    Impl* _impl;
};
