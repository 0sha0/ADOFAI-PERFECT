#include "RenderHook.h"
#include "CheatState.h"
#include "GameBridge.h"
#include "Log.h"
#include "Menu.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <detours/detours.h>
#include <atomic>
#include <cstring>

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

    // ============ ImGui 初始化 ============
    static bool InitImGui(IDXGISwapChain* sc, Api api)
    {
        DXGI_SWAP_CHAIN_DESC desc{};
        sc->GetDesc(&desc);
        g_hwnd = desc.OutputWindow;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

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
}
