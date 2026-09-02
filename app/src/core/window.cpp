#include "window.h"
#include "../../resources/resource.h"
#include <dwmapi.h>
#include <imgui.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace dnd {

Window* Window::s_instance = nullptr;

Window::Window() {
    s_instance = this;
}

Window::Window(HINSTANCE hInstance, const wchar_t* title, int width, int height, int nCmdShow) {
    s_instance = this;
    Create(hInstance, title, width, height, nCmdShow);
}

Window::~Window() {
    Destroy();
    if (s_instance == this) s_instance = nullptr;
}

bool Window::Create(HINSTANCE hInstance, const wchar_t* title, int width, int height, int nCmdShow) {
    m_hInstance = hInstance;
    m_width = width;
    m_height = height;

    HICON appIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                      0, 0, LR_DEFAULTSIZE | LR_SHARED);
    HICON appIconSmall = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON),
                                           IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                           GetSystemMetrics(SM_CYSMICON), LR_SHARED);

    WNDCLASSEXW wc = {
        sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance,
        appIcon, nullptr, nullptr, nullptr, m_className.c_str(), appIconSmall
    };
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowW(
        wc.lpszClassName, title,
        WS_OVERLAPPEDWINDOW, 80, 60, width, height,
        nullptr, nullptr, hInstance, nullptr);

    if (!m_hwnd) return false;

    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));

    Show(nCmdShow);
    return true;
}

void Window::Show(int nCmdShow) {
    if (m_hwnd) {
        ShowWindow(m_hwnd, nCmdShow);
        UpdateWindow(m_hwnd);
    }
}

void Window::Destroy() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    if (m_hInstance) {
        UnregisterClassW(m_className.c_str(), m_hInstance);
        m_hInstance = nullptr;
    }
}

bool Window::ProcessMessages() {
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (msg.message == WM_QUIT) return false;
    }
    return true;
}

LRESULT WINAPI Window::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        if (s_instance) {
            s_instance->m_width = (int)LOWORD(lParam);
            s_instance->m_height = (int)HIWORD(lParam);
            if (s_instance->m_onResize) {
                s_instance->m_onResize(s_instance->m_width, s_instance->m_height);
            }
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

} // namespace dnd
