#pragma once
#include "JsonLite.h"
#include <algorithm>
#include <deque>
#include <string>
#include <vector>

class PetBubbleQueue
{
public:
    struct Bubble
    {
        std::string text;
        std::vector<std::string> shown, options;
        std::vector<jsonlite::Value> actions;
        float duration = 3;
        float talkDur = 0;   // 口型时长；0 = 上屏时按文本估（speak_dur 等显式值走这里）
        bool talk = true;
    };
    /** 一条气泡的显示时长：自己的 duration 与配置的最短显示时间取大。
     *  只按 minimum 计时会让长气泡（LLM 回复最长 16 秒、提问 20 秒）在最短时间一到
     *  就被后到的台词顶掉——"新气泡打断老气泡"就是这么来的。*/
    static double Hold(const Bubble& bubble, double minimum)
    {
        return std::max((double)bubble.duration, minimum);
    }
    bool Offer(const Bubble& bubble, double now, double minimum)
    {
        if (now >= _until && _pending.empty())
        {
            _until = now + Hold(bubble, minimum);
            return true;
        }
        if (_pending.size() >= 8)
        {
            // Preserve queued questions; shed the oldest ordinary line under a burst.
            auto it = _pending.begin();
            for (; it != _pending.end() && !it->options.empty(); ++it) {}
            if (it == _pending.end()) return false;
            _pending.erase(it);
        }
        _pending.push_back(bubble);
        return false;
    }
    bool Take(double now, double minimum, bool awaitingChoice, Bubble* out)
    {
        if (awaitingChoice || now < _until || _pending.empty()) return false;
        *out = _pending.front();
        _pending.pop_front();
        _until = now + Hold(*out, minimum);
        return true;
    }
    bool HasQuestion() const
    {
        for (const auto& bubble : _pending) if (!bubble.options.empty()) return true;
        return false;
    }
    /** 排队等待显示的气泡数（status 上报，用来确认新气泡是排队而不是顶掉老气泡）*/
    size_t PendingCount() const { return _pending.size(); }
    void Clear() { _pending.clear(); _until = 0; }
private:
    double _until = 0;
    std::deque<Bubble> _pending;
};
