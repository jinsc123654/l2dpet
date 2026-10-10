/**
 * PetMemory.h — 桌宠的跨会话长期记忆（pet_memory.py 的 C++ 移植；设计见 DESIGN-PET-LIFE.md §10）
 *
 * 分层存储（全部人可读、可手改；Linux 下 0600/目录 0700、tmp+rename 原子写）：
 *   ~/.config/l2dpet/memory/owner.md                  关于主人（所有角色共享）
 *   ~/.config/l2dpet/memory/chars/<角色>.md           我和主人之间（每角色一份 = 模型指纹）
 *   ~/.config/l2dpet/memory/chars/<角色>.habits.json  习惯统计（程序算，不经 LLM）
 *   ~/.config/l2dpet/memory/chars/<角色>.pending.jsonl 待归纳要点（随时落盘）
 *   ~/.config/l2dpet/memory/chars/<角色>.episodes.jsonl 归档（按相关度召回）
 */
#pragma once

#include <ctime>

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "JsonLite.h"

class PetMemory
{
public:
    typedef std::function<std::string(const std::string&)> ZoneNameFn;   // zone 键 → 中文名

    PetMemory(const std::string& charId, const std::string& charName);

    // ---- 读
    std::string Owner() const;
    std::string Us() const;
    int OwnerKnown() const;                       // 关于主人真正了解几条（≤1 = 该找机会问）
    std::string HabitsText(const ZoneNameFn& zoneCn) const;
    std::string Context(const std::string& query, const ZoneNameFn& zoneCn) const;
    std::string PanelText(const ZoneNameFn& zoneCn) const;
    std::vector<std::string> Recall(const std::string& query, int k = 3) const;
    double Gap() const { return _gap; }           // 距上次见面（秒；首次 = -1）
    int PendingWeight() const;
    bool SessionTouchEmpty() const { return _sessionTouch.empty(); }
    std::vector<std::string> RecentChoices() const;

    // ---- 写（全部不抛异常：记忆是增强不是依赖）
    void Note(const std::string& text, int weight = 1);   // 攒要点（立即落盘）
    void Touch(const std::string& zone, bool rub);        // 点/搓只计数
    void ChoiceMade(const std::string& question, const std::string& label);
    void SaveHabits(bool force = false);
    void Heartbeat();                                     // 主循环里调：分钟级刷"上次见面"
    void SaveSessionTouchNow() { SaveHabits(true); }      // 换模型时把待整理留盘

    /** 归纳：llmJson(prompt) → {owner, us}（由调用方接 LlmClient）。返回 true = 记忆有变化。 */
    bool Consolidate(const std::function<bool(const std::string& prompt, std::string* owner, std::string* us)>& llmJson,
                     const ZoneNameFn& zoneCn, bool tidy, std::string* err);

    /**
     * LLM 压缩（每天自动跑一次）：把两份记忆合并去重、旧事折进"更早"、删低价值细节，
     * 并把老归档一并收编后截断 episodes；不新增事实。sizes 可回报压缩前后字数。
     */
    bool Compress(const std::function<bool(const std::string& prompt, std::string* owner, std::string* us)>& llmJson,
                  const ZoneNameFn& zoneCn, std::string* err, std::string* report);

    /**
     * **换语言改写**（面板切了 lang 时由 agent 触发）：把两份记忆原样改写成目标语言
     * （lang = zh|en|ja）——结构/条数一一对应、不增删事实、时间戳原样保留。
     * 与 Compress 的区别：压缩是"合并+删减"，这里是"同义换语言"（所以不做变长约束，
     * 只查结构：段名、条数、`## 最近发生` 还在）。
     */
    bool Rewrite(const std::string& lang,
                 const std::function<bool(const std::string& prompt, std::string* owner, std::string* us)>& llmJson,
                 std::string* err, std::string* report);

    /** 距上次压缩是否超过 maxAgeSec（没压缩过 = true） */
    bool NeedsCompress(double maxAgeSec) const;
    /** 上次压缩时间（epoch 秒；0 = 从没压缩过） */
    double LastCompressAt() const;

private:
    std::string _charId, _name;
    std::string _root, _charsDir;
    std::string _ownerPath, _charPath, _habitsPath, _pendingPath, _episodesPath;
    jsonlite::Value _habits;
    bool _habitsDirty = false;
    double _habitsSavedAt = 0.0;
    double _gap = -1.0;
    std::map<std::string, std::pair<int, int> > _sessionTouch;   // 本轮部位计数：zone → (点, 搓)
    double _legacyMtime = 0.0;

    std::vector<jsonlite::Value> PendingAll() const;
    std::string SessionTouchText(const ZoneNameFn& zoneCn) const;
    void MigrateLegacy();
};
