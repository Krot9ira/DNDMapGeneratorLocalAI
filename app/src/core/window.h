#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <functional>
#include <string>

namespace dnd {

class Window {
public:
    using ResizeCallback = std::function<void(int width, int height)>;

    Window();
    Window(HINSTANCE hInstance, const wchar_t* title = L"D&D Battle Map Generator", int width = 1560, int height = 960, int nCmdShow = SW_SHOWDEFAULT);
    ~Window();

    bool Create(HINSTANCE hInstance, const wchar_t* title, int width, int height, int nCmdShow = SW_SHOWDEFAULT);
    void Show(int nCmdShow = SW_SHOWDEFAULT);
    void Destroy();

    bool ProcessMessages();

    HWND GetHwnd() const { return m_hwnd; }
    HINSTANCE GetHInstance() const { return m_hInstance; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

    void SetResizeCallback(ResizeCallback cb) { m_onResize = std::move(cb); }

    static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

private:
    HWND m_hwnd = nullptr;
    HINSTANCE m_hInstance = nullptr;
    std::wstring m_className = L"DndBattleMapGenClass";
    int m_width = 1560;
    int m_height = 960;
    ResizeCallback m_onResize;

    static Window* s_instance;
};

} // namespace dnd
