#pragma once
#include <Windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <thread>
#include <mutex>
#include <deque>
#include <vector>
#include <atomic>

using Microsoft::WRL::ComPtr;

// Low-latency WASAPI shared-mode audio renderer.
// Accepts interleaved stereo float32 at 48 kHz.
class WASAPIRenderer {
public:
    WASAPIRenderer() = default;
    ~WASAPIRenderer();

    bool Init();
    void Shutdown();

    // Thread-safe: push PCM samples from the decoder thread.
    void Push(const float* samples, int count);
    void SetVolume(float v); // 0.0 – 1.0

private:
    void RenderLoop();

    ComPtr<IMMDevice>          m_device;
    ComPtr<IAudioClient>       m_client;
    ComPtr<IAudioRenderClient> m_renderClient;
    ComPtr<ISimpleAudioVolume> m_volume;

    HANDLE       m_eventHandle{nullptr};
    UINT32       m_bufferFrames{0};
    WAVEFORMATEX m_wfx{};

    std::thread      m_thread;
    std::atomic_bool m_stop{false};

    std::mutex           m_pcmMutex;
    std::deque<float>    m_pcmQueue;

    static constexpr size_t kMaxQueueSamples = 48000 * 2 * 2; // 2 sec buffer
};
