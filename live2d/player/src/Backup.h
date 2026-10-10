/**
 * Backup.h — 备份导出 / 导入（记忆 + 性格 + 题库 + 笑话 + 配置打成一个 JSON）。
 *
 * 从 l2dtest.cpp 拆出（纯搬移，逻辑未改）。
 */
#pragma once

#include <string>

bool BackupBuild(const std::string& root, std::string* out, int* files, std::string* err);
bool BackupRestore(const std::string& path, const std::string& root, int* files, std::string* err);

