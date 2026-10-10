// ============================================================
// GameRecorder.cpp — Media Foundation 录制实现
//   队列 / 缓冲池全部在工作线程之外只做"取缓冲→拷行→入队"，
//   编码在工作线程（CoInitializeEx + MFStartup 都在该线程，避免 COM 套间问题）。
// ============================================================
#include "GameRecorder.h"
#include "Log.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <icodecapi.h>
#include <codecapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdarg>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "strmiids.lib")

namespace GameRecorder
{
    namespace
    {
        std::mutex                        g_mx;
        std::condition_variable           g_cv;
        std::deque<std::vector<uint8_t>>  g_q;
        std::vector<std::vector<uint8_t>> g_pool;
        std::thread                       g_th;
        std::atomic<bool>                 g_run{ false };
        std::atomic<bool>                 g_active{ false };
        int                               g_w = 0, g_h = 0;
        int                               g_fps = 60, g_mbps = 20;
        char                              g_dir[MAX_PATH * 2] = { 0 };
        char                              g_file[MAX_PATH * 2] = { 0 };
        std::mutex                        g_errMx;
        char                              g_lastErr[256] = { 0 };   // 最近一次失败原因（UI 直接显示）
        static void SetErr(const char* fmt, ...)
        {
            char b[256];
            va_list ap; va_start(ap, fmt);
            vsnprintf(b, sizeof(b), fmt, ap);
            va_end(ap);
            std::lock_guard<std::mutex> lk(g_errMx);
            snprintf(g_lastErr, sizeof(g_lastErr), "%s", b);
        }
        std::atomic<int>                  g_framesIn{ 0 }, g_framesOut{ 0 }, g_dropped{ 0 };
        std::atomic<double>               g_seconds{ 0.0 };
        double                            g_t0 = 0.0;
        bool                              g_flip = true;    // true = 按 DIB 底行优先写入
        std::atomic<bool>                 g_swapRB{ false };  // 来源为 RGBA：写入时交换 R/B
        LONGLONG                          g_t0Qpc = 0;      // 与 g_t0 同一时刻的 QPC（音频时间基准）

        // ---------- 音频：WASAPI loopback（默认播放设备的回环）→ AAC ----------
        // 之前只有画面没声音：游戏音频走的就是默认播放端点，抓回环即可，
        // 转 PCM16 后按 QPC 时间戳写进同一个 MP4（MF SinkWriter 的第 2 条流）。
        std::atomic<bool>         g_aRun{ false };
        std::thread               g_aTh;
        std::mutex                g_aMx;
        std::condition_variable   g_aCv;
        struct AudioPkt { std::vector<int16_t> pcm; LONGLONG qpc = 0; };
        std::deque<AudioPkt>      g_aQ;
        size_t                    g_aQBytes = 0;
        std::mutex                g_fmtMx;
        std::condition_variable   g_fmtCv;
        bool                      g_aFmtDone = false;   // 格式已确定（成功或失败）
        bool                      g_aFmtOk = false;
        int                       g_aRate = 0;          // 源采样率
        int                       g_aCh = 0;            // 源声道数
        int                       g_aBits = 0;
        bool                      g_aFloat = false;

        static LONGLONG QpcFreq()
        {
            static LONGLONG f = 0;
            if (!f) { QueryPerformanceFrequency((LARGE_INTEGER*)&f); if (!f) f = 1; }
            return f;
        }
        static LONGLONG QpcNow()
        {
            LONGLONG q = 0;
            QueryPerformanceCounter((LARGE_INTEGER*)&q);
            return q;
        }
        static double NowSec() { return (double)QpcNow() / (double)QpcFreq(); }

        // 源数据 → 立体声 PCM16（ch0/ch1；单声道则复制）
        static void AqConvert(const BYTE* data, UINT32 frames, bool silent, std::vector<int16_t>* out)
        {
            const int ch = g_aCh, bits = g_aBits;
            const bool fl = g_aFloat;
            const size_t n = (size_t)frames * 2;
            out->assign(n, 0);
            if (silent || !data || ch <= 0) return;
            auto f2s = [](float v) -> int16_t {
                if (v > 1.f) v = 1.f; else if (v < -1.f) v = -1.f;
                return (int16_t)(v * 32767.f);
            };
            for (UINT32 i = 0; i < frames; i++)
            {
                int16_t v[2] = { 0, 0 };
                for (int c = 0; c < 2; c++)
                {
                    const int sc = (ch == 1) ? 0 : c;
                    if (sc >= ch) { v[c] = v[0]; continue; }
                    const BYTE* p = data + ((size_t)i * ch + sc) * (bits / 8);
                    if (bits == 32 && fl)        v[c] = f2s(*(const float*)p);
                    else if (bits == 32)         v[c] = (int16_t)(*(const int32_t*)p >> 16);
                    else if (bits == 16)         v[c] = *(const int16_t*)p;
                    else if (bits == 24)
                    {
                        const int32_t s = ((int32_t)p[2] << 16) | ((int32_t)p[1] << 8) | p[0];
                        v[c] = (int16_t)((s << 8) >> 16);
                    }
                    else v[c] = 0;
                }
                (*out)[(size_t)i * 2] = v[0];
                (*out)[(size_t)i * 2 + 1] = v[1];
            }
        }

        static void AqPush(const std::vector<int16_t>& pcm, LONGLONG qpc)
        {
            std::lock_guard<std::mutex> lk(g_aMx);
            // 上限 ~8s（2ch/16bit/48k ≈ 1.5MB）：编码器跟不上时丢最旧的，
            // 宁可音频丢一点也不能吃满内存。
            const size_t cap = (size_t)g_aRate * 2 * 2 * 8;
            while (g_aQBytes > cap && !g_aQ.empty())
            {
                g_aQBytes -= g_aQ.front().pcm.size() * sizeof(int16_t);
                g_aQ.pop_front();
            }
            AudioPkt p; p.pcm = pcm; p.qpc = qpc;
            g_aQBytes += p.pcm.size() * sizeof(int16_t);
            g_aQ.push_back(std::move(p));
            g_aCv.notify_one();
            g_cv.notify_one();     // 编码线程等的也是"队列非空"，音频到达也要唤醒它
        }

        static void AqSetFormat(int rate, int ch, int bits, bool isFloat)
        {
            {
                std::lock_guard<std::mutex> lk(g_fmtMx);
                g_aRate = rate; g_aCh = ch; g_aBits = bits; g_aFloat = isFloat;
                g_aFmtOk = (rate > 0 && ch > 0 && bits >= 16);
                g_aFmtDone = true;
            }
            g_fmtCv.notify_all();
        }

        static void EnsureDir(const char* dir)
        {
            // 逐级建目录；用宽字符版本，非 ASCII（中文/日文）目录名也能正确创建。
            wchar_t tmp[MAX_PATH * 2] = { 0 };
            if (MultiByteToWideChar(CP_UTF8, 0, dir, -1, tmp, MAX_PATH * 2) <= 0) return;
            for (wchar_t* p = tmp; *p; p++)
            {
                if (*p == L'\\' || *p == L'/')
                {
                    const wchar_t c = *p; *p = 0;
                    if (tmp[0] && !(tmp[1] == L':' && !tmp[2]))
                        CreateDirectoryW(tmp, nullptr);
                    *p = c;
                }
            }
            CreateDirectoryW(tmp, nullptr);
        }

        // WASAPI 回环采集线程：拿到混音格式 → 循环读包 → 转 PCM16 → 入队
        static void AudioLoop()
        {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            IMMDeviceEnumerator* en = nullptr;
            IMMDevice* dev = nullptr;
            IAudioClient* ac = nullptr;
            IAudioCaptureClient* cc = nullptr;
            WAVEFORMATEX* wf = nullptr;
            bool ready = false;
            HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                          IID_PPV_ARGS(&en));
            if (SUCCEEDED(hr) && en &&
                SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) && dev &&
                SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) && ac &&
                SUCCEEDED(ac->GetMixFormat(&wf)) && wf)
            {
                // 回环只支持共享模式，使用引擎默认周期
                hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                    0, 0, wf, nullptr);
                if (SUCCEEDED(hr) &&
                    SUCCEEDED(ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cc)) && cc)
                {
                    // wFormatTag：3 = IEEE_FLOAT，0xFFFE = EXTENSIBLE（共享混音几乎总是 float32）
                    const bool isFloat = (wf->wBitsPerSample == 32) &&
                                         (wf->wFormatTag == 3 || wf->wFormatTag == 0xFFFE);
                    AqSetFormat((int)wf->nSamplesPerSec, (int)wf->nChannels,
                                (int)wf->wBitsPerSample, isFloat);
                    ready = true;
                }
            }
            if (!ready)
            {
                AqSetFormat(0, 0, 0, false);      // 明确失败：编码器只写视频，不再等
                Log::Printf("[rec] audio: loopback unavailable (hr=0x%08X)", (unsigned)hr);
            }
            if (wf) { CoTaskMemFree(wf); wf = nullptr; }

            std::vector<int16_t> pcm;
            if (ready)
            {
                Log::Printf("[rec] audio: loopback %dHz %dch %dbit %s", g_aRate, g_aCh, g_aBits,
                            g_aFloat ? "f32" : "pcm");
                if (SUCCEEDED(ac->Start()))
                {
                    while (g_aRun.load(std::memory_order_relaxed))
                    {
                        UINT32 pkt = 0;
                        if (FAILED(cc->GetNextPacketSize(&pkt))) break;
                        if (pkt == 0) { Sleep(4); continue; }
                        BYTE* data = nullptr;
                        UINT32 frames = 0;
                        DWORD  flags = 0;
                        UINT64 qpc = 0;
                        if (FAILED(cc->GetBuffer(&data, &frames, &flags, nullptr, &qpc))) break;
                        if (frames)
                        {
                            AqConvert(data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0, &pcm);
                            if (!pcm.empty()) AqPush(pcm, (LONGLONG)qpc);
                        }
                        cc->ReleaseBuffer(frames);
                    }
                    ac->Stop();
                }
                else Log::Printf("[rec] audio: Start failed");
            }
            if (cc) cc->Release();
            if (ac) ac->Release();
            if (dev) dev->Release();
            if (en) en->Release();
            CoUninitialize();
        }

        struct Enc
        {
            IMFSinkWriter* writer = nullptr;
            DWORD          stream = 0;
            DWORD          aStream = 0;
            bool           aOk = false;        // AAC 流是否可用
            bool           ok = false;
            int            cw = 0, ch = 0;
            LONGLONG       frameDur = 166667;
            unsigned       idx = 0;
            int            writeFail = 0;
            bool           fatal = false;      // 编码器彻底坏了：停止录制而不是无限重试
            bool           firstWriteLogged = false;
            bool           reqSw = false;      // 要求使用软件编码器（硬件编码器建流/出帧失败后的兜底）

            bool Open(int w, int h, double t0, int aRate)
            {
                if (writer) { writer->Release(); writer = nullptr; }   // 清掉上一次失败的残留
                SYSTEMTIME st; GetLocalTime(&st);
                // 文件名带自增段号：开始 / 暂停 / 继续 会重开编码器，只用"秒级时间戳"
                // 会撞名 → SinkWriter 打不开文件（表现为"只涨 in 不涨 written"）。
                static unsigned s_seg = 0;
                snprintf(g_file, sizeof(g_file), "%s\\ADOFAI_PERFECT_%04d%02d%02d_%02d%02d%02d_%u.mp4",
                         g_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                         ++s_seg);
                EnsureDir(g_dir);
                wchar_t wpath[MAX_PATH * 2];
                wpath[0] = 0;
                if (MultiByteToWideChar(CP_UTF8, 0, g_file, -1, wpath, MAX_PATH * 2) <= 0)
                {
                    SetErr("path conversion failed (dir too long?)");
                    Log::Printf("[rec] path conversion failed for '%s'", g_file);
                    return false;
                }

                IMFAttributes* attrs = nullptr;
                if (FAILED(MFCreateAttributes(&attrs, 3)))
                {
                    SetErr("MFCreateAttributes failed");
                    return false;
                }
                // 硬件编码器（GPU 变体）优先；若建流失败或长时间不出帧，自动回退软件编码器
                // （GPU 被游戏占用时硬件 H.264 MFT 会卡住 → 表现为"只涨 in 不涨 written"）。
                attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, reqSw ? FALSE : TRUE);
                attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
                HRESULT hr = MFCreateSinkWriterFromURL(wpath, nullptr, attrs, &writer);
                attrs->Release();
                if (FAILED(hr) || !writer)
                {
                    Log::Printf("[rec] sink writer failed 0x%08X", (unsigned)hr);
                    SetErr("sink writer 0x%08X (dir='%s')", (unsigned)hr, g_dir);
                    return false;
                }
                frameDur = 10000000LL / (g_fps > 0 ? g_fps : 60);

                IMFMediaType* out = nullptr;
                MFCreateMediaType(&out);
                out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
                out->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)g_mbps * 1000000u);
                out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                MFSetAttributeSize(out, MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
                MFSetAttributeRatio(out, MF_MT_FRAME_RATE, (UINT32)(g_fps > 0 ? g_fps : 60), 1);
                MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                hr = writer->AddStream(out, &stream);
                out->Release();
                if (FAILED(hr))
                {
                    Log::Printf("[rec] AddStream failed 0x%08X", (unsigned)hr);
                    SetErr("video AddStream 0x%08X (H.264 编码器不可用?)", (unsigned)hr);
                    return false;
                }

                IMFMediaType* in = nullptr;
                MFCreateMediaType(&in);
                in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                MFSetAttributeSize(in, MF_MT_FRAME_SIZE, (UINT32)w, (UINT32)h);
                MFSetAttributeRatio(in, MF_MT_FRAME_RATE, (UINT32)(g_fps > 0 ? g_fps : 60), 1);
                MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                hr = writer->SetInputMediaType(stream, in, nullptr);
                in->Release();
                if (FAILED(hr))
                {
                    Log::Printf("[rec] SetInputMediaType failed 0x%08X", (unsigned)hr);
                    SetErr("video SetInputMediaType 0x%08X (RGB32 不被编码器接受?)", (unsigned)hr);
                    return false;
                }

                (void)t0;
                // ---- 音频流（AAC）：拿到回环采样率才加，失败就退回纯视频 ----
                if (aRate > 0)
                {
                    IMFMediaType* ao = nullptr;
                    if (SUCCEEDED(MFCreateMediaType(&ao)) && ao)
                    {
                        ao->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                        ao->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
                        ao->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                        ao->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)aRate);
                        ao->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                        ao->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);   // 192kbps
                        ao->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
                        ao->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);  // AAC-LC
                        if (SUCCEEDED(writer->AddStream(ao, &aStream)))
                        {
                            IMFMediaType* ai = nullptr;
                            if (SUCCEEDED(MFCreateMediaType(&ai)) && ai)
                            {
                                ai->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                                ai->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                                ai->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                                ai->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)aRate);
                                ai->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
                                ai->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
                                ai->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)aRate * 4);
                                ai->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
                                aOk = SUCCEEDED(writer->SetInputMediaType(aStream, ai, nullptr));
                                ai->Release();
                            }
                        }
                        ao->Release();
                    }
                    Log::Printf("[rec] audio stream: aac %dHz 2ch -> %s", aRate, aOk ? "ok" : "FAILED");
                }
                // IMF SinkWriter 要求所有流都在 BeginWriting 之前 AddStream，否则
                // muxer 直接丢弃后加的音轨（这正是"只有画面没声音"的根因）。
                hr = writer->BeginWriting();
                if (FAILED(hr))
                {
                    Log::Printf("[rec] BeginWriting failed 0x%08X", (unsigned)hr);
                    SetErr("BeginWriting 0x%08X", (unsigned)hr);
                    return false;
                }

                // 码率控制 / 关键帧间隔（失败不影响录制）
                ICodecAPI* api = nullptr;
                IMFGetService* svc = nullptr;
                if (SUCCEEDED(writer->QueryInterface(IID_PPV_ARGS(&svc))) && svc)
                {
                    svc->GetService(MF_SINK_WRITER_ENCODER_CONFIG, IID_PPV_ARGS(&api));
                    svc->Release();
                }
                if (api)
                {
                    VARIANT v; VariantInit(&v);
                    v.vt = VT_UI4; v.ulVal = eAVEncCommonRateControlMode_PeakConstrainedVBR;
                    api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
                    v.ulVal = (ULONG)g_mbps * 1000000u;
                    api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &v);
                    v.ulVal = (ULONG)g_mbps * 1500000u;
                    api->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &v);
                    v.ulVal = (ULONG)((g_fps > 0 ? g_fps : 60) * 2);
                    api->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
                    VariantClear(&v);
                    api->Release();
                }
                ok = true;
                cw = w; ch = h;
                SetErr("");
                Log::Printf("[rec] started %dx%d @%dfps %dMbps audio=%d -> %s", w, h, g_fps, g_mbps,
                            aOk ? 1 : 0, g_file);
                return true;
            }

            void Close()
            {
                if (writer)
                {
                    writer->Finalize();
                    writer->Release();
                    writer = nullptr;
                }
                aStream = 0;
                aOk = false;
                ok = false;
            }

            void Write(const std::vector<uint8_t>& f, int w, int h, double tNow)
            {
                if (!ok) return;
                const DWORD len = (DWORD)((size_t)w * 4 * h);
                IMFSample* sample = nullptr;
                IMFMediaBuffer* buf = nullptr;
                if (FAILED(MFCreateSample(&sample))) return;
                if (FAILED(MFCreateMemoryBuffer(len, &buf)))
                {
                    sample->Release();
                    return;
                }
                BYTE* dst = nullptr;
                if (SUCCEEDED(buf->Lock(&dst, nullptr, nullptr)) && dst)
                {
                    const bool sw = g_swapRB.load(std::memory_order_relaxed);
                    if (g_flip || sw)
                    {
                        const size_t row = (size_t)w * 4;
                        for (int y = 0; y < h; y++)
                        {
                            const uint8_t* s = f.data() + (size_t)(g_flip ? (h - 1 - y) : y) * row;
                            uint8_t* d = dst + (size_t)y * row;
                            if (sw)
                            {
                                for (size_t i = 0; i + 4 <= row; i += 4)
                                {
                                    const uint8_t b0 = s[i], b2 = s[i + 2];
                                    d[i] = b2; d[i + 1] = s[i + 1]; d[i + 2] = b0; d[i + 3] = s[i + 3];
                                }
                            }
                            else memcpy(d, s, row);
                        }
                    }
                    else
                        memcpy(dst, f.data(), len);
                    buf->Unlock();
                }
                buf->SetCurrentLength(len);
                sample->AddBuffer(buf);
                LONGLONG rt = (LONGLONG)((tNow - g_t0) * 10000000.0);
                if (rt < 0) rt = 0;
                sample->SetSampleTime(rt);
                sample->SetSampleDuration(frameDur);
                const HRESULT whr = writer->WriteSample(stream, sample);
                if (FAILED(whr))
                {
                    if (!firstWriteLogged)
                    {
                        firstWriteLogged = true;
                        Log::Printf("[rec] first WriteSample failed 0x%08X (w=%d h=%d len=%u)",
                                    (unsigned)whr, w, h, (unsigned)len);
                    }
                    // 写样本失败（编码器/磁盘问题）：累计到一定次数就停，别让它假装在录
                    if (++writeFail == 30)
                    {
                        SetErr("WriteSample failed 0x%08X (disk full? encoder lost?)", (unsigned)whr);
                        Log::Printf("[rec] WriteSample failed 30x (0x%08X) -> abort", (unsigned)whr);
                        fatal = true;
                    }
                }
                else
                {
                    if (!firstWriteLogged)
                    {
                        firstWriteLogged = true;
                        Log::Printf("[rec] first frame written (w=%d h=%d len=%u rt=%lld)",
                                    w, h, (unsigned)len, (long long)rt);
                    }
                    writeFail = 0;
                }
                sample->Release();
                buf->Release();
                idx++;
                g_framesOut.fetch_add(1, std::memory_order_relaxed);
            }

            // 写一包音频（PCM16 立体声）；时间戳用采集时刻的 QPC 换算，和视频同一基准
            void WriteAudio(const AudioPkt& p)
            {
                if (!ok || !aOk || p.pcm.empty() || g_aRate <= 0) return;
                // 录制起点（g_t0Qpc）之前采到的回环包不属于本次录制：直接丢弃，
                // 否则它们会被夹到 t=0，开头一坨音频挤在一起且和视频对不上。
                if (g_t0Qpc != 0 && p.qpc < g_t0Qpc) return;
                const DWORD len = (DWORD)(p.pcm.size() * sizeof(int16_t));
                IMFSample* sample = nullptr;
                IMFMediaBuffer* buf = nullptr;
                if (FAILED(MFCreateSample(&sample))) return;
                if (FAILED(MFCreateMemoryBuffer(len, &buf)))
                {
                    sample->Release();
                    return;
                }
                BYTE* dst = nullptr;
                if (SUCCEEDED(buf->Lock(&dst, nullptr, nullptr)) && dst)
                {
                    memcpy(dst, p.pcm.data(), len);
                    buf->Unlock();
                }
                buf->SetCurrentLength(len);
                sample->AddBuffer(buf);
                LONGLONG rt = (LONGLONG)((double)(p.qpc - g_t0Qpc) * 10000000.0 / (double)QpcFreq());
                if (rt < 0) rt = 0;
                const LONGLONG dur = (LONGLONG)((double)(p.pcm.size() / 2) * 10000000.0 / (double)g_aRate);
                sample->SetSampleTime(rt);
                sample->SetSampleDuration(dur > 0 ? dur : 1);
                writer->WriteSample(aStream, sample);
                sample->Release();
                buf->Release();
            }
        };

        static void Loop()
        {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            MFStartup(MF_VERSION, MFSTARTUP_LITE);
            Enc enc;
            int w = 0, h = 0;
            int openFail = 0;      // 连续建流失败次数（超过阈值就放弃，避免"0/N 永远不涨"）
            int sinceOpen = 0;     // 本次建流以来已投喂的帧数（用于"长时间不出帧"检测）
            // 音频按 QPC 时间戳写：一帧视频清一次队列（MP4 时间戳显式，允许突发）
            auto DrainAudio = [&enc]() {
                if (!enc.ok || !enc.aOk) return;
                for (int guard = 0; guard < 256; guard++)
                {
                    AudioPkt ap;
                    {
                        std::lock_guard<std::mutex> lk(g_aMx);
                        if (g_aQ.empty()) break;
                        ap = std::move(g_aQ.front());
                        g_aQ.pop_front();
                        g_aQBytes -= ap.pcm.size() * sizeof(int16_t);
                    }
                    enc.WriteAudio(ap);
                }
            };
            for (;;)
            {
                std::vector<uint8_t> f;
                double tNow = 0.0;
                bool haveFrame = false;
                {
                    std::unique_lock<std::mutex> lk(g_mx);
                    g_cv.wait_for(lk, std::chrono::milliseconds(20), [] {
                        if (!g_run.load()) return true;
                        if (!g_q.empty()) return true;
                        std::lock_guard<std::mutex> lk2(g_aMx);
                        return !g_aQ.empty();
                    });
                    if (g_q.empty())
                    {
                        if (!g_run.load())
                        {
                            DrainAudio();              // 收尾：剩余音频写完再退出
                            break;
                        }
                    }
                    else
                    {
                        f.swap(g_q.front());
                        g_q.pop_front();
                        tNow = NowSec();
                        haveFrame = true;
                    }
                }
                if (enc.ok && (g_w != enc.cw || g_h != enc.ch))
                    enc.Close();                       // 窗口尺寸变化：重开编码器
                if (!enc.ok)
                {
                    if (!haveFrame) continue;          // 还没有画面：先不建编码器
                    // 建流前等一下回环格式（最长 400ms）：MF 的 MP4 muxer 要在写样本前
                    // 加好所有流，否则音轨会缺失。
                    {
                        std::unique_lock<std::mutex> lk(g_fmtMx);
                        if (!g_aFmtDone)
                            g_fmtCv.wait_for(lk, std::chrono::milliseconds(150), [] { return g_aFmtDone; });
                    }
                    // 以首帧尺寸创建编码器；尺寸变了就忽略旧队列
                    w = g_w; h = g_h;
                    g_t0 = NowSec();
                    g_t0Qpc = QpcNow();
                    if (!enc.Open(w, h, g_t0, g_aFmtOk ? g_aRate : 0))
                    {
                        if (!enc.reqSw && !openFail)
                        {
                            enc.reqSw = true;
                            Log::Printf("[rec] hardware encoder open failed -> retry with software encoder");
                        }
                        if (++openFail >= 30)
                        {
                            Log::Printf("[rec] encoder open failed %d times -> abort recording", openFail);
                            std::unique_lock<std::mutex> lk(g_mx);
                            g_q.clear();
                            g_pool.push_back(std::move(f));
                            g_run.store(false);
                            break;      // 走收尾：g_active=false，UI 立即显示失败原因
                        }
                        std::unique_lock<std::mutex> lk(g_mx);
                        g_q.clear();
                        g_pool.push_back(std::move(f));
                        continue;
                    }
                    openFail = 0;
                    sinceOpen = 0;
                }
                if (haveFrame)
                {
                    // 兜底：硬件编码器建流成功但长时间一帧都不吐（GPU 被游戏抢占会这样）
                    //   → 关掉重开为软件编码器，而不是让用户对着 0/N 干等。
                    const int stallLimit = (g_fps > 0 ? g_fps : 60) * 3;
                    if (enc.ok && enc.idx == 0 && !enc.reqSw && ++sinceOpen > stallLimit)
                    {
                        Log::Printf("[rec] encoder produced no frame in %d -> switch to software encoder",
                                    sinceOpen);
                        enc.reqSw = true;
                        enc.Close();
                        std::lock_guard<std::mutex> lk(g_mx);
                        if (g_pool.size() < 6) g_pool.push_back(std::move(f));
                        continue;
                    }
                    enc.Write(f, w, h, tNow);
                }
                DrainAudio();
                if (enc.fatal)
                {
                    // 编码器已判定失效：清队列走收尾，g_active=false，UI 直接显示原因
                    std::lock_guard<std::mutex> lk(g_mx);
                    g_q.clear();
                    if (g_pool.size() < 6) g_pool.push_back(std::move(f));
                    g_run.store(false);
                    break;
                }
                if (!haveFrame)
                {
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lk(g_mx);
                    if (g_pool.size() < 6) g_pool.push_back(std::move(f));
                }
            }
            enc.Close();
            MFShutdown();
            CoUninitialize();
            g_active.store(false);
            g_run.store(false);
        }
    }   // namespace

    bool Start(const char* dir, int fps, int mbps)
    {
        Stop();
        if (!dir || !dir[0])
        {
            SetErr("no output directory");
            Log::Printf("[rec] start rejected: empty output directory");
            return false;
        }
        // 输出目录规范化：UTF-8 → 宽字符 → 绝对路径 → 回到 UTF-8，并去掉末尾斜杠。
        //   相对路径 / 末尾反斜杠 / 非 ASCII 目录名都可能导致 SinkWriter 打不开文件，
        //   那正是"一直 0/N 不涨"的常见根因，这里一次性归一。
        {
            wchar_t wdir[MAX_PATH * 2] = { 0 };
            if (MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, MAX_PATH * 2) > 0)
            {
                wchar_t wfull[MAX_PATH * 2] = { 0 };
                if (GetFullPathNameW(wdir, MAX_PATH * 2, wfull, nullptr) > 0)
                {
                    char u8[MAX_PATH * 4] = { 0 };
                    if (WideCharToMultiByte(CP_UTF8, 0, wfull, -1, u8, sizeof(u8), nullptr, nullptr) > 0)
                        snprintf(g_dir, sizeof(g_dir), "%s", u8);
                    else
                        snprintf(g_dir, sizeof(g_dir), "%s", dir);
                }
                else
                    snprintf(g_dir, sizeof(g_dir), "%s", dir);
            }
            else
                snprintf(g_dir, sizeof(g_dir), "%s", dir);
            size_t L = strlen(g_dir);
            while (L > 1 && (g_dir[L - 1] == '\\' || g_dir[L - 1] == '/')) g_dir[--L] = 0;
        }
        // 目录可用性检查：建不出来就直接报错，绝不让用户对着 0/N 干等
        EnsureDir(g_dir);
        {
            wchar_t wc[MAX_PATH * 2] = { 0 };
            MultiByteToWideChar(CP_UTF8, 0, g_dir, -1, wc, MAX_PATH * 2);
            const DWORD a = GetFileAttributesW(wc);
            if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY))
            {
                SetErr("output dir unavailable: %s", g_dir);
                Log::Printf("[rec] start rejected: dir unavailable '%s' err=%lu", g_dir, GetLastError());
                return false;
            }
        }
        g_fps = (fps >= 15 && fps <= 240) ? fps : 60;
        g_mbps = (mbps >= 2 && mbps <= 200) ? mbps : 20;
        g_framesIn = g_framesOut = g_dropped = 0;
        g_seconds = 0.0;
        g_w = g_h = 0;
        g_file[0] = 0;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_q.clear();
            g_pool.clear();
        }
        // 音频：先清掉上一轮的队列/格式，再拉起回环采集线程
        {
            std::lock_guard<std::mutex> lk(g_aMx);
            g_aQ.clear();
            g_aQBytes = 0;
        }
        {
            std::lock_guard<std::mutex> lk(g_fmtMx);
            g_aFmtDone = false;
            g_aFmtOk = false;
            g_aRate = g_aCh = g_aBits = 0;
            g_aFloat = false;
        }
        g_t0Qpc = 0;
        SetErr("");
        g_run.store(true);
        g_active.store(true);
        g_aRun.store(true);
        g_aTh = std::thread(AudioLoop);
        g_th = std::thread(Loop);
        return true;
    }

    void Stop()
    {
        if (!g_active.exchange(false) && !g_run.load()) return;
        // 音频线程：先停采集再收编码器（否则 Stop() 之后的包会写进已 Finalize 的 writer）
        g_aRun.store(false);
        g_aCv.notify_all();
        if (g_aTh.joinable())
        {
            HANDLE h = (HANDLE)g_aTh.native_handle();
            if (WaitForSingleObject(h, 1500) == WAIT_OBJECT_0) g_aTh.join();
            else { Log::Printf("[rec] audio thread stuck -> detaching"); g_aTh.detach(); }
        }
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_run.store(false);
            g_cv.notify_all();
        }
        // 有界等待：编码线程若在 MF 收尾里卡住（设备丢失/驱动异常），绝不拖住调用方
        // （渲染线程阻塞会让"退出游戏"看起来像卡死）
        if (g_th.joinable())
        {
            HANDLE h = (HANDLE)g_th.native_handle();
            if (WaitForSingleObject(h, 3000) == WAIT_OBJECT_0)
                g_th.join();
            else
            {
                Log::Printf("[rec] encoder thread stuck in finalize -> detaching");
                g_th.detach();
            }
        }
        Log::Printf("[rec] stopped: in=%d written=%d dropped=%d file='%s'",
                    g_framesIn.load(), g_framesOut.load(), g_dropped.load(), g_file);
    }

    bool Active() { return g_active.load(std::memory_order_relaxed); }

    void PushPixels(const void* data, int w, int h, int stride, bool swapRB)
    {
        if (!g_active.load(std::memory_order_relaxed) || !data || w <= 0 || h <= 0)
            return;
        // H.264 是 4:2:0，宽高必须是偶数：窗口被拖成奇数尺寸时编码器会直接拒绝建流
        // （症状同样是"只涨 in 不涨 written"）。这里统一裁掉最后一行 / 一列，
        // 保证任何窗口尺寸都录得进去。
        const int ew = w & ~1, eh = h & ~1;
        if (ew < 2 || eh < 2) return;
        g_swapRB.store(swapRB, std::memory_order_relaxed);
        const size_t need = (size_t)ew * 4 * eh;
        std::vector<uint8_t> buf;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            if (!g_pool.empty())
            {
                buf.swap(g_pool.back());
                g_pool.pop_back();
            }
            else if (g_q.size() >= 3)
            {
                g_dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        if (buf.size() != need) buf.resize(need);
        {
            const size_t row = (size_t)ew * 4;
            const uint8_t* s = (const uint8_t*)data;
            for (int y = 0; y < eh; y++)
                memcpy(buf.data() + (size_t)y * row, s + (size_t)y * stride, row);
        }
        g_w = ew; g_h = eh;    // 工作线程在写第一帧前读取（Start 后不再改尺寸）
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_q.push_back(std::move(buf));
            g_cv.notify_one();
        }
        g_framesIn.fetch_add(1, std::memory_order_relaxed);
    }

    void GetStats(int* framesIn, int* framesWritten, int* dropped, double* seconds)
    {
        const int fin = g_framesIn.load(std::memory_order_relaxed);
        const int fout = g_framesOut.load(std::memory_order_relaxed);
        if (framesIn)      *framesIn = fin;
        if (framesWritten) *framesWritten = fout;
        if (dropped)       *dropped = g_dropped.load(std::memory_order_relaxed);
        if (seconds)       *seconds = (g_active.load(std::memory_order_relaxed) && g_t0 > 0.0)
                                           ? (NowSec() - g_t0) : 0.0;
        // 采了很多帧却一帧都没进编码器（编码器卡死 / 磁盘不可写）：给出明确原因，
        //   不要让用户对着 "0/N" 干瞪眼。
        if (g_active.load(std::memory_order_relaxed) && fin > 120 && fout == 0)
        {
            std::lock_guard<std::mutex> lk(g_errMx);
            if (!g_lastErr[0])
                snprintf(g_lastErr, sizeof(g_lastErr),
                         "encoder produced no output after %d frames (GPU encoder stalled? disk full?)", fin);
        }
    }

    const char* CurrentFile() { return g_file; }

    // 最近一次失败原因（空串 = 无错误）；UI 在"只涨 in 不涨 written"时直接显示它
    const char* LastError()
    {
        std::lock_guard<std::mutex> lk(g_errMx);
        return g_lastErr;
    }
}
