#include "PlaybackEngine.h"
#include "renderer/D3D11Renderer.h"
#include "audio/WASAPIRenderer.h"
#include "playlist/Playlist.h"
#include "decoder/MediaDecoder.h"
#include "source/ImageSource.h"
#include <algorithm>

using namespace std::chrono;

PlaybackEngine::PlaybackEngine(D3D11Renderer* renderer,
                               WASAPIRenderer* audio,
                               Playlist*       playlist)
    : m_renderer(renderer), m_audio(audio), m_playlist(playlist)
{}

PlaybackEngine::~PlaybackEngine()
{
    Stop();
}

void PlaybackEngine::Start()
{
    if (m_running || !m_playlist || m_playlist->Size() == 0) return;
    m_running    = true;
    m_currentItem = 0;
    m_nextItem    = (m_currentItem + 1) % m_playlist->Size();
    m_fadeAlpha   = 1.f;
    m_fadeDurationSec = (float)m_playlist->TransitionDurationSec();
    m_renderer->slotAlpha.store(1.f);

    LoadItem(m_currentItem, 0);
    m_state = State::IDLE;
}

void PlaybackEngine::Stop()
{
    m_running = false;
    for (auto& dec : m_decoders) { if (dec) dec->Stop(); }
}

// ─── LoadItem ─────────────────────────────────────────────────────────────────
void PlaybackEngine::LoadItem(int itemIdx, int slotIdx)
{
    const PlaylistItem& item = m_playlist->GetItem(itemIdx);

    if (slotIdx == 0) {
        m_currentIsImage = (item.type == ItemType::Image);
    } else {
        m_nextIsImage = (item.type == ItemType::Image);
    }

    if (item.type == ItemType::Image) {
        m_images[slotIdx] = std::make_unique<ImageSource>();
        if (m_images[slotIdx]->Load(item.path, m_renderer->GetDevice(),
                                     m_renderer->GetContext())) {
            std::lock_guard lk(m_renderer->slots[slotIdx].lock);
            m_renderer->slots[slotIdx].frame.texture    = m_images[slotIdx]->GetTexture();
            m_renderer->slots[slotIdx].frame.arraySlice = 0;
            m_renderer->slots[slotIdx].frame.isNV12     = false;
            m_renderer->slots[slotIdx].frame.width      = m_images[slotIdx]->GetWidth();
            m_renderer->slots[slotIdx].frame.height     = m_images[slotIdx]->GetHeight();
            m_renderer->slots[slotIdx].hasFrame         = true;
        }
        m_currentDuration = (item.durationMs > 0) ? item.durationMs / 1000.0 : 5.0;
        if (slotIdx == 0) m_itemStartTime = steady_clock::now();
        m_decoders[slotIdx].reset();
        return;
    }

    // Video item
    m_images[slotIdx].reset();
    m_decoders[slotIdx] = std::make_unique<MediaDecoder>();
    m_decoders[slotIdx]->SetD3DDevice(m_renderer->GetDevice());

    if (!m_decoders[slotIdx]->Open(item.path)) {
        m_decoders[slotIdx].reset();
        return;
    }

    const int slot = slotIdx; // capture

    m_decoders[slotIdx]->SetVideoCallback([this, slot](DecodedVideoFrame f) {
        if (!m_running) return;
        if (f.isHW) {
            m_renderer->WrapHWTexture(m_renderer->slots[slot],
                                       f.texture.Get(), f.arraySlice,
                                       f.width, f.height);
        } else {
            m_renderer->UploadYUV420(m_renderer->slots[slot],
                                      f.yPlane.data(), f.yStride,
                                      f.uPlane.data(), f.uStride,
                                      f.vPlane.data(), f.vStride,
                                      f.width, f.height);
        }
    });

    m_decoders[slotIdx]->SetAudioCallback([this, slot](DecodedAudioFrame f) {
        if (!m_running || slot != 0) return; // only play audio from active slot
        if (m_audio) m_audio->Push(f.pcm.data(), (int)f.pcm.size());
    });

    m_decoders[slotIdx]->SetEOFCallback([this, slot]() {
        if (!m_running || slot != 0) return;
        // EOF from active slot → trigger transition if not already
        if (m_state == State::IDLE || m_state == State::PRELOADING)
            BeginTransition();
    });

    if (slotIdx == 0) {
        m_currentDuration = m_decoders[0]->GetDuration();
        m_itemStartTime   = steady_clock::now();
    }

    m_decoders[slotIdx]->Start();
}

// ─── Tick – called ~60 Hz from render loop ────────────────────────────────────
void PlaybackEngine::Tick()
{
    if (!m_running || m_playlist->Size() == 0) return;

    double elapsed = duration_cast<duration<double>>(
        steady_clock::now() - m_itemStartTime).count();

    switch (m_state) {
    case State::IDLE: {
        // Begin pre-loading next item when we're kPreloadLeadTime seconds from end
        double remaining = m_currentDuration - elapsed;
        if (remaining <= kPreloadLeadTime || m_currentDuration <= 0) {
            m_nextItem = (m_currentItem + 1) % m_playlist->Size();
            m_state    = State::PRELOADING;
            LoadItem(m_nextItem, 1);
        }
        break;
    }
    case State::PRELOADING:
        // Wait until slot[1] has a frame, then start fading
        if (m_renderer->slots[1].hasFrame) {
            BeginTransition();
        }
        break;

    case State::FADING: {
        double fadeSec = duration_cast<duration<double>>(
            steady_clock::now() - m_fadeStart).count();
        float t = (float)(fadeSec / m_fadeDurationSec);
        t = std::clamp(t, 0.f, 1.f);

        // slot[0] fades out, slot[1] fades in
        m_renderer->slotAlpha.store(1.f - t, std::memory_order_relaxed);

        if (t >= 1.f) {
            AdvanceToNextItem();
        }
        break;
    }
    case State::DONE:
        break;
    }
}

void PlaybackEngine::BeginTransition()
{
    m_state     = State::FADING;
    m_fadeStart = steady_clock::now();
    m_renderer->slotAlpha.store(1.f);
}

void PlaybackEngine::AdvanceToNextItem()
{
    // slot[1] is now fully visible → make it slot[0]
    // Stop old decoder
    if (m_decoders[0]) { m_decoders[0]->Stop(); m_decoders[0].reset(); }
    m_images[0].reset();

    // Move slot[1] data → slot[0]
    {
        std::lock_guard lk0(m_renderer->slots[0].lock);
        std::lock_guard lk1(m_renderer->slots[1].lock);
        m_renderer->slots[0] = std::move(m_renderer->slots[1]);
        m_renderer->slots[1].hasFrame = false;
    }

    m_decoders[0]  = std::move(m_decoders[1]);
    m_images[0]    = std::move(m_images[1]);
    m_currentIsImage = m_nextIsImage;
    m_currentItem  = m_nextItem;
    m_itemStartTime = steady_clock::now();

    if (m_decoders[0]) {
        m_currentDuration = m_decoders[0]->GetDuration();
        // Redirect audio callback to slot 0
        m_decoders[0]->SetAudioCallback([this](DecodedAudioFrame f) {
            if (!m_running) return;
            if (m_audio) m_audio->Push(f.pcm.data(), (int)f.pcm.size());
        });
        m_decoders[0]->SetEOFCallback([this]() {
            if (!m_running) return;
            if (m_state == State::IDLE || m_state == State::PRELOADING)
                BeginTransition();
        });
    } else {
        // Image – duration from playlist
        m_currentDuration = m_playlist->GetItem(m_currentItem).durationMs / 1000.0;
    }

    m_renderer->slotAlpha.store(1.f);
    m_state = State::IDLE;
}
