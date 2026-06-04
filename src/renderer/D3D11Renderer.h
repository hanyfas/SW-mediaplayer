#pragma once
#include <Windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <array>

using Microsoft::WRL::ComPtr;

// Decoded frame ready to display. Lifetime managed by the playback engine.
struct VideoFrame {
    ComPtr<ID3D11Texture2D> texture;   // NV12 or BGRA
    UINT                    arraySlice{0}; // D3D11VA surfaces are texture arrays
    bool                    isNV12{true};
    int                     width{0};
    int                     height{0};
};

// One display slot (current or next item).
struct DisplaySlot {
    std::mutex              lock;
    VideoFrame              frame;
    float                   alpha{1.f};    // 0=transparent, 1=opaque
    bool                    hasFrame{false};
};

class D3D11Renderer {
public:
    D3D11Renderer() = default;
    ~D3D11Renderer();

    bool Init(HWND hwnd, UINT width, UINT height);
    void Resize(UINT width, UINT height);
    void Shutdown();

    // Called from the render loop every vsync.
    void Present();

    // Upload a CPU YUV420P frame (software decode fallback).
    bool UploadYUV420(DisplaySlot& slot,
                      const uint8_t* y, int yStride,
                      const uint8_t* u, int uStride,
                      const uint8_t* v, int vStride,
                      int w, int h);

    // Wrap an existing D3D11VA hardware texture.
    bool WrapHWTexture(DisplaySlot& slot,
                       ID3D11Texture2D* tex, UINT arraySlice,
                       int w, int h);

    // Expose device for the decoder to create a shared context.
    ID3D11Device*        GetDevice()  const { return m_device.Get(); }
    ID3D11DeviceContext* GetContext() const { return m_ctx.Get(); }

    // Transition blend factor written by PlaybackEngine.
    std::atomic<float>   slotAlpha{1.f};  // alpha of slot[0]; slot[1] = 1-alpha

    std::array<DisplaySlot, 2> slots;

private:
    bool CreateSwapChain(HWND hwnd, UINT w, UINT h);
    bool CreateRenderTarget();
    bool CompileShaders();
    bool CreateSamplers();
    bool CreateUploadTextures(UINT w, UINT h);

    void RenderSlot(const DisplaySlot& slot, float alpha);
    void SetupFullscreenQuad();

    ComPtr<ID3D11Device>            m_device;
    ComPtr<ID3D11DeviceContext>     m_ctx;
    ComPtr<IDXGISwapChain2>         m_swapChain;
    HANDLE                          m_frameLatencyWaitable{nullptr};
    ComPtr<ID3D11RenderTargetView>  m_rtv;

    // Shaders
    ComPtr<ID3D11VertexShader>      m_vs;
    ComPtr<ID3D11PixelShader>       m_psNV12;   // hw decode (NV12)
    ComPtr<ID3D11PixelShader>       m_psBGRA;   // image / BGRA path
    ComPtr<ID3D11InputLayout>       m_inputLayout;
    ComPtr<ID3D11Buffer>            m_quadVB;
    ComPtr<ID3D11Buffer>            m_quadIB;
    ComPtr<ID3D11Buffer>            m_cbTransform; // per-draw constants

    ComPtr<ID3D11SamplerState>      m_samplerLinear;
    ComPtr<ID3D11BlendState>        m_blendState;

    // Staging textures for CPU->GPU upload (software decode path)
    ComPtr<ID3D11Texture2D>         m_stagingNV12;
    ComPtr<ID3D11Texture2D>         m_gpuNV12;
    ComPtr<ID3D11ShaderResourceView> m_srvY;
    ComPtr<ID3D11ShaderResourceView> m_srvUV;

    UINT m_width{0};
    UINT m_height{0};
    bool m_initialized{false};
};
