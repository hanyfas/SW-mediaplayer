#include "D3D11Renderer.h"
#include <dxgi1_6.h>
#include <stdexcept>
#include <string>
#include <vector>
#include <cassert>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

// ─────────────────────────────────────────────────────────────────────────────
// Vertex layout: full-screen quad, pos+uv in [-1,1] clip space
// ─────────────────────────────────────────────────────────────────────────────
struct QuadVertex { float x, y, z, u, v; };

static constexpr QuadVertex kQuadVerts[] = {
    {-1.f,  1.f, 0.f,  0.f, 0.f},
    { 1.f,  1.f, 0.f,  1.f, 0.f},
    { 1.f, -1.f, 0.f,  1.f, 1.f},
    {-1.f, -1.f, 0.f,  0.f, 1.f},
};
static constexpr UINT16 kQuadIdx[] = {0,1,2, 0,2,3};

// Per-draw constant buffer
struct alignas(16) CBDraw {
    float alpha;
    float pad[3];
};

// ─────────────────────────────────────────────────────────────────────────────
// HLSL source embedded so we don't depend on shader files at runtime
// ─────────────────────────────────────────────────────────────────────────────
static const char* kVS = R"hlsl(
struct VSIn  { float3 pos : POSITION; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(VSIn v) {
    VSOut o;
    o.pos = float4(v.pos, 1);
    o.uv  = v.uv;
    return o;
}
)hlsl";

// NV12: separate Y plane (R8) + interleaved UV plane (R8G8)
static const char* kPS_NV12 = R"hlsl(
Texture2D<float>  texY  : register(t0);
Texture2D<float2> texUV : register(t1);
SamplerState      samp  : register(s0);

cbuffer CB : register(b0) { float alpha; float3 pad; }

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float  y  = texY.Sample(samp, uv);
    float2 uv2 = texUV.Sample(samp, uv);
    float  cb = uv2.x - 0.5;
    float  cr = uv2.y - 0.5;

    // BT.709 limited range
    y  = (y  - 16.0/255.0) * (255.0/219.0);
    float r = saturate(y + 1.5748 * cr);
    float g = saturate(y - 0.1873 * cb - 0.4681 * cr);
    float b = saturate(y + 1.8556 * cb);
    return float4(r, g, b, alpha);
}
)hlsl";

// BGRA path for images / software decoded BGRA frames
static const char* kPS_BGRA = R"hlsl(
Texture2D<float4> tex  : register(t0);
SamplerState      samp : register(s0);
cbuffer CB : register(b0) { float alpha; float3 pad; }
float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float4 c = tex.Sample(samp, uv);
    c.a *= alpha;
    return c;
}
)hlsl";

// ─────────────────────────────────────────────────────────────────────────────
static ComPtr<ID3DBlob> CompileHLSL(const char* src, const char* entry,
                                     const char* target)
{
    ComPtr<ID3DBlob> code, err;
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#ifdef _DEBUG
    flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                             entry, target, flags, 0, &code, &err);
    if (FAILED(hr)) {
        std::string msg = err ? (char*)err->GetBufferPointer() : "unknown";
        throw std::runtime_error("Shader compile failed: " + msg);
    }
    return code;
}

// ─────────────────────────────────────────────────────────────────────────────
bool D3D11Renderer::Init(HWND hwnd, UINT width, UINT height)
{
    m_width  = width;
    m_height = height;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL featureLevels[] = { D3D_FEATURE_LEVEL_11_1,
                                           D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE,
                                    nullptr, flags,
                                    featureLevels, 2,
                                    D3D11_SDK_VERSION,
                                    &m_device, &got, &m_ctx);
    if (FAILED(hr)) return false;

    // Allow multithread protection for decoder sharing
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(m_device.As(&mt))) mt->SetMultithreadProtected(TRUE);

    if (!CreateSwapChain(hwnd, width, height)) return false;
    if (!CreateRenderTarget())                 return false;
    if (!CompileShaders())                     return false;
    if (!CreateSamplers())                     return false;
    SetupFullscreenQuad();

    m_initialized = true;
    return true;
}

bool D3D11Renderer::CreateSwapChain(HWND hwnd, UINT w, UINT h)
{
    ComPtr<IDXGIFactory2> factory;
    {
        ComPtr<IDXGIDevice1> dxgiDev;
        m_device.As(&dxgiDev);
        dxgiDev->SetMaximumFrameLatency(1);

        ComPtr<IDXGIAdapter> adapter;
        dxgiDev->GetAdapter(&adapter);

        ComPtr<IDXGIFactory2> f;
        adapter->GetParent(IID_PPV_ARGS(&factory));
    }

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width              = w;
    sd.Height             = h;
    sd.Format             = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.Stereo             = FALSE;
    sd.SampleDesc.Count   = 1;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount        = 3;                            // triple-buffer
    sd.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD; // tearing-free
    sd.Flags              = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
                          | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    sd.Scaling            = DXGI_SCALING_STRETCH;
    sd.AlphaMode          = DXGI_ALPHA_MODE_IGNORE;

    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = factory->CreateSwapChainForHwnd(m_device.Get(), hwnd,
                                                  &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) {
        // Retry without tearing flag (not all drivers support it)
        sd.Flags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        hr = factory->CreateSwapChainForHwnd(m_device.Get(), hwnd,
                                              &sd, nullptr, nullptr, &sc1);
        if (FAILED(hr)) return false;
    }
    sc1.As(&m_swapChain);

    // Restrict frame latency to 1 via waitable object → precise pacing
    m_swapChain->SetMaximumFrameLatency(1);
    m_frameLatencyWaitable = m_swapChain->GetFrameLatencyWaitableObject();

    // Disable Alt+Enter fullscreen (we manage it ourselves)
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    return true;
}

bool D3D11Renderer::CreateRenderTarget()
{
    ComPtr<ID3D11Texture2D> bb;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&bb));
    if (FAILED(hr)) return false;

    D3D11_RENDER_TARGET_VIEW_DESC rtvd{};
    rtvd.Format        = DXGI_FORMAT_B8G8R8A8_UNORM;
    rtvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    return SUCCEEDED(m_device->CreateRenderTargetView(bb.Get(), &rtvd, &m_rtv));
}

bool D3D11Renderer::CompileShaders()
{
    // Vertex shader
    auto vsBlob = CompileHLSL(kVS, "main", "vs_5_0");
    HRESULT hr  = m_device->CreateVertexShader(vsBlob->GetBufferPointer(),
                                                vsBlob->GetBufferSize(),
                                                nullptr, &m_vs);
    if (FAILED(hr)) return false;

    D3D11_INPUT_ELEMENT_DESC ied[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    hr = m_device->CreateInputLayout(ied, 2,
                                      vsBlob->GetBufferPointer(),
                                      vsBlob->GetBufferSize(), &m_inputLayout);
    if (FAILED(hr)) return false;

    // NV12 pixel shader
    auto psNV12Blob = CompileHLSL(kPS_NV12, "main", "ps_5_0");
    hr = m_device->CreatePixelShader(psNV12Blob->GetBufferPointer(),
                                      psNV12Blob->GetBufferSize(),
                                      nullptr, &m_psNV12);
    if (FAILED(hr)) return false;

    // BGRA pixel shader
    auto psBGRABlob = CompileHLSL(kPS_BGRA, "main", "ps_5_0");
    hr = m_device->CreatePixelShader(psBGRABlob->GetBufferPointer(),
                                      psBGRABlob->GetBufferSize(),
                                      nullptr, &m_psBGRA);
    if (FAILED(hr)) return false;

    // Constant buffer
    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth      = sizeof(CBDraw);
    cbd.Usage          = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(m_device->CreateBuffer(&cbd, nullptr, &m_cbTransform));
}

bool D3D11Renderer::CreateSamplers()
{
    D3D11_SAMPLER_DESC sd{};
    sd.Filter         = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy  = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;
    if (FAILED(m_device->CreateSamplerState(&sd, &m_samplerLinear))) return false;

    // Alpha-blend state for crossfade
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(m_device->CreateBlendState(&bd, &m_blendState));
}

void D3D11Renderer::SetupFullscreenQuad()
{
    D3D11_BUFFER_DESC vbd{};
    vbd.ByteWidth = sizeof(kQuadVerts);
    vbd.Usage     = D3D11_USAGE_IMMUTABLE;
    vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vd{ kQuadVerts };
    m_device->CreateBuffer(&vbd, &vd, &m_quadVB);

    D3D11_BUFFER_DESC ibd{};
    ibd.ByteWidth = sizeof(kQuadIdx);
    ibd.Usage     = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA id{ kQuadIdx };
    m_device->CreateBuffer(&ibd, &id, &m_quadIB);
}

// ─────────────────────────────────────────────────────────────────────────────
void D3D11Renderer::Resize(UINT width, UINT height)
{
    if (!m_initialized || (width == m_width && height == m_height)) return;
    m_width  = width;
    m_height = height;

    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    m_rtv.Reset();
    m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN,
                                DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT
                              | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
    CreateRenderTarget();
}

// ─────────────────────────────────────────────────────────────────────────────
// Main render call – called every frame from the render loop.
// Waits on the waitable object so we never get ahead of the display.
// ─────────────────────────────────────────────────────────────────────────────
void D3D11Renderer::Present()
{
    if (!m_initialized) return;

    // Wait until the swap chain is ready for a new frame (frame latency = 1)
    WaitForSingleObjectEx(m_frameLatencyWaitable, 1000, TRUE);

    // Black clear – only needed if slots don't cover the whole screen
    static constexpr float kBlack[4] = {0, 0, 0, 1};
    m_ctx->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
    m_ctx->ClearRenderTargetView(m_rtv.Get(), kBlack);

    D3D11_VIEWPORT vp{0, 0, (float)m_width, (float)m_height, 0, 1};
    m_ctx->RSSetViewports(1, &vp);

    // Setup common pipeline state
    UINT stride = sizeof(QuadVertex), offset = 0;
    m_ctx->IASetInputLayout(m_inputLayout.Get());
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->IASetVertexBuffers(0, 1, m_quadVB.GetAddressOf(), &stride, &offset);
    m_ctx->IASetIndexBuffer(m_quadIB.Get(), DXGI_FORMAT_R16_UINT, 0);
    m_ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    m_ctx->PSSetSamplers(0, 1, m_samplerLinear.GetAddressOf());
    m_ctx->PSSetConstantBuffers(0, 1, m_cbTransform.GetAddressOf());

    float blend[4] = {1,1,1,1};
    m_ctx->OMSetBlendState(m_blendState.Get(), blend, 0xFFFFFFFF);

    // slot[0] = current (alpha=1→0 during fade-out)
    // slot[1] = next    (alpha=0→1 during fade-in)
    float a0 = slotAlpha.load(std::memory_order_relaxed);
    float a1 = 1.f - a0;

    {
        std::lock_guard lk(slots[1].lock);
        if (slots[1].hasFrame) RenderSlot(slots[1], a1);
    }
    {
        std::lock_guard lk(slots[0].lock);
        if (slots[0].hasFrame) RenderSlot(slots[0], a0);
    }

    // DXGI_PRESENT_ALLOW_TEARING for variable-refresh-rate displays
    m_swapChain->Present(1, 0);  // vsync interval=1; use 0 + ALLOW_TEARING for VRR
}

void D3D11Renderer::RenderSlot(const DisplaySlot& slot, float alpha)
{
    if (!slot.hasFrame || alpha < 0.002f) return;

    // Update constant buffer
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (SUCCEEDED(m_ctx->Map(m_cbTransform.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        auto* cb  = reinterpret_cast<CBDraw*>(ms.pData);
        cb->alpha = alpha;
        m_ctx->Unmap(m_cbTransform.Get(), 0);
    }

    const VideoFrame& f = slot.frame;
    if (!f.texture) return;

    if (f.isNV12) {
        // Create SRVs for Y and UV planes from the NV12 texture
        D3D11_SHADER_RESOURCE_VIEW_DESC srvY{};
        srvY.Format                    = DXGI_FORMAT_R8_UNORM;
        srvY.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srvY.Texture2DArray.MipLevels  = 1;
        srvY.Texture2DArray.ArraySize  = 1;
        srvY.Texture2DArray.FirstArraySlice = f.arraySlice;

        D3D11_SHADER_RESOURCE_VIEW_DESC srvUV = srvY;
        srvUV.Format = DXGI_FORMAT_R8G8_UNORM;
        srvUV.Texture2DArray.FirstArraySlice = f.arraySlice;

        ComPtr<ID3D11ShaderResourceView> sY, sUV;
        m_device->CreateShaderResourceView(f.texture.Get(), &srvY,  &sY);
        m_device->CreateShaderResourceView(f.texture.Get(), &srvUV, &sUV);

        ID3D11ShaderResourceView* srvs[] = { sY.Get(), sUV.Get() };
        m_ctx->PSSetShaderResources(0, 2, srvs);
        m_ctx->PSSetShader(m_psNV12.Get(), nullptr, 0);
    } else {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format        = DXGI_FORMAT_B8G8R8A8_UNORM;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;

        ComPtr<ID3D11ShaderResourceView> sBGRA;
        m_device->CreateShaderResourceView(f.texture.Get(), &srv, &sBGRA);
        ID3D11ShaderResourceView* srvs[] = { sBGRA.Get(), nullptr };
        m_ctx->PSSetShaderResources(0, 2, srvs);
        m_ctx->PSSetShader(m_psBGRA.Get(), nullptr, 0);
    }

    m_ctx->DrawIndexed(6, 0, 0);

    // Unbind SRVs so textures aren't left bound as both SRV and RT
    ID3D11ShaderResourceView* nullSRVs[2] = {};
    m_ctx->PSSetShaderResources(0, 2, nullSRVs);
}

// ─────────────────────────────────────────────────────────────────────────────
// CPU upload path (software decode fallback)
// Creates / reuses NV12 staging + GPU texture, uploads YUV420P data.
// ─────────────────────────────────────────────────────────────────────────────
bool D3D11Renderer::UploadYUV420(DisplaySlot& slot,
                                  const uint8_t* y, int yStride,
                                  const uint8_t* u, int uStride,
                                  const uint8_t* v, int vStride,
                                  int w, int h)
{
    std::lock_guard lk(slot.lock);

    // (Re)create textures if dimensions changed
    bool recreate = !m_stagingNV12;
    if (!recreate) {
        D3D11_TEXTURE2D_DESC d{};
        m_stagingNV12->GetDesc(&d);
        recreate = (d.Width != (UINT)w || d.Height != (UINT)h);
    }
    if (recreate) {
        m_stagingNV12.Reset(); m_gpuNV12.Reset(); m_srvY.Reset(); m_srvUV.Reset();

        D3D11_TEXTURE2D_DESC td{};
        td.Width     = w;
        td.Height    = h * 3 / 2;  // NV12: Y plane + half-height UV plane
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format    = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage     = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_stagingNV12))) return false;

        // NV12 GPU texture (two planes via DXGI)
        D3D11_TEXTURE2D_DESC gd = td;
        gd.Format    = DXGI_FORMAT_NV12;
        gd.Height    = h;
        gd.Usage     = D3D11_USAGE_DEFAULT;
        gd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        gd.CPUAccessFlags = 0;
        if (FAILED(m_device->CreateTexture2D(&gd, nullptr, &m_gpuNV12))) return false;

        D3D11_SHADER_RESOURCE_VIEW_DESC svY{};
        svY.Format        = DXGI_FORMAT_R8_UNORM;
        svY.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        svY.Texture2D.MipLevels = 1;
        m_device->CreateShaderResourceView(m_gpuNV12.Get(), &svY, &m_srvY);

        D3D11_SHADER_RESOURCE_VIEW_DESC svUV = svY;
        svUV.Format = DXGI_FORMAT_R8G8_UNORM;
        m_device->CreateShaderResourceView(m_gpuNV12.Get(), &svUV, &m_srvUV);
    }

    // Map staging and write NV12 (convert YUV420P → NV12 on CPU)
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(m_ctx->Map(m_stagingNV12.Get(), 0, D3D11_MAP_WRITE, 0, &ms))) return false;

    auto* dst = reinterpret_cast<uint8_t*>(ms.pData);
    for (int row = 0; row < h; ++row)
        memcpy(dst + row * ms.RowPitch, y + row * yStride, w);

    uint8_t* uvDst = dst + h * ms.RowPitch;
    for (int row = 0; row < h / 2; ++row) {
        auto* uRow = u + row * uStride;
        auto* vRow = v + row * vStride;
        auto* dRow = uvDst + row * ms.RowPitch;
        for (int col = 0; col < w / 2; ++col) {
            dRow[col * 2 + 0] = uRow[col];
            dRow[col * 2 + 1] = vRow[col];
        }
    }
    m_ctx->Unmap(m_stagingNV12.Get(), 0);

    // Copy staging → GPU NV12 texture
    D3D11_BOX box{ 0, 0, 0, (UINT)w, (UINT)h, 1 };
    m_ctx->CopySubresourceRegion(m_gpuNV12.Get(), 0, 0, 0, 0,
                                  m_stagingNV12.Get(), 0, &box);

    slot.frame.texture    = m_gpuNV12;
    slot.frame.arraySlice = 0;
    slot.frame.isNV12     = true;
    slot.frame.width      = w;
    slot.frame.height     = h;
    slot.hasFrame         = true;
    return true;
}

bool D3D11Renderer::WrapHWTexture(DisplaySlot& slot,
                                   ID3D11Texture2D* tex, UINT arraySlice,
                                   int w, int h)
{
    std::lock_guard lk(slot.lock);
    slot.frame.texture    = tex;
    slot.frame.arraySlice = arraySlice;
    slot.frame.isNV12     = true;
    slot.frame.width      = w;
    slot.frame.height     = h;
    slot.hasFrame         = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
void D3D11Renderer::Shutdown()
{
    if (m_frameLatencyWaitable) {
        CloseHandle(m_frameLatencyWaitable);
        m_frameLatencyWaitable = nullptr;
    }
    if (m_ctx) m_ctx->ClearState();
    m_initialized = false;
}

D3D11Renderer::~D3D11Renderer()
{
    Shutdown();
}
