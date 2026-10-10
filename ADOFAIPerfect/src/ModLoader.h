#pragma once
// ============================================================
// ModLoader.h — MOD 内核（官方 UnityModManager）的 C++ 侧桥接
//
//  内核 = 官方 UnityModManager 0.32.5（umm\UnityModManager.dll，MIT，
//  与 MOD-MANAGER 分发的逐字节相同）。由本模块安装进游戏：
//    <游戏>_Data\Managed\UnityModManager\   官方内核（dll/Harmony/dnlib/Config.xml）
//    <游戏>_Data\Managed\AdofPerfectUmm\    桥接 Sidecar（AdofPerfectUmm.dll）
//    <游戏>\winhttp.dll + doorstop_config.ini（相对路径，与 MOD-MANAGER 同款）
//  更早版本的自研加载器（legacy）已退役：检测到即自动迁移到官方内核并清残留。
//
//  真正的“加载/驱动/界面”都在官方内核托管侧完成；本模块只负责：
//    · 安装 / 检查内核与 Sidecar
//    · 在游戏主线程（mono 托管线程）上通过 Mono API 调用
//      AdofPerfectUmm.Bridge（反射桥），查询 MOD 列表 / 开关 / 打开设置
//
//  线程模型：所有 Mono 调用都排进 GameBridge::QueueMainThreadWork，
//  由游戏主线程执行；渲染线程只读缓存结果。
// ============================================================
#include <string>

namespace ModLoader
{
    // ---- 安装与配置 ----
    bool        Installed();                 // 加载器文件是否齐备
    std::string LoaderDirPath();             // <游戏>_Data\Managed\UnityModManager
    std::string ModsDir();                   // 当前生效的 MOD 目录
    void        SetModsDir(const std::string& dir);   // 写入 AdofPerfectUmm.json
    bool        Install(std::string* message, bool force = false);   // 安装/更新加载器（幂等）
                                                             // force=true 时允许覆盖原版 UMM 的启动钩子

    // ---- 每帧驱动（渲染线程）----
    void        Tick();                      // 节流轮询：确保加载器启动 + 取状态

    // ---- 结果读取（渲染线程；来自最近一次主线程查询）----
    bool        Ready();                     // 加载器已在游戏里跑起来
    bool        Passive();                   // 原版 UMM 在场：已转入 passive 复用模式（不重复加载）
    std::string StateJson();                 // [{id,name,version,author,enabled,active,loaded,error,gui,open}]
    std::string LogTail();                   // 加载器最近日志
    std::string LastError();

    // ---- 操作（异步，主线程执行）----
    void SetEnabled(const std::string& id, bool on);
    void OpenSettings(const std::string& id);
    void CloseSettings(const std::string& id);
    void CloseAllWindows();
    void Reload();
    void SetTheme(const std::string& hexRgb);   // 把工具当前皮肤强调色推给游戏内窗口

    // 保存设置之后调用：把新的设置值热应用进正在运行的 MOD。
    // toggle=true 时额外把 MOD 关掉/打开一次，让只在 OnToggle 里应用设置的 MOD 也生效。
    void ApplySettings(const std::string& id, bool toggle);
    bool TakeApplyResult(std::string& id, std::string& res);   // 取一次「应用」结果（线程安全）
}
