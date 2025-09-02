/*
   Koya Plugin: FFmpeg Video (RGBA frames via swscale)

   JS API:
   - load(windowId, key, path) -> opens stream, creates texture, starts decode worker
   - play(windowId, key), pause(windowId, key)
   - seek(windowId, key, seconds)
   - stop(windowId, key) -> stop and remove stream (keeps texture until unload)
   - unload(windowId, key) -> stop and remove texture
   - details(windowId, key) -> { width, height, duration, fps, bitrate, codec }
*/
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <queue>
#include <thread>
#include <cstdlib>
#include <unordered_map>

#include "../../sdk/quickjs/quickjs.h"
#include "../../sdk/module_hooks.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

struct VideoStream {
    unsigned int windowId = 0;
    std::string key;
    int width = 0;
    int height = 0;
    std::atomic<bool> running{false};
    std::atomic<bool> playing{false};

    // FFmpeg state
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    int videoStreamIndex = -1;
    SwsContext* sws = nullptr;
    double timeBaseSec = 0.0;
    double avgFps = 0.0;
    double frameDurationSec = 0.0;

    // Custom AVIO for in-memory assets
    AVIOContext* avio = nullptr;
    void* avioOpaque = nullptr; // MemReader*
    bool usingMemAvio = false;
    const unsigned char* assetBuf = nullptr; // owned by engine; freed via asset_free_buffer
    size_t assetSize = 0;

    // Worker
    std::thread worker;

    // Latest-frame handoff (worker publishes when due; render consumes if version changes)
    std::mutex qmutex;
    std::vector<std::uint8_t> latestRgba; // width*height*4
    std::atomic<std::uint64_t> latestVersion{0};
    std::uint64_t lastConsumedVersion = 0;
    double nextDueSec = 0.0;        // next scheduled due time (for diagnostics)
    double firstPtsSec = std::numeric_limits<double>::quiet_NaN();
    double startEngineTime = std::numeric_limits<double>::quiet_NaN();

    // Playback clock
    std::chrono::steady_clock::time_point startTp;
    std::chrono::steady_clock::time_point pauseTp;
    double pausedAccumSec = 0.0;
};

static JSContext* g_ctx = nullptr;
static const KoyaRendererV1* g_renderer = nullptr;
static std::mutex g_mutex;
static std::map<std::string, std::unique_ptr<VideoStream>> g_streams; // key -> stream

// Build a unique id per window+key to avoid collisions across windows
static inline std::string make_stream_id(uint32_t windowId, const std::string& key)
{
    return std::to_string(windowId) + "|" + key;
}

// JS: load(windowId, key, path)
static JSValue js_load(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    if(argc < 3) return JS_ThrowTypeError(ctx, "load(windowId, key, path)");
    uint32_t windowId = 0; JS_ToUint32(ctx, &windowId, argv[0]);
    const char* k = JS_ToCString(ctx, argv[1]); if(!k) return JS_EXCEPTION;
    const char* vpath = JS_ToCString(ctx, argv[2]); if(!vpath) { JS_FreeCString(ctx, k); return JS_EXCEPTION; }

    // Prevent duplicate loads for the same windowId|key
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if(g_streams.find(make_stream_id(windowId, k)) != g_streams.end())
        {
            JS_FreeCString(ctx, vpath);
            JS_FreeCString(ctx, k);
            return JS_ThrowInternalError(ctx, "stream already loaded");
        }
    }

    // Allocate and open
    auto vs = std::make_unique<VideoStream>();
    vs->windowId = windowId;
    vs->key = k;
    JS_FreeCString(ctx, k);

    if(!g_renderer || !g_renderer->create_texture_rgba || !g_renderer->update_texture_rgba)
    { JS_FreeCString(ctx, vpath); return JS_ThrowInternalError(ctx, "renderer services unavailable"); }

    // Prefer in-memory IO via custom AVIO if available; fall back to temp file if needed.
    const unsigned char* fileData = nullptr;
    size_t fileSize = 0;
    if(g_renderer->asset_read_all && g_renderer->asset_read_all((void*)g_renderer->ctx, vpath, &fileData, &fileSize) && fileData && fileSize > 0)
    {
        // Setup AVIOContext backed by memory
        unsigned char* avioBuf = (unsigned char*)av_malloc(4096);
        struct MemReader { const unsigned char* p; size_t sz; size_t pos; };
        auto* mr = (MemReader*)std::malloc(sizeof(MemReader));
        if(!mr) { g_renderer->asset_free_buffer((void*)g_renderer->ctx, fileData); JS_FreeCString(ctx, vpath); return JS_ThrowInternalError(ctx, "mem alloc failed"); }
        mr->p = fileData; mr->sz = fileSize; mr->pos = 0;
        auto read_cb = [] (void* opaque, uint8_t* buf, int buf_size) -> int {
            MemReader* r = (MemReader*)opaque;
            size_t remain = (r->pos < r->sz) ? (r->sz - r->pos) : 0;
            size_t n = remain < (size_t)buf_size ? remain : (size_t)buf_size;
            if(n == 0) return AVERROR_EOF;
            std::memcpy(buf, r->p + r->pos, n);
            r->pos += n;
            return (int)n;
        };
        auto seek_cb = [] (void* opaque, int64_t offset, int whence) -> int64_t {
            MemReader* r = (MemReader*)opaque;
            if(whence == AVSEEK_SIZE) return (int64_t)r->sz;
            size_t base = 0;
            if(whence == SEEK_SET) base = 0; else if(whence == SEEK_CUR) base = r->pos; else if(whence == SEEK_END) base = r->sz; else return -1;
            int64_t np = (int64_t)base + offset;
            if(np < 0) np = 0;
            if((size_t)np > r->sz) np = (int64_t)r->sz;
            r->pos = (size_t)np;
            return np;
        };
        AVIOContext* avio = avio_alloc_context(avioBuf, 4096, 0, mr, read_cb, nullptr, seek_cb);
        if(!avio) { std::free(mr); g_renderer->asset_free_buffer((void*)g_renderer->ctx, fileData); JS_FreeCString(ctx, vpath); return JS_ThrowInternalError(ctx, "avio_alloc_context failed"); }
        vs->fmt = avformat_alloc_context();
        vs->fmt->pb = avio;
        vs->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
        if(avformat_open_input(&vs->fmt, nullptr, nullptr, nullptr) < 0)
        {
            std::free(mr);
            g_renderer->asset_free_buffer((void*)g_renderer->ctx, fileData);
            JS_FreeCString(ctx, vpath);
            return JS_ThrowInternalError(ctx, "avformat_open_input failed (mem)");
        }
        // Keep memory resources alive for lifetime of stream
        vs->usingMemAvio = true;
        vs->avio = avio;
        vs->avioOpaque = mr;
        vs->assetBuf = fileData;
        vs->assetSize = fileSize;
    }
    else
    {
        char resolved[4096];
        const char* pathForFfmpeg = vpath;
        if(g_renderer->asset_resolve_to_tempfile && vpath[0] == '/')
        {
            if(g_renderer->asset_resolve_to_tempfile((void*)g_renderer->ctx, vpath, resolved, sizeof(resolved)))
            {
                pathForFfmpeg = resolved;
            }
        }
        avformat_open_input(&vs->fmt, pathForFfmpeg, nullptr, nullptr);
    }
    JS_FreeCString(ctx, vpath);
    if(!vs->fmt) return JS_ThrowInternalError(ctx, "avformat_open_input failed");
    if(avformat_find_stream_info(vs->fmt, nullptr) < 0) return JS_ThrowInternalError(ctx, "find_stream_info failed");
    vs->videoStreamIndex = av_find_best_stream(vs->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if(vs->videoStreamIndex < 0) return JS_ThrowInternalError(ctx, "no video stream");
    AVStream* st = vs->fmt->streams[vs->videoStreamIndex];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if(!dec) return JS_ThrowInternalError(ctx, "decoder not found");
    vs->codec = avcodec_alloc_context3(dec);
    if(!vs->codec) return JS_ThrowInternalError(ctx, "alloc codec ctx failed");
    if(avcodec_parameters_to_context(vs->codec, st->codecpar) < 0) return JS_ThrowInternalError(ctx, "parameters_to_context failed");
    if(avcodec_open2(vs->codec, dec, nullptr) < 0) return JS_ThrowInternalError(ctx, "avcodec_open2 failed");
    vs->width = vs->codec->width;
    vs->height = vs->codec->height;
    vs->timeBaseSec = av_q2d(st->time_base);
    // Derive nominal FPS and frame duration using FFmpeg helpers
    AVRational g = av_guess_frame_rate(vs->fmt, st, nullptr);
    if(g.num > 0 && g.den > 0) vs->avgFps = (double)g.num / (double)g.den;
    else if(st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
        vs->avgFps = (double)st->avg_frame_rate.num / (double)st->avg_frame_rate.den;
    else if(st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0)
        vs->avgFps = (double)st->r_frame_rate.num / (double)st->r_frame_rate.den;
    else vs->avgFps = 30.0;
    if(vs->avgFps < 1.0) vs->avgFps = 30.0;
    vs->frameDurationSec = 1.0 / vs->avgFps;

    // Create texture
    int ok = g_renderer->create_texture_rgba((void*)g_renderer->ctx, windowId, vs->key.c_str(), vs->width, vs->height, 0);
    if(!ok) return JS_ThrowInternalError(ctx, "create_texture_rgba failed");

    vs->running.store(true);
    vs->playing.store(true);
    vs->startTp = std::chrono::steady_clock::now();
    vs->pausedAccumSec = 0.0;

    // Start worker
    vs->worker = std::thread([s=vs.get()](){
        AVPacket* pkt = av_packet_alloc();
        AVFrame* frm = av_frame_alloc();
        AVFrame* rgb = av_frame_alloc();
        int rgbBufSize = av_image_get_buffer_size(AV_PIX_FMT_RGBA, s->width, s->height, 1);
        std::vector<uint8_t> rgbbuf(static_cast<size_t>(rgbBufSize));
        av_image_fill_arrays(rgb->data, rgb->linesize, rgbbuf.data(), AV_PIX_FMT_RGBA, s->width, s->height, 1);
        s->sws = sws_getContext(s->width, s->height, s->codec->pix_fmt, s->width, s->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        while(s->running.load())
        {
            if(!s->playing.load()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
            if(av_read_frame(s->fmt, pkt) < 0)
            {
                // Loop: reset timing baseline so the next iteration starts fresh
                s->firstPtsSec = std::numeric_limits<double>::quiet_NaN();
                s->nextDueSec = 0.0;
                s->startTp = std::chrono::steady_clock::now();
                // Seek to start and continue
                av_seek_frame(s->fmt, s->videoStreamIndex, 0, AVSEEK_FLAG_BACKWARD);
                continue;
            }
            if(pkt->stream_index != s->videoStreamIndex) { av_packet_unref(pkt); continue; }
            if(avcodec_send_packet(s->codec, pkt) >= 0)
            {
                while(avcodec_receive_frame(s->codec, frm) >= 0)
                {
                    sws_scale(s->sws, frm->data, frm->linesize, 0, s->height, rgb->data, rgb->linesize);
                    {
                        const int stride = rgb->linesize[0];
                        // Derive a stable relative timeline from decoder timestamps
                        double ptsSec = NAN;
                        if(frm->best_effort_timestamp != AV_NOPTS_VALUE) ptsSec = (double)frm->best_effort_timestamp * s->timeBaseSec;
                        else if(frm->pts != AV_NOPTS_VALUE) ptsSec = (double)frm->pts * s->timeBaseSec;
                        else if(frm->pkt_dts != AV_NOPTS_VALUE) ptsSec = (double)frm->pkt_dts * s->timeBaseSec;
                        // Initialize the relative origin at first valid timestamp
                        if(std::isnan(s->firstPtsSec) && !std::isnan(ptsSec)) s->firstPtsSec = ptsSec;
                        double relPts = !std::isnan(ptsSec) && !std::isnan(s->firstPtsSec) ? (ptsSec - s->firstPtsSec) : -1.0;
                        if(relPts < 0.0)
                        {
                            // No usable PTS: fall back to fps-driven schedule
                            // Start immediately at t=0 then step by nominal fps for subsequent frames
                            if(s->nextDueSec <= 0.0) s->nextDueSec = 0.0; else s->nextDueSec += s->frameDurationSec;
                        }
                        else
                        {
                            // Use media timeline with monotonic clamp to fps step
                            double minNext = (s->nextDueSec <= 0.0) ? 0.0 : (s->nextDueSec + s->frameDurationSec * 0.50);
                            s->nextDueSec = relPts < minNext ? minNext : relPts;
                        }
                        // Wait until due (worker-side pacing) with fine-grained spin-sleep near target
                        for(;;)
                        {
                            double nowRel = std::chrono::duration<double>(std::chrono::steady_clock::now() - s->startTp).count() - s->pausedAccumSec;
                            double remain = s->nextDueSec - nowRel;
                            if(remain <= 0.0) break;
                            if(remain > 0.008) // >8ms: coarse sleep in small chunks
                            {
                                auto chunkMs = static_cast<int>(std::min(remain * 1000.0, 8.0));
                                std::this_thread::sleep_for(std::chrono::milliseconds(chunkMs));
                            }
                            else
                            {
                                // Busy-wait for sub-8ms to reduce overshoot
                                std::this_thread::yield();
                            }
                        }
                        // Publish latest frame
                        {
                            std::lock_guard<std::mutex> lk(s->qmutex);
                            if(s->latestRgba.size() != static_cast<size_t>(s->width)*static_cast<size_t>(s->height)*4)
                                s->latestRgba.resize(static_cast<size_t>(s->width)*static_cast<size_t>(s->height)*4);
                            for(int y=0; y<s->height; ++y)
                            {
                                std::memcpy(s->latestRgba.data() + static_cast<size_t>(y)*s->width*4,
                                            rgb->data[0] + static_cast<size_t>(y)*stride,
                                            static_cast<size_t>(s->width*4));
                            }
                            s->latestVersion.fetch_add(1, std::memory_order_release);
                        }
                        if(g_renderer && g_renderer->request_window_render)
                        {
                            int ok = g_renderer->request_window_render((void*)g_renderer->ctx, s->windowId);
                            if(!ok)
                            {
                                // Window likely destroyed; stop decoding this stream
                                s->running.store(false);
                                break;
                            }
                        }
                    }
                }
            }
            av_packet_unref(pkt);
        }
        av_frame_free(&frm);
        av_frame_free(&rgb);
        av_packet_free(&pkt);
    });

    // Store under composite key (windowId|key) to prevent cross-window collisions
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        g_streams[make_stream_id(windowId, vs->key)] = std::move(vs);
    }
    return JS_UNDEFINED;
}

// JS: stop(windowId, key)
static JSValue js_stop(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv)
{
    if(argc < 2) return JS_ThrowTypeError(ctx, "stop(windowId, key)");
    uint32_t windowId = 0; JS_ToUint32(ctx, &windowId, argv[0]);
    const char* k = JS_ToCString(ctx, argv[1]); if(!k) return JS_EXCEPTION;
    std::unique_ptr<VideoStream> victim;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        auto it = g_streams.find(make_stream_id(windowId, k));
        if(it != g_streams.end()) {
            it->second->running.store(false);
            if(it->second->worker.joinable()) it->second->worker.join();
            if(it->second->sws) { sws_freeContext(it->second->sws); it->second->sws = nullptr; }
            if(it->second->codec) { avcodec_free_context(&it->second->codec); }
            if(it->second->fmt) { avformat_close_input(&it->second->fmt); }
            if(it->second->usingMemAvio)
            {
                if(it->second->avio) {
                    if(it->second->avio->buffer) av_free(it->second->avio->buffer);
                    avio_context_free(&it->second->avio);
                }
                if(it->second->avioOpaque) { std::free(it->second->avioOpaque); it->second->avioOpaque = nullptr; }
                if(it->second->assetBuf && g_renderer && g_renderer->asset_free_buffer)
                {
                    g_renderer->asset_free_buffer((void*)g_renderer->ctx, it->second->assetBuf);
                    it->second->assetBuf = nullptr; it->second->assetSize = 0;
                }
            }
            victim = std::move(it->second);
            g_streams.erase(it);
        }
    }
    JS_FreeCString(ctx, k);
    return JS_UNDEFINED;
}

// JS: play/pause
static JSValue js_play(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if(argc < 2) return JS_ThrowTypeError(ctx, "play(windowId, key)");
    uint32_t windowId = 0; JS_ToUint32(ctx, &windowId, argv[0]);
    const char* k = JS_ToCString(ctx, argv[1]); if(!k) return JS_EXCEPTION;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_streams.find(make_stream_id(windowId, k));
    if(it != g_streams.end()) {
        if(!it->second->playing.load()) {
            auto now = std::chrono::steady_clock::now();
            if(it->second->pauseTp.time_since_epoch().count() != 0) {
                it->second->pausedAccumSec += std::chrono::duration<double>(now - it->second->pauseTp).count();
            }
        }
        it->second->playing.store(true);
    }
    JS_FreeCString(ctx, k);
    return JS_UNDEFINED;
}
static JSValue js_pause(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if(argc < 2) return JS_ThrowTypeError(ctx, "pause(windowId, key)");
    uint32_t windowId = 0; JS_ToUint32(ctx, &windowId, argv[0]);
    const char* k = JS_ToCString(ctx, argv[1]); if(!k) return JS_EXCEPTION;
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_streams.find(make_stream_id(windowId, k));
    if(it != g_streams.end()) {
        it->second->playing.store(false);
        it->second->pauseTp = std::chrono::steady_clock::now();
    }
    JS_FreeCString(ctx, k);
    return JS_UNDEFINED;
}

// JS: details(windowId, key)
static JSValue js_details(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv)
{
    if(argc < 2) return JS_ThrowTypeError(ctx, "details(windowId, key)");
    uint32_t windowId = 0; JS_ToUint32(ctx, &windowId, argv[0]);
    const char* k = JS_ToCString(ctx, argv[1]); if(!k) return JS_EXCEPTION;
    JSValue obj = JS_NewObject(ctx);
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_streams.find(make_stream_id(windowId, k));
    if(it != g_streams.end())
    {
        auto* s = it->second.get();
        JS_SetPropertyStr(ctx, obj, "width", JS_NewInt32(ctx, s->width));
        JS_SetPropertyStr(ctx, obj, "height", JS_NewInt32(ctx, s->height));
        if(s->fmt && s->fmt->duration > 0)
        {
            double sec = (double)s->fmt->duration / (double)AV_TIME_BASE;
            JS_SetPropertyStr(ctx, obj, "duration", JS_NewFloat64(ctx, sec));
        }
    }
    JS_FreeCString(ctx, k);
    return obj;
}

static int ffmpeg_module_init(JSContext* ctx, JSModuleDef* m)
{
    JS_SetModuleExport(ctx, m, "load", JS_NewCFunction(ctx, js_load, "load", 3));
    JS_SetModuleExport(ctx, m, "stop", JS_NewCFunction(ctx, js_stop, "stop", 2));
    JS_SetModuleExport(ctx, m, "play", JS_NewCFunction(ctx, js_play, "play", 2));
    JS_SetModuleExport(ctx, m, "pause", JS_NewCFunction(ctx, js_pause, "pause", 2));
    JS_SetModuleExport(ctx, m, "details", JS_NewCFunction(ctx, js_details, "details", 2));
    return 0;
}

// render_begin hook: update active streams
static void on_render_begin(void* data)
{
    const KoyaRenderBeginDataV1* rb = reinterpret_cast<const KoyaRenderBeginDataV1*>(data);
    if(!rb || !g_renderer) return;
    // Collect keys for this window
    std::vector<std::string> keys;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        for(const auto& kv : g_streams)
        {
            if(kv.second->windowId == rb->windowId && kv.second->running) keys.push_back(kv.first);
        }
    }
    const double slack = 0.0;
    for(const auto& key : keys)
    {
        VideoStream* s = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            auto it = g_streams.find(key);
            if(it == g_streams.end()) continue;
            s = it->second.get();
        }
        if(!s || !s->playing.load()) continue;
        std::lock_guard<std::mutex> lk2(s->qmutex);
        const std::uint64_t v = s->latestVersion.load(std::memory_order_acquire);
        if(v == s->lastConsumedVersion || s->latestRgba.empty()) continue;
        g_renderer->update_texture_rgba((void*)g_renderer->ctx, s->windowId, s->key.c_str(), s->latestRgba.data(), s->width*4, s->width, s->height, 0);
        s->lastConsumedVersion = v;
    }
}

// Forward declaration for cleanup hook
static void ffmpeg_cleanup(void* /*data*/);

extern "C" {
JSModuleDef* integrateV1(JSContext* ctx, const char* module_name, RegisterHookFunc registerHook, const KoyaRendererV1* renderer)
{
    g_ctx = ctx; g_renderer = renderer;
    registerHook("render_begin", on_render_begin);
    registerHook("cleanup", ffmpeg_cleanup);
    JSModuleDef* m = JS_NewCModule(ctx, module_name, ffmpeg_module_init);
    if(!m) return nullptr;
    JS_AddModuleExport(ctx, m, "load");
    JS_AddModuleExport(ctx, m, "stop");
    JS_AddModuleExport(ctx, m, "play");
    JS_AddModuleExport(ctx, m, "pause");
    JS_AddModuleExport(ctx, m, "details");
    return m;
}
}

// --- Cleanup hook to stop all streams safely on engine shutdown ---
static void ffmpeg_cleanup(void* /*data*/)
{
    // Move the map out under lock to avoid holding the mutex while joining/closing
    std::map<std::string, std::unique_ptr<VideoStream>> local;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        local.swap(g_streams);
    }
    // Helper to stop and free a single stream
    auto stop_stream = [] (std::unique_ptr<VideoStream>& s) {
        if(!s) return;
        s->running.store(false);
        if(s->worker.joinable()) s->worker.join();
        if(s->sws) { sws_freeContext(s->sws); s->sws = nullptr; }
        if(s->codec) { avcodec_free_context(&s->codec); }
        if(s->fmt) { avformat_close_input(&s->fmt); }
        if(s->usingMemAvio)
        {
            if(s->avio) {
                if(s->avio->buffer) av_free(s->avio->buffer);
                avio_context_free(&s->avio);
            }
            if(s->avioOpaque) { std::free(s->avioOpaque); s->avioOpaque = nullptr; }
            if(s->assetBuf && g_renderer && g_renderer->asset_free_buffer)
            {
                g_renderer->asset_free_buffer((void*)g_renderer->ctx, s->assetBuf);
                s->assetBuf = nullptr; s->assetSize = 0;
            }
        }
        s.reset();
    };
    for(auto& kv : local) stop_stream(kv.second);
}
