#pragma once
// ============================================================
// StreamMode.h — 直播模式（防采集覆盖层 / stream-proof overlay）
//
//   需求：直播推送软件（OBS / 哔哩哔哩直播姬 / 各种录制软件）在推流或录像时，
//   可以**选择性地不显示**本工具的某些元素（IMGUI 菜单界面 / 一般轨道辅助 /
//   辅助读谱 / 按键反馈），但"不显示"并不是真的不显示 ——
//   **人眼在显示器上仍然能看到，只有采集端看不到**；同时开启后游戏画面左上角
//   会有一枚 "ADOFAI-PERFECT" 水印（水印是给观众看的，属于"采集里能看到"）。
//
//   实现原理：
//     · 把"受保护"的元素画到一块**独立的覆盖窗口**上，并调用
//       SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE) ——
//       该窗口被 Windows 排除在屏幕捕获之外（窗口捕获 / 游戏捕获 / 显示器捕获
//       全部看不到），但人眼在显示器上照常可见（置顶 / 每像素透明 / 鼠标穿透）。
//     · 游戏后缓冲里不再包含这些元素，采集软件抓到的就是"干净"的画面。
//     · 覆盖窗口优先用 DirectComposition + flip 模型交换链（GPU 直出，零额外延迟）；
//       若该路径不可用（个别驱动/设备不支持 BGRA 合成），自动退回
//       "离屏渲染 + 回读 + 分层窗口"（兼容性优先，延迟约 1 帧）。
//
//   本文件只负责"画面"，不改动任何玩法逻辑。
// ============================================================
struct ImDrawData;

namespace StreamMode
{
    // 受保护元素种类（与 UI 上的开关一一对应）
    enum Elem { EL_MENU = 0, EL_TRACK, EL_READ, EL_KV, EL_N };

    // ---- 开关（持久化在 adofai_perfect.cfg）----
    bool Enabled();                       void SetEnabled(bool on);
    bool Hide(Elem e);                    void SetHide(Elem e, bool on);
    const char* ElemName(Elem e);         // 本地化后的名字（用于 UI 列表）

    // ---- 渲染线程：每帧生命周期 ----
    void BeginFrame();                                 // ImGui::NewFrame() 之后立刻调用（清空登记表）
    // 登记一个 ImGui 窗口名（含其所有子窗口/卡片）为"受保护元素"：
    //   ImGui 会给子窗口起名 "<父窗口名>/<子名>"，因此按名字前缀即可连同卡片一起隐藏。
    //   必须在 BeginFrame() 与 FilterForGame() 之间、该窗口 Begin 之后调用。
    void MarkWindow(const char* windowName, Elem e);
    // 把整帧 draw data 拆成"给游戏后缓冲"的部分（受保护项被剔除）。
    // 直播模式关闭 / 覆盖窗口不可用时原样返回 src（= 不做任何隐藏）。
    ImDrawData* FilterForGame(ImDrawData* src);
    // 记录/绘制"受保护部分"到覆盖窗口：
    //   d3d12CmdList != nullptr 时记录进该命令列表（D3D12）；
    //   为 nullptr 时用 D3D11 立即上下文直接画（D3D11）。
    void RenderOverlay(ImDrawData* src, void* d3d12CmdList);
    void AfterExecute();                               // D3D12：ExecuteCommandLists 之后调用

    // ---- 水印（画在游戏画面上；仅直播模式开启时）----
    void DrawWatermark();

    // ---- 生命周期（RenderHook 调用）----
    // backbufferFormat = 游戏后缓冲的 DXGI_FORMAT：D3D12 下覆盖层必须与 ImGui 后端
    // 创建 PSO 时所用的 RTVFormat 一致，否则通道错位（红蓝互换）。0 = 未知。
    void Attach(void* dev11, void* ctx11, void* dev12, void* queue12, void* srvHeap12,
                unsigned backbufferFormat);
    // gameWindowed = 游戏交换链是否窗口化；独占全屏时覆盖层不可能显示在游戏之上，
    // 此时会**拒绝隐藏**（人眼优先：宁可直播能看到，也不能让人眼看不到）。
    void SetGameWindow(void* hwnd, float w, float h, bool gameWindowed = true);
    void Shutdown();
    bool OverlayReady();                  // 覆盖窗口当前是否真的可用
    const char* StatusText();             // 诊断（设置页显示）

    // ---- 配置档案（整包保存 / 读取） ----
    int   SettingGet(int which);          // 0=on 1=menu 2=track 3=read 4=kv
    void  SettingSet(int which, int v);
    void  ConfigSave();
    void  ConfigLoadOnce();
}
