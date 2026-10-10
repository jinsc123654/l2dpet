// i18n.h — 面板多语言（中文 / English）
//
// 做法：**以中文原文为键**查表（表在 i18n.cpp），源码里的字面量保持中文不动——
//   面板每帧重建 UI，取词时翻译；查不到就原样返回（数据类文本如模型名、路径、
//   桌宠自己生成的动态/台词本来就不在表里，会自然透传）。
// 两种键：
//   1) 整句：T("行为") → "Behavior"；
//   2) 模板：TF("共 {} 条", {n}) —— 键里带 {} 占位符（顺序替换），
//      拼接句（计数、时长、名字）用这个，保证英文语序正确；
//      译文里的"碎片"（如 " 分钟前" → " min ago"）也在表里，按碎片拼。
//
// 语言值存桌宠配置 config.json 的 "str.lang"（zh / en / auto），面板没有自己的配置文件。
// 边界：面板自己的界面（导航/页面/菜单/托盘/toast）全量双语；桌宠进程自己生成的动态与
//   台词库是"内容"，不在本表（LLM 台词会按 lang 用英语写，见 PetAgent）。
#pragma once

#include <initializer_list>
#include <string>

namespace app {
namespace i18n {

enum class Lang { Zh, En, Ja };

/** 当前语言（默认 Zh） */
Lang Get();

/** 切换语言（会清掉"未命中"记录） */
void Set(Lang lang);

/** 配置里的取值：zh / en / ja / auto（auto = 按系统 locale 猜）；空/无效 → 默认（zh） */
Lang FromCode(const std::string& code);

/** 系统 locale 猜语言：LANG/LC_ALL/LC_MESSAGES 带 zh → 中文、带 ja → 日语，否则英文；都没有 → 中文 */
Lang Detect();

/** 当前语言写回配置用的代码（zh / en / ja） */
const char* Code();

/** 翻译（模板里的 {} 按 args 顺序替换）；未命中返回模板原文 */
std::string TF(const std::string& zh, std::initializer_list<std::string> args);

/** 无参数整句翻译 */
inline std::string T(const std::string& zh) { return TF(zh, {}); }

/** 测试用：本进程里出现过的、没命中表的原文（去重；空 = 全命中） */
const std::string& MissingReport();

} // namespace i18n
} // namespace app
