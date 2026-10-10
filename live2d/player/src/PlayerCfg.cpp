/**
 * PlayerCfg.cpp — L2DPlayer 的配置与 LLM实现（从 l2dtest.cpp 拆出，纯搬移，逻辑未改）。
 * 声明与数据成员见 L2DPlayer.h。
 */
#include "L2DPlayer.h"

#include "FileUtil.h"
#include "ModelImport.h"
#include "Backup.h"
#include "Platform.h"
#include "Port.hpp"
#include "JsonLite.h"
#include "CtlServer.hpp"
#include "PetI18n.h"
#include "l2d_config.h"

#include <CubismFramework.hpp>
#include <CubismDefaultParameterId.hpp>
#include <CubismModelSettingJson.hpp>
#include <ICubismModelSetting.hpp>
#include <Id/CubismIdManager.hpp>
#include <Model/CubismUserModel.hpp>
#include <Model/CubismModel.hpp>
#include <Motion/CubismMotion.hpp>
#include <Motion/CubismMotionManager.hpp>
#include <Motion/CubismExpressionMotionManager.hpp>
#include <Motion/ACubismMotion.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace Live2D::Cubism::Framework;
using Rendering::CubismRenderer_Software;
using Rendering::SoftTexture;

void L2DPlayer::CfgLoad()
{
    LlmCfgLoad();
    size_t n = 0;
    const CfgDef* d = CfgDefs(&n);

    for (size_t i = 0; i < n; ++i) _cfg[d[i].key] = d[i].def;
    FILE* f = fopen(CfgPath().c_str(), "rb");

    if (f != NULL)
    {
        std::string text;
        char buf[1024];
        size_t r;

        while ((r = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, r);
        fclose(f);
        jsonlite::Value v;

        if (jsonlite::parse(text.c_str(), text.size(), v, NULL) && v.isObj())
        {
            for (size_t i = 0; i < n; ++i)
            {
                const jsonlite::Value& x = v.get(d[i].key);
                if (x.isNum()) _cfg[d[i].key] = (csmFloat32)x.num;
                else if (x.isBool()) _cfg[d[i].key] = x.b ? 1.0f : 0.0f;
            }
            // 字符串配置（位置/地名之类，数值表放不下）：config.json 里的 "str" 段
            const jsonlite::Value& sv = v.get("str");
            if (sv.isObj())
            {
                for (size_t i = 0; i < sv.obj.size() && i < 16; ++i)
                {
                    const std::string k = sv.obj[i].first;
                    if (k.empty() || k.size() > 32 || !sv.obj[i].second.isStr()) continue;
                    std::string val = sv.obj[i].second.str;
                    if (val.size() > 200) continue;
                    _cfgStr[k] = val;
                }
            }
        }
    }
    _cfgLoaded = true;   // 从这儿起 config 回执里一定带 "str" 段（哪怕空对象）= "配置读好了"
    CfgApply();
}

std::string L2DPlayer::PanelStateJson(int agents)
{
    std::string o = "{\"model\":";

    JStr(o, BaseName(_dir));
    o += ",\"mood\":" + LifeMoodJson();
    o += ",\"life\":" + LifePanelJson();
    o += ",\"touches\":" + std::to_string(_lifeTouches) + ",\"session_touches\":" + std::to_string(_sessionTouches);
    o += ",\"uptime\":" + JNum(_perfClock) + ",\"agents\":" + std::to_string(agents);
    o += LlmStateJson(agents);
    o += ",\"playing\":" + std::string(_petSeqs.empty() ? "false" : "true");
    o += ",\"visible\":" + std::string(_port != NULL && _port->Hidden() ? "false" : "true");
    o += ",\"config\":" + CfgJson() + ",\"expressions\":[";
    for (size_t i = 0; i < _expressionNames.size(); ++i)
    {
        if (i) o += ",";
        JStr(o, _expressionNames[i]);
    }
    o += "],\"expression\":";
    JStr(o, _expressionIndex >= 0 && (size_t)_expressionIndex < _expressionNames.size()
                ? _expressionNames[(size_t)_expressionIndex] : std::string());
    o += ",\"actions\":[";
    bool first = true;
    for (size_t i = 0; i < _actions.size(); ++i)
    {
        if (!ActionReportable(_actions[i])) continue;
        if (!first) o += ",";
        first = false;
        o += "[";
        JStr(o, _actions[i].name);
        o += ",";
        JStr(o, _actions[i].desc.empty() ? _actions[i].name : _actions[i].desc);
        o += "]";
    }
    o += "],\"models\":[";
    for (size_t i = 0; i < _modelNames.size(); ++i)
    {
        if (i) o += ",";
        JStr(o, _modelNames[i]);
    }
    o += "],\"model_index\":" + std::to_string(_modelIndex);
    // 模型页要的解算信息：骨架等级 / 动作组 / 躯干通道（BODY，rigid=骨架驱动）/
    // 臂骨架是否建成（决定 16 区判定是否完整）/ profile 与能力缓存是否存在
    o += ",\"model_info\":{";
    o += "\"name\":";
    JStr(o, BaseName(_dir));
    o += ",\"fv\":";
    JStr(o, FingerprintText());
    o += ",\"dir\":";
    JStr(o, _dir);
    o += ",\"skeleton\":";
    JStr(o, SkeletonLevel());
    o += ",\"motions\":[";
    for (csmInt32 g = 0; g < MotionGroupCount(); ++g)
    {
        if (g) o += ",";
        o += "[";
        JStr(o, MotionGroupName(g));
        o += "," + std::to_string((int)MotionCount(g)) + "]";
    }
    o += "],\"body\":[";
    for (size_t i = 0; i < _channels.size(); ++i)
    {
        if (i) o += ",";
        o += "[";
        JStr(o, _channels[i].name);
        o += ",";
        o += (_channels[i].rigid || _channels[i].rig >= 0) ? "1" : "0";
        o += "]";
    }
    o += "],\"rig\":";
    o += _rigs.empty() ? "false" : "true";
    o += ",\"zone_mode\":";
    JStr(o, !_rigs.empty() ? "rig" : (_anchors.count("eyes") ? "bound" : "basic"));
    o += ",\"zone_count\":" + std::to_string(_rigs.empty() ? (_anchors.count("eyes") ? 14 : 2) : 16);
    o += ",\"profile\":";
    {
        const bool inModel = plat::IsFile(_dir + "/l2d.profile.json");
        const bool inStable = L2D_PROFILE_DIR[0] != '\0'
                              && plat::IsFile(std::string(L2D_PROFILE_DIR) + "/" + BaseName(_dir) + ".profile.json");
        o += (inModel || inStable) ? "true" : "false";
    }
    o += ",\"caps\":";
    o += plat::IsFile(CachePath()) ? "true" : "false";
    o += "}";
    o += ",\"llm\":" + LlmPublicJson() + ",\"llm_stats\":" + LlmStatsJson() + ",\"agent_info\":"
         + (_agentInfo.empty() ? std::string("null") : _agentInfo) + ",\"llm_log\":[";
    for (size_t i = _llmLog.size() > 60 ? _llmLog.size() - 60 : 0; i < _llmLog.size(); ++i)
    {
        if (o.back() != '[') o += ",";
        o += _llmLog[i];
    }
    o += "],\"cfg_path\":";
    JStr(o, CfgPath());
    o += ",\"life_path\":";
    JStr(o, LifePath());
    o += ",\"memory\":{\"text\":";
    JStr(o, _memoryText);
    o += ",\"at\":";
    JStr(o, _memoryAt);
    o += ",\"path\":";
    JStr(o, plat::HomeDir() + "/.config/l2dpet/memory.md");
    o += ",\"compressed\":";
    JStr(o, MemoryCompressedText());
    o += ",\"backup_default\":";
    {
        char day[16];
        time_t t = time(NULL);
        struct tm lt;
#ifdef _WIN32
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        strftime(day, sizeof(day), "%Y%m%d", &lt);
        JStr(o, plat::HomeDir() + "/l2dpet-backup-" + day + ".json");
    }
    o += "}";
    o += ",\"persona\":{\"text\":";
    JStr(o, _personaText);
    o += ",\"name\":";
    JStr(o, _personaName);
    o += ",\"by\":";
    JStr(o, _personaBy);
    o += ",\"nick\":";
    JStr(o, _personaNick);
    o += ",\"call\":";
    JStr(o, _personaCall);
    o += ",\"user_name\":";
    JStr(o, _personaUserName);
    o += "}";
    return o + "}";
}

/** 公开信息（不含密钥）：v2 全量（供应商列表带 key_set/key_tail）+ 顶部兼容字段（= 激活供应商） */
std::string L2DPlayer::LlmPublicJson() const
{
    const l2d::LlmProvider* cur = NULL;
    for (size_t i = 0; i < _llmProviders.size(); ++i)
    {
        if (_llmProviders[i].id == _llmActive) cur = &_llmProviders[i];
    }
    if (cur == NULL && !_llmProviders.empty()) cur = &_llmProviders.front();
    std::string o = "{\"key_set\":";
    o += (cur != NULL && !cur->apiKey.empty()) ? "true" : "false";
    o += ",\"key_tail\":";
    JStr(o, (cur != NULL && cur->apiKey.size() >= 8) ? cur->apiKey.substr(cur->apiKey.size() - 4) : std::string());
    o += ",\"model\":";
    JStr(o, cur != NULL ? cur->model : std::string());
    o += ",\"base_url\":";
    JStr(o, cur != NULL ? cur->baseUrl : std::string());
    o += ",\"provider\":";
    JStr(o, cur != NULL ? cur->name : std::string());
    o += ",\"active\":";
    JStr(o, _llmActive);
    o += ",\"providers\":[";
    for (size_t i = 0; i < _llmProviders.size(); ++i)
    {
        const l2d::LlmProvider& p = _llmProviders[i];
        o += i ? ",{" : "{";
        o += "\"id\":";
        JStr(o, p.id);
        o += ",\"name\":";
        JStr(o, p.name);
        o += ",\"base_url\":";
        JStr(o, p.baseUrl);
        o += ",\"model\":";
        JStr(o, p.model);
        o += ",\"key_set\":";
        o += p.apiKey.empty() ? "false" : "true";
        o += ",\"key_tail\":";
        JStr(o, p.apiKey.size() >= 8 ? p.apiKey.substr(p.apiKey.size() - 4) : std::string());
        o += ",\"models\":[";
        for (size_t k = 0; k < p.models.size(); ++k)
        {
            if (k) o += ",";
            JStr(o, p.models[k]);
        }
        o += "]}";
    }
    o += "],\"path\":";
    JStr(o, LlmCfgPath());
    return o + "}";
}
