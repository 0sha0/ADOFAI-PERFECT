#include "RenderHook.h"
#include "CheatState.h"
#include "GameBridge.h"
#include "GameRecorder.h"
#include "Log.h"
#include "Menu.h"

#include <windows.h>
#include <wincodec.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <detours/detours.h>
#include <atomic>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_dx11.h"

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace RenderHook
{
    // ============ 类型 ============
    typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDXGISwapChain*, UINT, UINT);
    typedef HRESULT(STDMETHODCALLTYPE* Present1_t)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
    typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffers_t)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
    typedef void(STDMETHODCALLTYPE* ECL_t)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

    // ============ 常量（vtable 索引） ============
    static constexpr int kIdxPresent = 8;
    static constexpr int kIdxResizeBuffers = 13;
    static constexpr int kIdxPresent1 = 18;
    static constexpr int kIdxECL = 10;

    static constexpr UINT kNumBackBuffers = 4;   // >= Unity 的 BufferCount
    static constexpr UINT kNumFramesInFlight = 2;
    static constexpr UINT kSrvHeapSize = 128;

    // ============ 挂钩目标 ============
    static Present_t       g_oPresent = nullptr;
    static Present1_t      g_oPresent1 = nullptr;
    static ResizeBuffers_t g_oResizeBuffers = nullptr;
    static ECL_t           g_oECL = nullptr;
    static bool g_hooksInstalled = false;

    // ============ 运行时状态 ============
    enum class Api { Unknown, D3D11, D3D12 };
    static std::atomic<Api> g_api{ Api::Unknown };
    static std::atomic<bool> g_imguiReady{ false };
    static bool g_initFailed = false;

    static HWND g_hwnd = nullptr;
    static WNDPROC g_origWndProc = nullptr;
    static IDXGISwapChain* g_sc = nullptr;       // 当前渲染的交换链（游戏主窗口）
    static IDXGISwapChain3* g_sc3 = nullptr;

    static ID3D12Device* g_dev12 = nullptr;
    static ID3D12CommandQueue* g_queue12 = nullptr;     // ECL 钩子捕获的 Unity 直队列
    static ID3D12DescriptorHeap* g_rtvHeap12 = nullptr;
    static ID3D12DescriptorHeap* g_srvHeap12 = nullptr;
    static D3D12_CPU_DESCRIPTOR_HANDLE g_rtvStart12{};
    static UINT g_rtvInc12 = 0;
    static ID3D12Resource* g_rtResources12[kNumBackBuffers] = {};
    static bool g_rtValid12 = false;

    struct FrameCtx12
    {
        ID3D12CommandAllocator* allocator = nullptr;
        UINT64 fenceValue = 0;
        ID3D12Resource* recBuf = nullptr;      // 录制回读（READBACK 堆）
        UINT   recW = 0, recH = 0, recPitch = 0;
        bool   recValid = false;
        bool   recSwap = false;                // 来源为 RGBA（R8G8B8A8）时写盘前交换 R/B
    };
    static FrameCtx12 g_frames12[kNumFramesInFlight];
    static ID3D12GraphicsCommandList* g_cmdList12 = nullptr;
    static ID3D12Fence* g_fence12 = nullptr;
    static HANDLE g_fenceEvent12 = nullptr;
    static UINT64 g_fenceLast = 0;

    static ID3D11Device* g_dev11 = nullptr;
    static ID3D11DeviceContext* g_ctx11 = nullptr;
    static ID3D11RenderTargetView* g_rtv11[kNumBackBuffers] = {};
    static bool g_rtValid11 = false;

    // ECL 捕获时防自触发
    static thread_local bool t_inOurRender = false;

    // ============ 游戏画面捕获（左下角缩略图"小窗"用） ============
    // 在 Present 钩子内、绘制覆盖层"之前"把后缓冲拷进独立纹理 —— 得到的是
    // 游戏自己的干净画面（Unity 渲染结果，不含本工具任何 UI）。
    static std::atomic<bool> g_capWanted{ false };
    static std::atomic<bool> g_recWanted{ false };   // 录制开关（渲染线程每帧设置）
    static ID3D11Texture2D*          g_capTex11 = nullptr;
    static ID3D11ShaderResourceView* g_capSrv11 = nullptr;
    static UINT g_capW11 = 0, g_capH11 = 0;
    static ID3D12Resource* g_capTex12 = nullptr;
    static void*           g_capId12 = nullptr;   // GPU descriptor handle
    static UINT g_capW12 = 0, g_capH12 = 0;
    static int  g_capSrvIdx12 = -1;               // DX12 保留的描述符槽

    // ============ SRV 描述符分配器（ImGui 1.92 要求回调） ============
    struct HeapAllocState
    {
        ID3D12DescriptorHeap* heap = nullptr;
        D3D12_CPU_DESCRIPTOR_HANDLE cpuStart{};
        D3D12_GPU_DESCRIPTOR_HANDLE gpuStart{};
        UINT inc = 0;
        int freeList[kSrvHeapSize];
        int freeCount = 0;
    };
    static HeapAllocState g_srvAlloc;

    static void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
    {
        int idx = g_srvAlloc.freeList[--g_srvAlloc.freeCount];
        cpu->ptr = g_srvAlloc.cpuStart.ptr + (SIZE_T)idx * g_srvAlloc.inc;
        gpu->ptr = g_srvAlloc.gpuStart.ptr + (UINT64)idx * g_srvAlloc.inc;
    }
    static void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
    {
        int idx = (int)((cpu.ptr - g_srvAlloc.cpuStart.ptr) / g_srvAlloc.inc);
        g_srvAlloc.freeList[g_srvAlloc.freeCount++] = idx;
    }

    // ============ 工具 ============
    static bool IsUnityWindow(HWND h)
    {
        if (!h) return false;
        wchar_t cls[64] = {};
        GetClassNameW(h, cls, 64);
        return wcsncmp(cls, L"UnityWndClass", 13) == 0;
    }

    // ============ 主线程初始化消息 ============
    static constexpr UINT kMsgBridgeInit = WM_APP + 0x1BD;

    static void WINAPI PostBridgeInit()
    {
        if (g_hwnd)
            PostMessageW(g_hwnd, kMsgBridgeInit, 0, 0);
    }

    // ============ WndProc ============
    static LRESULT WINAPI HK_WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        // 游戏退出中：立刻停手（不再派发桥接任务、不再碰 ImGui），把消息原样还给 Unity。
        if (msg == WM_CLOSE || msg == WM_DESTROY || msg == WM_NCDESTROY ||
            msg == WM_QUERYENDSESSION || msg == WM_ENDSESSION)
        {
            if (!CheatState::GameQuitting.exchange(true, std::memory_order_relaxed))
                Log::Printf("[Main] game shutdown detected (msg=0x%04X) - tool dormant", (unsigned)msg);
            return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
        }
        if (CheatState::GameQuitting.load(std::memory_order_relaxed))
            return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
        if (msg == kMsgBridgeInit)
        {
            GameBridge::MainThreadInitTask(); // 在游戏主线程（mono 托管线程）执行
            return 0;
        }

        if (g_imguiReady.load(std::memory_order_relaxed))
        {
            if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp))
                return 1;

            if (msg == WM_KEYDOWN && wp == VK_INSERT)
            {
                bool v = CheatState::MenuVisible.load(std::memory_order_relaxed);
                CheatState::MenuVisible.store(!v, std::memory_order_relaxed);
            }
            else if (msg == WM_KEYDOWN && wp == VK_END)
            {
                Log::Printf("[Render] End key pressed — requesting unload");
                CheatState::ExitRequested.store(true, std::memory_order_relaxed);
            }
            else if (CheatState::MenuVisible.load(std::memory_order_relaxed) &&
                     ImGui::GetIO().WantCaptureMouse)
            {
                switch (msg)
                {
                case WM_LBUTTONDOWN: case WM_LBUTTONUP:
                case WM_RBUTTONDOWN: case WM_RBUTTONUP:
                case WM_MBUTTONDOWN: case WM_MBUTTONUP:
                case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
                case WM_MOUSEMOVE:
                    return 0; // 菜单打开且指针悬停时吞掉鼠标消息
                }
            }
        }
        return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
    }

    // ============ 渲染目标管理 ============
    static void ReleaseRT12()
    {
        for (auto& r : g_rtResources12)
        {
            if (r) { r->Release(); r = nullptr; }
        }
        g_rtValid12 = false;
    }

    static bool EnsureRT12(IDXGISwapChain3* sc3)
    {
        if (g_rtValid12)
            return true;
        DXGI_SWAP_CHAIN_DESC desc{};
        sc3->GetDesc(&desc);
        UINT count = desc.BufferCount;
        if (count > kNumBackBuffers) count = kNumBackBuffers;
        for (UINT i = 0; i < count; i++)
        {
            ID3D12Resource* res = nullptr;
            if (FAILED(sc3->GetBuffer(i, IID_PPV_ARGS(&res))))
                return false;
            D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvStart12;
            h.ptr += (SIZE_T)i * g_rtvInc12;
            g_dev12->CreateRenderTargetView(res, nullptr, h);
            g_rtResources12[i] = res;
        }
        g_rtValid12 = true;
        Log::Printf("[Render] DX12 render targets created (buffers=%u, %ux%u)",
                    count, desc.BufferDesc.Width, desc.BufferDesc.Height);
        return true;
    }

    static void ReleaseRT11()
    {
        for (auto& v : g_rtv11)
        {
            if (v) { v->Release(); v = nullptr; }
        }
        g_rtValid11 = false;
    }

    static bool EnsureRT11(IDXGISwapChain* sc)
    {
        if (g_rtValid11)
            return true;
        DXGI_SWAP_CHAIN_DESC desc{};
        sc->GetDesc(&desc);
        UINT count = desc.BufferCount;
        if (count > kNumBackBuffers) count = kNumBackBuffers;
        for (UINT i = 0; i < count; i++)
        {
            ID3D11Texture2D* surf = nullptr;
            if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&surf))))
                return false;
            g_dev11->CreateRenderTargetView(surf, nullptr, &g_rtv11[i]);
            surf->Release();
        }
        g_rtValid11 = true;
        Log::Printf("[Render] DX11 render targets created (buffers=%u)", count);
        return true;
    }

    // ---- 捕获纹理（缩略图） ----
    static void ReleaseCapture()
    {
        if (g_capSrv11) { g_capSrv11->Release(); g_capSrv11 = nullptr; }
        if (g_capTex11) { g_capTex11->Release(); g_capTex11 = nullptr; }
        g_capW11 = g_capH11 = 0;
        if (g_capTex12) { g_capTex12->Release(); g_capTex12 = nullptr; }
        g_capId12 = nullptr;
        g_capW12 = g_capH12 = 0;
    }

    static bool EnsureCapture11(IDXGISwapChain* sc)
    {
        if (g_capSrv11)
            return true;
        if (!g_dev11 || !sc)
            return false;
        ID3D11Texture2D* back = nullptr;
        if (FAILED(sc->GetBuffer(0, IID_PPV_ARGS(&back))) || !back)
            return false;
        D3D11_TEXTURE2D_DESC d{};
        back->GetDesc(&d);
        back->Release();
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = 0;
        d.MiscFlags = 0;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.SampleDesc.Count = 1;
        d.SampleDesc.Quality = 0;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(g_dev11->CreateTexture2D(&d, nullptr, &tex)) || !tex)
            return false;
        ID3D11ShaderResourceView* srv = nullptr;
        if (FAILED(g_dev11->CreateShaderResourceView(tex, nullptr, &srv)) || !srv)
        {
            tex->Release();
            return false;
        }
        g_capTex11 = tex;
        g_capSrv11 = srv;
        g_capW11 = d.Width;
        g_capH11 = d.Height;
        Log::Printf("[Render] capture tex ready %ux%u", d.Width, d.Height);
        return true;
    }

    static bool EnsureCapture12(IDXGISwapChain3* sc3)
    {
        if (g_capTex12)
            return true;
        if (!g_dev12 || !sc3 || !g_srvHeap12 || g_capSrvIdx12 < 0)
            return false;
        ID3D12Resource* back = nullptr;
        if (FAILED(sc3->GetBuffer(0, IID_PPV_ARGS(&back))) || !back)
            return false;
        D3D12_RESOURCE_DESC bd = back->GetDesc();
        back->Release();
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = bd;
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;
        ID3D12Resource* tex = nullptr;
        if (FAILED(g_dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                    nullptr, IID_PPV_ARGS(&tex))) || !tex)
            return false;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_srvAlloc.cpuStart;
        cpu.ptr += (SIZE_T)g_capSrvIdx12 * g_srvAlloc.inc;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = bd.Format;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        g_dev12->CreateShaderResourceView(tex, &sd, cpu);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_srvAlloc.gpuStart;
        gpu.ptr += (UINT64)g_capSrvIdx12 * g_srvAlloc.inc;
        g_capTex12 = tex;
        g_capId12 = (void*)(uintptr_t)gpu.ptr;
        g_capW12 = (UINT)bd.Width;
        g_capH12 = bd.Height;
        Log::Printf("[Render] capture tex ready (DX12) %ux%u", g_capW12, g_capH12);
        return true;
    }

    // 录制回读缓冲（D3D12，READBACK 堆；每帧槽一份，尺寸随窗口）
    static bool EnsureRecBuf12(FrameCtx12& fc, ID3D12Resource* capTex)
    {
        if (!g_dev12 || !capTex) return false;
        D3D12_RESOURCE_DESC cd = capTex->GetDesc();
        DXGI_FORMAT cf = cd.Format;
        if (cf == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) cf = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (cf == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) cf = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (cf != DXGI_FORMAT_B8G8R8A8_UNORM && cf != DXGI_FORMAT_R8G8B8A8_UNORM)
            return false;
        const UINT w = (UINT)cd.Width, h = cd.Height;
        const UINT pitch = (w * 4 + 255u) & ~255u;
        if (fc.recBuf && fc.recW == w && fc.recH == h)
            return true;
        if (fc.recBuf) { fc.recBuf->Release(); fc.recBuf = nullptr; }
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (UINT64)pitch * h;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* buf = nullptr;
        if (FAILED(g_dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buf))) || !buf)
            return false;
        fc.recBuf = buf;
        fc.recW = w; fc.recH = h; fc.recPitch = pitch;
        fc.recValid = false;
        fc.recSwap = (cf == DXGI_FORMAT_R8G8B8A8_UNORM);
        Log::Printf("[rec] DX12 readback %ux%u pitch=%u fmt=%d", w, h, pitch, (int)cd.Format);
        return true;
    }

    // ============ ImGui 初始化 ============
    static bool InitImGui(IDXGISwapChain* sc, Api api)
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        sc->GetDesc(&desc);
        g_hwnd = desc.OutputWindow;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        // 不写 imgui.ini：避免在游戏/运行目录留下临时文件（窗口位置由代码持久管理）
        ImGui::GetIO().IniFilename = nullptr;

        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 12.f;
        style.ChildRounding = 10.f;
        style.FrameRounding = 8.f;
        style.GrabRounding = 6.f;
        style.PopupRounding = 8.f;
        style.ScrollbarRounding = 8.f;
        style.WindowBorderSize = 1.f;
        style.ChildBorderSize = 0.f;
        style.FramePadding = ImVec2(8, 5);
        style.ItemSpacing = ImVec2(8, 6);
        style.ScrollbarSize = 10.f;
        ImGui::StyleColorsDark();
        ImVec4* c = style.Colors;
        c[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.06f, 0.095f, 0.88f);   // 半透明
        c[ImGuiCol_ChildBg] = ImVec4(1.f, 1.f, 1.f, 0.04f);
        c[ImGuiCol_Border] = ImVec4(0.35f, 0.85f, 0.75f, 0.25f);
        c[ImGuiCol_Text] = ImVec4(0.92f, 0.94f, 0.98f, 1.f);
        c[ImGuiCol_TextDisabled] = ImVec4(0.55f, 0.58f, 0.66f, 1.f);
        c[ImGuiCol_Button] = ImVec4(0.13f, 0.14f, 0.20f, 0.85f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.20f, 0.28f, 0.95f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.22f, 0.25f, 0.35f, 1.f);
        c[ImGuiCol_CheckMark] = ImVec4(0.24f, 0.85f, 0.72f, 1.f);
        c[ImGuiCol_SliderGrab] = ImVec4(0.24f, 0.85f, 0.72f, 1.f);
        c[ImGuiCol_PlotHistogram] = ImVec4(0.24f, 0.85f, 0.72f, 0.6f);
        c[ImGuiCol_Separator] = ImVec4(1.f, 1.f, 1.f, 0.08f);

        // 中文字体（微软雅黑；缺失时回退 ImGui 默认字体）
        {
            char fontPath[MAX_PATH] = {};
            GetWindowsDirectoryA(fontPath, MAX_PATH);
            strcat_s(fontPath, "\\Fonts\\msyh.ttc");
            const ImWchar* ranges = ImGui::GetIO().Fonts->GetGlyphRangesChineseSimplifiedCommon();
            ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(fontPath, 18.0f, nullptr, ranges);
            if (!font)
                Log::Printf("[Render] msyh.ttc load failed, fallback to default font");
        }

        if (!ImGui_ImplWin32_Init((void*)g_hwnd))
        {
            Log::Printf("[Render] ImGui_ImplWin32_Init failed");
            return false;
        }

        if (api == Api::D3D12)
        {
            D3D12_DESCRIPTOR_HEAP_DESC srvDesc = {};
            srvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            srvDesc.NumDescriptors = kSrvHeapSize;
            srvDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(g_dev12->CreateDescriptorHeap(&srvDesc, IID_PPV_ARGS(&g_srvHeap12))))
            {
                Log::Printf("[Render] CreateDescriptorHeap(SRV) failed");
                return false;
            }
            g_srvAlloc.heap = g_srvHeap12;
            g_srvAlloc.cpuStart = g_srvHeap12->GetCPUDescriptorHandleForHeapStart();
            g_srvAlloc.gpuStart = g_srvHeap12->GetGPUDescriptorHandleForHeapStart();
            g_srvAlloc.inc = g_dev12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            g_srvAlloc.freeCount = (int)kSrvHeapSize;
            for (int i = 0; i < (int)kSrvHeapSize; i++)
                g_srvAlloc.freeList[i] = (int)kSrvHeapSize - 1 - i;
            // 预留 0 号槽给"游戏画面捕获"缩略图（永久占用，不参与 ImGui 分配）
            if (g_srvAlloc.freeCount > 0)
                g_capSrvIdx12 = g_srvAlloc.freeList[--g_srvAlloc.freeCount];

            D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
            rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            rtvDesc.NumDescriptors = kNumBackBuffers;
            if (FAILED(g_dev12->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&g_rtvHeap12))))
            {
                Log::Printf("[Render] CreateDescriptorHeap(RTV) failed");
                return false;
            }
            g_rtvStart12 = g_rtvHeap12->GetCPUDescriptorHandleForHeapStart();
            g_rtvInc12 = g_dev12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

            for (UINT i = 0; i < kNumFramesInFlight; i++)
            {
                if (FAILED(g_dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                           IID_PPV_ARGS(&g_frames12[i].allocator))))
                {
                    Log::Printf("[Render] CreateCommandAllocator failed");
                    return false;
                }
            }
            if (FAILED(g_dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  g_frames12[0].allocator, nullptr,
                                                  IID_PPV_ARGS(&g_cmdList12))) ||
                FAILED(g_cmdList12->Close()))
            {
                Log::Printf("[Render] CreateCommandList failed");
                return false;
            }
            if (FAILED(g_dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence12))))
            {
                Log::Printf("[Render] CreateFence failed");
                return false;
            }
            g_fenceEvent12 = CreateEventW(nullptr, FALSE, FALSE, nullptr);

            ImGui_ImplDX12_InitInfo info = {};
            info.Device = g_dev12;
            info.CommandQueue = g_queue12;   // Unity 直队列（ECL 钩子捕获）
            info.NumFramesInFlight = (int)kNumFramesInFlight;
            info.RTVFormat = desc.BufferDesc.Format;
            info.DSVFormat = DXGI_FORMAT_UNKNOWN;
            info.SrvDescriptorHeap = g_srvHeap12;
            info.SrvDescriptorAllocFn = SrvAlloc;
            info.SrvDescriptorFreeFn = SrvFree;
            if (!ImGui_ImplDX12_Init(&info))
            {
                Log::Printf("[Render] ImGui_ImplDX12_Init failed");
                return false;
            }
        }
        else // D3D11
        {
            if (!g_dev11 || !g_ctx11)
                return false;
            if (!ImGui_ImplDX11_Init(g_dev11, g_ctx11))
            {
                Log::Printf("[Render] ImGui_ImplDX11_Init failed");
                return false;
            }
        }

        g_api.store(api, std::memory_order_release);
        g_imguiReady.store(true, std::memory_order_release);

        g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)HK_WndProc);

        // 桥接主线程初始化通道（PostMessage → 主线程 WndProc）
        GameBridge::SetMainThreadPoster(&PostBridgeInit);

        Log::Printf("[Render] ImGui initialized (api=%s hwnd=%p)",
                    api == Api::D3D12 ? "D3D12" : "D3D11", (void*)g_hwnd);
        return true;
    }

    // ============ 每帧入口（Present / Present1 共用） ============
    static void RenderDX12(IDXGISwapChain3* sc3);
    static void RenderDX11(IDXGISwapChain* sc);

    static void ProcessFrame(IDXGISwapChain* sc, UINT flags)
    {
        if (flags & DXGI_PRESENT_TEST)
            return;

        // 每帧打点：主线程侧凭它判断"游戏还在出帧"，退出/设备丢失时自动停手
        CheatState::LastPresentMs.store((unsigned long)GetTickCount(), std::memory_order_relaxed);

        // 游戏已进入退出流程：不做任何绘制/捕获/录制（录制只收尾一次），
        // 避免在设备销毁过程中与 Unity 抢资源或阻塞退出。
        if (CheatState::GameQuitting.load(std::memory_order_relaxed))
        {
            static bool s_recStopped = false;
            if (!s_recStopped)
            {
                s_recStopped = true;
                GameRecorder::Stop();
                RenderHook_SetCaptureWanted(false);
                g_capWanted.store(false, std::memory_order_relaxed);
                g_recWanted.store(false, std::memory_order_relaxed);
                Log::Printf("[Main] render hook dormant (game quitting)");
            }
            return;
        }

        // 调试开关：ADOF_NOUI=1 禁用覆盖层（仅挂钩不绘制）
        static bool s_noUi = false;
        {
            char envBuf[4] = {};
            s_noUi = GetEnvironmentVariableA("ADOF_NOUI", envBuf, 4) > 0 && envBuf[0] == '1';
        }
        if (s_noUi)
            return;

        if (!g_imguiReady.load(std::memory_order_acquire))
        {
            if (g_initFailed)
                return;

            // 只接受 Unity 主窗口的交换链
            DXGI_SWAP_CHAIN_DESC desc{};
            sc->GetDesc(&desc);
            if (!IsUnityWindow(desc.OutputWindow))
                return;

            Api api = Api::Unknown;
            if (SUCCEEDED(sc->GetDevice(IID_ID3D12Device, (void**)&g_dev12)) && g_dev12)
                api = Api::D3D12;
            else if (SUCCEEDED(sc->GetDevice(IID_ID3D11Device, (void**)&g_dev11)) && g_dev11)
            {
                api = Api::D3D11;
                g_dev11->GetImmediateContext(&g_ctx11); // ImGui DX11 后端必需
            }

            if (api == Api::Unknown)
            {
                Log::Printf("[Render] Present: unknown device type, overlay disabled");
                g_initFailed = true;
                return;
            }
            if (api == Api::D3D12 && !g_queue12)
            {
                // Unity 直队列还没被 ECL 钩子捕获，下一帧再试
                g_dev12->Release();
                g_dev12 = nullptr;
                return;
            }

            sc->QueryInterface(IID_PPV_ARGS(&g_sc3));
            g_sc = sc;

            if (!InitImGui(sc, api))
            {
                Log::Printf("[Render] ImGui init failed; overlay disabled");
                g_initFailed = true;
            }
            return;
        }

        // 已初始化：跟随交换链重建（窗口模式切换 / alt-enter）
        if (sc != g_sc)
        {
            DXGI_SWAP_CHAIN_DESC desc{};
            sc->GetDesc(&desc);
            if (IsUnityWindow(desc.OutputWindow))
            {
                ReleaseRT12();
                ReleaseRT11();
                ReleaseCapture();
                if (g_sc3) { g_sc3->Release(); g_sc3 = nullptr; }
                sc->QueryInterface(IID_PPV_ARGS(&g_sc3));
                g_sc = sc;
                Log::Printf("[Render] swapchain replaced -> re-targeted");
            }
            else
            {
                return; // 其他窗口的交换链不渲染
            }
        }

        // （mono 读写已移至 GameBridge 的独立循环线程，渲染线程只做绘制）

        if (g_api.load(std::memory_order_relaxed) == Api::D3D12 && g_sc3)
            RenderDX12(g_sc3);
        else if (g_api.load(std::memory_order_relaxed) == Api::D3D11)
            RenderDX11(sc);
    }

    // ============ D3D12 渲染 ============
    static void RenderDX12(IDXGISwapChain3* sc3)
    {
        UINT bb = sc3->GetCurrentBackBufferIndex();
        UINT fi = bb % kNumFramesInFlight;
        FrameCtx12& fc = g_frames12[fi];

        if (g_fence12->GetCompletedValue() < fc.fenceValue)
        {
            g_fence12->SetEventOnCompletion(fc.fenceValue, g_fenceEvent12);
            WaitForSingleObject(g_fenceEvent12, 1000);
        }

        // 录制回读（D3D12）：上一轮用该槽写好的 staging 已由上面的 fence 保证完成
        if (g_recWanted.load(std::memory_order_relaxed) && fc.recValid && fc.recBuf)
        {
            void* p = nullptr;
            D3D12_RANGE rr = { 0, (SIZE_T)fc.recPitch * fc.recH };
            if (SUCCEEDED(fc.recBuf->Map(0, &rr, &p)) && p)
            {
                GameRecorder::PushPixels(p, (int)fc.recW, (int)fc.recH, (int)fc.recPitch, fc.recSwap);
                D3D12_RANGE wr = { 0, 0 };
                fc.recBuf->Unmap(0, &wr);
            }
            fc.recValid = false;
        }

        if (!EnsureRT12(sc3))
            return;

        ID3D12Resource* res = g_rtResources12[bb];
        if (!res)
            return;

        fc.allocator->Reset();
        g_cmdList12->Reset(fc.allocator, nullptr);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = res;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        g_cmdList12->ResourceBarrier(1, &barrier);

        // 缩略图：趁后缓冲还是"游戏本帧画面"（未叠加 UI）先拷走
        if (g_capWanted.load(std::memory_order_relaxed) && EnsureCapture12(sc3) && g_capTex12)
        {
            D3D12_RESOURCE_BARRIER cb = {};
            cb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            cb.Transition.pResource = g_capTex12;
            cb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cb.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            cb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            g_cmdList12->ResourceBarrier(1, &cb);
            g_cmdList12->CopyResource(g_capTex12, res);
            cb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            cb.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            g_cmdList12->ResourceBarrier(1, &cb);
        }

        // 录制：capture 贴图 → READBACK 缓冲（下一轮的 fence 之后再 Map）
        if (g_recWanted.load(std::memory_order_relaxed) && g_capTex12 &&
            EnsureRecBuf12(fc, g_capTex12))
        {
            D3D12_RESOURCE_BARRIER rb = {};
            rb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            rb.Transition.pResource = g_capTex12;
            rb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            rb.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            rb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            g_cmdList12->ResourceBarrier(1, &rb);
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = fc.recBuf;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = 0;
            dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            dst.PlacedFootprint.Footprint.Width = fc.recW;
            dst.PlacedFootprint.Footprint.Height = fc.recH;
            dst.PlacedFootprint.Footprint.Depth = 1;
            dst.PlacedFootprint.Footprint.RowPitch = fc.recPitch;
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = g_capTex12;
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            g_cmdList12->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            rb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            rb.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            g_cmdList12->ResourceBarrier(1, &rb);
            fc.recValid = true;
        }

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_cmdList12->ResourceBarrier(1, &barrier);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvStart12;
        rtv.ptr += (SIZE_T)bb * g_rtvInc12;
        g_cmdList12->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        g_cmdList12->SetDescriptorHeaps(1, &g_srvHeap12);

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        Menu::Draw();
        ImGui::Render();
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmdList12);

        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        g_cmdList12->ResourceBarrier(1, &barrier);
        g_cmdList12->Close();

        t_inOurRender = true;
        g_queue12->ExecuteCommandLists(1, (ID3D12CommandList* const*)&g_cmdList12);
        t_inOurRender = false;

        g_queue12->Signal(g_fence12, ++g_fenceLast);
        fc.fenceValue = g_fenceLast;
    }

    // ============ D3D11 渲染 ============
    // 录制回读（D3D11）：3 槽 staging 环；Map(DO_NOT_WAIT) 永远只读"两帧前"的槽，
    // GPU 此时已完成该拷贝 —— 渲染线程零等待，读不到就丢这一帧。
    static ID3D11Texture2D* g_stage11[3] = {};
    static int g_stageW11 = 0, g_stageH11 = 0, g_stageIdx11 = 0;

    static void RecordFrameDX11(ID3D11Texture2D* back)
    {
        if (!g_ctx11 || !g_dev11 || !back || !GameRecorder::Active())
            return;
        D3D11_TEXTURE2D_DESC d{};
        back->GetDesc(&d);
        // 后缓冲常见两种：B8G8R8A8（BGRA，直接用）与 R8G8B8A8（RGBA，编码前交换 R/B）
        DXGI_FORMAT bf = d.Format;
        if (bf == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) bf = DXGI_FORMAT_B8G8R8A8_UNORM;
        if (bf == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) bf = DXGI_FORMAT_R8G8B8A8_UNORM;
        const bool swapRB = (bf == DXGI_FORMAT_R8G8B8A8_UNORM);
        if (bf != DXGI_FORMAT_B8G8R8A8_UNORM && bf != DXGI_FORMAT_R8G8B8A8_UNORM)
        {
            static bool s_fmtLogged = false;
            if (!s_fmtLogged)
            {
                s_fmtLogged = true;
                Log::Printf("[rec] unsupported backbuffer format %d -> recording disabled", (int)d.Format);
            }
            return;
        }
        if (!g_stage11[0] || g_stageW11 != (int)d.Width || g_stageH11 != (int)d.Height)
        {
            for (int i = 0; i < 3; i++)
                if (g_stage11[i]) { g_stage11[i]->Release(); g_stage11[i] = nullptr; }
            D3D11_TEXTURE2D_DESC sd = d;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.SampleDesc.Count = 1;
            sd.SampleDesc.Quality = 0;
            for (int i = 0; i < 3; i++)
                if (FAILED(g_dev11->CreateTexture2D(&sd, nullptr, &g_stage11[i])))
                    g_stage11[i] = nullptr;
            if (!g_stage11[0] || !g_stage11[1] || !g_stage11[2])
            {
                for (int i = 0; i < 3; i++)
                    if (g_stage11[i]) { g_stage11[i]->Release(); g_stage11[i] = nullptr; }
                return;
            }
            g_stageW11 = (int)d.Width;
            g_stageH11 = (int)d.Height;
            g_stageIdx11 = 0;
            Log::Printf("[rec] DX11 staging ring %dx%d", g_stageW11, g_stageH11);
        }
        const int i = g_stageIdx11;
        g_stageIdx11 = (g_stageIdx11 + 1) % 3;
        g_ctx11->CopyResource(g_stage11[i], back);
        const int r = (i + 1) % 3;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (g_ctx11->Map(g_stage11[r], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m) == S_OK)
        {
            GameRecorder::PushPixels(m.pData, g_stageW11, g_stageH11, (int)m.RowPitch, swapRB);
            g_ctx11->Unmap(g_stage11[r], 0);
            static bool s_pushLogged = false;
            if (!s_pushLogged)
            {
                s_pushLogged = true;
                Log::Printf("[rec] first DX11 frame pushed %dx%d rowPitch=%u fmt=%d swapRB=%d",
                            g_stageW11, g_stageH11, m.RowPitch, (int)d.Format, (int)swapRB);
            }
        }
        else
        {
            static int s_mapFail = 0;
            if (++s_mapFail == 60)
                Log::Printf("[rec] staging map still busy after 60 frames");
        }
    }

    static void RenderDX11(IDXGISwapChain* sc)
    {
        if (!EnsureRT11(sc))
            return;
        UINT bb = 0;
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc3))))
        {
            bb = sc3->GetCurrentBackBufferIndex();
            sc3->Release();
        }
        bb %= kNumBackBuffers;
        if (!g_rtv11[bb])
            return;

        // 缩略图：先拷走"游戏本帧干净画面"，再往上叠本工具 UI
        if (g_capWanted.load(std::memory_order_relaxed) && EnsureCapture11(sc) && g_capTex11)
        {
            ID3D11Texture2D* back = nullptr;
            if (SUCCEEDED(sc->GetBuffer(bb, IID_PPV_ARGS(&back))) && back)
            {
                g_ctx11->CopyResource(g_capTex11, back);
                back->Release();
            }
        }

        // 录制：干净画面 → staging 环（独立于小窗）
        if (g_recWanted.load(std::memory_order_relaxed))
        {
            ID3D11Texture2D* back = nullptr;
            if (SUCCEEDED(sc->GetBuffer(bb, IID_PPV_ARGS(&back))) && back)
            {
                RecordFrameDX11(back);
                back->Release();
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        Menu::Draw();
        ImGui::Render();

        g_ctx11->OMSetRenderTargets(1, &g_rtv11[bb], nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        ID3D11RenderTargetView* nullRTV = nullptr;
        g_ctx11->OMSetRenderTargets(1, &nullRTV, nullptr);
    }

    // ============ 钩子 ============
    static HRESULT WINAPI HK_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
    {
        if (!CheatState::ExitRequested.load(std::memory_order_relaxed))
            ProcessFrame(sc, flags);
        return g_oPresent(sc, sync, flags);
    }

    static HRESULT WINAPI HK_Present1(IDXGISwapChain* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
    {
        if (!CheatState::ExitRequested.load(std::memory_order_relaxed))
            ProcessFrame(sc, flags);
        return g_oPresent1(sc, sync, flags, pp);
    }

    static HRESULT WINAPI HK_ResizeBuffers(IDXGISwapChain* sc, UINT bufCount, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
    {
        ReleaseRT12();
        ReleaseRT11();
        ReleaseCapture();
        ReleaseCapture();
        return g_oResizeBuffers(sc, bufCount, w, h, fmt, flags);
    }

    static void STDMETHODCALLTYPE HK_ECL(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists)
    {
        if (!g_queue12 && !t_inOurRender)
        {
            D3D12_COMMAND_QUEUE_DESC d = queue->GetDesc();
            if (d.Type == D3D12_COMMAND_LIST_TYPE_DIRECT)
            {
                g_queue12 = queue;
                g_queue12->AddRef();
                Log::Printf("[Render] captured Unity direct command queue = %p", (void*)queue);
            }
        }
        g_oECL(queue, count, lists);
    }

    // ============ 哑设备取 vtable ============
    template <typename T>
    static void* VTableIdx(T obj, int idx)
    {
        return (*(void***)obj)[idx];
    }

    static HWND MakeDummyWindow()
    {
        return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                               L"STATIC", L"", WS_POPUP, 0, 0, 8, 8,
                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    }

    static bool CreateDummyD3D11(void*& presentAddr, void*& resizeAddr)
    {
        presentAddr = resizeAddr = nullptr;
        HWND w = MakeDummyWindow();
        if (!w) return false;

        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 2;
        sd.BufferDesc.Width = 8;
        sd.BufferDesc.Height = 8;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = w;
        sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

        IDXGISwapChain* sc = nullptr;
        ID3D11Device* dev = nullptr;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                                   levels, 2, D3D11_SDK_VERSION, &sd, &sc,
                                                   &dev, nullptr, nullptr);
        if (FAILED(hr))
        {
            sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
            hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               levels, 2, D3D11_SDK_VERSION, &sd, &sc,
                                               &dev, nullptr, nullptr);
        }
        if (SUCCEEDED(hr) && sc)
        {
            presentAddr = VTableIdx(sc, kIdxPresent);
            resizeAddr = VTableIdx(sc, kIdxResizeBuffers);
            Log::Printf("[Render] dummy D3D11: Present=%p ResizeBuffers=%p", presentAddr, resizeAddr);
        }
        if (sc) sc->Release();
        if (dev) dev->Release();
        DestroyWindow(w);
        return SUCCEEDED(hr);
    }

    static bool CreateDummyD3D12(void*& eclAddr)
    {
        eclAddr = nullptr;
        HWND w = MakeDummyWindow();
        if (!w) return false;

        ID3D12Device* dev = nullptr;
        HRESULT hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev));
        if (FAILED(hr))
        {
            DestroyWindow(w);
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ID3D12CommandQueue* q = nullptr;
        hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
        if (FAILED(hr))
        {
            dev->Release();
            DestroyWindow(w);
            return false;
        }

        IDXGIFactory2* factory = nullptr;
        IDXGISwapChain1* sc = nullptr;
        hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr))
        {
            DXGI_SWAP_CHAIN_DESC1 sd = {};
            sd.BufferCount = 2;
            sd.Width = 8;
            sd.Height = 8;
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.SampleDesc.Count = 1;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            hr = factory->CreateSwapChainForHwnd(q, w, &sd, nullptr, nullptr, &sc);
        }
        if (SUCCEEDED(hr) && sc)
        {
            eclAddr = VTableIdx(q, kIdxECL);
            Log::Printf("[Render] dummy D3D12: ECL=%p", eclAddr);
        }
        if (sc) sc->Release();
        if (factory) factory->Release();
        if (q) q->Release();
        dev->Release();
        DestroyWindow(w);
        return SUCCEEDED(hr);
    }

    // ============ 安装 ============
    bool Install()
    {
        if (g_hooksInstalled)
            return true;

        if (!GetModuleHandleW(L"dxgi.dll"))
            return false; // 由 worker 稍后重试

        void* presentAddr = nullptr;
        void* resizeAddr = nullptr;
        void* eclAddr = nullptr;

        CreateDummyD3D11(presentAddr, resizeAddr);
        CreateDummyD3D12(eclAddr);

        if (!presentAddr)
        {
            Log::Printf("[Render] failed to locate Present address");
            return false;
        }

        LONG rv = DetourTransactionBegin();
        if (rv != NO_ERROR)
        {
            Log::Printf("[Render] TransactionBegin failed %ld", rv);
            return false;
        }
        DetourUpdateThread(GetCurrentThread());

        g_oPresent = (Present_t)presentAddr;
        rv = DetourAttach((PVOID*)&g_oPresent, (PVOID)HK_Present);
        if (rv != NO_ERROR)
        {
            DetourTransactionAbort();
            Log::Printf("[Render] attach Present failed %ld", rv);
            return false;
        }

        // Present1 地址若与 Present 不同则一并挂钩（Unity 某些版本走 Present1）
        void* present1Addr = nullptr;
        {
            HWND w = MakeDummyWindow();
            if (w)
            {
                ID3D12Device* dev = nullptr;
                if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))))
                {
                    D3D12_COMMAND_QUEUE_DESC qd = {};
                    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
                    ID3D12CommandQueue* q = nullptr;
                    if (SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))))
                    {
                        IDXGIFactory2* factory = nullptr;
                        if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
                        {
                            DXGI_SWAP_CHAIN_DESC1 sd = {};
                            sd.BufferCount = 2;
                            sd.Width = 8; sd.Height = 8;
                            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                            sd.SampleDesc.Count = 1;
                            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                            IDXGISwapChain1* sc = nullptr;
                            if (SUCCEEDED(factory->CreateSwapChainForHwnd(q, w, &sd, nullptr, nullptr, &sc)))
                            {
                                present1Addr = VTableIdx(sc, kIdxPresent1);
                                sc->Release();
                            }
                            factory->Release();
                        }
                        q->Release();
                    }
                    dev->Release();
                }
                DestroyWindow(w);
            }
        }

        if (resizeAddr)
        {
            g_oResizeBuffers = (ResizeBuffers_t)resizeAddr;
            if (DetourAttach((PVOID*)&g_oResizeBuffers, (PVOID)HK_ResizeBuffers) != NO_ERROR)
                g_oResizeBuffers = nullptr;
        }
        if (eclAddr)
        {
            g_oECL = (ECL_t)eclAddr;
            if (DetourAttach((PVOID*)&g_oECL, (PVOID)HK_ECL) != NO_ERROR)
                g_oECL = nullptr;
        }
        if (present1Addr && present1Addr != presentAddr)
        {
            g_oPresent1 = (Present1_t)present1Addr;
            if (DetourAttach((PVOID*)&g_oPresent1, (PVOID)HK_Present1) != NO_ERROR)
                g_oPresent1 = nullptr;
        }

        rv = DetourTransactionCommit();
        if (rv != NO_ERROR)
        {
            Log::Printf("[Render] commit failed %ld", rv);
            return false;
        }

        g_hooksInstalled = true;
        Log::Printf("[Render] hooks installed (Present=%p Present1=%p Resize=%p ECL=%p)",
                    presentAddr, (void*)g_oPresent1, (void*)g_oResizeBuffers, (void*)g_oECL);
        return true;
    }

    // ============ 卸载（worker 线程调用，渲染线程已因 ExitRequested 早退） ============
    void Shutdown()
    {
        if (!g_hooksInstalled)
            return;

        // 1) 停止一切钩子内活动
        g_imguiReady.store(false, std::memory_order_release);
        Sleep(150);

        // 2) 恢复窗口过程（必须最先做，防止消息进入已释放代码）
        if (g_hwnd && g_origWndProc)
        {
            SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
            g_origWndProc = nullptr;
            Log::Printf("[Render] wndproc restored");
        }

        // 3) ImGui / 后端清理
        if (ImGui::GetCurrentContext())
        {
            if (g_api.load(std::memory_order_relaxed) == Api::D3D12)
                ImGui_ImplDX12_Shutdown();
            else if (g_api.load(std::memory_order_relaxed) == Api::D3D11)
                ImGui_ImplDX11_Shutdown();
            ImGui_ImplWin32_Shutdown();
            ImGui::DestroyContext();
        }

        // 4) 释放 D3D 资源
        ReleaseRT12();
        ReleaseRT11();
        ReleaseCapture();
        if (g_cmdList12) { g_cmdList12->Release(); g_cmdList12 = nullptr; }
        for (auto& fc : g_frames12)
            if (fc.allocator) { fc.allocator->Release(); fc.allocator = nullptr; }
        if (g_fence12) { g_fence12->Release(); g_fence12 = nullptr; }
        if (g_fenceEvent12) { CloseHandle(g_fenceEvent12); g_fenceEvent12 = nullptr; }
        if (g_rtvHeap12) { g_rtvHeap12->Release(); g_rtvHeap12 = nullptr; }
        if (g_srvHeap12) { g_srvHeap12->Release(); g_srvHeap12 = nullptr; }
        if (g_queue12) { g_queue12->Release(); g_queue12 = nullptr; }
        if (g_dev12) { g_dev12->Release(); g_dev12 = nullptr; }
        if (g_ctx11) { g_ctx11->Release(); g_ctx11 = nullptr; }
        if (g_dev11) { g_dev11->Release(); g_dev11 = nullptr; }
        if (g_sc3) { g_sc3->Release(); g_sc3 = nullptr; }
        g_sc = nullptr;

        // 5) 摘掉 DXGI 钩子
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach((PVOID*)&g_oPresent, (PVOID)HK_Present);
        if (g_oPresent1) DetourDetach((PVOID*)&g_oPresent1, (PVOID)HK_Present1);
        if (g_oResizeBuffers) DetourDetach((PVOID*)&g_oResizeBuffers, (PVOID)HK_ResizeBuffers);
        if (g_oECL) DetourDetach((PVOID*)&g_oECL, (PVOID)HK_ECL);
        DetourTransactionCommit();

        g_hooksInstalled = false;
        Log::Printf("[Render] shutdown complete");
    }

    void GetGameWindowSize(float* w, float* h)
    {
        *w = 1280.f; *h = 720.f;
        if (g_hwnd)
        {
            RECT rc;
            if (GetClientRect(g_hwnd, &rc))
            {
                *w = (float)(rc.right - rc.left);
                *h = (float)(rc.bottom - rc.top);
            }
        }
    }

    // ============ 皮肤贴图加载（WIC 解码，DX11 / DX12 各一条上传路径） ============
    // WIC → RGBA8 像素（两条 API 共用）
    static bool DecodeImageRGBA(const char* path, std::vector<BYTE>* px, UINT* outW, UINT* outH)
    {
        IWICImagingFactory* wic = nullptr;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* conv = nullptr;
        bool ok = false;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateDecoderFromFilename(std::wstring(path, path + strlen(path)).c_str(),
                                                     nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                     &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &frame)) &&
            SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                       WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom)))
        {
            UINT w = 0, h = 0;
            conv->GetSize(&w, &h);
            px->resize((size_t)w * h * 4);
            WICRect rc = { 0, 0, (INT)w, (INT)h };
            if (SUCCEEDED(conv->CopyPixels(&rc, w * 4, (UINT)px->size(), px->data())) && w && h)
            {
                *outW = w; *outH = h;
                ok = true;
            }
        }
        if (conv) conv->Release();
        if (frame) frame->Release();
        if (dec) dec->Release();
        if (wic) wic->Release();
        return ok;
    }

    static std::map<std::string, void*>& TextureCache()
    {
        static std::map<std::string, void*> s_cache;
        return s_cache;
    }

    // WIC 内存解码（RCDATA 内嵌 PNG 用；流式解码，无需落盘）
    static bool DecodeImageRGBAFromMemory(const void* data, size_t size,
                                          std::vector<BYTE>* px, UINT* outW, UINT* outH)
    {
        if (!data || size < 8)
            return false;
        IWICImagingFactory* wic = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapDecoder* dec = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* conv = nullptr;
        bool ok = false;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&wic))) &&
            SUCCEEDED(wic->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory((BYTE*)data, (DWORD)size)) &&
            SUCCEEDED(wic->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) &&
            SUCCEEDED(dec->GetFrame(0, &frame)) &&
            SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                       WICBitmapDitherTypeNone, nullptr, 0.f, WICBitmapPaletteTypeCustom)))
        {
            UINT w = 0, h = 0;
            conv->GetSize(&w, &h);
            px->resize((size_t)w * h * 4);
            WICRect rc = { 0, 0, (INT)w, (INT)h };
            if (SUCCEEDED(conv->CopyPixels(&rc, w * 4, (UINT)px->size(), px->data())) && w && h)
            {
                *outW = w; *outH = h;
                ok = true;
            }
        }
        if (conv) conv->Release();
        if (frame) frame->Release();
        if (dec) dec->Release();
        if (stream) stream->Release();
        if (wic) wic->Release();
        return ok;
    }

    // DX11：RGBA 像素 → 不可变贴图 + SRV
    static void* UploadRGBA11(const BYTE* px, UINT w, UINT h)
    {
        if (!g_dev11 || !px || !w || !h)
            return nullptr;
        void* srv = nullptr;
        ID3D11Texture2D* tex = nullptr;
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = w; td.Height = h;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = { px, w * 4, 0 };
        if (SUCCEEDED(g_dev11->CreateTexture2D(&td, &sd, &tex)))
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC sd2 = {};
            sd2.Format = td.Format;
            sd2.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd2.Texture2D.MipLevels = 1;
            g_dev11->CreateShaderResourceView(tex, &sd2, (ID3D11ShaderResourceView**)&srv);
            tex->Release();
        }
        return srv;
    }

    void* RenderHook_LoadTextureDX11(const char* path)
    {
        if (g_api.load(std::memory_order_relaxed) != Api::D3D11 || !g_dev11)
            return nullptr;

        auto& cache = TextureCache();
        auto it = cache.find(path);
        if (it != cache.end())
            return it->second;

        void* srv = nullptr;
        std::vector<BYTE> px;
        UINT w = 0, h = 0;
        if (DecodeImageRGBA(path, &px, &w, &h))
            srv = UploadRGBA11(px.data(), w, h);
        if (srv)
            cache[path] = srv;
        return srv;
    }

    // ============ D3D12 贴图上传（WIC → 默认堆资源 + SRV） ============
    // 从 ImGui 的 SRV 描述符堆的同一空闲表里永久占用一格
    static bool AllocHeapSlot(D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
    {
        if (g_srvAlloc.freeCount <= 0 || !g_srvAlloc.heap || !g_srvAlloc.inc)
            return false;
        int idx = g_srvAlloc.freeList[--g_srvAlloc.freeCount];
        cpu->ptr = g_srvAlloc.cpuStart.ptr + (SIZE_T)idx * g_srvAlloc.inc;
        gpu->ptr = g_srvAlloc.gpuStart.ptr + (UINT64)idx * g_srvAlloc.inc;
        return true;
    }

    // DX12：RGBA 像素 → 默认堆贴图 + SRV（上传后同步等待拷贝完成）
    //   依赖尚未就绪时返回 nullptr，调用方下一帧重试
    static void* UploadRGBA12(const BYTE* px, UINT w, UINT h)
    {
        if (!g_dev12 || !g_srvHeap12 || !g_queue12 || !g_fence12 || !g_fenceEvent12)
            return nullptr;
        if (!px || !w || !h)
            return nullptr;

        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        ID3D12Resource* tex = nullptr;
        HRESULT hr = g_dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                      D3D12_RESOURCE_STATE_COPY_DEST,
                                                      nullptr, IID_PPV_ARGS(&tex));
        if (FAILED(hr) || !tex)
            return nullptr;

        UINT64 totalBytes = 0;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
        UINT numRows = 0;
        UINT64 rowPitchBytes = 0;
        g_dev12->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &numRows, &rowPitchBytes, &totalBytes);

        D3D12_HEAP_PROPERTIES up = {};
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC ud = {};
        ud.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ud.Width = totalBytes;
        ud.Height = 1;
        ud.DepthOrArraySize = 1;
        ud.MipLevels = 1;
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.SampleDesc.Count = 1;
        ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ID3D12Resource* upload = nullptr;
        hr = g_dev12->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &ud,
                                              D3D12_RESOURCE_STATE_GENERIC_READ,
                                              nullptr, IID_PPV_ARGS(&upload));
        if (FAILED(hr) || !upload)
        {
            tex->Release();
            return nullptr;
        }

        void* mapped = nullptr;
        D3D12_RANGE readRange = { 0, 0 };
        if (FAILED(upload->Map(0, &readRange, &mapped)) || !mapped)
        {
            upload->Release();
            tex->Release();
            return nullptr;
        }
        for (UINT y = 0; y < numRows; y++)
        {
            memcpy((BYTE*)mapped + fp.Offset + (UINT64)y * fp.Footprint.RowPitch,
                   px + (size_t)y * (size_t)w * 4, (size_t)w * 4);
        }
        upload->Unmap(0, nullptr);

        D3D12_CPU_DESCRIPTOR_HANDLE srvCpu = {};
        D3D12_GPU_DESCRIPTOR_HANDLE srvGpu = {};
        if (!AllocHeapSlot(&srvCpu, &srvGpu))
        {
            Log::Printf("[Render] DX12 tex: SRV heap exhausted");
            upload->Release();
            tex->Release();
            return nullptr;
        }

        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* list = nullptr;
        if (FAILED(g_dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
            FAILED(g_dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr,
                                              IID_PPV_ARGS(&list))))
        {
            if (alloc) alloc->Release();
            upload->Release();
            tex->Release();
            return nullptr;
        }

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = upload;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = fp;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = tex;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        list->ResourceBarrier(1, &barrier);
        list->Close();

        g_queue12->ExecuteCommandLists(1, (ID3D12CommandList* const*)&list);
        const UINT64 waitValue = ++g_fenceLast;
        g_queue12->Signal(g_fence12, waitValue);
        if (g_fence12->GetCompletedValue() < waitValue)
        {
            g_fence12->SetEventOnCompletion(waitValue, g_fenceEvent12);
            WaitForSingleObject(g_fenceEvent12, 3000);
        }
        list->Release();
        alloc->Release();
        upload->Release();

        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        g_dev12->CreateShaderResourceView(tex, &sd, srvCpu);

        return (void*)(uintptr_t)srvGpu.ptr;
    }

    void* RenderHook_LoadTextureDX12(const char* path)
    {
        if (g_api.load(std::memory_order_relaxed) != Api::D3D12)
            return nullptr;
        if (!g_dev12)
            return nullptr;

        auto& cache = TextureCache();
        auto it = cache.find(path);
        if (it != cache.end())
            return it->second;

        std::vector<BYTE> px;
        UINT w = 0, h = 0;
        if (!DecodeImageRGBA(path, &px, &w, &h))
        {
            Log::Printf("[Render] DX12 tex decode failed: %s", path);
            return nullptr;
        }
        void* id = UploadRGBA12(px.data(), w, h);
        if (id)
            cache[path] = id;
        return id;
    }

    // ============ 内嵌资源贴图（LOGO）：RCDATA(PNG) → WIC 内存解码 → 上传 ============
    //   资源随 DLL 编译进来，不依赖外部文件；同一 key 只解码/上传一次。
    void* RenderHook_LoadEmbeddedPng(int resId, const char* key)
    {
        auto& cache = TextureCache();
        std::string k = std::string("res:") + (key ? key : "");
        auto it = cache.find(k);
        if (it != cache.end())
            return it->second;

        HMODULE hm = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)&RenderHook_LoadEmbeddedPng, &hm) || !hm)
            return nullptr;
        HRSRC hr = FindResourceA(hm, MAKEINTRESOURCEA(resId), (LPCSTR)RT_RCDATA);
        if (!hr)
            return nullptr;
        DWORD size = SizeofResource(hm, hr);
        HGLOBAL hg = LoadResource(hm, hr);
        const void* data = hg ? LockResource(hg) : nullptr;
        if (!data || size < 8)
            return nullptr;

        std::vector<BYTE> px;
        UINT w = 0, h = 0;
        if (!DecodeImageRGBAFromMemory(data, size, &px, &w, &h))
        {
            Log::Printf("[Render] embedded png decode failed: %s", key ? key : "?");
            return nullptr;
        }
        void* srv = nullptr;
        Api api = g_api.load(std::memory_order_relaxed);
        if (api == Api::D3D12)
            srv = UploadRGBA12(px.data(), w, h);
        else if (api == Api::D3D11)
            srv = UploadRGBA11(px.data(), w, h);
        if (srv)
        {
            cache[k] = srv;
            Log::Printf("[Render] embedded png ok: %s (%ux%u)", key ? key : "?", w, h);
        }
        return srv;
    }

    // 统一入口：按当前图形 API 分派
    void* RenderHook_LoadTexture(const char* path)
    {
        Api api = g_api.load(std::memory_order_relaxed);
        if (api == Api::D3D12)
            return RenderHook_LoadTextureDX12(path);
        if (api == Api::D3D11)
            return RenderHook_LoadTextureDX11(path);
        return nullptr;
    }

    // ---- 游戏画面缩略图：开关（渲染线程每帧设置）+ 取贴图 ----
    void SetCaptureWanted(bool on)
    {
        g_capWanted.store(on, std::memory_order_relaxed);
    }

    // 录制开关：开启时渲染线程按 GAME→staging→CPU 回读（DO_NOT_WAIT，不阻塞）
    void SetRecordWanted(bool on)
    {
        g_recWanted.store(on, std::memory_order_relaxed);
    }

    void* GetCaptureTex(int* w, int* h)
    {
        if (w) *w = 0;
        if (h) *h = 0;
        Api api = g_api.load(std::memory_order_relaxed);
        if (api == Api::D3D11)
        {
            if (!g_capSrv11)
                return nullptr;
            if (w) *w = (int)g_capW11;
            if (h) *h = (int)g_capH11;
            return g_capSrv11;
        }
        if (api == Api::D3D12)
        {
            if (!g_capId12)
                return nullptr;
            if (w) *w = (int)g_capW12;
            if (h) *h = (int)g_capH12;
            return g_capId12;
        }
        return nullptr;
    }

    // ---- 皮肤贴图信息：自然尺寸 + 非透明包围盒（缓存） ----
    struct ImgInfo { int w = 0, h = 0, bx = 0, by = 0, bw = 0, bh = 0; bool ok = false; };
    static std::map<std::string, ImgInfo>& InfoCache()
    {
        static std::map<std::string, ImgInfo> c;
        return c;
    }
    bool RenderHook_ImageInfo(const char* path, int* w, int* h, int* bx, int* by, int* bw, int* bh)
    {
        if (!path || !path[0]) return false;
        auto& cache = InfoCache();
        auto it = cache.find(path);
        if (it == cache.end())
        {
            ImgInfo inf{};
            std::vector<BYTE> px;
            UINT iw = 0, ih = 0;
            if (DecodeImageRGBA(path, &px, &iw, &ih) && iw > 0 && ih > 0 && px.size() >= (size_t)iw * ih * 4)
            {
                int x0 = (int)iw, y0 = (int)ih, x1 = -1, y1 = -1;
                for (UINT y = 0; y < ih; y++)
                {
                    const BYTE* row = &px[(size_t)y * iw * 4];
                    for (UINT x = 0; x < iw; x++)
                    {
                        if (row[x * 4 + 3] > 8)
                        {
                            if ((int)x < x0) x0 = (int)x;
                            if ((int)x > x1) x1 = (int)x;
                            if ((int)y < y0) y0 = (int)y;
                            if ((int)y > y1) y1 = (int)y;
                        }
                    }
                }
                inf.w = (int)iw; inf.h = (int)ih;
                if (x1 >= x0 && y1 >= y0) { inf.bx = x0; inf.by = y0; inf.bw = x1 - x0 + 1; inf.bh = y1 - y0 + 1; }
                else { inf.bx = 0; inf.by = 0; inf.bw = (int)iw; inf.bh = (int)ih; }
                inf.ok = true;
            }
            it = cache.emplace(path, inf).first;
        }
        const ImgInfo& inf = it->second;
        if (!inf.ok) return false;
        if (w) *w = inf.w;
        if (h) *h = inf.h;
        if (bx) *bx = inf.bx;
        if (by) *by = inf.by;
        if (bw) *bw = inf.bw;
        if (bh) *bh = inf.bh;
        return true;
    }
}

// 全局包装（供 Chart4K 调用）
void* RenderHook_LoadTextureDX11(const char* path)
{
    return RenderHook::RenderHook_LoadTextureDX11(path);
}

void* RenderHook_LoadTexture(const char* path)
{
    return RenderHook::RenderHook_LoadTexture(path);
}

void* RenderHook_LoadEmbeddedPng(int resId, const char* key)
{
    return RenderHook::RenderHook_LoadEmbeddedPng(resId, key);
}

bool RenderHook_ImageInfo(const char* path, int* w, int* h, int* bx, int* by, int* bw, int* bh)
{
    return RenderHook::RenderHook_ImageInfo(path, w, h, bx, by, bw, bh);
}

void RenderHook_SetCaptureWanted(bool on)
{
    RenderHook::SetCaptureWanted(on);
}

void RenderHook_SetRecordWanted(bool on)
{
    RenderHook::SetRecordWanted(on);
}

void* RenderHook_GetCaptureTex(int* w, int* h)
{
    return RenderHook::GetCaptureTex(w, h);
}
