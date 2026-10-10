/**
 * PetQuestions.h — 桌宠的"问题库"：她想了解主人的问题（分题材、不重复、带答案）
 *
 * 为什么有它：原来"提问"只有一句含糊引导（"称呼/作息/喜好之类"，且只在还不了解主人时给），
 * 其它时候 LLM 只能自由发挥 → 每次都落回最笼统的"你在干什么呀？"。现在：
 *   · 由 LLM **按题材清单**一次生成 12 题（称呼/作息/工作学习/口味/爱好/心情/家人/回忆/打算/关系/她的请求/闲聊），
 *     每题带 2~3 个可点选项；
 *   · 按**模型指纹**落盘：~/.config/l2dpet/questions/<指纹>.json（0600，人可读可手改）；
 *   · 问过记 asked、主人答了记 answer；取候选时优先没问过的，避免重复；答案同时进跨会话记忆；
 *   · 题库问完/太旧/答够了几题 → 带着"已知"重新生成一批（新题材 + 会避开重复）。
 */
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "JsonLite.h"

class PetQuestions
{
public:
    /** 一次纯文本 LLM 调用（system, user → out） */
    typedef std::function<bool(const std::string& system, const std::string& user, std::string* out,
                               std::string* err)>
        LlmFn;

    struct Item
    {
        std::string topic;    // 题材（作息/口味/爱好…）
        std::string q;        // 问题（她口吻的短句）
        std::vector<std::string> options;
        int asked = 0;        // 问过几次
        std::string answer;   // 主人的回答（选项文本）
        std::string at;       // 回答时间（%m-%d %H:%M）
    };

    /** 读缓存（fp = 模型指纹） */
    bool Load(const std::string& fp);
    bool Save(std::string* err) const;
    void Reset() { _items.clear(); _fp.clear(); }

    /**
     * LLM 生成题库：personaBlurb = 性格一句（问题要符合她的口吻）、knownQA = 已知问答（避免重复）。
     * 生成的题会替换整个题库（保留旧库里已答过的答案，按问题文本匹配）。
     */
    bool Generate(const std::string& fp, const std::string& modelName, const std::string& personaBlurb,
                  const std::vector<std::string>& knownQA, const LlmFn& callFn, std::string* err);

    bool Empty() const { return _items.empty(); }
    size_t Count() const { return _items.size(); }
    int AskedCount() const;
    int AnsweredCount() const;
    bool Stale(double maxAgeSec) const;   // 生成时间超过 maxAgeSec（或没有时间戳）

    /** 还没问过的候选（≤n 条，按题材轮转：先没答过的题材，再没问过的） */
    std::vector<const Item*> Fresh(size_t n) const;
    /** 记录"问了这题"：按问题文本模糊匹配；找不到（LLM 自创）则新增一条 asked=1 */
    void MarkAsked(const std::string& q, const std::vector<std::string>& options);
    /** 记录答案：找到对应题填 answer；找不到就按 label 新记一条 */
    void MarkAnswered(const std::string& q, const std::string& label);

    /** 给提示词的"已知"摘要（已答/已问的题面，防重复） */
    std::string KnownText() const;
    /** 面板/日志用一行摘要 */
    std::string Summary() const;

private:
    std::string _fp, _model, _at;
    double _t = 0.0;   // 生成时间（epoch 秒；旧文件可能为 0）
    std::vector<Item> _items;
    std::string _path;

    Item* FindByQ(const std::string& q);
};
