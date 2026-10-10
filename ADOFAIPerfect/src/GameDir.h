#pragma once
// ============================================================
// GameDir.h — 游戏目录（设置页可选；未设置时 = 进程 exe 所在目录）
//   · 供皮肤/资源定位使用，不依赖任何 MOD 加载器
//   · 持久化在 adofai_perfect.cfg 的 game_dir= 键（I18N::Prefs）
// ============================================================
namespace GameDir
{
    const char* Get();                    // 生效中的游戏目录（无末尾反斜杠）
    void        Set(const char* dir);     // 校验 -> 持久化（空串/无效 = 恢复自动）
    bool        Valid(const char* dir);   // 目录内存在游戏数据 / exe
    void        OpenFolder();
}

// ============================================================
// ShellAsync — 阻塞式 shell / 模态对话框必须离开渲染线程
//   证据：设置页点“浏览…”/“打开”时，这些调用发生在 D3D11 Present 钩子内
//   （渲染线程）。Win32 模态对话框与 ShellExecute(DDE) 会阻塞该线程，
//   而 Unity 主线程每帧都要等渲染线程 → 整个游戏卡死（Windows 事件日志
//   AppHangTransient；日志里 song time 停止推进）。
//   这里统一投递到独立工作线程，UI 线程只轮询结果，绝不阻塞。
// ============================================================
namespace ShellAsync
{
    void Open(const char* utf8Target);                            // 后台 ShellExecuteA("open", target)
    bool PickFolder(const char* titleUtf8);                       // 后台弹出选目录对话框（同时只允许一个）
    bool TakeFolder(char* outUtf8, int n);                        // UI 线程轮询：有结果则取走并返回 true
    bool PickFile(const wchar_t* filter, const wchar_t* title);   // 后台弹出选文件对话框
    bool TakeFile(char* outUtf8, int n);                          // UI 线程轮询：同上
    bool Busy();                                                  // 是否已有对话框在等待用户选择
}
