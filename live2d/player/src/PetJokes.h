/**
 * PetJokes.h — 桌宠的笑话池（LLM 定期增量生成，按模型指纹落盘，讲过的记 used）
 *
 * 为什么要独立一份：笑话只在"台词库"里生成的话，一辈子就那 6 句；这里做成**池子**——
 * 每次让 LLM 生成几条**新的**（把"已有的"喂回去要求不重复），池子随时间长起来；
 * 讲笑话仍是**本地秒回**（pet 端从推下去的那份池子里轮着念），只在需要补货时才调 LLM。
 */
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "JsonLite.h"

class PetJokes
{
public:
    typedef std::function<bool(const std::string& system, const std::string& user, std::string* out,
                               std::string* err)>
        LlmFn;

    struct Item
    {
        std::string j;      // 笑话正文
        int used = 0;       // 讲给主人听过几次
        std::string u;      // 最近一次讲的时间（%m-%d %H:%M）
    };

    bool Load(const std::string& fp);
    bool Save(std::string* err) const;
    void Reset() { _items.clear(); _fp.clear(); }

    /** LLM 补货：生成 n 条新的（已有的作为"不要重复"喂回去）；追加进池子，超上限丢最旧的已讲条目 */
    bool Generate(const std::string& fp, const std::string& modelName, const std::string& personaBlurb, int n,
                  const LlmFn& callFn, std::string* err);

    bool Empty() const { return _items.empty(); }
    size_t Count() const { return _items.size(); }
    int UnusedCount() const;
    /** 该不该补货：没讲过的太少（<2）；且距上次生成超过 minGapSec（或不常调） */
    bool NeedMore(double minGapSec) const;
    /** 标记"讲过这条"（按文本匹配，更新 used/u） */
    void MarkUsed(const std::string& j);

    /** 池子全量（新→旧排：新的在后面，pet 端拿到后从头开始念新增那部分） */
    std::vector<std::string> Texts() const;
    std::string Summary() const;

private:
    std::string _fp, _model, _at;
    double _t = 0.0;   // 上次生成时间（epoch 秒）
    std::vector<Item> _items;
    std::string _path;
};
