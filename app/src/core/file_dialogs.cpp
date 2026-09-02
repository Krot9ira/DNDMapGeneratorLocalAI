#include "file_dialogs.h"

#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <algorithm>

namespace dnd {

void FileDialogs::OpenFolderInExplorer(const std::string& path) {
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), n);
    ShellExecuteW(nullptr, L"open", wpath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void FileDialogs::OpenFileInDungeondraft(const std::string& appPath, const std::string& mapPath) {
    int na = MultiByteToWideChar(CP_UTF8, 0, appPath.c_str(), -1, nullptr, 0);
    std::wstring wapp(na, 0);
    MultiByteToWideChar(CP_UTF8, 0, appPath.c_str(), -1, wapp.data(), na);

    int nm = MultiByteToWideChar(CP_UTF8, 0, mapPath.c_str(), -1, nullptr, 0);
    std::wstring wmap(nm, 0);
    MultiByteToWideChar(CP_UTF8, 0, mapPath.c_str(), -1, wmap.data(), nm);

    ShellExecuteW(nullptr, L"open", wapp.c_str(), wmap.c_str(), nullptr, SW_SHOWNORMAL);
}

std::string FileDialogs::PickFolderDialog(const std::string& title, HWND hwnd) {
    wchar_t path[MAX_PATH] = L"";
    BROWSEINFOW bi{};
    bi.hwndOwner = hwnd;
    int nTitle = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
    std::wstring wTitle(nTitle, 0);
    MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, wTitle.data(), nTitle);
    bi.lpszTitle = wTitle.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        SHGetPathFromIDListW(pidl, path);
        CoTaskMemFree(pidl);
        int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
        std::string out((size_t)std::max(0, n - 1), 0);
        WideCharToMultiByte(CP_UTF8, 0, path, -1, out.data(), n, nullptr, nullptr);
        return out;
    }
    return {};
}

std::string FileDialogs::PickExecutableFile(const std::string& title, HWND hwnd) {
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Executables (*.exe)\0*.exe\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    int nTitle = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
    std::wstring wTitle(nTitle, 0);
    MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, wTitle.data(), nTitle);
    ofn.lpstrTitle = wTitle.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)std::max(0, n - 1), 0);
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::string FileDialogs::PickDungeondraftSaveFile(const std::string& defaultPath, HWND hwnd) {
    wchar_t buf[MAX_PATH] = L"";
    if (!defaultPath.empty()) {
        MultiByteToWideChar(CP_UTF8, 0, defaultPath.c_str(), -1, buf, MAX_PATH);
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Dungeondraft Map (*.dungeondraft_map)\0*.dungeondraft_map\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"dungeondraft_map";
    ofn.lpstrTitle = L"Save Dungeondraft Map As";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)std::max(0, n - 1), 0);
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::string FileDialogs::PickMapFile(HWND hwnd) {
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Map plans (map.json)\0*.json\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Open a map.json";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)std::max(0, n - 1), 0);
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::string FileDialogs::PickSaveMapFileDialog(const std::string& defaultPath, HWND hwnd) {
    wchar_t buf[MAX_PATH] = L"";
    if (!defaultPath.empty()) {
        MultiByteToWideChar(CP_UTF8, 0, defaultPath.c_str(), -1, buf, MAX_PATH);
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Map Files (*.json)\0*.json\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"json";
    ofn.lpstrTitle = L"Save Map Plan As";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)std::max(0, n - 1), 0);
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), n, nullptr, nullptr);
    return out;
}

} // namespace dnd
