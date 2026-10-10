/**
 * ModelImport.h — 模型导入（面板「模型」页把 .zip / 目录搬进模型库）。
 *
 * 从 l2dtest.cpp 拆出（纯搬移，逻辑未改）。目录名优先取包内 <名>.model3.json 的 <名>。
 */
#pragma once

#include <string>

void RemoveDirTree(const std::string& dir);
bool ImportModelInto(const std::string& srcPath, const std::string& root, std::string* outName, std::string* err);

