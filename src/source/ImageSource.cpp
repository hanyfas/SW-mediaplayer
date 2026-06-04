#include "ImageSource.h"
#include <wincodec.h>
#include <stdexcept>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

static std::wstring ToWide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring ws(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, ws.data(), n);
    return ws;
}

bool ImageSource::Load(const std::string& path,
                        ID3D11Device* device,
                        ID3D11DeviceContext* ctx)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IWICImagingFactory> wic;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapDecoder> decoder;
    hr = wic->CreateDecoderFromFilename(ToWide(path).c_str(), nullptr,
                                         GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                         &decoder);
    if (FAILED(hr)) return false;

    ComPtr<IWICBitmapFrameDecode> frame;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr)) return false;

    UINT w = 0, h = 0;
    frame->GetSize(&w, &h);
    m_width  = (int)w;
    m_height = (int)h;

    // Convert to 32bppBGRA regardless of source format
    ComPtr<IWICFormatConverter> conv;
    wic->CreateFormatConverter(&conv);
    hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                           WICBitmapDitherTypeNone, nullptr, 0.0,
                           WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) return false;

    std::vector<BYTE> pixels(w * h * 4);
    hr = conv->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data());
    if (FAILED(hr)) return false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_IMMUTABLE;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{ pixels.data(), w * 4, 0 };
    hr = device->CreateTexture2D(&td, &init, &m_texture);
    return SUCCEEDED(hr);
}
