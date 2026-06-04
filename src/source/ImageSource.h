#pragma once
#include <string>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

// Loads image files via WIC and uploads to a BGRA D3D11 texture.
class ImageSource {
public:
    ImageSource() = default;

    bool Load(const std::string& path,
              ID3D11Device* device,
              ID3D11DeviceContext* ctx);

    ID3D11Texture2D* GetTexture() const { return m_texture.Get(); }
    int GetWidth()  const { return m_width; }
    int GetHeight() const { return m_height; }

private:
    ComPtr<ID3D11Texture2D> m_texture;
    int m_width{0}, m_height{0};
};
