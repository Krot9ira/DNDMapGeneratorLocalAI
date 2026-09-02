#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <vector>
#include <string>
#include <unordered_map>
#include <cstdint>

namespace dnd {

class DX11Renderer;

class TextureLoader {
public:
    TextureLoader(DX11Renderer* renderer = nullptr);
    TextureLoader(ID3D11Device* device, ID3D11DeviceContext* context);
    ~TextureLoader();

    void Initialize(DX11Renderer* renderer);
    void Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    void Shutdown();

    ID3D11ShaderResourceView* CreateTextureRGBA(const unsigned char* pixels, int width, int height);
    ID3D11ShaderResourceView* CreateTextureFromMemory(const void* data, size_t size, int* outW = nullptr, int* outH = nullptr);

    void SetResultImage(const std::vector<uint8_t>& png);
    void ClearResultImage();

    ID3D11ShaderResourceView* GetResultTexture() const { return m_resultTex; }
    int GetResultWidth() const { return m_resultW; }
    int GetResultHeight() const { return m_resultH; }
    const std::vector<uint8_t>& GetResultPng() const { return m_resultPng; }

    ID3D11ShaderResourceView* GetOrCreateThumbnail(const std::string& imagePath);
    void ClearThumbnails();

private:
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    ID3D11ShaderResourceView* m_resultTex = nullptr;
    int m_resultW = 0;
    int m_resultH = 0;
    std::vector<uint8_t> m_resultPng;

    std::unordered_map<std::string, ID3D11ShaderResourceView*> m_thumbnails;
};

} // namespace dnd
