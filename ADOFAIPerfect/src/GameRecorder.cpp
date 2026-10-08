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
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstring>

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
        std::atomic<int>                  g_framesIn{ 0 }, g_framesOut{ 0 }, g_dropped{ 0 };
        std::atomic<double>               g_seconds{ 0.0 };
        double                            g_t0 = 0.0;
        bool                              g_flip = true;    // true = 按 DIB 底行优先写入
        std::atomic<bool>                 g_swapRB{ false };  // 来源为 RGBA：写入时交换 R/B

        static void EnsureDir(const char* dir)
        {
            char tmp[MAX_PATH * 2];
            snprintf(tmp, sizeof(tmp), "%s", dir);
            for (char* p = tmp; *p; p++)
            {
                if (*p == '\\' || *p == '/')
                {
                    char c = *p; *p = 0;
                    if (tmp[0] && !(tmp[1] == ':' && !tmp[2]))
                        CreateDirectoryA(tmp, nullptr);
                    *p = c;
                }
            }
            CreateDirectoryA(tmp, nullptr);
        }

        struct Enc
        {
            IMFSinkWriter* writer = nullptr;
            DWORD          stream = 0;
            bool           ok = false;
            int            cw = 0, ch = 0;
            LONGLONG       frameDur = 166667;
            unsigned       idx = 0;

            bool Open(int w, int h, double t0)
            {
                SYSTEMTIME st; GetLocalTime(&st);
                snprintf(g_file, sizeof(g_file), "%s\\ADOFAI_PERFECT_%04d%02d%02d_%02d%02d%02d.mp4",
                         g_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                EnsureDir(g_dir);
                wchar_t wpath[MAX_PATH * 2];
                MultiByteToWideChar(CP_UTF8, 0, g_file, -1, wpath, MAX_PATH * 2);

                IMFAttributes* attrs = nullptr;
                if (FAILED(MFCreateAttributes(&attrs, 3))) return false;
                attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
                attrs->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
                HRESULT hr = MFCreateSinkWriterFromURL(wpath, nullptr, attrs, &writer);
                attrs->Release();
                if (FAILED(hr) || !writer)
                {
                    Log::Printf("[rec] sink writer failed 0x%08X", (unsigned)hr);
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
                if (FAILED(hr)) { Log::Printf("[rec] AddStream failed 0x%08X", (unsigned)hr); return false; }

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
                if (FAILED(hr)) { Log::Printf("[rec] SetInputMediaType failed 0x%08X", (unsigned)hr); return false; }

                hr = writer->BeginWriting();
                if (FAILED(hr)) { Log::Printf("[rec] BeginWriting failed 0x%08X", (unsigned)hr); return false; }

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
                (void)t0;
                ok = true;
                cw = w; ch = h;
                Log::Printf("[rec] started %dx%d @%dfps %dMbps -> %s", w, h, g_fps, g_mbps, g_file);
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
                writer->WriteSample(stream, sample);
                sample->Release();
                buf->Release();
                idx++;
                g_framesOut.fetch_add(1, std::memory_order_relaxed);
            }
        };

        static void Loop()
        {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            MFStartup(MF_VERSION, MFSTARTUP_LITE);
            Enc enc;
            int w = 0, h = 0;
            for (;;)
            {
                std::vector<uint8_t> f;
                double tNow = 0.0;
                {
                    std::unique_lock<std::mutex> lk(g_mx);
                    g_cv.wait(lk, [] { return !g_run.load() || !g_q.empty(); });
                    if (g_q.empty())
                    {
                        if (!g_run.load()) break;
                        continue;
                    }
                    f.swap(g_q.front());
                    g_q.pop_front();
                    tNow = (double)GetTickCount64() / 1000.0;
                }
                if (enc.ok && (g_w != enc.cw || g_h != enc.ch))
                    enc.Close();                       // 窗口尺寸变化：重开编码器
                if (!enc.ok)
                {
                    // 以首帧尺寸创建编码器；尺寸变了就忽略旧队列
                    w = g_w; h = g_h;
                    g_t0 = tNow;
                    if (!enc.Open(w, h, tNow))
                    {
                        std::unique_lock<std::mutex> lk(g_mx);
                        g_q.clear();
                        g_pool.push_back(std::move(f));
                        continue;
                    }
                }
                enc.Write(f, w, h, tNow);
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
        if (!dir || !dir[0]) return false;
        snprintf(g_dir, sizeof(g_dir), "%s", dir);
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
        g_run.store(true);
        g_active.store(true);
        g_th = std::thread(Loop);
        return true;
    }

    void Stop()
    {
        if (!g_active.exchange(false) && !g_run.load()) return;
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
        g_swapRB.store(swapRB, std::memory_order_relaxed);
        const size_t need = (size_t)w * 4 * h;
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
        if (stride == w * 4)
            memcpy(buf.data(), data, need);
        else
        {
            const size_t row = (size_t)w * 4;
            const uint8_t* s = (const uint8_t*)data;
            for (int y = 0; y < h; y++)
                memcpy(buf.data() + (size_t)y * row, s + (size_t)y * stride, row);
        }
        g_w = w; g_h = h;      // 工作线程在写第一帧前读取（Start 后不再改尺寸）
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_q.push_back(std::move(buf));
            g_cv.notify_one();
        }
        g_framesIn.fetch_add(1, std::memory_order_relaxed);
    }

    void GetStats(int* framesIn, int* framesWritten, int* dropped, double* seconds)
    {
        if (framesIn)      *framesIn = g_framesIn.load(std::memory_order_relaxed);
        if (framesWritten) *framesWritten = g_framesOut.load(std::memory_order_relaxed);
        if (dropped)       *dropped = g_dropped.load(std::memory_order_relaxed);
        if (seconds)       *seconds = g_seconds.load(std::memory_order_relaxed);
    }

    const char* CurrentFile() { return g_file; }
}
