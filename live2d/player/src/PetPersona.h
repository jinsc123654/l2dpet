/**
 * PetPersona.h — 每个模型一个"性格"（由该模型解算出的能力/外观生成，跨会话保留）
 *
 * 为什么有它：能力表说明"这个模型能做什么"（有哪些表情/道具/动作/分区），但"她是谁"——
 * 脾气、口癖、怎么称呼主人——原来所有模型共用同一套通用提示词，切换模型只换了张皮，
 * 说话方式完全一样。现在：模型能力摘要交给 LLM 生成性格（无 key 时本地启发式兜底），
 * 按**模型指纹**落盘：
 *   ~/.config/l2dpet/personas/<指纹>.json   （0600；人可读可手改，删掉即重新生成）
 *
 * 生成结果注入两处提示词：
 *   ① 段落生成（PetAgent::LlmSequence 系统提示）——回应/闲时小剧场的说话方式与表情偏好；
 *   ② 即时台词库（PetAgent::LlmReactLines）——被点/被摸时本地播的台词也用她的口吻。
 * 于是同一个"摸头"在不同模型上说法不同、偏爱用的表情也不同。
 */
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "JsonLite.h"

class PetPersona
{
public:
    /** 一次纯文本 LLM 调用（system, user → out）；由调用方接 LlmClient（温度低） */
    typedef std::function<bool(const std::string& system, const std::string& user, std::string* out,
                               std::string* err)>
        LlmFn;

    struct Data
    {
        std::string fp;           // 模型指纹（文件名用它）
        std::string model;        // 模型目录名（面板显示）
        std::string name;         // 角色自己的名字/自称
        std::string persona;      // 性格（2~4 句）
        std::string speech;       // 说话风格（口癖、称呼主人的方式）
        std::string likes;        // 喜欢
        std::string hates;        // 不喜欢
        std::vector<std::string> moods;      // 常用情绪（emotion 名）
        std::vector<std::string> favExpr;     // 偏好表情 fv（必须来自该模型能力表）
        std::string by;           // llm | heuristic
        // 主人在面板里手动设的（优先于生成的；重新生成性格时保留）
        std::string userName;     // 名字（空 = 用生成的 name）
        std::string userNick;     // 小名
        std::string userCall;     // 她怎么称呼主人（空 = 按性格/记忆）
        std::string userGuidance; // 主人提供的性格方向，每模型保存
        std::string at;           // 生成时间（%m-%d %H:%M）
    };

    /** 读缓存（fp = 模型指纹；false = 没有/解析失败/指纹不符） */
    bool Load(const std::string& fp);
    bool Save(std::string* err) const;
    /** 清掉内存态（换模型时用） */
    void Reset() { _d = Data(); }

    /**
     * LLM 生成性格：capsDigest = 该模型能力摘要（名字/骨架/表情道具/动作/分区），
     * exprFv = 可用的表情 fv 列表（生成的 fav_expr 会被校验，非法的丢弃）。
     */
    bool Generate(const std::string& fp, const std::string& modelName, const std::string& capsDigest,
                  const std::vector<std::string>& exprFv, const LlmFn& callFn, std::string* err,
                  const std::string& guidance = std::string());

    /** 本地启发式兜底（没有 API Key / 生成失败）：由自带表情道具 + 指纹决定，各模型仍然不同 */
    void Heuristic(const std::string& fp, const std::string& modelName, const std::vector<std::string>& exprFv);

    void SetGuidance(const std::string& guidance);

    bool Empty() const { return _d.persona.empty(); }
    const Data& Get() const { return _d; }

    /** 「你是谁」段：拼进段落生成的系统提示（紧跟在开场白之后） */
    std::string PromptBlock() const;
    /** 台词库提示词的开头角色描述（kLinesPromptA 之后那句） */
    std::string LinesRole() const;
    /** 主人手动设定名字/小名/对主人的称呼（空串 = 清除该项）；立即落盘 */
    bool SetNames(const std::string& name, const std::string& nick, const std::string& call, std::string* err);
    /** 显示用名字：手动设的 > 生成的 > 模型目录名 */
    std::string DisplayName() const;
    /** 面板展示（多行短文本 + 来源标签） */
    std::string PanelText() const;

private:
    void CarryOverrides(Data* d) const;   // 重新生成时把主人手设的三项带过去（从盘上读，内存态可能已 Reset）
    Data _d;
    std::string _path;
};
