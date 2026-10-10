#pragma once
// ============================================================
// RenderHook.h — DXGI/D3D 呈现链挂钩 + ImGui 覆盖层
//
// 通过创建一次性"哑设备"取得 IDXGISwapChain::Present / Present1 /
// ResizeBuffers 与 ID3D12CommandQueue::ExecuteCommandLists 的真实地址
// （vtable 索引：Present=8、ResizeBuffers=13、Present1=18、ECL=10），
// 用 Detours 挂钩。首次 Present 时检测游戏图形 API（本作为 D3D12）
// 并初始化 ImGui（1.92，D3D12 后端；D3D11 为兼容回退）。
// ============================================================
namespace RenderHook
{
    bool Install();   // worker 线程调用（需 dxgi.dll 已加载）
    void Shutdown();  // 卸载前调用：恢复 WndProc、摘钩、释放资源
    void GetGameWindowSize(float* w, float* h); // 游戏客户区尺寸（渲染线程调用）
    bool GameCursorClientPos(float* outX, float* outY); // 系统光标在游戏客户区的坐标（不走 ImGui IO）
}

// ---- 游戏画面捕获（左下角缩略图用；DX11/DX12 通用）----
// SetCaptureWanted：渲染线程每帧设置是否拷贝后缓冲（关闭时不产生 GPU 开销）
// GetCaptureTex    ：取"不含覆盖层"的游戏画面贴图句柄（ImGui 用），未就绪返回 nullptr
void  RenderHook_SetCaptureWanted(bool on);
void* RenderHook_GetCaptureTex(int* w, int* h);
// SetRecordWanted：游戏原生画面（未叠加覆盖层）录制回读开关；
//   开启后 RenderHook 把后缓冲拷进 staging 环并回读给 GameRecorder（DO_NOT_WAIT，不阻塞渲染）
void  RenderHook_SetRecordWanted(bool on);

// ---- 皮肤贴图信息（自然尺寸 + 非透明包围盒；带缓存，可每帧调用）----
//   Malody 的 note/hold 贴图常带透明留白（例：notex-1.png 420x400，可见部分仅顶部 185px），
//   整屏渲染要按可见区域裁剪，故需要 alpha 包围盒。
//   返回 false = 解码失败。
bool RenderHook_ImageInfo(const char* path, int* w, int* h,
                          int* bx, int* by, int* bw, int* bh);

// ---- 内嵌资源贴图（LOGO 等，资源以 RCDATA 编进 DLL，运行时 WIC 内存解码）----
//   resId：.rc 里 RCDATA 的 ID；key：缓存键（同一 key 只解码/上传一次）。
//   图形 API / 解码未就绪时返回 nullptr，调用方可下一帧重试。
void* RenderHook_LoadEmbeddedPng(int resId, const char* key);
