// ============================================================
// StreamMode.cpp — 直播模式（防采集覆盖层）实现
//   详见 StreamMode.h 的说明。这里只列关键约定：
//   · 覆盖窗口 = 一块置顶 / 鼠标穿透 / 每像素透明的独立窗口，尺寸与游戏客户区
//     一一对应，坐标系与 ImGui 主视口完全一致（1:1 像素）。
//   · 受保护元素从游戏后缓冲里"剔除"，只画在覆盖窗口上 →
//     采集软件抓游戏画面时看不到它们，人眼在显示器上照常看到。
//   · 水印画在游戏画面上（观众可见）。
//   · 覆盖窗口创建顺序：DirectComposition+flip 交换链（首选）
//     → 分层窗口 + 离屏回读（兜底）→ 都失败则整个直播模式自动禁用（不影响其它功能）。
// ============================================================
#include "StreamMode.h"
#include "Log.h"
#include "Lang.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <atomic>
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dxgi.lib")

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace StreamMode
{
namespace
{
    // ---------------- 开关 ----------------
    std::atomic<bool> g_on{ false };
    std::atomic<bool> g_hide[EL_N] = { {true}, {true}, {true}, {true} };
    std::atomic<bool> g_cfgLoaded{ false };

    // ---------------- 设备 / 游戏窗口 ----------------
    ID3D11Device*        g_dev11   = nullptr;
    ID3D11DeviceContext* g_ctx11   = nullptr;
    ID3D12Device*        g_dev12   = nullptr;
    ID3D12CommandQueue*  g_queue12 = nullptr;
    ID3D12DescriptorHeap* g_srvHeap12 = nullptr;
    unsigned             g_gameFmt = 0;   // 游戏后缓冲 DXGI_FORMAT（0 = 未知）
    HWND  g_gameHwnd = nullptr;
    UINT  g_gw = 0, g_gh = 0;
    bool  g_gameWindowed = true;          // 游戏交换链是否窗口化（独占全屏 = false）
    long long g_frameNo = 0;              // 渲染帧序号（仅渲染线程使用）

    // ---------------- 每帧"受保护窗口"登记表（仅渲染线程） ----------------
    //   登记的是 ImGui 窗口名：ImGui 给子窗口起名 "<父名>/<子名>"，
    //   所以只要 owner 名等于登记名、或以 "登记名/" 开头，就归为同一元素。
    struct RegEntry { std::string name; int elem; };
    std::vector<RegEntry> g_reg;

    // 返回该 draw list 属于哪个受保护元素；不属于任何元素返回 -1
    int ProtectedElemOf(const char* owner)
    {
        if (!owner) return -1;
        const size_t len = strlen(owner);
        for (size_t i = 0; i < g_reg.size(); i++)
        {
            const char* n = g_reg[i].name.c_str();
            const size_t nl = g_reg[i].name.size();
            if (len == nl && memcmp(owner, n, nl) == 0)
                return g_reg[i].elem;
            if (len > nl && owner[nl] == '/' && memcmp(owner, n, nl) == 0)
                return g_reg[i].elem;
        }
        return -1;
    }

    bool IsProtected(ImDrawList* dl)
    {
        if (!dl) return false;
        const int e = ProtectedElemOf(dl->_OwnerName);
        if (e < 0) return false;
        return g_hide[e].load(std::memory_order_relaxed);
    }

    // ---------------- 覆盖窗口 ----------------
    struct Overlay
    {
        HWND  hwnd    = nullptr;
        bool  ok      = false;
        bool  layered = false;      // true = 回读 + 分层窗口路径
        bool  shown   = false;
        UINT  w = 0, h = 0;
        int   x = 0, y = 0;

        // ---- DComp + flip 交换链 ----
        IDXGISwapChain1*      sc = nullptr;
        IDCompositionDevice*  dcomp = nullptr;
        IDCompositionTarget*  target = nullptr;
        IDCompositionVisual*  visual = nullptr;
        UINT  bbCount = 2, bbIdx = 0;
        ID3D12Resource*       bb12[2] = {};
        bool                  bb12RT[2] = {};
        ID3D12DescriptorHeap* rtvHeap = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv12[2] = {};
        ID3D11RenderTargetView* rtv11[2] = {};
        bool  presentPending = false;
        long long lastPresent = -(1LL << 40);   // 最近一次"真的上屏"的帧号
        // ---- 持续隐藏（防"推流里一闪而过"）----
        //   只要覆盖窗口成功创建并上屏过一次，就**不再因为单帧 Present 失败而回退隐藏**：
        //   分层窗口/DComp 视觉都会保留上一次的画面，人眼照常看得到，
        //   而受保护元素绝不会偶发地画回游戏后缓冲（那是推流能抓到的唯一原因）。
        bool      everPresented = false;        // 至少成功上屏过一次
        bool      holdHide = false;             // 覆盖窗口里"还有内容"（即便当前后端暂时不可用）
        bool      failed  = false;              // 首选的两种后端都建不起来
        long long retryAt = 0;                  // 失败后允许重试的帧号

        // ---- 分层窗口回读路径 ----
        // DX11
        ID3D11Texture2D*        off11 = nullptr;
        ID3D11RenderTargetView* off11Rtv = nullptr;
        ID3D11Texture2D*        stg11[2] = {};
        ID3D11Query*            q11[2] = {};
        int  stgIdx = 0;
        // DX12
        ID3D12Resource*  off12 = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE off12Rtv = {};
        ID3D12Resource*  rb12[2] = {};
        ID3D12Fence*     fen = nullptr;
        UINT64           fenVal[2] = {};
        HANDLE           fenEv = nullptr;
        int  rbIdx = 0;
        // 位图
        HDC     dc = nullptr;
        HBITMAP dib = nullptr;
        void*   bits = nullptr;
        int     dibW = 0, dibH = 0;
    };
    Overlay g_ov;

    static ImDrawData s_gameDD;
    static ImDrawData s_protDD;

    // 覆盖层上屏成功：记录帧号。FilterForGame 用它判断"敢不敢隐藏"。
    static void MarkPresented() { g_ov.lastPresent = g_frameNo; g_ov.holdHide = true; }

    // ---- 诊断：为什么这一帧没有把受保护元素从游戏后缓冲里剔除 ----
    //   只在状态切换时打一行（不刷屏）。若日志里反复出现 "hide skipped"，
    //   说明有帧把受保护元素画回了游戏后缓冲 —— 那就是推流"一闪而过"的来源。
    static bool      g_skipLogged = false;
    static long long g_skipFrames = 0;      // 连续未隐藏的帧数
    static void LogSkipReasonOnce(const char* reason)
    {
        g_skipFrames++;
        if (g_skipLogged) return;
        g_skipLogged = true;
        Log::Printf("[stream] hide skipped (%s) - 受保护元素这一帧留在游戏后缓冲", reason);
    }
    static void LogHideOnce()
    {
        if (!g_skipLogged) return;
        g_skipLogged = false;
        Log::Printf("[stream] hide active - 受保护元素已从游戏后缓冲剔除（采集端看不到）；此前连续跳过 %lld 帧",
                    g_skipFrames);
        g_skipFrames = 0;
    }

    // ---------------- 小工具 ----------------
    static bool FileOk(bool b) { return b; }

    static void ReleaseOverlay()
    {
        Overlay& o = g_ov;
        for (int i = 0; i < 2; i++)
        {
            if (o.bb12[i]) { o.bb12[i]->Release(); o.bb12[i] = nullptr; o.bb12RT[i] = false; }
            if (o.rtv11[i]) { o.rtv11[i]->Release(); o.rtv11[i] = nullptr; }
            if (o.stg11[i]) { o.stg11[i]->Release(); o.stg11[i] = nullptr; }
            if (o.q11[i])   { o.q11[i]->Release();   o.q11[i] = nullptr; }
            if (o.rb12[i])  { o.rb12[i]->Release();  o.rb12[i] = nullptr; }
        }
        if (o.rtvHeap) { o.rtvHeap->Release(); o.rtvHeap = nullptr; }
        if (o.off11Rtv) { o.off11Rtv->Release(); o.off11Rtv = nullptr; }
        if (o.off11) { o.off11->Release(); o.off11 = nullptr; }
        if (o.off12) { o.off12->Release(); o.off12 = nullptr; }
        if (o.fen)   { o.fen->Release(); o.fen = nullptr; }
        if (o.fenEv) { CloseHandle(o.fenEv); o.fenEv = nullptr; }
        if (o.visual) { o.visual->Release(); o.visual = nullptr; }
        if (o.target) { o.target->Release(); o.target = nullptr; }
        if (o.dcomp)  { o.dcomp->Release();  o.dcomp = nullptr; }
        if (o.sc)     { o.sc->Release();     o.sc = nullptr; }
        if (o.dib)    { DeleteObject(o.dib); o.dib = nullptr; o.bits = nullptr; }
        if (o.dc)     { DeleteDC(o.dc); o.dc = nullptr; }
        o.dibW = o.dibH = 0;
        o.ok = false; o.layered = false; o.w = 0; o.h = 0;
        o.holdHide = false;
    }

    static void HideOverlay()
    {
        if (g_ov.hwnd && g_ov.shown)
        {
            ShowWindow(g_ov.hwnd, SW_HIDE);
            g_ov.shown = false;
        }
    }

    static void PushLayeredBitmap();   // 定义在下方：分层路径"位图 → 窗口"

    static void ShowOverlay()
    {
        if (!g_ov.hwnd) return;
        if (!g_ov.shown)
        {
            ShowWindow(g_ov.hwnd, SW_SHOWNOACTIVATE);
            g_ov.shown = true;
        }
        SetWindowPos(g_ov.hwnd, HWND_TOPMOST, g_ov.x, g_ov.y, (int)g_ov.w, (int)g_ov.h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        // 窗口句柄不变（WDA 防采集属性不会掉），只保证几何与置顶。
    }

    // 立刻把一帧"全透明"内容推上覆盖窗口：
    //   · 窗口从创建的第一帧起就被 DWM 合成 → SetWindowDisplayAffinity 立即生效；
    //   · g_ov.everPresented 立刻为真 → 本帧就可以开始"持续隐藏"，不必等下一帧
    //     （否则启用直播模式的最初 1~2 帧受保护元素会画回游戏后缓冲 = 推流闪一下）。
    static void PresentEmptyFrame()
    {
        if (!g_ov.hwnd) return;
        if (g_ov.layered)
        {
            if (g_ov.bits && g_ov.dibW > 0 && g_ov.dibH > 0)
                memset(g_ov.bits, 0, (size_t)g_ov.dibW * (size_t)g_ov.dibH * 4);
            PushLayeredBitmap();
        }
        else if (g_dev11 && g_ctx11 && g_ov.sc && g_ov.rtv11[0])
        {
            const float clear[4] = { 0.f, 0.f, 0.f, 0.f };
            g_ctx11->ClearRenderTargetView(g_ov.rtv11[0], clear);
            g_ctx11->OMSetRenderTargets(0, nullptr, nullptr);
            if (SUCCEEDED(g_ov.sc->Present(0, 0)))
                g_ov.everPresented = true;
        }
        ShowOverlay();
        g_ov.everPresented = g_ov.everPresented || g_ov.layered;
        g_ov.holdHide = true;      // 窗口里已有（空透明）内容，可安全持续隐藏
    }

    // 窗口类（只注册一次）
    static void EnsureWindowClass()
    {
        static bool s_done = false;
        if (s_done) return;
        s_done = true;
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"ADOFAIPerfect_StreamOverlay";
        RegisterClassExW(&wc);
    }

    // 创建窗口（不含交换链）
    static bool EnsureHwnd(bool layered)
    {
        if (g_ov.hwnd) return true;
        EnsureWindowClass();
        DWORD ex = WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        if (layered) ex |= WS_EX_LAYERED;
        else         ex |= WS_EX_NOREDIRECTIONBITMAP;
        HWND h = CreateWindowExW(ex, L"ADOFAIPerfect_StreamOverlay", L"",
                                 WS_POPUP, 0, 0, 16, 16,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!h)
        {
            Log::Printf("[stream] CreateWindowEx failed err=%lu", GetLastError());
            return false;
        }
        // 调试钩子：ADOFAI_PERFECT_STREAM_CAPTURABLE=1 时不做防采集，
        //   便于用普通截图/录制验证"人眼所见"的覆盖层内容是否正确。
        static const bool s_ovCapturable = [] {
            char v[8] = { 0 };
            return GetEnvironmentVariableA("ADOFAI_PERFECT_STREAM_CAPTURABLE", v, sizeof(v)) > 0 && v[0] == '1';
        }();
        if (s_ovCapturable)
        {
            SetWindowDisplayAffinity(h, WDA_NONE);
            Log::Printf("[stream] overlay hwnd=%p CAPTURABLE (debug hook)", (void*)h);
        }
        else if (!SetWindowDisplayAffinity(h, WDA_EXCLUDEFROMCAPTURE))
            Log::Printf("[stream] WDA_EXCLUDEFROMCAPTURE unsupported err=%lu (采集端可能仍可见)",
                        GetLastError());
        else
            Log::Printf("[stream] overlay hwnd=%p excluded from capture", (void*)h);
        g_ov.hwnd = h;
        return true;
    }

    // ---- 窗口位置 / 尺寸跟随游戏客户区 ----
    static bool InitDComp();
    static bool InitLayered();

    // 尺寸变化：**只重建尺寸相关的 GPU 资源，绝不销毁窗口**。
    //   窗口一旦销毁重建，就会有几帧 g_ov.ok == false → FilterForGame 不隐藏
    //   → 受保护元素画回游戏后缓冲 → 推流里"一闪而过"。窗口保留则隐藏始终连续。
    static bool ResizeOverlay()
    {
        if (!g_ov.hwnd) return false;
        const bool wasLayered = g_ov.layered;
        ReleaseOverlay();                       // 释放尺寸相关资源（不销毁 hwnd / 不丢 WDA）
        const bool ok = wasLayered ? (InitLayered() || InitDComp())
                                   : (InitDComp() || InitLayered());
        if (ok) PresentEmptyFrame();
        // 分层窗口即使本次重建失败，窗口里仍保留上一帧画面 → 保持"持续隐藏"，
        // 避免尺寸变化期间受保护元素画回游戏后缓冲（推流一闪而过）。
        g_ov.holdHide = ok || wasLayered;
        return ok;
    }

    static bool SyncGeometry(bool allowResize = true)
    {
        if (!g_ov.hwnd || !g_gameHwnd) return false;
        RECT rc = {};
        if (!GetClientRect(g_gameHwnd, &rc)) return false;
        const UINT w = (UINT)(rc.right - rc.left);
        const UINT h = (UINT)(rc.bottom - rc.top);
        if (w < 8 || h < 8) return false;
        POINT pt = { 0, 0 };
        ClientToScreen(g_gameHwnd, &pt);
        if (g_ov.w == w && g_ov.h == h && g_ov.x == pt.x && g_ov.y == pt.y)
            return true;
        const bool sizeChanged = (g_ov.w != w || g_ov.h != h);
        g_ov.w = w; g_ov.h = h; g_ov.x = pt.x; g_ov.y = pt.y;
        SetWindowPos(g_ov.hwnd, HWND_TOPMOST, pt.x, pt.y, (int)w, (int)h,
                     SWP_NOACTIVATE | (g_ov.shown ? SWP_SHOWWINDOW : 0));
        if (sizeChanged && allowResize && !ResizeOverlay())
            return false;
        return true;
    }

    // ---------------- 创建覆盖窗口的渲染后端 ----------------
    static IDXGIFactory2* GetFactory2()
    {
        IDXGIDevice* dxgiDev = nullptr;
        IDXGIAdapter* ad = nullptr;
        IDXGIFactory2* fac = nullptr;
        IUnknown* dev = (IUnknown*)(g_dev12 ? (void*)g_dev12 : (void*)g_dev11);
        if (!dev) return nullptr;
        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev))) && dxgiDev)
        {
            if (SUCCEEDED(dxgiDev->GetAdapter(&ad)) && ad)
            {
                ad->GetParent(IID_PPV_ARGS(&fac));
                ad->Release();
            }
            dxgiDev->Release();
        }
        return fac;
    }

    static bool IsBgraFmt(DXGI_FORMAT f)
    {
        return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    }

    // 覆盖层交换链 / 离屏目标格式：D3D12 下必须与 ImGui 后端创建 PSO 时所用的
    // RTVFormat（= 游戏后缓冲格式）一致，否则通道错位（红蓝互换）。
    // DX11 没有这个约束，统一走 BGRA 合成格式即可。
    static DXGI_FORMAT OverlayFmt()
    {
        // 与游戏后缓冲同格式：DComp 交换链用错误的通道序会成为"不可渲染"的目标
        // （实测 R8G8B8A8 后缓冲上建 BGRA 交换链 RTV 会 E_INVALIDARG）。
        const DXGI_FORMAT f = (DXGI_FORMAT)g_gameFmt;
        if (f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM ||
            f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
            return f;
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    // 首选：DirectComposition + flip 交换链
    static bool InitDComp()
    {
        if (!g_ov.hwnd) return false;
        if (!g_dev11 && !g_dev12) return false;
        IDXGIFactory2* fac = GetFactory2();
        if (!fac) { Log::Printf("[stream] no IDXGIFactory2"); return false; }

        DXGI_SWAP_CHAIN_DESC1 sd = {};
        sd.Width  = g_ov.w ? g_ov.w : 1920;
        sd.Height = g_ov.h ? g_ov.h : 1080;
        sd.Format = OverlayFmt();
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sd.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;
        sd.Scaling     = DXGI_SCALING_STRETCH;

        HRESULT hr;
        if (g_dev12 && g_queue12)
            hr = fac->CreateSwapChainForComposition(g_queue12, &sd, nullptr, &g_ov.sc);
        else
            hr = fac->CreateSwapChainForComposition(g_dev11, &sd, nullptr, &g_ov.sc);
        if (FAILED(hr) || !g_ov.sc)
        {
            Log::Printf("[stream] CreateSwapChainForComposition failed 0x%08X", (unsigned)hr);
            g_ov.sc = nullptr;
            fac->Release();
            return false;
        }
        fac->Release();
        g_ov.bbCount = 2;

        // 回退到 DComp 合成（每像素 alpha）
        IUnknown* dev = (IUnknown*)(g_dev12 ? (void*)g_dev12 : (void*)g_dev11);
        IDXGIDevice* dxgiDev = nullptr;
        hr = dev->QueryInterface(IID_PPV_ARGS(&dxgiDev));
        if (FAILED(hr) || !dxgiDev)
        {
            Log::Printf("[stream] QueryInterface(IDXGIDevice) failed 0x%08X", (unsigned)hr);
            return false;
        }
        hr = DCompositionCreateDevice(dxgiDev, IID_PPV_ARGS(&g_ov.dcomp));
        dxgiDev->Release();
        if (FAILED(hr) || !g_ov.dcomp)
        {
            Log::Printf("[stream] DCompositionCreateDevice failed 0x%08X", (unsigned)hr);
            return false;
        }
        if (FAILED(g_ov.dcomp->CreateTargetForHwnd(g_ov.hwnd, TRUE, &g_ov.target)) || !g_ov.target ||
            FAILED(g_ov.dcomp->CreateVisual(&g_ov.visual)) || !g_ov.visual)
        {
            Log::Printf("[stream] DComp target/visual failed");
            return false;
        }
        {
            // 逐项检查：DComp 视觉链如果静默失败，窗口会"什么都没有"，
            // 而我们已经把受保护元素从游戏后缓冲剔除了 → 人眼也跟着看不见。
            HRESULT hr2 = g_ov.visual->SetContent(g_ov.sc);
            if (SUCCEEDED(hr2)) { g_ov.target->SetRoot(g_ov.visual); hr2 = g_ov.dcomp->Commit(); }
            if (FAILED(hr2))
            {
                Log::Printf("[stream] DComp setup failed 0x%08X -> fallback to layered", (unsigned)hr2);
                return false;
            }
        }

        // 资源视图
        if (g_dev12)
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            hd.NumDescriptors = 2;
            if (FAILED(g_dev12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_ov.rtvHeap))) || !g_ov.rtvHeap)
            {
                Log::Printf("[stream] CreateDescriptorHeap(RTV) failed");
                return false;
            }
            const UINT inc = g_dev12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_ov.rtvHeap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < g_ov.bbCount; i++)
            {
                if (FAILED(g_ov.sc->GetBuffer(i, IID_PPV_ARGS(&g_ov.bb12[i]))) || !g_ov.bb12[i])
                {
                    Log::Printf("[stream] GetBuffer(%u) failed", i);
                    return false;
                }
                g_dev12->CreateRenderTargetView(g_ov.bb12[i], nullptr, cpu);
                g_ov.rtv12[i] = cpu;
                g_ov.bb12RT[i] = false;
                cpu.ptr += inc;
            }
        }
        else
        {
            for (UINT i = 0; i < g_ov.bbCount; i++)
            {
                ID3D11Texture2D* bb = nullptr;
                if (FAILED(g_ov.sc->GetBuffer(i, IID_PPV_ARGS(&bb))) || !bb)
                {
                    Log::Printf("[stream] DX11 GetBuffer(%u) failed", i);
                    return false;
                }
                HRESULT r2 = g_dev11->CreateRenderTargetView(bb, nullptr, &g_ov.rtv11[i]);
                bb->Release();
                if (FAILED(r2))
                {
                    Log::Printf("[stream] DX11 CreateRenderTargetView failed 0x%08X", (unsigned)r2);
                    return false;
                }
            }
        }
        Log::Printf("[stream] DirectComposition overlay ready (%ux%u, %s)",
                    g_ov.w, g_ov.h, g_dev12 ? "D3D12" : "D3D11");
        return true;
    }

    // 兜底：离屏渲染 + 回读 + 分层窗口
    static bool InitLayered()
    {
        if (!g_ov.hwnd) return false;
        const UINT w = g_ov.w ? g_ov.w : 1280;
        const UINT h = g_ov.h ? g_ov.h : 720;

        // 位图（UpdateLayeredWindow 用；预乘 BGRA，顶行优先）
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = (LONG)w;
        bi.bmiHeader.biHeight = -(LONG)h;      // 负数 = 顶行优先
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        HDC screen = GetDC(nullptr);
        g_ov.dc = CreateCompatibleDC(screen);
        if (screen) ReleaseDC(nullptr, screen);
        if (!g_ov.dc) { Log::Printf("[stream] CreateCompatibleDC failed"); return false; }
        g_ov.dib = CreateDIBSection(g_ov.dc, &bi, DIB_RGB_COLORS, &g_ov.bits, nullptr, 0);
        if (!g_ov.dib || !g_ov.bits) { Log::Printf("[stream] CreateDIBSection failed"); return false; }
        SelectObject(g_ov.dc, g_ov.dib);
        g_ov.dibW = (int)w; g_ov.dibH = (int)h;

        if (g_dev11)
        {
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(g_dev11->CreateTexture2D(&td, nullptr, &g_ov.off11)) || !g_ov.off11)
            { Log::Printf("[stream] offscreen RT failed"); return false; }
            if (FAILED(g_dev11->CreateRenderTargetView(g_ov.off11, nullptr, &g_ov.off11Rtv)) || !g_ov.off11Rtv)
            { Log::Printf("[stream] offscreen RTV failed"); return false; }
            D3D11_TEXTURE2D_DESC sd = td;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            for (int i = 0; i < 2; i++)
            {
                if (FAILED(g_dev11->CreateTexture2D(&sd, nullptr, &g_ov.stg11[i])) || !g_ov.stg11[i])
                { Log::Printf("[stream] staging %d failed", i); return false; }
                D3D11_QUERY_DESC qd = {};
                qd.Query = D3D11_QUERY_EVENT;
                if (FAILED(g_dev11->CreateQuery(&qd, &g_ov.q11[i])) || !g_ov.q11[i])
                { Log::Printf("[stream] query %d failed", i); return false; }
            }
        }
        else if (g_dev12)
        {
            D3D12_HEAP_PROPERTIES hp = {};
            hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd = {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.Format = OverlayFmt();
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (FAILED(g_dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&g_ov.off12))) || !g_ov.off12)
            { Log::Printf("[stream] DX12 offscreen RT failed"); return false; }
            g_ov.off12Rtv = g_ov.rtvHeap ? g_ov.rtvHeap->GetCPUDescriptorHandleForHeapStart()
                                         : D3D12_CPU_DESCRIPTOR_HANDLE{};
            if (!g_ov.rtvHeap)
            {
                D3D12_DESCRIPTOR_HEAP_DESC hd = {};
                hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
                hd.NumDescriptors = 3;
                if (FAILED(g_dev12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_ov.rtvHeap))) || !g_ov.rtvHeap)
                { Log::Printf("[stream] DX12 RTV heap failed"); return false; }
                g_ov.off12Rtv = g_ov.rtvHeap->GetCPUDescriptorHandleForHeapStart();
            }
            g_dev12->CreateRenderTargetView(g_ov.off12, nullptr, g_ov.off12Rtv);

            D3D12_HEAP_PROPERTIES hpr = {};
            hpr.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rdb = {};
            rdb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            const UINT64 pitch = ((UINT64)w * 4 + 255) & ~255ULL;
            rdb.Width = pitch * h;
            rdb.Height = 1; rdb.DepthOrArraySize = 1; rdb.MipLevels = 1;
            rdb.SampleDesc.Count = 1;
            rdb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            for (int i = 0; i < 2; i++)
                if (FAILED(g_dev12->CreateCommittedResource(&hpr, D3D12_HEAP_FLAG_NONE, &rdb,
                        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_ov.rb12[i]))) || !g_ov.rb12[i])
                { Log::Printf("[stream] readback %d failed", i); return false; }
            if (FAILED(g_dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_ov.fen))) || !g_ov.fen)
            { Log::Printf("[stream] fence failed"); return false; }
            g_ov.fenEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }
        else return false;

        Log::Printf("[stream] layered overlay ready (%ux%u, %s)", w, h, g_dev11 ? "D3D11" : "D3D12");
        g_ov.layered = true;
        return true;
    }

    static bool EnsureOverlay()
    {
        if (g_ov.ok) return SyncGeometry();
        // 建不起来时不要每帧死磕（会拖慢游戏渲染线程）：2 秒后再试。
        if (g_ov.failed && g_frameNo < g_ov.retryAt) return false;
        if (!g_ov.hwnd)
        {
            // DX11：实测 DComp 交换链的 RTV 在本机 E_INVALIDARG（D3D11 + 游戏设备），
            //   而"离屏渲染 + 回读 + 分层窗口"路径稳定可用 → 直接把分层路径作为
            //   DX11 的首选，省掉一次注定失败的 DComp 初始化（也省一次窗口销毁重建）。
            // DX12：DComp 交换链正常，仍作首选。
            const bool layeredFirst = (g_dev11 && !g_dev12);
            const bool okFirst = layeredFirst
                ? (EnsureHwnd(true)  && SyncGeometry(false) && InitLayered())
                : (EnsureHwnd(false) && SyncGeometry(false) && InitDComp());
            if (!okFirst)
            {
                // 首选路径失败 → 重建一个分层窗口再试
                if (g_ov.hwnd) { DestroyWindow(g_ov.hwnd); g_ov.hwnd = nullptr; }
                ReleaseOverlay();
                const bool okSecond = layeredFirst
                    ? (EnsureHwnd(false) && SyncGeometry(false) && InitDComp())
                    : (EnsureHwnd(true)  && SyncGeometry(false) && InitLayered());
                if (!okSecond)
                {
                    Log::Printf("[stream] overlay unavailable - stream mode disabled (绘制回退到游戏画面)");
                    if (g_ov.hwnd) { DestroyWindow(g_ov.hwnd); g_ov.hwnd = nullptr; }
                    ReleaseOverlay();
                    g_ov.failed  = true;
                    g_ov.retryAt = g_frameNo + 120;
                    return false;
                }
            }
        }
        if (!SyncGeometry()) return false;
        g_ov.ok = true;
        g_ov.failed = false;
        // 立刻推一帧空透明内容：窗口第一帧就被合成（防采集立即生效），
        // 且 everPresented 立刻为真 → 本帧起就能"持续隐藏"，不必等下一次 Present。
        PresentEmptyFrame();
        g_ov.everPresented = true;
        return true;
    }
}

// ============================================================
//  公共 API
// ============================================================
namespace
{
    static int  g_resolveSlot = -1;      // 分层路径：待结算的槽位（-1 = 无）
    static bool g_pendingSignal = false; // DX12：本帧 Execute 之后要 Signal 一次
    static UINT64 g_fenNext = 0;

    static void BuildSubset(ImDrawData* src, ImDrawData* dst, bool wantProtected)
    {
        dst->Valid            = src->Valid;
        dst->DisplayPos       = src->DisplayPos;
        dst->DisplaySize      = src->DisplaySize;
        dst->FramebufferScale = src->FramebufferScale;
        dst->OwnerViewport    = src->OwnerViewport;
        dst->Textures         = src->Textures;
        dst->CmdLists.clear();
        dst->CmdListsCount = 0;
        dst->TotalVtxCount = 0;
        dst->TotalIdxCount = 0;
        if (!src->Valid) return;
        for (int i = 0; i < src->CmdLists.Size; i++)
        {
            ImDrawList* dl = src->CmdLists[i];
            if (IsProtected(dl) == wantProtected)
                dst->AddDrawList(dl);
        }
    }

    // ---- 分层路径：位图 → UpdateLayeredWindow（预乘 BGRA、顶行优先） ----
    static void PushLayeredBitmap()
    {
        if (!g_ov.hwnd || !g_ov.dc || !g_ov.bits) return;
        HDC screen = GetDC(nullptr);
        POINT dstPt = { g_ov.x, g_ov.y };
        SIZE  sz    = { (LONG)g_ov.dibW, (LONG)g_ov.dibH };
        POINT srcPt = { 0, 0 };
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        if (UpdateLayeredWindow(g_ov.hwnd, screen, &dstPt, &sz, g_ov.dc, &srcPt, 0, &bf, ULW_ALPHA))
            MarkPresented();
        if (screen) ReleaseDC(nullptr, screen);
        if (!g_ov.shown)
        {
            ShowWindow(g_ov.hwnd, SW_SHOWNOACTIVATE);
            SetWindowPos(g_ov.hwnd, HWND_TOPMOST, g_ov.x, g_ov.y, (int)g_ov.dibW, (int)g_ov.dibH,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
            g_ov.shown = true;
        }
    }

    static void CopyToDib(const uint8_t* src, int pitch, bool swapRB)
    {
        const int w = g_ov.dibW, h = g_ov.dibH;
        uint8_t* dst = (uint8_t*)g_ov.bits;
        if (!dst) return;
        for (int y = 0; y < h; y++)
        {
            const uint8_t* s = src + (size_t)y * pitch;
            uint8_t* d = dst + (size_t)y * w * 4;
            for (int x = 0; x < w; x++)
            {
                if (swapRB) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; }
                else        { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3]; }
                s += 4; d += 4;
            }
        }
    }

    // ---- 分层路径：结算上一帧（DX11 / DX12） ----
    static void ResolveLayered11()
    {
        if (!g_ov.q11[0] || !g_ov.stg11[0]) return;
        BOOL done = FALSE;
        if (g_ctx11->GetData(g_ov.q11[0], &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || !done)
            return;
        D3D11_MAPPED_SUBRESOURCE m = {};
        if (FAILED(g_ctx11->Map(g_ov.stg11[0], 0, D3D11_MAP_READ, 0, &m)) || !m.pData)
            return;
        CopyToDib((const uint8_t*)m.pData, (int)m.RowPitch, true);   // DX11 离屏固定 R8G8B8A8
        g_ctx11->Unmap(g_ov.stg11[0], 0);
        PushLayeredBitmap();
    }

    static void ResolveLayered12()
    {
        if (!g_ov.fen || !g_ov.rb12[0] || g_resolveSlot < 0) return;
        if (g_ov.fen->GetCompletedValue() < g_fenNext)
        {
            g_ov.fen->SetEventOnCompletion(g_fenNext, g_ov.fenEv);
            WaitForSingleObject(g_ov.fenEv, 8);
            if (g_ov.fen->GetCompletedValue() < g_fenNext) return;
        }
        void* p = nullptr;
        D3D12_RANGE rr = { 0, 0 };
        if (FAILED(g_ov.rb12[0]->Map(0, &rr, &p)) || !p) return;
        const UINT pitch = (UINT)((((UINT)g_ov.dibW * 4) + 255u) & ~255u);
        CopyToDib((const uint8_t*)p, (int)pitch, !IsBgraFmt(OverlayFmt()));
        D3D12_RANGE wr = { 0, 0 };
        g_ov.rb12[0]->Unmap(0, &wr);
        g_resolveSlot = -1;
        PushLayeredBitmap();
    }

    static void RenderLayered(ImDrawData* dd, void* cmd12)
    {
        const float clear[4] = { 0.f, 0.f, 0.f, 0.f };
        if (g_dev12 && cmd12)
        {
            ID3D12GraphicsCommandList* cl = (ID3D12GraphicsCommandList*)cmd12;
            ResolveLayered12();
            cl->ClearRenderTargetView(g_ov.off12Rtv, clear, 0, nullptr);
            cl->OMSetRenderTargets(1, &g_ov.off12Rtv, FALSE, nullptr);
            ImGui_ImplDX12_RenderDrawData(dd, cl);
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = g_ov.off12;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            cl->ResourceBarrier(1, &b);
            const UINT pitch = (UINT)((((UINT)g_ov.dibW * 4) + 255u) & ~255u);
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = g_ov.rb12[0];
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = 0;
            dst.PlacedFootprint.Footprint.Format = OverlayFmt();
            dst.PlacedFootprint.Footprint.Width = g_ov.dibW;
            dst.PlacedFootprint.Footprint.Height = g_ov.dibH;
            dst.PlacedFootprint.Footprint.Depth = 1;
            dst.PlacedFootprint.Footprint.RowPitch = pitch;
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = g_ov.off12;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            cl->ResourceBarrier(1, &b);
            g_resolveSlot = 0;
            g_pendingSignal = true;
        }
        else if (g_dev11 && g_ctx11)
        {
            ResolveLayered11();
            g_ctx11->ClearRenderTargetView(g_ov.off11Rtv, clear);
            ID3D11RenderTargetView* rtv = g_ov.off11Rtv;
            g_ctx11->OMSetRenderTargets(1, &rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(dd);
            ID3D11RenderTargetView* nullRTV = nullptr;
            g_ctx11->OMSetRenderTargets(1, &nullRTV, nullptr);
            g_ctx11->CopyResource(g_ov.stg11[0], g_ov.off11);
            g_ctx11->End(g_ov.q11[0]);
        }
    }

    static void RenderDComp(ImDrawData* dd, void* cmd12)
    {
        const float clear[4] = { 0.f, 0.f, 0.f, 0.f };
        if (g_dev12 && cmd12)
        {
            ID3D12GraphicsCommandList* cl = (ID3D12GraphicsCommandList*)cmd12;
            UINT idx = 0;
            IDXGISwapChain3* sc3 = nullptr;
            if (SUCCEEDED(g_ov.sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3)
            {
                idx = sc3->GetCurrentBackBufferIndex();
                sc3->Release();
            }
            if (idx >= g_ov.bbCount || !g_ov.bb12[idx]) return;
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = g_ov.bb12[idx];
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            if (!g_ov.bb12RT[idx])
            {
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
                cl->ResourceBarrier(1, &b);
                g_ov.bb12RT[idx] = true;
            }
            cl->ClearRenderTargetView(g_ov.rtv12[idx], clear, 0, nullptr);
            cl->OMSetRenderTargets(1, &g_ov.rtv12[idx], FALSE, nullptr);
            ImGui_ImplDX12_RenderDrawData(dd, cl);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
            cl->ResourceBarrier(1, &b);
            g_ov.bb12RT[idx] = false;
            g_ov.presentPending = true;
        }
        else if (g_dev11 && g_ctx11)
        {
            UINT idx = 0;
            IDXGISwapChain3* sc3 = nullptr;
            if (SUCCEEDED(g_ov.sc->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3)
            {
                idx = sc3->GetCurrentBackBufferIndex();
                sc3->Release();
            }
            if (idx >= g_ov.bbCount || !g_ov.rtv11[idx]) return;
            g_ctx11->ClearRenderTargetView(g_ov.rtv11[idx], clear);
            ID3D11RenderTargetView* rtv = g_ov.rtv11[idx];
            g_ctx11->OMSetRenderTargets(1, &rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(dd);
            ID3D11RenderTargetView* nullRTV = nullptr;
            g_ctx11->OMSetRenderTargets(1, &nullRTV, nullptr);
            if (SUCCEEDED(g_ov.sc->Present(0, 0)))   // 覆盖层不参与节流：绝不阻塞游戏渲染线程
                MarkPresented();
        }
    }
}

// ---------------- 开关 / 配置 ----------------
void ConfigLoadOnce()
{
    if (g_cfgLoaded.exchange(true)) return;
    g_on.store(I18N::Prefs::GetInt("stream_on", 0) != 0);
    g_hide[EL_MENU].store(I18N::Prefs::GetInt("stream_hide_menu", 1) != 0);
    g_hide[EL_TRACK].store(I18N::Prefs::GetInt("stream_hide_track", 1) != 0);
    g_hide[EL_READ].store(I18N::Prefs::GetInt("stream_hide_read", 1) != 0);
    g_hide[EL_KV].store(I18N::Prefs::GetInt("stream_hide_kv", 1) != 0);
}

void ConfigSave()
{
    I18N::Prefs::SetInt("stream_on", g_on.load() ? 1 : 0);
    I18N::Prefs::SetInt("stream_hide_menu", g_hide[EL_MENU].load() ? 1 : 0);
    I18N::Prefs::SetInt("stream_hide_track", g_hide[EL_TRACK].load() ? 1 : 0);
    I18N::Prefs::SetInt("stream_hide_read", g_hide[EL_READ].load() ? 1 : 0);
    I18N::Prefs::SetInt("stream_hide_kv", g_hide[EL_KV].load() ? 1 : 0);
    I18N::Prefs::Save();
}

bool Enabled() { ConfigLoadOnce(); return g_on.load(std::memory_order_relaxed); }
void SetEnabled(bool on)
{
    ConfigLoadOnce();
    const bool was = g_on.exchange(on, std::memory_order_relaxed);
    if (was != on)
    {
        Log::Printf("[stream] live/stream mode -> %s", on ? "ON" : "OFF");
        if (!on) HideOverlay();          // 关闭时立刻收起覆盖窗口
    }
    ConfigSave();
}

bool Hide(Elem e)
{
    ConfigLoadOnce();
    if (e < 0 || e >= EL_N) return false;
    return g_hide[e].load(std::memory_order_relaxed);
}

void SetHide(Elem e, bool on)
{
    ConfigLoadOnce();
    if (e < 0 || e >= EL_N) return;
    g_hide[e].store(on, std::memory_order_relaxed);
    ConfigSave();
    Log::Printf("[stream] hide elem %d -> %d", (int)e, (int)on);
}

const char* ElemName(Elem e)
{
    switch (e)
    {
    case EL_MENU:  return I18N::Tr(I18N::LIVE_HIDE_MENU);
    case EL_TRACK: return I18N::Tr(I18N::LIVE_HIDE_TRACK);
    case EL_READ:  return I18N::Tr(I18N::LIVE_HIDE_READ);
    case EL_KV:    return I18N::Tr(I18N::LIVE_HIDE_KV);
    default:       return "";
    }
}

int SettingGet(int which)
{
    ConfigLoadOnce();
    switch (which)
    {
    case 0: return g_on.load() ? 1 : 0;
    case 1: return g_hide[EL_MENU].load() ? 1 : 0;
    case 2: return g_hide[EL_TRACK].load() ? 1 : 0;
    case 3: return g_hide[EL_READ].load() ? 1 : 0;
    case 4: return g_hide[EL_KV].load() ? 1 : 0;
    default: return 0;
    }
}

void SettingSet(int which, int v)
{
    if (which == 0) { SetEnabled(v != 0); return; }
    if (which >= 1 && which <= 4) { SetHide((Elem)(which - 1), v != 0); return; }
}

// ---------------- 生命周期 ----------------
void Attach(void* dev11, void* ctx11, void* dev12, void* queue12, void* srvHeap12,
            unsigned backbufferFormat)
{
    g_dev11 = (ID3D11Device*)dev11;
    g_ctx11 = (ID3D11DeviceContext*)ctx11;
    g_dev12 = (ID3D12Device*)dev12;
    g_queue12 = (ID3D12CommandQueue*)queue12;
    g_srvHeap12 = (ID3D12DescriptorHeap*)srvHeap12;
    g_gameFmt = backbufferFormat;
}

void SetGameWindow(void* hwnd, float w, float h, bool gameWindowed)
{
    g_gameHwnd = (HWND)hwnd;
    g_gw = (UINT)(w > 0 ? w : 0);
    g_gh = (UINT)(h > 0 ? h : 0);
    g_gameWindowed = gameWindowed;
}

void Shutdown()
{
    HideOverlay();
    if (g_ov.hwnd) { DestroyWindow(g_ov.hwnd); g_ov.hwnd = nullptr; }
    ReleaseOverlay();
    // 显式卸载（End 退出 / 交换链重建）：清空"持续隐藏"闩锁，等下一次重新建好窗口再隐藏。
    g_ov.everPresented = false;
    g_ov.failed = false;
    g_ov.retryAt = 0;
    g_skipLogged = false;
    g_skipFrames = 0;
}

bool OverlayReady() { return g_ov.ok; }

const char* StatusText()
{
    if (!Enabled()) return "off";
    if (g_ov.ok)    return g_ov.layered ? "layered" : "dcomp";
    return "unavailable";
}

// ---------------- 每帧 ----------------
void BeginFrame()
{
    g_frameNo++;
    g_reg.clear();
}

void MarkWindow(const char* windowName, Elem e)
{
    if (!windowName || !windowName[0]) return;
    if (e < 0 || e >= EL_N) return;
    for (size_t i = 0; i < g_reg.size(); i++)
        if (g_reg[i].name == windowName) { g_reg[i].elem = (int)e; return; }
    g_reg.push_back({ std::string(windowName), (int)e });
}

    ImDrawData* FilterForGame(ImDrawData* src)
    {
        if (!src || !Enabled()) return src;
        bool anyProt = false;
        for (size_t i = 0; i < g_reg.size(); i++)
            if (g_hide[g_reg[i].elem].load(std::memory_order_relaxed)) { anyProt = true; break; }
        if (!anyProt) return src;                  // 没有需要隐藏的元素：原样走游戏画面
        EnsureOverlay();                           // 失败也不要紧：下面用 holdHide 判定
        // 安全网 1：独占全屏时覆盖层不可能显示在游戏之上 → 不能隐藏。
        if (!g_gameWindowed)
        {
            LogSkipReasonOnce("exclusive-fullscreen");
            return src;
        }
        // 安全网 2：覆盖窗口必须"已经成功上屏过一次"才敢隐藏。
        //   注意这里**只看 everPresented（一次性闩锁），不再看最近是否上屏** ——
        //   分层 / DComp 覆盖窗口都会保留上一帧画面，偶发的一两帧 Present 失败
        //   只会让覆盖层内容晚一帧，人眼完全无感；但如果因此回退隐藏，
        //   受保护元素就会画回游戏后缓冲，被推流 / 录像抓到 = "一闪而过"。
        if (!g_ov.everPresented) { LogSkipReasonOnce("overlay-not-presented"); return src; }
        // 安全网 3：后端暂时不可用（例如尺寸变化正在重建）。只要窗口里还留着内容
        //   （holdHide，分层窗口天然保留上一帧），就继续隐藏 —— 推流端画面不中断。
        if (!g_ov.ok && !g_ov.holdHide) { LogSkipReasonOnce("overlay-lost"); return src; }
        LogHideOnce();
        BuildSubset(src, &s_gameDD, false);
        return &s_gameDD;
    }

void RenderOverlay(ImDrawData* src, void* d3d12CmdList)
{
    if (!src || !Enabled()) { HideOverlay(); return; }
    bool anyProt = false;
    for (size_t i = 0; i < g_reg.size(); i++)
        if (g_hide[g_reg[i].elem].load(std::memory_order_relaxed)) { anyProt = true; break; }
    if (!anyProt) { HideOverlay(); return; }
    // 覆盖窗口暂时不可用：**保持上一帧的窗口画面**（不要把窗口藏起来）。
    //   窗口里还留着上一帧的受保护元素，人眼照常可见；藏起来只会让人眼也看不到。
    if (!EnsureOverlay()) return;
    if (!g_ov.shown)
    {
        ShowOverlay();
    }
    BuildSubset(src, &s_protDD, true);
    // 本帧没有受保护元素内容可画（例如元素刚被关掉）：保留上一帧画面即可，
    // 不 HideOverlay —— 避免"藏/显"抖动造成推流端的闪烁。
    if (s_protDD.CmdListsCount <= 0) return;
    if (g_ov.layered) RenderLayered(&s_protDD, d3d12CmdList);
    else              RenderDComp(&s_protDD, d3d12CmdList);
}

void AfterExecute()
{
    if (g_pendingSignal && g_ov.fen && g_queue12)
    {
        g_pendingSignal = false;
        g_fenNext++;
        g_queue12->Signal(g_ov.fen, g_fenNext);
    }
    if (g_ov.presentPending && g_ov.sc)
    {
        g_ov.presentPending = false;
        if (SUCCEEDED(g_ov.sc->Present(0, 0)))
            MarkPresented();
    }
}

void DrawWatermark()
{
    if (!Enabled()) return;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    if (!dl) return;
    ImFont* f = ImGui::GetFont();
    if (!f) return;
    const char* txt = "ADOFAI-PERFECT";
    const float sz = 21.f;
    const ImVec2 ts = f->CalcTextSizeA(sz, FLT_MAX, 0.f, txt);
    const ImVec2 p(26.f, 22.f);
    dl->AddRectFilled(ImVec2(p.x - 12.f, p.y - 7.f),
                      ImVec2(p.x + ts.x + 12.f, p.y + ts.y + 7.f),
                      IM_COL32(8, 10, 18, 130), 8.f);
    dl->AddRect(ImVec2(p.x - 12.f, p.y - 7.f),
                ImVec2(p.x + ts.x + 12.f, p.y + ts.y + 7.f),
                IM_COL32(120, 200, 255, 120), 8.f, 0, 1.4f);
    dl->AddText(f, sz, ImVec2(p.x + 1.5f, p.y + 1.5f), IM_COL32(0, 0, 0, 170), txt);
    dl->AddText(f, sz, p, IM_COL32(255, 255, 255, 240), txt);
}

} // namespace StreamMode
