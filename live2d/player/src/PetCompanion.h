// Local companion feedback for external agents. No inference or network requests.
#pragma once
#include <algorithm>
#include <cmath>
#include <deque>
#include <string>

class PetCompanion
{
public:
    struct Event { std::string kind, source, message, speech, identity; double at; };
    struct Result { bool shown = false; std::string reason, speech, action; };
    static bool ValidKind(const std::string& kind)
    {
        return kind == "started" || kind == "working" || kind == "progress" || kind == "waiting"
            || kind == "failed" || kind == "completed" || kind == "interrupted";
    }
    Result Receive(const std::string& kind, const std::string& source, const std::string& message,
                   const std::string& identity, double now, double interval, bool enabled,
                   bool awaitingChoice, const std::string& lang)
    {
        Result out;
        if (!ValidKind(kind)) { out.reason = "invalid_kind"; return out; }
        if (!enabled) { out.reason = "disabled"; return out; }
        interval = std::isfinite(interval) ? std::max(10.0, std::min(120.0, interval)) : 30.0;
        const std::string key = kind + "|" + source + "|" + identity + "|" + (identity.empty() ? message : std::string());
        for (const Seen& seen : _seen)
            if (seen.key == key && now - seen.at < (identity.empty() ? 10.0 : 3600.0))
            { out.reason = "duplicate"; return out; }
        if ((kind == "working" || kind == "progress") && now - _shownAt < interval)
        { out.reason = "rate_limited"; return out; }
        _seen.push_back({key, now});
        while (_seen.size() > 200) _seen.pop_front();
        const int variant = (int)(_counter++ % 3);
        out.speech = Line(kind, lang, variant);
        out.action = kind == "started" || kind == "working" ? "think"
                   : kind == "waiting" ? "shy" : kind == "failed" ? "sad"
                   : kind == "interrupted" ? "bow" : "nod_yes";
        _events.push_back({kind, source, message, out.speech, identity, now});
        while (_events.size() > 60) _events.pop_front();   // 面板"最近的工作消息"（只存于本次运行，不落盘）
        if (awaitingChoice) { out.reason = "awaiting_choice"; return out; }
        _shownAt = now;
        out.shown = true;
        return out;
    }
    const std::deque<Event>& Events() const { return _events; }
private:
    struct Seen { std::string key; double at; };
    std::deque<Seen> _seen;
    std::deque<Event> _events;
    unsigned long _counter = 0;
    double _shownAt = -1e9;
    static std::string Line(const std::string& kind, const std::string& lang, int variant)
    {
        if (lang == "en")
        {
            if (kind == "started") return "On it! Get comfy, I'll keep you company.";
            if (kind == "working") return "Still working away. I'm right here with you.";
            if (kind == "progress") return "A little progress! Let's take it one step at a time.";
            if (kind == "waiting") return "Your call on this one. I'll wait right here!";
            if (kind == "failed") return "That step hit a snag. Take a breath; let's check it together.";
            if (kind == "interrupted") return "Pausing now. I'm still here when you need me.";
            return "This round is done. Come take a look at the reply!";
        }
        if (lang == "ja")
        {
            if (kind == "started") return "了解！楽にしてね。一緒に取り組もう～";
            if (kind == "working") return "作業中だよ。ここで一緒に待っているね。";
            if (kind == "progress") return "少し進んだよ！一歩ずついこう～";
            if (kind == "waiting") return "ここはあなたに決めてほしいな。待っているね～";
            if (kind == "failed") return "ここでつまずいたみたい。焦らず一緒に確認しよう。";
            if (kind == "interrupted") return "いったんお休みだね。私はここにいるよ～";
            return "今回の応答が終わったよ。内容を見てみよう～";
        }
        if (kind == "started")
        {
            const char* lines[] = {"收到～我陪你一起把它搞定。", "小助手开工啦，你先坐舒服～", "好啦，开始忙活！我在旁边陪你。"};
            return lines[variant];
        }
        if (kind == "working")
        {
            const char* lines[] = {"正在努力啃这块硬骨头，别急呀～", "还在认真忙活，我陪你等一会儿。", "小脑袋转转转，正在一步步处理呢～"};
            return lines[variant];
        }
        if (kind == "progress") return "又往前走了一小步～来看看新进展。";
        if (kind == "waiting") return "这里需要你拿个主意，我乖乖等你哦～";
        if (kind == "failed") return "这一步卡住啦，不急，我们一起看看。";
        if (kind == "interrupted") return "收到，先停一下。我还在这儿陪你～";
        const char* lines[] = {"这一轮忙完啦，快看看新消息～", "叮！回复送到，陪你一起看结果。", "新回复准备好啦，辛苦你等我～"};
        return lines[variant];
    }
};
