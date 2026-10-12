#include "DesktopStartup.h"
#include "../player/src/Platform.h"
#include <cstdlib>
#include <sstream>
#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
static std::wstring StartupWide(const std::string& text)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &result[0], size);
    result.resize(size - 1);
    return result;
}
#endif
namespace desktop {
// ---------------------------------------------------------------- 开机自启（关于页的开关）
/** 自启要指向**启动器**，不能指向便携包解出来的那份：解包目录名带 payload hash，
 *  换个版本就换目录，指过去就是一条死链。便携启动器会设 L2DPET_LAUNCHER=自身路径；
 *  文件夹版/构建版没有这个变量 → 用面板同目录的桌宠 exe（双击它就会把面板一起拉起来）。*/
static std::string AutostartTarget()
{
    if (const char* launcher = getenv("L2DPET_LAUNCHER"))
    {
        if (*launcher) return launcher;
    }
    const std::string self = plat::SelfExePath();
    const size_t slash = self.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? std::string() : self.substr(0, slash + 1);
#if defined(_WIN32)
    return dir + "l2dtest.exe";
#else
    return dir + "l2dtest";
#endif
}

#if defined(_WIN32)
// 使用用户 Startup 快捷方式，明确工作目录；保留旧 Run 项读取以便迁移。
static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static std::wstring StartupShortcut()
{
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Startup, 0, nullptr, &folder))) return {};
    std::wstring path = std::wstring(folder) + L"\\l2dpet.lnk";
    CoTaskMemFree(folder);
    return path;
}
static bool LegacyRunEnabled()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
    const LONG result = RegQueryValueExW(key, L"l2dpet", nullptr, nullptr, nullptr, nullptr);
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}
bool AutostartEnabled()
{
    const std::wstring path = StartupShortcut();
    return (!path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) || LegacyRunEnabled();
}
bool SetAutostart(bool on)
{
    const std::wstring path = StartupShortcut();
    if (path.empty()) return false;
    if (on)
    {
        const std::wstring target = StartupWide(AutostartTarget());
        if (target.empty() || GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(init) && init != RPC_E_CHANGED_MODE) return false;
        IShellLinkW* link = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_IShellLinkW, (void**)&link);
        if (SUCCEEDED(hr))
        {
            hr = link->SetPath(target.c_str());
            const size_t slash = target.find_last_of(L"/\\");
            if (SUCCEEDED(hr) && slash != std::wstring::npos)
                hr = link->SetWorkingDirectory(target.substr(0, slash).c_str());
            if (SUCCEEDED(hr)) hr = link->SetDescription(L"Live2D desktop pet");
            if (SUCCEEDED(hr)) hr = link->SetShowCmd(SW_SHOWNORMAL);
            IPersistFile* file = nullptr;
            if (SUCCEEDED(hr)) hr = link->QueryInterface(IID_IPersistFile, (void**)&file);
            if (SUCCEEDED(hr)) { hr = file->Save(path.c_str(), TRUE); file->Release(); }
            link->Release();
        }
        if (SUCCEEDED(init)) CoUninitialize();
        if (FAILED(hr)) return false;
    }
    else if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) return false;
    // 只有快捷方式写入成功后才移除旧入口，避免重复启动。
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS)
    {
        const LONG result = RegDeleteValueW(key, L"l2dpet");
        RegCloseKey(key);
        if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) return false;
    }
    return AutostartEnabled() == on;
}

#else
/** Linux：XDG 自启项。打包里的启动器写的也是这个文件（l2dpet autostart on/off），
 *  两边互不冲突——谁写的都是同一个自启入口，off 都是删掉它。 */
std::string AutostartDesktopPath()
{
    const char* xdg = getenv("XDG_CONFIG_HOME");
    const std::string base = xdg && xdg[0] == '/' ? xdg : plat::HomeDir() + "/.config";
    return base + "/autostart/l2dpet.desktop";
}

bool AutostartEnabled()
{
    const std::string entry = plat::ReadTextFile(AutostartDesktopPath());
    std::istringstream lines(entry);
    std::string line;
    bool desktop = false, application = false, exec = false, hidden = false;
    while (std::getline(lines, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line.front() == '[') { desktop = line == "[Desktop Entry]"; continue; }
        if (!desktop) continue;
        if (line == "Type=Application") application = true;
        if (line.rfind("Exec=", 0) == 0 && line.size() > 5) exec = true;
        if (line == "Hidden=true" || line == "X-GNOME-Autostart-enabled=false") hidden = true;
    }
    return application && exec && !hidden;
}

/** Exec 的写法跟启动器一致：/usr/bin/env "<路径>"。
 *  为什么套 env：GLib 展开 %% 之前要求第一个词是可执行文件，用 env 兜住路径里的转义；
 *  路径里的 % 会被当字段码 → 必须写成 %%，引号/反引号/$ 也要转义。 */
static std::string AutostartExec(const std::string& path)
{
    std::string quoted = "\"";
    for (size_t i = 0; i < path.size(); ++i)
    {
        const char c = path[i];
        if (c == '%') quoted += "%%";
        else if (c == '\\') quoted += "\\\\\\\\";
        else if (c == '"' || c == '$' || c == '`') { quoted += "\\\\"; quoted += c; }
        else if (c == '\n') quoted += "\\n";
        else if (c == '\t') quoted += "\\t";
        else quoted += c;
    }
    quoted += "\"";
    return "/usr/bin/env " + quoted;
}

bool SetAutostart(bool on)
{
    const std::string path = AutostartDesktopPath();
    if (!on)
    {
        plat::RemoveFile(path);
        return !plat::Exists(path);
    }
    const std::string target = AutostartTarget();
    if (target.empty()) return false;
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && !plat::CreateDir(path.substr(0, slash), true)) return false;
    std::string entry;
    entry += "[Desktop Entry]\n";
    entry += "Type=Application\n";
    entry += "Name=Live2D Pet\n";
    entry += "Name[zh_CN]=Live2D 桌宠\n";
    entry += "Comment=Start the desktop pet at login\n";
    entry += "Comment[zh_CN]=登录后自动启动桌宠\n";
    entry += "Exec=" + AutostartExec(target) + "\n";
    entry += "Terminal=false\n";
    entry += "X-GNOME-Autostart-enabled=true\n";
    return plat::WritePrivateFile(path, entry) && plat::Exists(path);
}
#endif

} // namespace desktop
