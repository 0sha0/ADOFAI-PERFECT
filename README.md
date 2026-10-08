<p align="center">
  <img src="assets/adoFaiPerfect-icon.png" width="128" alt="ADOFAI PERFECT">
</p>

<h1 align="center">ADOFAI PERFECT</h1>

<p align="center">
  <b>A Dance of Fire and Ice</b> 本地辅助工具 · v1.2 · Powered by <b>SHASHEN4404</b>
</p>

<p align="center">
  <a href="../../releases"><img src="https://img.shields.io/badge/download-Release-blueviolet" alt="Release"></a>
  <img src="https://img.shields.io/badge/version-1.2-brightgreen" alt="version">
  <img src="https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-blue" alt="platform">
  <img src="https://img.shields.io/badge/game-ADOFAI%20Steam-orange" alt="game">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C" alt="cpp">
  <img src="https://img.shields.io/badge/license-MIT-green" alt="license">
</p>

---

## 这是什么

一个注入式（DLL）辅助工具，覆盖在 Steam 版《A Dance of Fire and Ice》上运行：

- **实时转谱**：直接读游戏 Mono 运行时的关卡数据（砖块时间 / 转向角 / 长按），
  把**无轨谱面**实时转成 **4K / 5K / 6K / 10K 下坠谱**，边玩原版边打下落式；
- **冰与火宏**：宏直接代打**游戏本体**判定线，成绩真实（不是 100% 自动演奏）；
- **辅助读谱 / KeyViewer / 录制 / 皮肤系统**等一整套工具。

技术栈：纯 C++20 + VS2022 · Dear ImGui 1.92 覆盖层 · Microsoft Detours ·
**静态 CRT（`/MT`）—— 发布版不需要 VC 运行库，也不需要 .NET**，解压即用。

---

## 功能一览

| 页面 | 功能 |
|---|---|
| **功能** | 不死模式（失误不死亡、断连可续打）、自动连击（官方 / 自定义 / 额外关卡通用）、一键卸载并还原游戏 |
| **状态** | 关卡名 / 游戏状态 / 进度 / 实时 Acc 与 XAcc / 死亡次数 / 检查点 / 实时角度与 BPM |
| **读谱** | 无轨读谱辅助：侧边 / 底部横条 / 顶部布局、提前量与密度、时间网格、转盘与节奏提示、标记信息、校准；打歌时也能看（底部横条不挡视线） |
| **4K / 5K / 6K / 10K** | 四套**互相独立**的下坠谱引擎（默认键位 `DFJK` / `SDFJK` / `SDFJKL` / `ASDFG` + `HJKL;`），键位可改；流速、延迟、判定宽度、上隐、下隐、自动调整延迟、底板透明度、打击特效与判定文字、小人、小窗位置与尺寸、**自动打歌**、**宏打歌**、录制开关 |
| **按键（KeyViewer）** | 类经典 KeyViewer 的按键反馈：按下高亮、按键次数、KPS、总 KPS、逐键自定义、任意键位数（不止 4 键）、位置与缩放 |
| **皮肤** | 一键切换皮肤；**支持导入 Malody `.msp` 皮肤包**（MSP 字段解析 + `info.lua` 脚本），内置皮肤与其他皮肤互不影响 |
| **录制** | 小窗录制游戏原生窗口：开始 / 暂停 / 继续、输出目录、帧率、码率、实时统计（H.264 / MP4 硬件编码，掉帧丢弃不卡游戏） |
| **宏模式** | **冰与火模式**（宏代打游戏本体）+ 4K/5K/6K/10K 宏打歌；目标精准度 90–100%、拟人程度 0–100%、按歌曲一键校准延迟 |
| **设置** | 选择**冰与火之舞游戏目录**（自动探测 + 手动浏览）、i18n 语言切换（简体中文 / 繁體中文 / English / 日本語 / Русский）、配置文件整套保存 / 加载 |
| **关于** | LOGO、软件名称、版本 1.2、演示视频（打开本页自动跳转）、GitHub 仓库、配置目录 |

页签顺序：`功能 → 状态 → 读谱 → 4K → 5K → 6K → 10K → 按键 → 皮肤 → 录制 → 宏模式 → 设置 → 关于`

---

## 冰与火宏模式

在「宏模式」页勾选 **冰与火模式** 后，工具按下面的链条代打：

1. 每帧读取当前砖块判定现场（下一砖、角度误差、`crotchet`、转速、当前状态）；
2. 用与 4K 宏打歌同一套**拟人抖动模型**算出目标毫秒偏移，换算成角度；
3. 在误差到达目标角（含半帧量化补偿）时调用游戏自己的
   `scrPlayer.Hit(isAuto: false)` —— 与玩家出刀同一条判定链：
   `scrPlayer.Hit → scrPlanet.SwitchChosen → scrMisc.GetHitMargin`；
4. 长按砖按游戏 Otto 逻辑在结尾再补一次。

因此**连击 / 准确率 / 失误全部由游戏本体记录**（目标精准度与拟人程度越高越接近真人成绩），
且**不注入任何系统输入，不动你的鼠标键盘**。

- **开局无需人工按键**：勾选后会写游戏自己的 `scrController.levelWasSkipped` 标记，
  自动跳过「按任意键开始」的等待（与 debug 的 Beat Level 同源，只跳过等待、不结算关卡）。
- 宏模式与游戏内置自动演奏互斥（开启时抑制 `RDC.auto`），并强制不死，避免拟人失误直接死关卡。
- 支持官方 / 自定义 / 额外关卡。

## 下坠谱（4K / 5K / 6K / 10K）

1. 先在游戏里**真正进入关卡**；
2. 到对应页打开「**N K 下坠谱面**」开关（四种模式互斥）；
3. 谱面按当前关卡实时生成，**开始打歌后**才下落；判定线在面板下方，显示连击与中央 PERFECT / GOOD / MISS。

### 转换风格（每套模式都能选）

| 风格 | 说明 |
|---|---|
| **经典** | 每个模式的原生引擎（连打换手、外键重音、长按条） |
| **叠** | Jack 向：同轨连续重音、短促叠音 |
| **技** | Tech 向：楼梯 / 交叉 / 组合位移 |
| **乱** | Random 向：走向打散，避免规律感 |
| **切** | Trill 向：两键切分、双键交替 |
| **冰火手法** | 按 ADOFAI 键盘手法生成（`AdoGen.h`）：交互 / 轮指 / 混指插指 / 押，长按占手自动拆手 |

### 改键

点某个键槽后按新键，**必须点「确认」才真正生效**；「取消」原样回退，「恢复默认」回到该模式默认键位。

---

## 录制

- **开始 / 暂停 / 继续**：暂停 = 收尾当前 MP4（写 moov），继续 = 新开一段文件；
- **输出目录**：可直接编辑 / 应用 / 一键打开；
- **帧率 15–144、码率 2–100 Mbps**：Media Foundation H.264 硬件编码，编码队列满时丢帧而不卡游戏；
- **实时统计**：写入帧数 / 总帧数 / 是否掉帧 / 当前文件名；
- 也可在 4K/5K/6K/10K 页打开「录制」开关，或在录制页勾选「自动」跟随打歌自动开始。

## 皮肤

- **内置皮肤**：发布包自带 `skin\`，开箱即用，且不受外部皮肤影响；
- **外部皮肤**：皮肤页 → **导入 MSP 皮肤…** 选 `.msp` 包即可导入并切换
  （也可把皮肤文件夹放进运行目录的 `skin\` 后点「重新扫描」）；
- 皮肤目录解析顺序：环境变量 `ADOFAI_PERFECT_SKIN` → `adofai_perfect.cfg` 的 `skin_dir=` → 运行目录 `skin\`；
- 找不到皮肤时功能仍可用，只是没有贴图。

---

## 下载与使用

前往 [Releases](../../releases) 下载 `ADOFAI-PERFECT-vX.X-win64.zip`（免构建），或按下方说明自行构建。

1. 启动游戏（到标题画面即可）
2. 双击 `run.cmd`（或运行 `Injector.exe`，会自动等待游戏与 Mono 就绪后再注入）
3. 游戏内出现悬浮面板后即可操作

| 按键 / 操作 | 作用 |
|---|---|
| **Insert** | 显示 / 隐藏面板 |
| **End** | 卸载模块并还原游戏（也可在「功能」页点卸载） |

---

## 构建

- Visual Studio 2022（v143，x64），C++20
- 依赖（全部随仓库提供）：

| 依赖 | 版本 | 位置 |
|---|---|---|
| Dear ImGui | 1.92 | `deps/imgui/` |
| Microsoft Detours | 头文件 + `detours.lib` | `deps/detours/` |
| Lua | 5.3.x 源码 | `deps/lua/` |
| sol2 | v3.3.0 单头文件 | `deps/sol2/` |

打开 `ADOFAI-PERFECT.sln` → `Release | x64` → 生成：

```
bin/Release/Injector.exe        注入器
bin/Release/ADOFAIPerfect.dll   覆盖层模块（内嵌 LOGO 资源）
```

Release 使用**静态 CRT**（`/MT`），产物不依赖 `vcruntime140.dll` / `msvcp140.dll`。

> 本地跑起来还需要把仓库根目录的 `skin\` 复制到 `bin\Release\`（皮肤按“运行目录 `skin\`”解析；
> 直接用 Release 里的 zip 则已经带好）。

> ⚠️ **不要用「重新生成（Rebuild）」**：MSBuild 的 Clean 会清空 `bin\Release\`，
> 把 `skin\`、`adofai_perfect.cfg`、`Injector.exe` 一起删掉。请用「生成（Build）」；
> 需要全量重编时先把运行目录备份出来（`obj\` 可随时删，只影响编译缓存）。

## 目录

```
├── ADOFAI-PERFECT.sln
├── run.cmd                     一键注入
├── assets/                     图标 / LOGO（PNG + ICO，构建时 LOGO 编进 DLL）
├── skin/                       内置皮肤（发布包带着它）
├── ADOFAIPerfect/
│   ├── ADOFAIPerfect.rc        版本资源（1.2.0.0）+ 内嵌 LOGO（RCDATA）
│   └── src/
│       ├── Menu.cpp            主界面（标题栏 / 页签 / 功能 / 状态 / 关于）
│       ├── ChartCore.cpp/.h    下坠谱核心：取谱、分键、判定、贴图、页面
│       ├── AdoGen.h            冰火手法生成引擎（交互 / 轮指 / 混插 / 押）
│       ├── Chart4K/5K/6K/10K   四套独立引擎与设置页
│       ├── SightRead.cpp       辅助读谱（无轨）
│       ├── KeyViewer.cpp       按键反馈
│       ├── SkinMsp.cpp / SkinLua.cpp   MSP 皮肤解析 + info.lua（sol2）
│       ├── GameRecorder.cpp    原生画面录制
│       ├── RenderHook.cpp      DXGI/D3D 挂钩 + ImGui + 贴图上传
│       ├── GameBridge.cpp      Mono 桥接（关卡 / 状态 / 判定现场）
│       ├── GameDetour.cpp      Detours 钩子（不死 / 退出 / 冰与火宏）
│       ├── Lang.cpp            i18n（5 语言，可追加字符串）
│       └── GameDir.cpp         游戏目录探测
├── Injector/                   注入器
└── deps/                       imgui / detours / lua / sol2
```

## 仓库内容与第三方许可

| 内容 | 说明 |
|---|---|
| `ADOFAIPerfect/` · `Injector/` · `deps/` · `ADOFAI-PERFECT.sln` | 源码 + 依赖，可完整构建（VS2022 / x64 / Release） |
| `assets/` | 图标与 LOGO（构建时 LOGO 编进 DLL） |
| `skin/` | 内置皮肤（发布包需要它） |
| `README.md` · `使用说明.txt` · `LICENSE` · `run.cmd` | 文档与一键注入脚本 |
| `ADOFAI-PERFECT-v1.2.0-win64.zip` | 发布包（上传到 Releases 即可；`.gitignore` 已忽略 `*.zip`，不进源码提交） |
| `bin/` · `obj/` | 构建产物，不提交（`.gitignore` 已忽略） |

### 第三方依赖

| 依赖 | 用途 | 许可 | 许可文件 |
|---|---|---|---|
| [Dear ImGui](https://github.com/ocornut/imgui) 1.92 | 覆盖层 GUI | MIT | `deps/imgui/LICENSE.txt` |
| [Microsoft Detours](https://github.com/microsoft/Detours) | 方法挂钩 | MIT | `deps/detours/LICENSE.md` |
| [Lua](https://www.lua.org/) 5.3.x | 皮肤 `info.lua` 脚本 | MIT | `deps/lua/LICENSE.txt` |
| [sol2](https://github.com/ThePhD/sol2) v3.3.0 | Lua ↔ C++ 绑定 | MIT | `deps/sol2/LICENSE.txt` |

> 本项目自身以 MIT 许可发布，见 [LICENSE](LICENSE)；游戏本体 © 7th Beat Games，皮肤版权归原作者所有。

## FAQ

**Q：提示缺少 `vcruntime140.dll` / `msvcp140.dll`？**
A：Release 为静态 CRT，不存在该依赖。若仍报错，说明用的是自行构建的 Debug 产物，请换 Release。

**Q：转谱失败 / 谱面不动？**
A：先真正进入关卡再打开开关；日志 `%LOCALAPPDATA%\ADOFvec\adofai_perfect.log` 里有逐条转谱诊断（砖数、长按数、时长、Lv、风格）。

**Q：音画不同步 / 判定总觉得早或晚？**
A：用对应模式页的「延迟」滑杆微调，或打开「自动调整延迟」按歌曲动态校准；判定宽度可选宽 / 中 / 严。

**Q：冰与火宏不代打？**
A：确认「宏模式」页勾选了冰与火模式，并已真正进入关卡；日志里会出现
`[Detour] fire macro hooked`、`[Detour] ctrl Update hooked` 与 `[fire] hit floor=... ok=1`。

**Q：页面文字被挡住 / 控件重叠？**
A：内容区随窗口宽度自适应并可上下滚动，把窗口拖宽即可完整显示。

**Q：打开「关于」页会弹浏览器？**
A：这是设计行为（自动跳转演示视频）。不想被弹到就停在别的页；本页也有手动按钮。

**Q：注入被拦截？**
A：注入器需要 `CreateRemoteThread` 权限，被杀软 / Defender 拦截时请放行。

## 免责声明

本项目仅供**单机游戏的学习与交流**用途，请勿用于破坏他人体验或商业用途。
使用本工具产生的存档 / 成就等后果由使用者自行承担。
游戏本体 © 7th Beat Games，皮肤版权归原作者所有。

---
**ADOFAI PERFECT** © 2026 SHASHEN4404 · [MIT License](LICENSE)
