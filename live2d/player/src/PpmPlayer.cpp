/** PpmPlayer.cpp — 见 PpmPlayer.h（从 l2dtest.cpp 拆出，纯搬移） */
#include "PpmPlayer.h"

#include "Platform.h"
#include "Port.hpp"
#include "l2d_config.h"   // L2D_PORT_DEFAULT 等默认值
#include <CubismFramework.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace Live2D::Cubism::Framework;

//
// 用途：把**外部渲染器**产出的 PPM 帧序列直接放到真实屏幕上，
// 便于肉眼验收与对照——不加载模型、不初始化 Cubism 框架，只做"读帧 → 上屏"。
// 帧按不透明处理（A=255）：PPM 无 alpha，透明区域在产出侧已是黑底（与播放器落帧观感一致）。

/** 读一个十进制整数（P6 头解析用；跳过空白与 # 注释行；有界，无 scanf） */
static bool ReadPpmInt(FILE* fp, int* out)
{
    int c = EOF;
    for (;;)
    {
        c = fgetc(fp);
        if (c == EOF)
        {
            return false;
        }
        if (c == '#')
        {
            do
            {
                c = fgetc(fp);
            } while (c != EOF && c != '\n');
            continue;
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
        {
            break;
        }
    }
    int v = 0;
    int digits = 0;
    while (c >= '0' && c <= '9')
    {
        if (v > 100000)
        {
            return false; // 溢出/异常长数字
        }
        v = v * 10 + (c - '0');
        ++digits;
        c = fgetc(fp);
    }
    if (digits == 0)
    {
        return false;
    }
    *out = v;
    return true; // 结束符（空白/EOF）已被消费；256 色 P6 的数据紧接着开始
}

/** 读 P6 PPM → ARGB8888（不透明） */
static bool ReadPpmArgb(const char* path, std::vector<csmUint32>* out, int* outW, int* outH)
{
    FILE* fp = fopen(path, "rb");
    if (fp == NULL)
    {
        return false;
    }
    int w = 0, h = 0, maxv = 0;
    bool ok = true;
    if (fgetc(fp) != 'P' || fgetc(fp) != '6')
    {
        ok = false;
    }
    if (ok && (!ReadPpmInt(fp, &w) || !ReadPpmInt(fp, &h) || !ReadPpmInt(fp, &maxv)))
    {
        ok = false;
    }
    if (ok && (maxv != 255 || w <= 0 || h <= 0 || w > 4096 || h > 4096))
    {
        ok = false;
    }
    if (ok)
    {
        out->resize((size_t)w * (size_t)h);
        std::vector<csmUint8> row((size_t)w * 3);
        for (int y = 0; y < h; ++y)
        {
            if (fread(row.data(), 1, row.size(), fp) != row.size())
            {
                ok = false;
                break;
            }
            csmUint32* dst = out->data() + (size_t)y * (size_t)w;
            for (int x = 0; x < w; ++x)
            {
                dst[x] = 0xff000000u | ((csmUint32)row[x * 3] << 16)
                         | ((csmUint32)row[x * 3 + 1] << 8) | (csmUint32)row[x * 3 + 2];
            }
        }
    }
    fclose(fp);
    if (!ok)
    {
        return false;
    }
    *outW = w;
    *outH = h;
    return true;
}

/** 收集目录下 frame_*.ppm 并排序 */
static void CollectPpmFrames(const char* dir, std::vector<std::string>* out)
{
    std::vector<std::string> names;
    if (!plat::ListDir(dir, &names))
    {
        return;
    }
    for (size_t k = 0; k < names.size(); ++k)
    {
        const std::string& name = names[k];
        const size_t n = name.size();
        const bool isFrame = (n > 10) && (strncmp(name.c_str(), "frame_", 6) == 0)
                             && (strcmp(name.c_str() + (n - 4), ".ppm") == 0);
        if (isFrame)
        {
            std::string p = dir;
            p += "/";
            p += name;
            out->push_back(p);
        }
    }
    std::sort(out->begin(), out->end());
}

/** --play-ppm 模式：循环播放目录内的 PPM 帧（真实屏幕输出） */
int RunPpmPlayer(const char* dir, int fps, const char* portRequest, int width, int height)
{
    std::vector<std::string> frames;
    CollectPpmFrames(dir, &frames);
    if (frames.empty())
    {
        fprintf(stderr, "[ppm] 目录内没有 frame_*.ppm：%s\n", dir);
        return 1;
    }

    PortConfig cfg;
    cfg.width = width;
    cfg.height = height;
    cfg.maxFps = fps;
    cfg.device = NULL;
    IPort* port = CreatePort(portRequest != NULL ? portRequest : L2D_PORT_DEFAULT);
    if (port == NULL)
    {
        fprintf(stderr, "[ppm] 端口不可用\n");
        return 1;
    }
    if (!port->Initialize(cfg))
    {
        const bool busy = port->ScreenBusy();
        delete port;
        if (busy)
        {
            fprintf(stderr, "[ppm] 屏幕已被其它程序占用——让位（退出码 3）\n");
            return 3;
        }
        fprintf(stderr, "[ppm] 端口初始化失败\n");
        return 1;
    }

    printf("[ppm] port=%s 屏 %dx%d | %d 帧 @%dfps 循环播放（Ctrl-C 退出）\n",
           port->Name(), port->Width(), port->Height(), (int)frames.size(), fps);
    fflush(stdout);

    std::vector<csmUint32> buf;
    int fw = 0, fh = 0;
    size_t idx = 0;
    bool warned = false;
    while (!g_quit)
    {
        if (!ReadPpmArgb(frames[idx].c_str(), &buf, &fw, &fh))
        {
            if (!warned)
            {
                fprintf(stderr, "[ppm] 读帧失败：%s\n", frames[idx].c_str());
                warned = true;
            }
        }
        else
        {
            port->Present(buf.data(), fw, fh);
        }
        port->WaitFrame();
        if (port->ShouldQuit())
        {
            break;
        }
        idx = (idx + 1) % frames.size();
    }
    delete port;
    printf("[ppm] 播放结束%s\n", g_quit ? "（收到退出信号）" : "");
    return 0;
}
