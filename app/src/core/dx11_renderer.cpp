#include "dx11_renderer.h"
#include <imgui.h>
#include <backends/imgui_impl_win32.h>
#include <backends/imgui_impl_dx11.h>

namespace dnd {

DX11Renderer::DX11Renderer() = default;

DX11Renderer::~DX11Renderer() {
    Shutdown();
}

bool DX11Renderer::Init(HWND hwnd, int width, int height) {
    m_hwnd = hwnd;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = width;
    sd.BufferDesc.Height = height;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 2, D3D11_SDK_VERSION, &sd, &m_swapChain,
        &m_device, &level, &m_context);

    if (hr == DXGI_ERROR_UNSUPPORTED) {
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION, &sd, &m_swapChain,
            &m_device, &level, &m_context);
    }

    if (FAILED(hr)) return false;

    CreateRenderTarget();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "imgui.ini";

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(m_device, m_context);

    return true;
}

void DX11Renderer::Shutdown() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    if (ImGui::GetCurrentContext()) {
        ImGui::DestroyContext();
    }

    CleanupRenderTarget();
    if (m_swapChain) { m_swapChain->Release(); m_swapChain = nullptr; }
    if (m_context) { m_context->Release(); m_context = nullptr; }
    if (m_device) { m_device->Release(); m_device = nullptr; }
}

void DX11Renderer::Resize(int width, int height) {
    if (!m_swapChain || width <= 0 || height <= 0) return;
    CleanupRenderTarget();
    m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    CreateRenderTarget();
}

void DX11Renderer::BeginFrame(const float clearColor[4]) {
    if (clearColor) {
        for (int i = 0; i < 4; ++i) m_defaultClearColor[i] = clearColor[i];
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void DX11Renderer::EndFrame() {
    ImGui::Render();
    if (m_context && m_rtv) {
        m_context->OMSetRenderTargets(1, &m_rtv, nullptr);
        m_context->ClearRenderTargetView(m_rtv, m_defaultClearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    if (m_swapChain) {
        m_swapChain->Present(1, 0);
    }
}

ID3D11ShaderResourceView* DX11Renderer::CreateTextureRGBA(const uint8_t* pixels, int w, int h) {
    if (!pixels || w <= 0 || h <= 0 || !m_device) return nullptr;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = pixels;
    init.SysMemPitch = w * 4;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(m_device->CreateTexture2D(&desc, &init, &tex)) || !tex) return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = desc.Format;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;

    ID3D11ShaderResourceView* view = nullptr;
    m_device->CreateShaderResourceView(tex, &srv, &view);
    tex->Release();
    return view;
}

void DX11Renderer::CreateRenderTarget() {
    if (!m_swapChain || !m_device) return;
    ID3D11Texture2D* backBuffer = nullptr;
    m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (backBuffer) {
        m_device->CreateRenderTargetView(backBuffer, nullptr, &m_rtv);
        backBuffer->Release();
    }
}

void DX11Renderer::CleanupRenderTarget() {
    if (m_rtv) {
        m_rtv->Release();
        m_rtv = nullptr;
    }
}

} // namespace dnd
