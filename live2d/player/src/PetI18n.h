// PetI18n.h — 桌宠进程自己的 i18n（气泡台词 / 最近动态 / 记忆页里"自带"的文案）
//
// 为什么要单独一份：面板那份（panel/i18n.*）只管面板自己的界面；而**会出现在气泡、
// 最近动态、记忆页正文里的中文**是桌宠进程生成的——一部分是 LLM 出力（跟 config.str.lang 走，
// 见 PetAgent 的 SpeechLangHint + 切语言重写），另一部分就是这份表里的"自带兜底文案"
// （没配 Key / 台词库没下发时她说的话，以及"摸了头""累了"这类动态描述）。
//
// 用法与面板一致：**以中文原文为键**查表，查不到原样返回（数据类文本自然透传）；
// 语言来自 config.str.lang（zh/en/ja/auto），由 CfgApply 设置。
#pragma once

#include <initializer_list>
#include <string>

namespace peti18n {

enum class Lang { Zh, En, Ja };

/** 设置语言（zh / en / ja / auto；auto 按系统 locale 猜，与面板同口径） */
void SetLangCode(const std::string& code);
Lang Get();
/** 语言代码（zh / en / ja）与中文名（提示词用） */
const char* Code();
const char* LangCn();

/** 查表翻译（未命中返回原文，并记账；模板里的 {} 按 args 顺序替换） */
std::string TF(const std::string& zh, std::initializer_list<std::string> args);
inline std::string Tr(const std::string& zh) { return TF(zh, {}); }

/** 测试用：本进程里出现过但没命中表的原文（去重；空 = 全命中） */
const std::string& MissingReport();

} // namespace peti18n
