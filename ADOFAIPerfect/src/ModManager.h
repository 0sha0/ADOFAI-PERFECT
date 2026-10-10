#pragma once
// ============================================================
// ModManager.h — Mod Manager 页面
//
//   · 扫描 UMM 规范的 MOD（<MOD 目录>\<名称>\Info.json）
//   · 每个 MOD 一张卡片，卡片列表可无限向下滚动
//   · 卡片：状态点（绿=启用 / 红=停用）+「开启/关闭」「设置」「删除」
//   · 删除弹自绘 Dialog 二次确认；「设置」用自绘 IMGUI 风格打开 MOD 设置
//   · 启用状态写到 UMM 的 Params.xml，并记入本工具配置档案
// ============================================================
#include <cstdio>
#include <string>

namespace ModManager
{
    void DrawPage();       // 页面主体（在内容面板子窗口内绘制）
    void DrawModal();      // 模态层（删除确认 / MOD 设置）；在 Menu 主窗口最上层调用
    bool ModalActive();    // 是否有模态正在显示（供 Menu 决定是否吞输入）

    // ---- 配置档案集成（Menu::ProfileWrite / ProfileRead 调用）----
    void ProfileWrite(FILE* f);                                       // 写出 mod.* 键
    bool ProfileReadKey(const std::string& key, const std::string& val); // 读入一条；返回是否已处理

    // 切换模块整体开关时用：把当前所有 MOD 的启用状态写进 Params.xml
    void ApplyStartupStates();

    // 读取配置档案之后调用：保存 prefs、重扫 MOD、把开关写进 Params.xml
    void OnProfileApplied();
}