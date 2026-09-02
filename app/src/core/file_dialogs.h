#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

namespace dnd {

class FileDialogs {
public:
    static void OpenFolderInExplorer(const std::string& path);
    static void OpenFileInDungeondraft(const std::string& appPath, const std::string& mapPath);

    static std::string PickFolderDialog(const std::string& title = "Select Folder", HWND hwnd = nullptr);
    static std::string PickExecutableFile(const std::string& title = "Select Executable", HWND hwnd = nullptr);
    static std::string PickDungeondraftSaveFile(const std::string& defaultPath = "", HWND hwnd = nullptr);
    static std::string PickMapFile(HWND hwnd = nullptr);
    static std::string PickSaveMapFileDialog(const std::string& defaultPath = "", HWND hwnd = nullptr);
};

} // namespace dnd
