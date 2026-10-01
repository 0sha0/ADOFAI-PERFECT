# ADOFAI PERFECT

**A Dance of Fire and Ice** 的 ImGui 本地辅助工具 · Powered by **SHASHEN4404**

> 适用于 Steam 版 ADOFAI（Unity 6000.3.10f1 / Mono / D3D11）。
> 纯 C++ / VS2022 解决方案 · Dear ImGui 1.92 覆盖层 · 直接读写游戏 Mono 运行时内部状态。

[![Release](https://img.shields.io/badge/download-Release-blueviolet)](../../releases)
![platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-blue)
![game](https://img.shields.io/badge/game-ADOFAI%20Steam-orange)

## ✨ 功能

| 功能 | 说明 |
|---|---|
| 🛡 **不死模式** | 失误不再死亡，连断后可继续游玩；**官方关卡同样可用**，通关结算照常显示最终精准度 |
| ⚡ **自动连击** | 游戏自动完美命中每一块地砖（内置 Auto），**官方关卡同样可用**，连击永不断 |
| 📊 **实时状态** | 关卡 / 场景状态 / 进度条 / 实时 Acc & XAcc / 死亡次数 / 检查点数 |
| 🎛 **现代 UI** | 半透明正方形小窗 · 功能 / 实时状态双页 · 可折叠成一枚可拖动的小圆点 |

两个开关均可随时点击取消，取消后立即还原游戏原始状态（`GCS.useNoFail`）。

## 📦 下载

前往 [Releases](../../releases) 下载 `ADOFAI-PERFECT-vX.X.zip`（免构建，解压即用），
或按下方说明自行构建。

## 🚀 使用

1. 启动游戏（到标题画面即可）
2. 运行 `Injector.exe`（或双击 `run.cmd`，会自动等待游戏与 Mono 就绪后注入）
3. 游戏内出现悬浮窗：

| 按键 / 操作 | 作用 |
|---|---|
| **Insert** | 显示 / 隐藏面板 |
| **End** | 卸载模块并还原游戏状态 |
| 标题栏 **—** | 折叠成小圆点（圆点可拖动，点击展开） |

## 🔨 构建

- Visual Studio 2022（v143，x64）
- vcpkg：`vcpkg install detours:x64-windows`（默认查找 `D:\vcpkg`，可在 vcxproj 中改 `VcpkgRoot`）
- `deps/imgui/`：Dear ImGui 1.92（已附带）

打开 `ADOFAI-PERFECT.sln` → `Release | x64` → 生成即可，产物位于 `bin/Release/`。

## ⚙️ 实现要点

- **注入**：`Injector` 等待游戏进程与 `mono-2.0-bdwgc.dll` 加载后 `CreateRemoteThread + LoadLibraryW` 注入
- **渲染**：一次性哑设备取 `IDXGISwapChain::Present/Present1/ResizeBuffers` 与
  `ID3D12CommandQueue::ExecuteCommandLists` 真实地址，Detours 挂钩；
  首帧识别图形 API（D3D11/D3D12 双支持）并初始化 ImGui
- **游戏交互**（详见源码注释，均为踩坑结论）：
  - Unity 6 的 Mono 为协作式运行时——**任何 mono API 在非托管线程调用都会直接带崩游戏**
    （含 `mono_field_static_get_value`、`mono_compile_method`）
  - `mono_class_init / mono_class_vtable(创建)` 仅在游戏主线程执行（PostMessage → WndProc 投递）
  - 运行时读写 = 缓存字段偏移 + 纯指针运算，**零 mono API 调用**（Boehm GC 不移动对象）
  - 引用型静态字段的 `field->offset` 不可靠，采用静态数据区扫描 + 类指针/类名匹配发现
- **不死模式不覆盖 hitbox 死亡**（需 JIT detour，官方关卡亦无 hitbox 地块）
- 使用不死模式且有死亡记录时，游戏不会把该成绩写入最佳记录，但结算照常显示精准度

## 📁 目录

```
├── ADOFAI-PERFECT.sln
├── run.cmd                  一键注入
├── ADOFAIPerfect/           DLL：Mono 桥接 + 渲染挂钩 + UI
├── Injector/                注入器
└── deps/imgui/              Dear ImGui 1.92
```

## ⚠️ 免责声明

本项目仅供**单机游戏的学习与交流**用途，请勿用于破坏他人体验或商业用途。
使用本工具产生的存档/成就等后果由使用者自行承担。 underlying game
© 7th Beat Games。

---
**ADOFAI PERFECT** © 2026 SHASHEN4404 · [LICENSE](LICENSE)
