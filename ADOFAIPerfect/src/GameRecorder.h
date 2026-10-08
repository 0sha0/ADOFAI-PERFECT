#pragma once
// ============================================================
// GameRecorder.h — 游戏原生窗口录制（Media Foundation / H.264 MP4）
//   · 画面来源：RenderHook 在 Present 时先拷走的"未叠加本工具 UI"的后缓冲
//     （与左下角小窗同源，保证录到的是游戏原生画面，不含辅助工具覆盖层）
//   · 线程模型：渲染线程只做 GPU→CPU 拷出 + 入队（队列满即丢帧，绝不阻塞渲染）；
//               编码线程独立跑 MF SinkWriter（H.264，支持硬件编码 MFT）
//   · 输出目录/帧率/码率来自 adofai_perfect.cfg（rec_dir / rec_fps / rec_mbps）
// ============================================================
namespace GameRecorder
{
    bool Start(const char* dir, int fps, int mbps);   // 开始录制（首帧到达时创建编码器）
    void Stop();                                      // 结束录制并写完 MP4 尾部
    bool Active();
    void PushPixels(const void* data, int w, int h, int stride);   // BGRA 顶行优先
    // swapRB=true：来源是 DXGI R8G8B8A8（RGBA），编码线程写入时交换 R/B
    void PushPixels(const void* data, int w, int h, int stride, bool swapRB);
    void GetStats(int* framesIn, int* framesWritten, int* dropped, double* seconds);
    const char* CurrentFile();                        // 当前/最近一次输出文件（无则 ""）
}
