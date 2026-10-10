// ============================================================
//  Bridge.cs — Sidecar 主入口（协议与 umm/host 的 Bridge 完全一致）
//
//  AdofPerfectUmm.Bridge / Protocol = "adofperfect-umm/1"。
//  Sidecar 不加载任何 MOD：StartWith 恒返回 "passive"，
//  其余命令经 Foreign 反射驱动官方 UnityModManager。
//
//  C++ 侧的 ResolveBridge 优先打开本程序集（AdofPerfectUmm.dll），
//  找不到才回退旧版自研加载器（legacy UnityModManager.dll）。
// ============================================================
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
using TinyJson;

namespace AdofPerfectUmm
{
    public static class Bridge
    {
        public const string Protocol = "adofperfect-umm/1";

        public static string Ping()
        {
            return Protocol + " " + ForeignVersion();
        }

        private static string ForeignVersion()
        {
            try
            {
                Assembly a = Foreign.Detect();
                return a != null ? a.GetName().Version.ToString() : "?";
            }
            catch { return "?"; }
        }

        public static void EnsureStarted()
        {
            // Sidecar 没有自己的启动流程；官方内核由 doorstop 自行启动。
            // 这里只做一次探测日志，方便排查「桥上了没有」。
            if (!s_probed)
            {
                s_probed = true;
                SideLogger.Log(Foreign.Active
                    ? "Bridged to the native UMM kernel (version " + ForeignVersion() + ")."
                    : "Native UMM kernel not loaded yet; will keep probing.");
            }
        }
        private static bool s_probed;

        private class ModState
        {
            public string id;
            public string name;
            public string author;
            public string version;
            public bool enabled;
            public bool active;
            public bool loaded;
            public bool error;
            public bool gui;
            public bool open;
            public string assembly;
        }

        public static string State()
        {
            try
            {
                List<ModState> list = new List<ModState>();
                IList entries = Foreign.Entries();
                if (entries != null)
                {
                    foreach (object entry in entries)
                    {
                        if (entry == null) continue;
                        ModState state = new ModState();
                        state.id = Foreign.IdOf(entry) ?? "";
                        state.name = Foreign.NameOf(entry) ?? state.id;
                        state.author = Foreign.AuthorOf(entry);
                        state.version = Foreign.VersionOf(entry);
                        state.enabled = Foreign.EnabledOf(entry);
                        state.active = Foreign.ActiveOf(entry);
                        state.loaded = Foreign.LoadedOf(entry);
                        state.error = Foreign.ErrorOf(entry);
                        state.gui = Foreign.HasGui(entry);
                        state.open = Foreign.ShowSettingsOf(entry);
                        state.assembly = Foreign.AssemblyNameOf(entry);
                        list.Add(state);
                    }
                }
                return list.ToJson();
            }
            catch (Exception e)
            {
                SideLogger.LogException("State", e);
                return "[]";
            }
        }

        public static string ModsPath()
        {
            string p = Foreign.ModsPath();
            return p ?? string.Empty;
        }

        // 工具侧主入口：Sidecar 永远不加载 MOD（官方内核已由 doorstop 完成）
        public static string StartWith(string modsDir)
        {
            try
            {
                EnsureStarted();
                return Foreign.Active ? "passive" : "err:kernel-not-loaded";
            }
            catch (Exception e)
            {
                SideLogger.LogException("StartWith", e);
                return "err:" + e.Message;
            }
        }

        public static string Log(int maxLines)
        {
            try { return Foreign.LogTail(maxLines); }
            catch { return string.Empty; }
        }

        public static string Invoke(string command, string argument)
        {
            try
            {
                if (string.IsNullOrEmpty(command)) return "err:no-command";
                switch (command)
                {
                    case "start": return StartWith(argument);
                    case "state": return State();
                    case "log": return Log(80);
                    case "modsPath": return ModsPath();
                    case "reload": return "passive";          // 没有自己的加载流程可重载
                    case "enable": return Foreign.SetEnabled(argument, true) ? "ok" : "err:not-found";
                    case "disable": return Foreign.SetEnabled(argument, false) ? "ok" : "err:not-found";
                    case "open": return Foreign.SetShowSettings(argument, true) ? "ok" : "err:not-found";
                    case "close": return Foreign.SetShowSettings(argument, false) ? "ok" : "err:not-found";
                    case "closeAll":
                    {
                        IList all = Foreign.Entries();
                        if (all != null)
                            foreach (object e in all)
                                if (e != null) Foreign.SetShowSettings(Foreign.IdOf(e), false);
                        return "ok";
                    }
                    case "theme": return "ok";                // 官方 GUI 用它自己的配色，跳过
                    case "hasGui":
                    {
                        object e = Foreign.Entry(argument);
                        return e != null && Foreign.HasGui(e) ? "1" : "0";
                    }
                    case "apply":
                    {
                        // 参数：<ModId> 或 <ModId>|1（1 = 关→开一次，让只在
                        // OnToggle 里应用设置的 MOD 也生效）
                        string mid = argument ?? string.Empty;
                        bool toggle = false;
                        int bar = mid.IndexOf('|');
                        if (bar >= 0)
                        {
                            toggle = mid.Substring(bar + 1).Trim() == "1";
                            mid = mid.Substring(0, bar);
                        }
                        return Apply.Run(mid, toggle);
                    }
                    default: return "err:unknown-command";
                }
            }
            catch (Exception e)
            {
                SideLogger.LogException("Invoke(" + command + ")", e);
                return "err:" + e.Message;
            }
        }
    }
}

// doorstop 占位：Sidecar 不该被 doorstop 直接指到；万一被指到也安静存在，
// 不碰 Unity（与 Loader.Boot 的教训一致：doorstop 阶段绝不能调 Unity API）。
namespace Doorstop
{
    public class Entrypoint
    {
        public static void Start()
        {
            try { AdofPerfectUmm.Bridge.EnsureStarted(); } catch { }
        }
    }
}
