/**
 * FileUtil.h — 文件/路径工具（读文件、目录扫描、路径切分、PPM 落盘）。
 *
 * 从 l2dtest.cpp 拆出（纯搬移，逻辑未改）。
 * 路径切分同时认 '/' 与 '\'：Windows 上 plat::Canonicalize 产出的是反斜杠路径。
 */
#pragma once

#include <CubismFramework.hpp>

#include <string>
#include <vector>

using namespace Live2D::Cubism::Framework;

std::string DirName(const std::string& path);
std::string BaseName(const std::string& path);
bool IsAbsPath(const std::string& p);
std::string RealPath(const char* path);
int ScanModelDir(const char* dir, std::vector<std::string>* out);
std::string FindModel3Under(const std::string& dir, int maxDepth);
bool CopyDirTree(const std::string& src, const std::string& dst);
csmByte* ReadFile(const char* path, csmSizeInt* outSize);
csmByte* ReadJsonFile(const char* path, csmSizeInt* outSize);
csmByte* ReadModel3(const char* path, csmSizeInt* outSize);
bool WritePpm(const char* path, const csmUint32* framebuffer, int width, int height);

