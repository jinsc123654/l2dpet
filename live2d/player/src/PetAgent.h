/**
 * PetAgent.h — 桌宠内置 agent（pet_agent.py 的 C++ 移植）：独立线程 + 内部调用桌宠协议接口。
 *
 * 线程模型：LLM 调用阻塞可达 60s，绝不能占主循环——agent 自己一个线程；
 *   ① 事件流入：主循环把每条广播事件拷一份给 PushEvent()；
 *   ② 命令执行：agent 把协议命令行经 Rpc() 投给主线程（PopPending/FinishPending），
 *      主线程用 L2DPlayer::PetCommand() 执行（与 TCP/面板同一入口，完全绕过 socket）。
 * 协议端口 23333 保留：UI 进程 / 调试工具 / 将来的板卡照常走 TCP。
 */
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "JsonLite.h"
#include "LlmClient.h"
#include "PetJokes.h"
#include "PetMemory.h"
#include "PetPersona.h"
#include "PetQuestions.h"
#include "PetWeather.h"

class PetAgent
{
public:
    struct RpcCall
    {
        std::string line;
        std::string reply;
        bool done = false;
        bool abandoned = false;
        std::condition_variable cv;
        std::mutex mu;
    };
    typedef std::shared_ptr<RpcCall> RpcPtr;

    PetAgent() {}
    ~PetAgent();

    void Start();
    void Stop();
    bool Running() const { return _run.load(); }

    // ---- 主线程侧
    void PushEvent(const std::string& jsonLine);   // 事件流入（内部有界，满了丢最旧）
    bool PopPending(RpcPtr* out);                  // 主循环取一条待执行协议命令
    void FinishPending(const RpcPtr& r);           // 主线程填好 r->reply 后调用（唤醒 agent）

private:
    void Run();
    std::string Rpc(const std::string& line);      // agent 线程侧：同步请求-回复
    bool LoadCaps();                               // query 能力表（换模型时重调）
    std::string ZoneCn(const std::string& key) const;
    static bool JsonRpcOk(const std::string& reply, jsonlite::Value* out);
    void ApplyMood(const jsonlite::Value* mood);
    void ApplyConfig(const jsonlite::Value* cfg);
    std::string CfgGetStr(const char* key) const;   // 字符串配置（config 的 str 段：weather_loc / lang）
    std::string SpeechLangHint() const;             // 界面语言非中文时给提示词加一句"用该语言说"（中文时为空串）
    void LangRewriteAll(const std::string& lang);   // 界面语言变了：重写记忆展示 + 重生成性格/台词库/题库/笑话
    std::string MoodText() const;
    std::string BacklogText() const;
    static std::vector<std::string> SummarizeSay(const jsonlite::Value& seq);
    bool LlmSequence(const std::string& note, const std::string& memory, jsonlite::Value* seqOut, std::string* err);
    bool LlmReactLines(const std::string& memory, jsonlite::Value* linesOut, std::string* err);
    void Announce();
    void OpenMemory();
    void PushMemory();
    void OpenPersona(bool regen);                  // 读/生成这个模型的性格（regen=强制重新生成）
    std::string CapsDigest() const;                // 能力摘要（喂给性格生成的提示词）
    void PushPersona();                            // 报给面板（模型页显示）
    void OpenQuestions(bool regen);                // 读/生成"问题库"（regen=强制重新生成一批）
    std::string AskCandidatesNote(const std::vector<const PetQuestions::Item*>& candidates) const;
    void OpenJokes(bool force);                    // 笑话池：读盘 / 不够就补货（LLM）→ 推给桌宠
    void RefreshWeather();
    void PushJokes();                              // 把池子推给桌宠（本地讲笑话用）
    void CompressMemory(const std::string& reason);   // 记忆压缩（每天自动 / 面板立即）
    void PushLines(const std::string& reason);
    void Consolidate(const std::string& reason, bool tidy);
    void Report(const std::string& kind, const std::string& note, const jsonlite::Value* seq,
                bool ok, const std::string& err, bool asked, int warn);

    // ---- 状态（python main() 的局部量）
    struct Caps
    {
        std::string raw;              // 能力表 JSON 文本（进提示词）
        std::string modelName, modelFv;
        std::map<std::string, std::string> zoneZh;   // zone 键 → 中文名
        std::vector<std::string> exprFv;             // 可用表情 fv（性格的 fav_expr 白名单）
        std::vector<std::string> exprZh;             // 同上，中文名（摘要用）
    };

    std::thread _th;
    std::atomic<bool> _run{ false };
    std::mutex _evMu;
    std::vector<std::string> _events;
    std::mutex _rpcMu;
    std::deque<RpcPtr> _pending;

    Caps _caps;
    std::vector<l2d::LlmMsg> _history;
    std::vector<jsonlite::Value> _lastWarn;
    l2d::LlmUsage _lastCall;
    long _segN = 0;
    double _nextAt = 0.0;
    double _lastLlmAt = 0.0;
    double _lastQuestionAt = -1e9;
    double _awaitingUntil = 0.0;
    std::map<std::string, long> _backlog[2];   // [0]=click [1]=rub：zone → 次数
    double _lastTouchAt = 0.0;
    std::string _lastHover;
    // mood / config（事件更新）
    std::string _moodState = "awake";
    std::string _moodWhy;
    std::string _moodEmoName = "neutral";
    double _moodEmoIntensity = 0.0, _moodAffection = 0.3, _moodAnnoy = 0.0, _moodLonely = 0.0;
    double _cfgIdlePeriod = 60.0;
    bool _cfgLlmIdle = true;
    bool _cfgLlmQuestions = true;
    bool _cfgWeather = false;         // 天气闲聊（配置里打开才在闲时提天气）
    PetWeather _weather;
    std::string _cfgWeatherLoc;       // 主人所在位置（可空）
    std::string _cfgLang = "zh";      // 界面语言（config.str.lang：zh/en/ja/auto）→ 她说的话也用这个语言
    bool _deferLangRewrite = false;   // 语言变了要重写存起来的内容：睡着/暂停时排队，醒来补做
    std::string _pendingLang;         // 排队时的目标语言（zh/en/ja）
    std::string _historyLang;         // _history（多轮对话）是用哪种语言说的；与当前语言不同就整段丢掉（新开一轮对话）
    void ResetConversation(const std::string& why);
    /** 盘上缓存（题库/性格）的语言跟当前不符？（语言在桌宠没跑时被改过、导入备份、切模型都会撞上） */
    bool QuestionsStaleLang() const;
    bool PersonaStaleLang() const;
    // 台词库
    bool _linesOk = false;
    double _linesLast = 0.0;
    bool _linesDirty = false;
    bool _linesUrgent = false;   // 主人刚答了问题：下次空闲立刻重写台词库（不等 kLinesRefresh）
    std::string _linesDirtyReason;
    // 记忆
    std::unique_ptr<PetMemory> _mem;
    double _memLast = 0.0;
    std::string _lastQText;
    std::vector<std::string> _lastQOpts;
    // 性格（每个模型一份，按指纹落盘；见 PetPersona.h）
    PetPersona _persona;
    // 问题库（每个模型一份：想了解主人的问题 + 已问/已答；见 PetQuestions.h）
    PetQuestions _questions;
    // 笑话池（每个模型一份：LLM 定期增量补货，桌宠本地念；见 PetJokes.h）
    PetJokes _jokes;
    double _jokesLastGenAt = -1e9;
    // 睡眠省 token：睡着即暂停 LLM；**只有主人点她**（或被戳醒）才恢复——自己睡饱醒来只用本地台词库
    bool _llmPaused = false;
    bool _choiceDid = false;   // 刚点的选项自带了动作（已在桌宠上演）：续戏用 queue 不打断它
    bool _deferPersona = false, _deferCompress = false;   // 暂停期间面板要求的 LLM 活：恢复后再做
    void SetLlmPaused(bool paused, const std::string& why);   // 上次补货尝试（防抖：至少间隔 120s）
};
