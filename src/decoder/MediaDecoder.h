#pragma once
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <d3d11.h>
#include <wrl/client.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

using Microsoft::WRL::ComPtr;

struct DecodedVideoFrame {
    ComPtr<ID3D11Texture2D> texture;    // null = software frame
    UINT                    arraySlice{};
    bool                    isHW{false};

    // Software fallback – raw YUV420P planes
    std::vector<uint8_t> yPlane, uPlane, vPlane;
    int yStride{}, uStride{}, vStride{};
    int width{}, height{};

    double pts{0.0}; // seconds
};

struct DecodedAudioFrame {
    std::vector<float> pcm;  // interleaved stereo float32
    int                sampleRate{48000};
    int                channels{2};
    double             pts{0.0};
};

using VideoFrameCallback = std::function<void(DecodedVideoFrame)>;
using AudioFrameCallback = std::function<void(DecodedAudioFrame)>;
using EOFCallback        = std::function<void()>;

class MediaDecoder {
public:
    MediaDecoder() = default;
    ~MediaDecoder();

    // D3D11 device must be set before Open() to enable hardware decode.
    void SetD3DDevice(ID3D11Device* dev) { m_d3dDevice = dev; }

    bool Open(const std::string& path);
    void SetVideoCallback(VideoFrameCallback cb) { m_videoCb = std::move(cb); }
    void SetAudioCallback(AudioFrameCallback cb) { m_audioCb = std::move(cb); }
    void SetEOFCallback(EOFCallback cb)          { m_eofCb   = std::move(cb); }

    void Start();
    void Stop();
    void Seek(double seconds);

    double GetDuration() const { return m_duration; }
    bool   IsHWDecode()  const { return m_hwCtx != nullptr; }

private:
    bool InitHWDecode();
    bool InitSWDecode();
    void DecodeLoop();
    void ProcessVideoFrame(AVFrame* frame);
    void ProcessAudioFrame(AVFrame* frame);

    AVFormatContext* m_fmtCtx{nullptr};
    AVCodecContext*  m_vCtx{nullptr};
    AVCodecContext*  m_aCtx{nullptr};
    AVBufferRef*     m_hwCtx{nullptr};  // D3D11VA device context
    SwrContext*      m_swr{nullptr};
    int              m_vStream{-1};
    int              m_aStream{-1};
    double           m_duration{0.0};

    ID3D11Device*    m_d3dDevice{nullptr};

    VideoFrameCallback m_videoCb;
    AudioFrameCallback m_audioCb;
    EOFCallback        m_eofCb;

    std::thread      m_thread;
    std::atomic_bool m_stop{false};
    std::atomic_bool m_seekPending{false};
    double           m_seekTarget{0.0};
    std::mutex       m_seekMtx;
};
