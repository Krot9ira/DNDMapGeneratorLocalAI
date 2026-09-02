#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdint>

namespace dnd {

class DX11Renderer {
public:
    DX11Renderer();
    ~DX11Renderer();

    bool Init(HWND hwnd, int width = 0, int height = 0);
    bool Initialize(HWND hwnd, int width = 0, int height = 0) { return Init(hwnd, width, height); }
    void Shutdown();

    void Resize(int width, int height);
    void BeginFrame(const float clearColor[4] = nullptr);
    void EndFrame();

    ID3D11ShaderResourceView* CreateTextureRGBA(const uint8_t* pixels, int w, int h);

    ID3D11Device* GetDevice() const { return m_device; }
    ID3D11DeviceContext* GetContext() const { return m_context; }
    IDXGISwapChain* GetSwapChain() const { return m_swapChain; }
    ID3D11RenderTargetView* GetRTV() const { return m_rtv; }

private:
    void CreateRenderTarget();
    void CleanupRenderTarget();

    HWND m_hwnd = nullptr;
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    IDXGISwapChain* m_swapChain = nullptr;
    ID3D11RenderTargetView* m_rtv = nullptr;
    float m_defaultClearColor[4] = {0.07f, 0.08f, 0.10f, 1.0f};
};

} // namespace dnd
