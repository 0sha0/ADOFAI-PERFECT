<p align="center">
  <img src="assets/adoFaiPerfect-icon.png" width="132" alt="ADOFAI PERFECT">
</p>

# ADOFAI PERFECT

**A Dance of Fire and Ice** 的本地辅助工具 · Powered by **SHASHEN4404**

> 适用于 Steam 版 ADOFAI（Unity 6000 / Mono / D3D11 & D3D12）。
> 纯 C++20 / VS2022 解决方案 · Dear ImGui 1.92 覆盖层 · 直接读写游戏 Mono 运行时内部状态，
> 把**无轨谱面实时转换成 Malody 式 4K / 6K 下坠谱**，边玩原版边打下落式。
> 无需 vcpkg、无需 .NET、**发布版不依赖 VC 运行库**（静态链接 CRT，纯净系统可直接运行）。

[![Release](https://img.shields.io/badge/download-Release-blueviolet)](../../releases)
![platform](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-blue)
![game](https://img.shields.io/badge/game-ADOFAI%20Steam-orange)
![license](https://img.shields.io/badge/license-MIT-green)

## 功能一览

| 功能 | 说明 |
|---|---|
| 🛡 **不死模式** | 失误不再死亡，连断后可继续游玩；官方关卡同样可用，结算照常显示精准度 |
| ⚡ **自动连击** | 自动完美命中每一块地砖；官方 / 自定义 / 额外关卡均可用 |
| 📊 **实时状态** | 关卡 / 状态 / 进度 / 实时 Acc & XAcc / 死亡 / 检查点 |
| 🎹 **4K 下坠谱面** | 无轨 → **DFJK** 四键，Malody Rurudo 皮肤；连打换手、大回转外键重音、长按条 |
| 🎼 **6K 下坠谱面** | 无轨 → **SDFJKL** 六键，独立引擎：24 音双螺旋循环，长时间跑动依旧左右均匀 |
| ⌨️ **键位自定义** | 4K / 6K 键位都能改，**点「确定」才真正生效**，改错可「取消」 |
| 👀 **画面小窗** | 左下角常驻一张无覆盖层的游戏画面缩略图，位置 / 大小可调 |
| 💬 **判定提示** | 中央 Malody 式 **PERFECT / GOOD / MISS** 弹字 + 打击特效 |
| 🎨 **现代 UI** | 暗色系界面，标题栏 + TAB 分页，可拖动、右下角拖拽改窗口大小，支持折叠 |

## 下载

前往 [Releases](../../releases) 下载 `ADOFAI-PERFECT-vX.X-win64.zip`（免构建，解压即用），
或按下方说明自行构建。

## 使用

1. 启动游戏（到标题画面即可）
2. 运行 `Injector.exe`（或双击 `run.cmd`，会自动等待游戏与 Mono 就绪后再注入）
3. 游戏内出现悬浮窗：

| 按键 / 操作 | 作用 |
|---|---|
| **Insert** | 显示 / 隐藏面板 |
| **End** | 卸载模块并还原游戏状态 |
| 标题栏拖动 / 右下角拖拽 | 移动窗口 / 调整窗口大小（控件自适应） |
| 标题栏 **—** | 折叠成小圆点（圆点可拖动，点击展开） |

### 4K / 6K 下坠谱

在 **4K辅助** / **6K模式** 页打开开关即可。**谱面只在关卡真正开始后下落**，选歌、加载、暂停时不会提前掉。

- 4K 默认键位 **D F J K**，6K 默认键位 **S D F J K L**，两个模式互斥
- 流速 / 延迟（ms）/ 底板不透明度可调；延迟用于对齐音画（音画不同步时微调它）
- 判定、连击、Acc、Lv 定级全部按 Malody 规则实时统计
- 支持官方关卡、自定义关卡、额外关卡

### 修改键位

`4K辅助` / `6K模式` 页 → **键位** → `修改`：

1. 点一个键位槽 → 直接按键盘上的新键（按 `Esc` 取消捕获）
2. 键位重复会变红提示，不允许确定
3. 点 **`确定`** 才真正写入生效，点 `取消` 原样回退
4. `恢复默认` 一键回到 DFJK / SDFJKL

### 皮肤（Malody Rurudo，已内置）

4K / 6K 的贴图直接复用 Malody Rurudo 皮肤（音符、判定线、按键、小人等），发布包内已自带 `skin\` 文件夹，
**开箱即用、无需任何配置**。程序按以下顺序解析皮肤目录：

1. 环境变量 `ADOFAI_PERFECT_SKIN`
2. 运行目录（`Injector.exe` / `ADOFAIPerfect.dll` 同级）下的 `skin_dir.txt`（第一行写自定义皮肤目录）
3. 运行目录下的 `skin\`（发布包自带；若内含多个子目录，自动选择第一个包含 `rurudokey.png` 的）
4. 兜底：注册表 Steam 安装目录及各常见 Steam 库下的 `steamapps\common\MalodyV\skin\<皮肤>`

皮肤目录需包含 `rurudokey.png`；想换皮肤时把 `skin_dir.example.txt` 改名为 `skin_dir.txt` 并填写路径即可。
找不到皮肤时功能仍可用，只是没有贴图。

## 构建

- Visual Studio 2022（v143，x64），C++20
- 依赖已内置，**不需要 vcpkg**：
  - `deps/imgui/` — Dear ImGui 1.92
  - `deps/detours/` — Microsoft Detours（头文件 + 静态库）
- Release 使用 **静态 CRT**（`/MT`），产物不依赖 `vcruntime140.dll` / `msvcp140.dll`

打开 `ADOFAI-PERFECT.sln` → `Release | x64` → 生成，产物在 `bin/Release/`：

```
bin/Release/Injector.exe        注入器（带图标）
bin/Release/ADOFAIPerfect.dll   覆盖层模块
```

## 目录

```
├── ADOFAI-PERFECT.sln
├── run.cmd                     一键注入
├── assets/                     图标资源（png + ico）
├── skin/                       Malody Rurudo 皮肤（运行目录解析，发布包自带）
├── ADOFAIPerfect/              DLL：Mono 桥接 + 渲染挂钩 + ImGui 界面 + 4K/6K 引擎
├── Injector/                   注入器
└── deps/                       imgui / detours（已内置）
```

## FAQ

**Q：提示缺少 vcruntime140.dll / msvcp140.dll？**
A：本项目 Release 为静态 CRT，不存在该依赖。若仍报错说明下载的是自行构建的 Debug 版本，请用 Release 产物。

**Q：4K/6K 转谱失败或谱面不动？**
A：先进入关卡再打开开关；日志在 `%LOCALAPPDATA%\\ADOFvec\\adofai_perfect.log`，里面有转谱诊断行。

**Q：音画不同步？**
A：用 4K / 6K 页的「延迟」滑杆微调（毫秒）。

**Q：改键位后想反悔？**
A：没点「确定」就点「取消」；已经确定的点「恢复默认」。

## 免责声明

本项目仅供**单机游戏的学习与交流**用途，请勿用于破坏他人体验或商业用途。
使用本工具产生的存档 / 成就等后果由使用者自行承担。游戏本体 © 7th Beat Games，皮肤版权归原作者所有。

---
**ADOFAI PERFECT** © 2026 SHASHEN4404 · [LICENSE](LICENSE)
