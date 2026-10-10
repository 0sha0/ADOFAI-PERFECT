// ============================================================
//  UnityModManager.cs — 我们自己实现的 UMM 兼容加载器（1/4）
//
//  游戏里的 MOD 在编译期都引用了名为 UnityModManager 的程序集：
//  UnityModManagerNet.UnityModManager 及其嵌套类型 ModEntry / ModInfo /
//  ModSettings / Logger / Param / UI。下面按原版 UMM 的语义把这些成员
//  补齐（成员清单来自对 MOD 元数据引用表的逆向），使任何 UMM 规范的
//  MOD 都能绑定到本加载器；而“加载/排序/驱动/界面”全部由我们实现。
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
        //  ModInfo —— Info.json 的映射
        // ------------------------------------------------------------
        public class ModInfo : IEquatable<ModInfo>
        {
            public string Id;
            public string DisplayName;
            public string Author;
            public string Version;
            public string ManagerVersion;
            public string GameVersion;
            public string[] Requirements;
            public string[] LoadAfter;
            public string AssemblyName;
            public string EntryMethod;
            public string HomePage;
            public string Repository;
            public string ContentType;
            [NonSerialized] public bool IsCheat = true;

            public static implicit operator bool(ModInfo exists) { return exists != null; }

            public bool Equals(ModInfo other) { return other != null && Id == other.Id; }
            public override bool Equals(object obj) { return Equals(obj as ModInfo); }
            public override int GetHashCode() { return Id != null ? Id.GetHashCode() : 0; }
        }

        public delegate void ToggleModsListen(ModEntry modEntry, bool result);

        // ------------------------------------------------------------
        //  Param —— Params.xml（启用状态）
        //  与原版 UMM 同名同格式：<加载器目录>\Params.xml，形状
        //  <Param><ModParams><Mod Id=".." Enabled="true" />。</Param>
        //  为了「生态直接迁移」：原版 UMM 目录（Managed\UnityModManager）
        //  里也有 Params.xml 时，读：自己没有就读它；写：两边都写，
        //  这样两边看到的启用状态永远一致。
        // ------------------------------------------------------------
        [XmlRoot("Param")]
        public sealed class Param
        {
            [Serializable]
            public class Mod
            {
                [XmlAttribute] public string Id;
                [XmlAttribute] public bool Enabled = true;
            }

            public float UIScale = 1f;

            public List<Mod> ModParams = new List<Mod>();

            private static readonly string filepath = Path.Combine(
                Path.GetDirectoryName(typeof(UnityModManager).Assembly.Location), "Params.xml");

            public static string FilePath { get { return filepath; } }

            // 原版 UMM 的 Params.xml（若它也装在这台机器上）
            public static string ForeignFilePath
            {
                get
                {
                    string dir = ForeignDir();
                    return string.IsNullOrEmpty(dir) ? null : Path.Combine(dir, "Params.xml");
                }
            }

            public void Save()
            {
                SaveTo(filepath);
                // 生态迁移：原版 UMM 在场就把同一份状态镜像过去（它的 GUI / 下次
                // 启动的 doorstop 加载读到的就是同一份启用状态）
                string foreign = ForeignFilePath;
                if (!string.IsNullOrEmpty(foreign) &&
                    !string.Equals(foreign, filepath, StringComparison.OrdinalIgnoreCase))
                    SaveTo(foreign);
            }

            private void SaveTo(string path)
            {
                if (string.IsNullOrEmpty(path)) return;
                try
                {
                    using (StreamWriter writer = new StreamWriter(path))
                        new XmlSerializer(typeof(Param)).Serialize(writer, this);
                }
                catch (Exception e)
                {
                    Logger.Error("Can't write file '" + path + "'.");
                    Logger.LogException(e);
                }
            }

            public static Param Load()
            {
                if (File.Exists(filepath))
                {
                    Param own = ReadFile(filepath);
                    if (own != null) return own;
                }
                // 自己还没有：接手原版 UMM 已经写好的那份（保留用户已有的启用状态）
                string foreign = ForeignFilePath;
                if (!string.IsNullOrEmpty(foreign) && File.Exists(foreign))
                {
                    Param p = ReadFile(foreign);
                    if (p != null)
                    {
                        Logger.Log("Params: adopting '" + foreign + "'.");
                        return p;
                    }
                }
                return new Param();
            }

            private static Param ReadFile(string path)
            {
                try
                {
                    using (FileStream stream = File.OpenRead(path))
                        return new XmlSerializer(typeof(Param)).Deserialize(stream) as Param;
                }
                catch (Exception e)
                {
                    Logger.Error("Can't read file '" + path + "'.");
                    Logger.LogException(e);
                    return null;
                }
            }

            internal void ReadModParams()
            {
                foreach (Mod mod in ModParams)
                {
                    ModEntry entry = FindMod(mod.Id);
                    if (entry != null) entry.Enabled = mod.Enabled;
                }
            }
        }

        // ------------------------------------------------------------
        //  目录：我们自己（Managed\AdofPerfectUmm）与原版 UMM（Managed\UnityModManager）
        // ------------------------------------------------------------
        internal static string OwnDir
        {
            get { return Path.GetDirectoryName(typeof(UnityModManager).Assembly.Location); }
        }

        internal static string ForeignDir()
        {
            try
            {
                // <自己目录>\..\UnityModManager —— 同一个 Managed 下的原版 UMM 目录
                string candidate = Path.GetFullPath(Path.Combine(OwnDir, "..", "UnityModManager"));
                if (File.Exists(Path.Combine(candidate, "UnityModManager.dll"))) return candidate;
            }
            catch { }
            return null;
        }

        // ------------------------------------------------------------
        //  GameInfo —— 加载器配置
        //  生态兼容：优先读原版 UMM 的 Config.xml（与它的 XmlSerializer 形状
        //  完全一致：Name 是根属性、其余是元素），自己的 AdofPerfectUmm.json
        //  只作为老部署的回退。
        // ------------------------------------------------------------
        public class GameInfo
        {
            [XmlAttribute] public string Name = "A Dance of Fire and Ice";
            public string Folder = "ADOFAI";
            public string ModsDirectory = "Mods";
            public string ModInfo = "Info.json";
            public string GameExe = "A Dance of Fire and Ice.exe";
            public string EntryPoint = "[UnityEngine.CoreModule.dll]UnityEngine.MonoBehaviour.cctor:Before";
            public string StartingPoint = "[Assembly-CSharp.dll]ADOStartup.Startup:Before";
            public string UIStartingPoint = "[Assembly-CSharp.dll]ADOStartup.Startup:After";
            public string MinimalManagerVersion = "0.22.14";
            public string ModsPath = string.Empty;
        }

        // ------------------------------------------------------------
        //  静态状态
        // ------------------------------------------------------------
        public static readonly List<ModEntry> modEntries = new List<ModEntry>();
        public static string modsPath { get; private set; }
        public static Version version { get; private set; }
        public static Version gameVersion { get; private set; }
        public static Version unityVersion { get; private set; }
        public static GameInfo Config { get; set; }
        public static Param Params { get; set; }

        internal static readonly Version VER_0 = new Version();
        internal static bool initialized;
        internal static bool started;
        internal static bool forbidDisableMods;
        // 原版 UMM 在场（doorstop 已把它加载进进程）：我们整体转入 passive 复用模式，
        // 不扫 MOD、不加载、不出 GUI，Bridge 的操作经 AdofPerfectUmm.Foreign 反射转发。
        internal static bool passive;

        // Bridge.State() 的行结构（我们自己的 modEntries 与 passive 模式下的
        // 原版 modEntries 都映射成这个，工具侧 JSON 形状保持一致）
        public class BridgeState
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

        public static event ToggleModsListen toggleModsListen;

        public static Version GetVersion() { return version; }
        internal static void SetVersion(Version v) { version = v; }
        internal static void SetModsPath(string path) { modsPath = path; RebuildModsPaths(); }
        internal static void SetGameVersion(Version v) { gameVersion = v; }

        public static Version ParseVersion(string str)
        {
            if (string.IsNullOrEmpty(str)) return new Version();
            string[] parts = str.Split('.');
            List<int> numbers = new List<int>();
            foreach (string part in parts)
            {
                Match match = Regex.Match(part, "\\d+");
                if (!match.Success) break;
                int n;
                if (!int.TryParse(match.Value, out n)) break;
                numbers.Add(n);
            }
            while (numbers.Count < 2) numbers.Add(0);
            if (numbers.Count > 4) numbers.RemoveRange(4, numbers.Count - 4);
            return new Version(numbers[0], numbers[1], numbers.Count > 2 ? numbers[2] : 0, numbers.Count > 3 ? numbers[3] : 0);
        }

        public static ModEntry FindMod(string id)
        {
            for (int i = 0; i < modEntries.Count; i++)
                if (modEntries[i].Info.Id == id) return modEntries[i];
            return null;
        }

        internal static bool TryGetEntryPoint(Assembly assembly, string entry, out Type type, out MethodInfo method)
        {
            type = null;
            method = null;
            if (string.IsNullOrEmpty(entry) || assembly == null) return false;

            string text = entry;
            int hash = text.IndexOf('#');
            if (hash != -1) text = text.Substring(0, hash);

            string className;
            string methodName;
            int sep = text.IndexOf("::", StringComparison.Ordinal);
            if (sep != -1)
            {
                className = text.Substring(0, sep);
                methodName = text.Substring(sep + 2);
            }
            else
            {
                int dot = text.LastIndexOf('.');
                if (dot == -1) return false;
                className = text.Substring(0, dot);
                methodName = text.Substring(dot + 1);
            }

            type = assembly.GetType(className) ?? assembly.GetType(className.Replace('.', '+'));
            if (type == null) return false;

            // 入口方法可能被重载：GetMethod 遇到重载会抛 AmbiguousMatchException，
            // 这里改成列出同名静态方法，优先挑 UMM 规范的两种签名：(ModEntry) 或 ()。
            const BindingFlags flags = BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic;
            MethodInfo[] all;
            try { all = type.GetMethods(flags); } catch { return false; }
            MethodInfo first = null, byMod = null, byNone = null;
            for (int i = 0; i < all.Length; i++)
            {
                MethodInfo m = all[i];
                if (!string.Equals(m.Name, methodName, StringComparison.Ordinal)) continue;
                if (first == null) first = m;
                ParameterInfo[] ps = m.GetParameters();
                if (ps.Length == 0) { if (byNone == null) byNone = m; }
                else if (ps.Length == 1 && ps[0].ParameterType == typeof(ModEntry)) { if (byMod == null) byMod = m; }
            }
            method = byMod ?? byNone ?? first;
            return method != null;
        }

        internal static void OnModToggled(ModEntry entry, bool value)
        {
            ToggleModsListen handler = toggleModsListen;
            if (handler != null) handler(entry, value);
        }
    }
}

namespace UnityModManagerNet
{
    // 原版 UMM 的「允许热重载」标记。少数 MOD 的元数据里带这个特性，
    // 缺失时反射读取自定义特性会抛异常，这里补一个空实现。
    [AttributeUsage(AttributeTargets.Class)]
    public class EnableReloadingAttribute : Attribute { }

    // 原版 UMM 的「重载时自动保存」标记（SaveOnReload）。
    [AttributeUsage(AttributeTargets.Class)]
    public class SaveOnReloadAttribute : Attribute { }

    // ============================================================
    //  原版 UMM 0.32 的「设置 GUI 注解框架」+ 基础类型。
    //  Overlayer 等 MOD 的设置类大量使用这些注解（官方 GUI 靠它们渲染
    //  设置页），个别 MOD 还会实现 IDrawable / ICopyable 或直接用
    //  Vector2i/Vector3i 做设置字段 —— 类型缺失就是 TypeLoadException。
    //  本加载器的设置页由工具侧（C++ / ImGui）直接编辑设置文件，
    //  所以这里只提供「惰性」定义：形状与原版一致，行为为空。
    // ============================================================
    public enum DrawType
    {
        Auto, Ignore, Field, Slider, Toggle, ToggleGroup, ToggleMulti,
        PopupToggleMulti, PopupList, KeyBinding, KeyBindingNoMod, CustomGUI,
    }

    [Flags]
    public enum DrawFieldMask
    {
        Any = 0, Public = 1, Serialized = 2, SkipNotSerialized = 4,
        OnlyDrawAttr = 8,
    }

    [Flags]
    public enum CopyFieldMask
    {
        Any = 0, Public = 1, Serialized = 2, SkipNotSerialized = 4,
        OnlyCopyAttr = 8, Matching = 16,
    }

    // MOD 可实现它让官方 GUI 在值变化时收到回调
    public interface IDrawable
    {
        void OnChange();
    }

    // MOD 可实现它支持官方 GUI 的「配置档案复制」
    public interface ICopyable
    {
    }

    [AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct)]
    public class DrawFieldsAttribute : Attribute
    {
        public DrawFieldMask Mask = DrawFieldMask.Any;
        public DrawFieldsAttribute() { }
        public DrawFieldsAttribute(DrawFieldMask mask) { Mask = mask; }
    }

    [AttributeUsage(AttributeTargets.Class | AttributeTargets.Struct)]
    public class CopyFieldsAttribute : Attribute
    {
        public CopyFieldMask Mask = CopyFieldMask.Any;
        public CopyFieldsAttribute() { }
        public CopyFieldsAttribute(CopyFieldMask mask) { Mask = mask; }
    }

    [AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
    public class DrawAttribute : Attribute
    {
        public DrawType Type = DrawType.Auto;
        public string Label;
        public int Width;
        public int Height;
        public double Min;
        public double Max;
        public int Precision = 2;
        public int MaxLength;
        public string VisibleOn;
        public string InvisibleOn;
        public bool Box;
        public bool Collapsible;
        public bool Vertical;
        public string Tooltip;
        public bool TextArea;
        public bool NoFlexibleSpace;
        public int Unique;
        public DrawAttribute() { }
        public DrawAttribute(DrawType type) { Type = type; }
    }

    [AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
    public class CopyAttribute : Attribute
    {
        public string Alias;
        public CopyAttribute() { }
        public CopyAttribute(string alias) { Alias = alias; }
    }

    [AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
    public class HorizontalAttribute : Attribute
    {
        public int Space = 10;
        public HorizontalAttribute() { }
        public HorizontalAttribute(int space) { Space = space; }
    }

    [AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
    public class DrawSpaceAttribute : Attribute
    {
        public int Height;
        public DrawSpaceAttribute() { }
        public DrawSpaceAttribute(int height) { Height = height; }
    }

    [AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
    public class DrawHeaderAttribute : Attribute
    {
        public string Text;
        public DrawHeaderAttribute() { }
        public DrawHeaderAttribute(string text) { Text = text; }
    }

    [AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
    public class DrawBeginHorizontalAttribute : Attribute { }

    [AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
    public class DrawEndHorizontalAttribute : Attribute { }

    [AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
    public class DrawBeginVerticalAttribute : Attribute { }

    [AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
    public class DrawEndVerticalAttribute : Attribute { }

    [AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
    public class DrawFlexibleSpaceAttribute : Attribute { }

    // 整型向量（原版 UMM 自带；个别 MOD 的设置/存档里直接用）
    public struct Vector2i
    {
        public int x, y;

        public Vector2i(int x, int y) { this.x = x; this.y = y; }

        public void Set(int nx, int ny) { x = nx; y = ny; }

        public static float Distance(Vector2i a, Vector2i b)
        {
            double dx = a.x - b.x, dy = a.y - b.y;
            return (float)Math.Sqrt(dx * dx + dy * dy);
        }

        public static Vector2i Min(Vector2i a, Vector2i b) { return new Vector2i(Math.Min(a.x, b.x), Math.Min(a.y, b.y)); }
        public static Vector2i Max(Vector2i a, Vector2i b) { return new Vector2i(Math.Max(a.x, b.x), Math.Max(a.y, b.y)); }
        public static Vector2i Scale(Vector2i a, Vector2i b) { return new Vector2i(a.x * b.x, a.y * b.y); }
        public void Scale(Vector2i s) { x *= s.x; y *= s.y; }
        public void Clamp(Vector2i min, Vector2i max)
        {
            x = Math.Max(min.x, Math.Min(max.x, x));
            y = Math.Max(min.y, Math.Min(max.y, y));
        }
    }

    public struct Vector3i
    {
        public int x, y, z;

        public Vector3i(int x, int y, int z) { this.x = x; this.y = y; this.z = z; }

        public void Set(int nx, int ny, int nz) { x = nx; y = ny; z = nz; }

        public static float Distance(Vector3i a, Vector3i b)
        {
            double dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return (float)Math.Sqrt(dx * dx + dy * dy + dz * dz);
        }

        public static Vector3i Min(Vector3i a, Vector3i b) { return new Vector3i(Math.Min(a.x, b.x), Math.Min(a.y, b.y), Math.Min(a.z, b.z)); }
        public static Vector3i Max(Vector3i a, Vector3i b) { return new Vector3i(Math.Max(a.x, b.x), Math.Max(a.y, b.y), Math.Max(a.z, b.z)); }
        public static Vector3i Scale(Vector3i a, Vector3i b) { return new Vector3i(a.x * b.x, a.y * b.y, a.z * b.z); }
        public void Scale(Vector3i s) { x *= s.x; y *= s.y; z *= s.z; }
        public void Clamp(Vector3i min, Vector3i max)
        {
            x = Math.Max(min.x, Math.Min(max.x, x));
            y = Math.Max(min.y, Math.Min(max.y, y));
            z = Math.Max(min.z, Math.Min(max.z, z));
        }
    }
}

