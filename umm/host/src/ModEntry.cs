// ============================================================
//  ModEntry.cs — 我们自己实现的 UMM 兼容加载器（2/4）
//    ModEntry / ModEntry.ModLogger / ModSettings / Logger
// ============================================================
using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Text.RegularExpressions;
using System.Xml.Serialization;
using UnityEngine;

namespace UnityModManagerNet
{
    public partial class UnityModManager
    {
        // ------------------------------------------------------------
        //  Logger —— 全局日志（写 Log.txt，同时进 Unity 控制台）
        // ------------------------------------------------------------
        public static class Logger
        {
            private const string Prefix = "[Manager] ";
            private const string PrefixError = "[Manager] [Error] ";
            private const string PrefixException = "[Manager] [Exception] ";
            private const int BufferCapacity = 400;

            public static readonly string filepath = Path.Combine(
                Path.GetDirectoryName(typeof(UnityModManager).Assembly.Location), "Log.txt");

            private static readonly List<string> buffer = new List<string>(BufferCapacity);
            private static readonly object gate = new object();

            // 只有确认 Unity 的脚本运行时可用之后才允许走 Debug.Log。
            // 门卫（doorstop）阶段调用 Debug.Log 会让 Mono 永久缓存一个失败的
            // 内部调用包装（DebugLogHandler.Internal_Log_Injected 是 [ThreadSafe]），
            // 之后整个进程的 Debug.Log 都会抛 MissingMethodException。
            public static bool UnityLogEnabled;

            // 同一个 MOD 每帧抛异常时不要无限刷日志（曾出现过 146MB 的 Log.txt）。
            private static string lastLine;
            private static int lastCount;
            private const int RepeatKeep = 500;
            private const long MaxFileBytes = 8L * 1024 * 1024;

            public static string[] History { get { lock (gate) return buffer.ToArray(); } }

            public static void Clear()
            {
                lock (gate)
                {
                    buffer.Clear();
                    try { File.WriteAllText(filepath, string.Empty); } catch { }
                }
            }

            public static void NativeLog(string str) { NativeLog(str, Prefix); }
            public static void NativeLog(string str, string prefix) { Write(prefix + str, true); }
            public static void Log(string str) { Log(str, Prefix); }
            public static void Log(string str, string prefix) { Write(prefix + str, false); }
            public static void Error(string str) { Error(str, PrefixError); }
            public static void Error(string str, string prefix) { Write(prefix + str, false); }
            public static void Warning(string str) { Write("[Manager] [Warning] " + str, false); }
            public static void LogException(Exception e) { LogException(null, e, PrefixException); }
            public static void LogException(string key, Exception e) { LogException(key, e, PrefixException); }

            public static void LogException(string key, Exception e, string prefix)
            {
                Write(prefix + (key ?? string.Empty) + (e != null ? e.ToString() : "null"), false);
            }

            private static void Write(string str, bool onlyNative)
            {
                string line = DateTime.Now.ToString("HH:mm:ss.fff") + " " + str;
                bool skipped = false;
                lock (gate)
                {
                    buffer.Add(line);
                    while (buffer.Count > BufferCapacity) buffer.RemoveAt(0);

                    if (line == lastLine)
                    {
                        lastCount++;
                        if (lastCount % RepeatKeep != 0) skipped = true;
                    }
                    else
                    {
                        lastLine = line;
                        lastCount = 1;
                    }

                    if (!skipped)
                    {
                        try
                        {
                            FileInfo fi = new FileInfo(filepath);
                            if (fi.Exists && fi.Length > MaxFileBytes)
                                File.WriteAllText(filepath, DateTime.Now.ToString("HH:mm:ss.fff") + " [Manager] (日志超过上限，已截断)" + Environment.NewLine);
                            File.AppendAllText(filepath, line + Environment.NewLine);
                        }
                        catch { }
                    }
                }
                if (!onlyNative && UnityLogEnabled)
                {
                    try { Debug.Log(str); } catch { }
                }
            }
        }

        // ------------------------------------------------------------
        //  ModSettings —— MOD 自己那份 Settings.xml 的读写
        // ------------------------------------------------------------
        public class ModSettings
        {
            public virtual void Save(ModEntry modEntry) { Save(this, modEntry); }
            // 与原版 UMM 一致：默认设置文件是「类名.xml」（GetType().Name + ".xml"）。
            // 兼容取舍：类名文件不存在、但同目录确实有 Settings.xml 时沿用 Settings.xml，
            // 这样既符合 UMM 语义，又不会丢掉已经写在 Settings.xml 里的老配置。
            public virtual string GetPath(ModEntry modEntry)
            {
                string named = Path.Combine(modEntry.Path, GetType().Name + ".xml");
                if (File.Exists(named)) return named;
                string legacy = Path.Combine(modEntry.Path, "Settings.xml");
                if (File.Exists(legacy)) return legacy;
                return named;
            }

            public static void Save<T>(T data, ModEntry modEntry) where T : ModSettings, new()
            {
                Save(data, modEntry, null);
            }

            public static void Save<T>(T data, ModEntry modEntry, XmlAttributeOverrides attributes) where T : ModSettings, new()
            {
                string path = data.GetPath(modEntry);
                try
                {
                    using (StreamWriter writer = new StreamWriter(path))
                        new XmlSerializer(data.GetType(), attributes).Serialize(writer, data);
                }
                catch (Exception e)
                {
                    modEntry.Logger.Error("Can't save " + path + ".");
                    modEntry.Logger.LogException(e);
                }
            }

            public static T Load<T>(ModEntry modEntry) where T : ModSettings, new()
            {
                return Load<T>(modEntry, null);
            }

            public static T Load<T>(ModEntry modEntry, XmlAttributeOverrides attributes) where T : ModSettings, new()
            {
                T data = new T();
                string path = data.GetPath(modEntry);
                if (File.Exists(path))
                {
                    try
                    {
                        T loaded;
                        using (FileStream stream = File.OpenRead(path))
                            loaded = (T)new XmlSerializer(typeof(T), attributes).Deserialize(stream);
                        if (loaded != null)
                        {
                            // 记下这个活对象：工具改完设置文件后，靠它把新值「热应用」进去
                            SettingsSync.Register(modEntry, path, loaded);
                            return loaded;
                        }
                    }
                    catch (Exception e)
                    {
                        modEntry.Logger.Error("Can't read " + path + ".");
                        modEntry.Logger.LogException(e);
                    }
                }
                SettingsSync.Register(modEntry, path, data);
                return data;
            }
        }

        // ------------------------------------------------------------
        //  ModEntry —— 单个 MOD 的运行时对象
        //  注意：以下成员名/可见性会被 MOD 通过反射/元数据引用，
        //  改动前请先确认没有 MOD 依赖（例如 Overlayer 写 mAssembly）。
        // ------------------------------------------------------------
        public class ModEntry
        {
            public class ModLogger
            {
                protected readonly string Prefix;
                protected readonly string PrefixError;
                protected readonly string PrefixCritical;
                protected readonly string PrefixWarning;
                protected readonly string PrefixException;

                public ModLogger(string Id)
                {
                    Prefix = "[" + Id + "] ";
                    PrefixError = "[" + Id + "] [Error] ";
                    PrefixCritical = "[" + Id + "] [Critical] ";
                    PrefixWarning = "[" + Id + "] [Warning] ";
                    PrefixException = "[" + Id + "] [Exception] ";
                }

                public void Log(string str) { UnityModManager.Logger.Log(str, Prefix); }
                public void Error(string str) { UnityModManager.Logger.Log(str, PrefixError); }
                public void Critical(string str) { UnityModManager.Logger.Log(str, PrefixCritical); }
                public void Warning(string str) { UnityModManager.Logger.Log(str, PrefixWarning); }
                public void NativeLog(string str) { UnityModManager.Logger.NativeLog(str, Prefix); }
                public void LogException(string key, Exception e) { UnityModManager.Logger.LogException(key, e, PrefixException); }
                public void LogException(Exception e) { UnityModManager.Logger.LogException(null, e, PrefixException); }
            }

            public readonly ModInfo Info;
            public readonly string Path;
            public readonly Version Version;
            public readonly Version ManagerVersion;
            public readonly Version GameVersion;
            public Version NewestVersion;
            public readonly Dictionary<string, Version> Requirements = new Dictionary<string, Version>();
            public readonly List<string> LoadAfter = new List<string>();
            public string CustomRequirements = string.Empty;
            public readonly ModLogger Logger;

            public bool HasUpdate;

            public Func<ModEntry, bool> OnUnload;
            public Func<ModEntry, bool, bool> OnToggle;
            public Action<ModEntry> OnGUI;
            public Action<ModEntry> OnFixedGUI;
            public Action<ModEntry> OnShowGUI;
            public Action<ModEntry> OnHideGUI;
            public Action<ModEntry> OnSaveGUI;
            public Action<ModEntry, float> OnUpdate;
            public Action<ModEntry, float> OnLateUpdate;
            public Action<ModEntry, float> OnFixedUpdate;
            public Action<ModEntry> OnSessionStart;
            public Action<ModEntry> OnSessionStop;

            public bool Enabled = true;

            private Assembly mAssembly;
            private bool mStarted;
            private bool mErrorOnLoading;
            private bool mActive;

            public Assembly Assembly { get { return mAssembly; } }

            public bool HasAssembly
            {
                get
                {
                    if (string.IsNullOrEmpty(Info.AssemblyName)) return !string.IsNullOrEmpty(Info.EntryMethod);
                    return true;
                }
            }

            public bool CanReload { get; private set; }
            public bool Started { get { return mStarted; } }
            public bool ErrorOnLoading { get { return mErrorOnLoading; } }
            public bool Toggleable { get { return OnToggle != null || !HasAssembly; } }

            public bool Loaded
            {
                get
                {
                    if (mAssembly == null) return !HasAssembly && mStarted;
                    return true;
                }
            }

            public bool Active
            {
                get { return mActive; }
                set
                {
                    if (value && !Loaded)
                    {
                        Load();
                        return;
                    }
                    if (!mStarted || mErrorOnLoading) return;
                    try
                    {
                        if (value)
                        {
                            if (mActive) return;
                            if (OnToggle == null || OnToggle(this, true))
                            {
                                mActive = true;
                                Logger.Log("Active.");
                                OnModToggled(this, true);
                            }
                            else
                            {
                                Logger.Log("Unsuccessfully.");
                                Logger.NativeLog("OnToggle(true) failed.");
                            }
                        }
                        else
                        {
                            if (!mActive) return;
                            if ((OnToggle != null && OnToggle(this, false)) || !HasAssembly)
                            {
                                mActive = false;
                                Logger.Log("Inactive.");
                                OnModToggled(this, false);
                            }
                            else if (OnToggle != null)
                            {
                                Logger.NativeLog("OnToggle(false) failed.");
                            }
                        }
                    }
                    catch (Exception e)
                    {
                        Logger.LogException("OnToggle", e);
                    }
                }
            }

            public ModEntry(ModInfo info, string path)
            {
                Info = info;
                Path = path;
                Logger = new ModLogger(Info.Id);
                Version = ParseVersion(info.Version);
                ManagerVersion = !string.IsNullOrEmpty(info.ManagerVersion)
                    ? ParseVersion(info.ManagerVersion)
                    : (!string.IsNullOrEmpty(Config.MinimalManagerVersion) ? ParseVersion(Config.MinimalManagerVersion) : new Version());
                GameVersion = !string.IsNullOrEmpty(info.GameVersion) ? ParseVersion(info.GameVersion) : new Version();

                if (info.Requirements != null && info.Requirements.Length > 0)
                {
                    Regex regex = new Regex("(.*)-(\\d+\\.\\d+\\.\\d+).*");
                    foreach (string requirement in info.Requirements)
                    {
                        Match match = regex.Match(requirement);
                        if (match.Success) Requirements[match.Groups[1].Value] = ParseVersion(match.Groups[2].Value);
                        else if (!Requirements.ContainsKey(requirement)) Requirements[requirement] = null;
                    }
                }
                if (info.LoadAfter != null && info.LoadAfter.Length > 0) LoadAfter.AddRange(info.LoadAfter);
            }

            public bool Load()
            {
                if (Loaded) return !mErrorOnLoading;
                mErrorOnLoading = false;

                Logger.Log("Version '" + Info.Version + "'. Loading.");

                if (string.IsNullOrEmpty(Info.AssemblyName) && !string.IsNullOrEmpty(Info.EntryMethod))
                {
                    mErrorOnLoading = true;
                    Logger.Error("AssemblyName is null.");
                }
                if (!string.IsNullOrEmpty(Info.AssemblyName) && string.IsNullOrEmpty(Info.EntryMethod))
                {
                    mErrorOnLoading = true;
                    Logger.Error("EntryMethod is null.");
                }
                if (!string.IsNullOrEmpty(Info.ManagerVersion) && ManagerVersion > GetVersion())
                {
                    mErrorOnLoading = true;
                    Logger.Error("Mod Manager must be version '" + Info.ManagerVersion + "' or higher.");
                }

                if (Requirements.Count > 0)
                {
                    foreach (KeyValuePair<string, Version> requirement in Requirements)
                    {
                        ModEntry mod = FindMod(requirement.Key);
                        if (mod == null)
                        {
                            mErrorOnLoading = true;
                            Logger.Error("Required mod '" + requirement.Key + "' missing.");
                        }
                        else if (!mod.Active && mod.Enabled)
                        {
                            mod.Active = true;
                            if (!mod.Active) Logger.Log("Required mod '" + requirement.Key + "' enabled, but inactive.");
                        }
                    }
                }

                if (mErrorOnLoading) return false;

                if (!HasAssembly)
                {
                    mStarted = true;
                    Active = true;
                    return true;
                }

                string file = System.IO.Path.Combine(Path, Info.AssemblyName);
                if (!File.Exists(file))
                {
                    mErrorOnLoading = true;
                    Logger.Error("Cannot find file '" + file + "'.");
                    return false;
                }

                try
                {
                    mAssembly = Assembly.LoadFrom(file);
                }
                catch (Exception e)
                {
                    mErrorOnLoading = true;
                    Logger.Error("Error loading file '" + file + "'.");
                    Logger.LogException(e);
                    return false;
                }

                Type type;
                MethodInfo method;
                if (!TryGetEntryPoint(mAssembly, Info.EntryMethod, out type, out method))
                {
                    mErrorOnLoading = true;
                    Logger.Error("No entry point found '" + Info.EntryMethod + "'.");
                    return false;
                }

                try
                {
                    // 按真实签名传参：UMM 规范入口是 (ModEntry) 或 ()，两种都要支持
                    // （原版 UMM 也是这么做的，只认 (ModEntry) 会把无参入口的 MOD 判失败）。
                    ParameterInfo[] parms = method.GetParameters();
                    object[] callArgs = null;
                    if (parms.Length == 1)
                    {
                        Type pt = parms[0].ParameterType;
                        if (pt.IsInstanceOfType(this)) callArgs = new object[] { this };
                        else if (pt == typeof(string)) callArgs = new object[] { Info.Id };
                    }
                    object result = method.Invoke(null, callArgs);
                    if (result is bool && !(bool)result)
                    {
                        mErrorOnLoading = true;
                        Logger.Error("Entry point failed.");
                        return false;
                    }
                }
                catch (Exception e)
                {
                    mErrorOnLoading = true;
                    Logger.Error("Error invoking entry point '" + Info.EntryMethod + "'.");
                    Logger.LogException(e);
                    return false;
                }

                mStarted = true;
                Active = true;
                return true;
            }

            public void Unload()
            {
                if (!mStarted) return;
                try
                {
                    if (OnUnload != null && !OnUnload(this)) return;
                }
                catch (Exception e) { Logger.LogException("OnUnload", e); }
                mActive = false;
                mStarted = false;
                Logger.Log("Unloaded.");
            }

            public bool HasContentType(string str)
            {
                if (!string.IsNullOrEmpty(Info.ContentType))
                    return new Regex("\\b" + str + "\\b", RegexOptions.IgnoreCase).IsMatch(Info.ContentType);
                return false;
            }

            // 原版 UMM 的 ModEntry 可以直接当 bool 用（MOD 里常见 if (modEntry) / !modEntry），
            // 少了这个隐式转换，这类 MOD 一编译不通过、二在运行时反射取不到该成员。
            public static implicit operator bool(ModEntry exists) { return exists != null; }

            public override string ToString() { return Info.Id + " " + Info.Version; }
        }
    }
}

