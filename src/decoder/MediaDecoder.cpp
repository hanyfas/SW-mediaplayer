#include "MediaDecoder.h"
#include <stdexcept>
#include <cassert>

extern "C" {
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/channel_layout.h>
}

// ─── helpers ─────────────────────────────────────────────────────────────────
static double TsToSeconds(int64_t ts, AVRational tb)
{
    return (ts == AV_NOPTS_VALUE) ? 0.0 : av_q2d(tb) * (double)ts;
}

// ─── Open ────────────────────────────────────────────────────────────────────
bool MediaDecoder::Open(const std::string& path)
{
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
    m_fmtCtx = fmt;

    if (avformat_find_stream_info(fmt, nullptr) < 0) return false;

    // Find best video / audio streams
    m_vStream = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    m_aStream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    m_duration = (fmt->duration > 0)
               ? (double)fmt->duration / AV_TIME_BASE
               : 0.0;

    // ── Video codec ──────────────────────────────────────────────────────────
    if (m_vStream >= 0) {
        AVStream* vs = fmt->streams[m_vStream];
        const AVCodec* codec = avcodec_find_decoder(vs->codecpar->codec_id);
        if (!codec) return false;

        m_vCtx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(m_vCtx, vs->codecpar);

        // Try D3D11VA hardware decode first, fallback to software
        if (m_d3dDevice && InitHWDecode()) {
            // HW path configured
        } else {
            // Software fallback – still fine for Intel integrated GPU
            m_hwCtx = nullptr;
        }

        m_vCtx->thread_count = 0; // auto thread count for SW decode
        if (avcodec_open2(m_vCtx, codec, nullptr) < 0) return false;
    }

    // ── Audio codec ──────────────────────────────────────────────────────────
    if (m_aStream >= 0) {
        AVStream* as = fmt->streams[m_aStream];
        const AVCodec* codec = avcodec_find_decoder(as->codecpar->codec_id);
        m_aCtx = avcodec_alloc_context3(codec);
        avcodec_parameters_to_context(m_aCtx, as->codecpar);
        avcodec_open2(m_aCtx, codec, nullptr);

        // Setup resampler → stereo float32 @ 48 kHz
        m_swr = swr_alloc();
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        av_opt_set_chlayout(m_swr,  "out_chlayout",  &stereo, 0);
        av_opt_set_int(m_swr, "out_sample_rate",     48000,   0);
        av_opt_set_sample_fmt(m_swr, "out_sample_fmt", AV_SAMPLE_FMT_FLT, 0);
        av_opt_set_chlayout(m_swr,  "in_chlayout",   &m_aCtx->ch_layout, 0);
        av_opt_set_int(m_swr, "in_sample_rate",      m_aCtx->sample_rate, 0);
        av_opt_set_sample_fmt(m_swr, "in_sample_fmt", m_aCtx->sample_fmt, 0);
        swr_init(m_swr);
    }

    return true;
}

// ─── Hardware decode setup (D3D11VA) ─────────────────────────────────────────
bool MediaDecoder::InitHWDecode()
{
    // Create a D3D11VA hardware device context wrapping our existing D3D11 device
    AVBufferRef* hwDevCtx = nullptr;
    int ret = av_hwdevice_ctx_create_derived(&hwDevCtx,
                                              AV_HWDEVICE_TYPE_D3D11VA,
                                              nullptr, 0);
    if (ret < 0) {
        // Try creating from scratch (lets FFmpeg pick the adapter)
        ret = av_hwdevice_ctx_create(&hwDevCtx, AV_HWDEVICE_TYPE_D3D11VA,
                                      nullptr, nullptr, 0);
        if (ret < 0) return false;
    }

    // Override the D3D11 device in the hw context to use our existing device
    auto* hwDevCtxData = reinterpret_cast<AVD3D11VADeviceContext*>(
        reinterpret_cast<AVHWDeviceContext*>(hwDevCtx->data)->hwctx);
    hwDevCtxData->device = m_d3dDevice;
    m_d3dDevice->AddRef();

    // Create frames context
    AVBufferRef* framesCtx = av_hwframe_ctx_alloc(hwDevCtx);
    if (!framesCtx) { av_buffer_unref(&hwDevCtx); return false; }

    auto* framesCtxData = reinterpret_cast<AVHWFramesContext*>(framesCtx->data);
    framesCtxData->format    = AV_PIX_FMT_D3D11;
    framesCtxData->sw_format = AV_PIX_FMT_NV12;
    framesCtxData->width     = m_vCtx->width;
    framesCtxData->height    = m_vCtx->height;
    framesCtxData->initial_pool_size = 20; // pre-allocate 20 decode surfaces

    if (av_hwframe_ctx_init(framesCtx) < 0) {
        av_buffer_unref(&framesCtx);
        av_buffer_unref(&hwDevCtx);
        return false;
    }

    m_vCtx->hw_device_ctx = hwDevCtx;
    m_vCtx->hw_frames_ctx = framesCtx;
    m_vCtx->get_format     = [](AVCodecContext*, const AVPixelFormat* fmts) {
        for (auto f = fmts; *f != AV_PIX_FMT_NONE; ++f)
            if (*f == AV_PIX_FMT_D3D11) return AV_PIX_FMT_D3D11;
        return fmts[0];
    };

    m_hwCtx = hwDevCtx;
    return true;
}

// ─── Decode loop ─────────────────────────────────────────────────────────────
void MediaDecoder::Start()
{
    m_stop = false;
    m_thread = std::thread([this] { DecodeLoop(); });
}

void MediaDecoder::Stop()
{
    m_stop = true;
    if (m_thread.joinable()) m_thread.join();
}

void MediaDecoder::Seek(double seconds)
{
    std::lock_guard lk(m_seekMtx);
    m_seekTarget  = seconds;
    m_seekPending = true;
}

void MediaDecoder::DecodeLoop()
{
    AVPacket* pkt  = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();

    auto flushDecoder = [&](AVCodecContext* ctx) {
        avcodec_send_packet(ctx, nullptr); // send flush packet
        while (avcodec_receive_frame(ctx, frame) == 0) {}
        avcodec_flush_buffers(ctx);
    };

    while (!m_stop) {
        // Handle seek
        if (m_seekPending.exchange(false)) {
            double target;
            { std::lock_guard lk(m_seekMtx); target = m_seekTarget; }
            int64_t ts = (int64_t)(target * AV_TIME_BASE);
            av_seek_frame(m_fmtCtx, -1, ts, AVSEEK_FLAG_BACKWARD);
            if (m_vCtx) flushDecoder(m_vCtx);
            if (m_aCtx) flushDecoder(m_aCtx);
        }

        int ret = av_read_frame(m_fmtCtx, pkt);
        if (ret == AVERROR_EOF) {
            if (m_eofCb) m_eofCb();
            break;
        }
        if (ret < 0) continue;

        if (pkt->stream_index == m_vStream && m_vCtx) {
            if (avcodec_send_packet(m_vCtx, pkt) == 0) {
                while (avcodec_receive_frame(m_vCtx, frame) == 0) {
                    ProcessVideoFrame(frame);
                    av_frame_unref(frame);
                }
            }
        } else if (pkt->stream_index == m_aStream && m_aCtx) {
            if (avcodec_send_packet(m_aCtx, pkt) == 0) {
                while (avcodec_receive_frame(m_aCtx, frame) == 0) {
                    ProcessAudioFrame(frame);
                    av_frame_unref(frame);
                }
            }
        }
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
    av_frame_free(&frame);
}

// ─── Process decoded video frame ─────────────────────────────────────────────
void MediaDecoder::ProcessVideoFrame(AVFrame* frame)
{
    DecodedVideoFrame out;
    out.width  = frame->width;
    out.height = frame->height;
    out.pts    = TsToSeconds(frame->pts, m_fmtCtx->streams[m_vStream]->time_base);

    if (frame->format == AV_PIX_FMT_D3D11) {
        // Hardware decoded – frame->data[0] is the D3D11 texture,
        // frame->data[1] is the array slice index.
        out.isHW       = true;
        out.texture    = reinterpret_cast<ID3D11Texture2D*>(frame->data[0]);
        out.arraySlice = (UINT)(uintptr_t)frame->data[1];
        // AddRef so the texture stays alive after av_frame_unref
        out.texture->AddRef();
        // Note: ComPtr takes ownership from the raw AddRef'd pointer

        if (m_videoCb) m_videoCb(std::move(out));
    } else {
        // Software decode fallback – convert to YUV420P if needed
        AVFrame* sw = frame;
        AVFrame* converted = nullptr;

        if (frame->format != AV_PIX_FMT_YUV420P) {
            converted = av_frame_alloc();
            converted->format = AV_PIX_FMT_YUV420P;
            converted->width  = frame->width;
            converted->height = frame->height;
            av_frame_get_buffer(converted, 32);

            SwsContext* sws = sws_getContext(
                frame->width, frame->height, (AVPixelFormat)frame->format,
                frame->width, frame->height, AV_PIX_FMT_YUV420P,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
                      converted->data, converted->linesize);
            sws_freeContext(sws);
            sw = converted;
        }

        int ySize = sw->linesize[0] * sw->height;
        int uSize = sw->linesize[1] * (sw->height / 2);
        int vSize = sw->linesize[2] * (sw->height / 2);

        out.isHW   = false;
        out.yStride = sw->linesize[0];
        out.uStride = sw->linesize[1];
        out.vStride = sw->linesize[2];
        out.yPlane.assign(sw->data[0], sw->data[0] + ySize);
        out.uPlane.assign(sw->data[1], sw->data[1] + uSize);
        out.vPlane.assign(sw->data[2], sw->data[2] + vSize);

        if (converted) av_frame_free(&converted);
        if (m_videoCb) m_videoCb(std::move(out));
    }
}

// ─── Process decoded audio frame ─────────────────────────────────────────────
void MediaDecoder::ProcessAudioFrame(AVFrame* frame)
{
    if (!m_swr || !m_audioCb) return;

    DecodedAudioFrame out;
    out.sampleRate = 48000;
    out.channels   = 2;
    out.pts = TsToSeconds(frame->pts, m_fmtCtx->streams[m_aStream]->time_base);

    int outSamples = swr_get_out_samples(m_swr, frame->nb_samples);
    out.pcm.resize(outSamples * 2);

    uint8_t* outData = reinterpret_cast<uint8_t*>(out.pcm.data());
    int converted = swr_convert(m_swr, &outData, outSamples,
                                 (const uint8_t**)frame->data, frame->nb_samples);
    out.pcm.resize(converted * 2);
    m_audioCb(std::move(out));
}

// ─── Destructor ──────────────────────────────────────────────────────────────
MediaDecoder::~MediaDecoder()
{
    Stop();
    if (m_swr)    swr_free(&m_swr);
    if (m_vCtx)   avcodec_free_context(&m_vCtx);
    if (m_aCtx)   avcodec_free_context(&m_aCtx);
    if (m_hwCtx)  av_buffer_unref(&m_hwCtx);
    if (m_fmtCtx) avformat_close_input(&m_fmtCtx);
}
