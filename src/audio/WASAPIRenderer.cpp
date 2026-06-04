#include "WASAPIRenderer.h"
#include <stdexcept>
#include <algorithm>

#pragma comment(lib, "ole32.lib")

bool WASAPIRenderer::Init()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                   CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return false;

    hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &m_device);
    if (FAILED(hr)) return false;

    hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                             nullptr, (void**)&m_client);
    if (FAILED(hr)) return false;

    // Request stereo float32 @ 48 kHz (matches our resampler output)
    m_wfx.wFormatTag      = WAVE_FORMAT_IEEE_FLOAT;
    m_wfx.nChannels       = 2;
    m_wfx.nSamplesPerSec  = 48000;
    m_wfx.wBitsPerSample  = 32;
    m_wfx.nBlockAlign     = m_wfx.nChannels * (m_wfx.wBitsPerSample / 8);
    m_wfx.nAvgBytesPerSec = m_wfx.nSamplesPerSec * m_wfx.nBlockAlign;

    // 20 ms buffer in shared mode
    REFERENCE_TIME bufDuration = 200000; // 100ns units
    hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                               bufDuration, 0, &m_wfx, nullptr);
    if (FAILED(hr)) return false;

    m_eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_client->SetEventHandle(m_eventHandle);
    m_client->GetBufferSize(&m_bufferFrames);

    m_client->GetService(IID_PPV_ARGS(&m_renderClient));
    m_client->GetService(IID_PPV_ARGS(&m_volume));

    m_client->Start();
    m_stop   = false;
    m_thread = std::thread([this] { RenderLoop(); });
    return true;
}

void WASAPIRenderer::RenderLoop()
{
    // Boost this thread to audio priority
    HANDLE task{};
    DWORD taskIdx = 0;
    task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);

    while (!m_stop) {
        DWORD ret = WaitForSingleObjectEx(m_eventHandle, 200, TRUE);
        if (ret != WAIT_OBJECT_0) continue;

        UINT32 padding = 0;
        if (FAILED(m_client->GetCurrentPadding(&padding))) continue;

        UINT32 available = m_bufferFrames - padding;
        if (available == 0) continue;

        BYTE* buf = nullptr;
        if (FAILED(m_renderClient->GetBuffer(available, &buf))) continue;

        auto* dst = reinterpret_cast<float*>(buf);
        UINT32 framesWritten = 0;
        {
            std::lock_guard lk(m_pcmMutex);
            UINT32 samplesAvail = (UINT32)(m_pcmQueue.size());
            framesWritten = std::min(available, samplesAvail / 2);
            for (UINT32 i = 0; i < framesWritten * 2; ++i) {
                dst[i] = m_pcmQueue.front();
                m_pcmQueue.pop_front();
            }
        }

        // Silence any remaining frames we got from the buffer
        UINT32 silent = available - framesWritten;
        if (silent > 0)
            memset(dst + framesWritten * 2, 0, silent * 2 * sizeof(float));

        m_renderClient->ReleaseBuffer(available,
            (framesWritten == 0 && silent > 0) ? AUDCLNT_BUFFERFLAGS_SILENT : 0);
    }

    if (task) AvRevertMmThreadCharacteristics(task);
}

void WASAPIRenderer::Push(const float* samples, int count)
{
    std::lock_guard lk(m_pcmMutex);
    // Drop oldest samples if queue is full (prevents unbounded lag)
    while ((int)m_pcmQueue.size() + count > (int)kMaxQueueSamples) {
        m_pcmQueue.pop_front();
    }
    m_pcmQueue.insert(m_pcmQueue.end(), samples, samples + count);
}

void WASAPIRenderer::SetVolume(float v)
{
    if (m_volume) m_volume->SetMasterVolume(std::clamp(v, 0.f, 1.f), nullptr);
}

void WASAPIRenderer::Shutdown()
{
    m_stop = true;
    if (m_eventHandle) SetEvent(m_eventHandle);
    if (m_thread.joinable()) m_thread.join();
    if (m_client) m_client->Stop();
    if (m_eventHandle) { CloseHandle(m_eventHandle); m_eventHandle = nullptr; }
}

WASAPIRenderer::~WASAPIRenderer()
{
    Shutdown();
}
