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
}

// ---- 游戏画面捕获（左下角缩略图用；DX11/DX12 通用）----
// SetCaptureWanted：渲染线程每帧设置是否拷贝后缓冲（关闭时不产生 GPU 开销）
// GetCaptureTex    ：取"不含覆盖层"的游戏画面贴图句柄（ImGui 用），未就绪返回 nullptr
void  RenderHook_SetCaptureWanted(bool on);
void* RenderHook_GetCaptureTex(int* w, int* h);
