#pragma once

#include <string>
#include <vector>

#include "core/window/window_backend.h"

namespace core::platform {

struct TrayOptions {
    std::string tooltip;
    std::string iconPath;
};

// l2dpet 补丁：应用自定义托盘菜单（可嵌套一层子菜单；separator=true 时其余字段忽略）
struct TrayMenuItem {
    int id = 0;
    std::string label;
    bool checked = false;
    bool enabled = true;
    bool separator = false;
    bool radio = false;
    bool checkable = false;   // 勾选项（没勾上也显示成可勾选）
    std::vector<TrayMenuItem> children;
};

struct FileDialogOptions {
    std::string prompt;
    std::vector<std::string> allowedExtensions;
    std::string initialDirectory;
    std::string filterName;
    bool allowMultiple = false;
};

enum class FileDialogStatus {
    Selected,
    Cancelled,
    Failed
};

struct FileDialogResult {
    FileDialogStatus status = FileDialogStatus::Cancelled;
    std::vector<std::string> paths;
    std::string error;

    bool selected() const {
        return status == FileDialogStatus::Selected && !paths.empty();
    }
};

bool repairCurrentWorkingDirectory();
std::string resolveResourcePath(const std::string& path);
bool openUrl(const std::string& url);
FileDialogResult openFileDialog(const FileDialogOptions& options = {});
std::string chooseFile(const FileDialogOptions& options = {});
std::vector<std::string> chooseFiles(const FileDialogOptions& options = {});
bool initializeTray(const TrayOptions& options);
bool isTrayInitialized();
void pollTray(bool blocking = false);
bool consumeTrayShowRequested();
bool consumeTrayExitRequested();
void shutdownTray();
void setTrayMenu(const std::vector<TrayMenuItem>& items);   // 空 = 恢复默认 Show / Exit
bool consumeTrayMenuClick(int* id);                         // 取一个被点的菜单项 id
void setImeCursorRect(window::Handle window, float x, float y, float width, float height);
void requestFrame();
void requestUiUpdate();
bool consumeUiUpdate();
bool consumeFrameRequest();

} // namespace core::platform
