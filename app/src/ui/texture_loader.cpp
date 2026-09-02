#include "texture_loader.h"
#include "../core/dx11_renderer.h"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <fstream>

namespace dnd {

TextureLoader::TextureLoader(DX11Renderer* renderer) {
    if (renderer) Initialize(renderer);
}

TextureLoader::TextureLoader(ID3D11Device* device, ID3D11DeviceContext* context)
    : m_device(device), m_context(context) {}

TextureLoader::~TextureLoader() {
    Shutdown();
}

void TextureLoader::Initialize(DX11Renderer* renderer) {
    if (renderer) {
        m_device = renderer->GetDevice();
        m_context = renderer->GetContext();
    }
}

void TextureLoader::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    m_device = device;
    m_context = context;
}

void TextureLoader::Shutdown() {
    ClearResultImage();
    ClearThumbnails();
}

ID3D11ShaderResourceView* TextureLoader::CreateTextureRGBA(const unsigned char* pixels, int width, int height) {
    if (!pixels || width <= 0 || height <= 0 || !m_device) return nullptr;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = pixels;
    init.SysMemPitch = width * 4;

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

ID3D11ShaderResourceView* TextureLoader::CreateTextureFromMemory(const void* data, size_t size, int* outW, int* outH) {
    if (!data || size == 0 || !m_device) return nullptr;
    int w = 0, h = 0, comp = 0;
    unsigned char* pixels = stbi_load_from_memory((const unsigned char*)data, (int)size, &w, &h, &comp, 4);
    if (!pixels) return nullptr;

    ID3D11ShaderResourceView* srv = CreateTextureRGBA(pixels, w, h);
    stbi_image_free(pixels);
    if (outW) *outW = w;
    if (outH) *outH = h;
    return srv;
}

void TextureLoader::SetResultImage(const std::vector<uint8_t>& png) {
    ClearResultImage();
    if (png.empty()) return;
    m_resultPng = png;
    m_resultTex = CreateTextureFromMemory(png.data(), png.size(), &m_resultW, &m_resultH);
}

void TextureLoader::ClearResultImage() {
    if (m_resultTex) {
        m_resultTex->Release();
        m_resultTex = nullptr;
    }
    m_resultW = 0;
    m_resultH = 0;
    m_resultPng.clear();
}

ID3D11ShaderResourceView* TextureLoader::GetOrCreateThumbnail(const std::string& imagePath) {
    auto it = m_thumbnails.find(imagePath);
    if (it != m_thumbnails.end()) return it->second;

    std::ifstream file(imagePath, std::ios::binary);
    if (!file.is_open()) return nullptr;

    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    int w = 0, h = 0;
    ID3D11ShaderResourceView* srv = CreateTextureFromMemory(buffer.data(), buffer.size(), &w, &h);
    if (srv) {
        m_thumbnails[imagePath] = srv;
    }
    return srv;
}

void TextureLoader::ClearThumbnails() {
    for (auto& pair : m_thumbnails) {
        if (pair.second) pair.second->Release();
    }
    m_thumbnails.clear();
}

} // namespace dnd
