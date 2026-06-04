#pragma once
#include <memory>
#include <atomic>
#include <thread>
#include <functional>
#include <chrono>

class D3D11Renderer;
class WASAPIRenderer;
class Playlist;
class MediaDecoder;
class ImageSource;

// Manages two decode slots and orchestrates seamless crossfade transitions.
//
// Transition state machine:
//   IDLE        – only slot[0] is playing
//   PRELOADING  – slot[1] decoder is starting for the next item
//   FADING      – alpha tween: slot[0] α→0, slot[1] α→1
//   SWAP        – swap decoder pointers, reset slot[1], go back to IDLE
class PlaybackEngine {
public:
    PlaybackEngine(D3D11Renderer* renderer,
                   WASAPIRenderer* audio,
                   Playlist*       playlist);
    ~PlaybackEngine();

    void Start();
    void Stop();

    // Called from the render loop ~60 Hz
    void Tick();

private:
    void LoadItem(int idx, int slotIdx);
    void BeginTransition();
    void AdvanceToNextItem();

    D3D11Renderer* m_renderer{};
    WASAPIRenderer* m_audio{};
    Playlist*       m_playlist{};

    std::unique_ptr<MediaDecoder> m_decoders[2];
    std::unique_ptr<ImageSource>  m_images[2];

    int  m_currentItem{0};
    int  m_nextItem{0};
    bool m_currentIsImage{false};
    bool m_nextIsImage{false};

    enum class State { IDLE, PRELOADING, FADING, DONE };
    std::atomic<State> m_state{State::IDLE};

    // Crossfade timing
    float  m_fadeAlpha{1.f};      // 1 = slot[0] fully visible
    float  m_fadeDurationSec{0.5f};
    std::chrono::steady_clock::time_point m_fadeStart;

    // When to begin pre-loading the next item (seconds before EOF)
    static constexpr double kPreloadLeadTime = 2.0;
    double m_currentDuration{0.0};
    std::chrono::steady_clock::time_point m_itemStartTime;

    bool m_running{false};
};
