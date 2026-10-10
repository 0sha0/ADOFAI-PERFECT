// ============================================================
//  SettingsSync.cs — 让工具侧写进设置文件的值对正在运行的 MOD 立刻生效
//
//  MOD 在加载时把 <MOD>/Settings.xml(或 .json) 反序列化成内存对象，
//  之后基本不再回读文件。工具直接改文件，MOD 自然「参数不生效」，
//  「语言设成中文却还是显示英文」也是同一个根因。本类做三件事：
//    1) 记住 MOD 通过 ModSettings.Load<T>() 读进来的对象引用；
//    2) 反射找 MOD 程序集里的静态设置对象（Settings / Config / …）；
//    3) 用文件里的值就地覆盖这些对象；必要时再调用 MOD 自己的
//       OnToggle（关掉→打开），让它重新读取并应用设置。
// ============================================================
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
using System.Xml.Serialization;

namespace UnityModManagerNet
{
    public partial class UnityModManager
    {
        internal static class SettingsSync
        {
            private class Reg
            {
                public ModEntry mod;
                public object instance;
                public string path;
            }

            private class Item
            {
                public string key;
                public string val;
                public bool isNull;
            }

            private static readonly List<Reg> regs = new List<Reg>();
            private static readonly object gate = new object();

            // ---------- 登记：MOD 通过 ModSettings.Load<T>() 读到的实例 ----------
            public static void Register(ModEntry mod, string path, object instance)
            {
                if (mod == null || instance == null) return;
                try
                {
                    lock (gate)
                    {
                        for (int i = 0; i < regs.Count; i++)
                        {
                            if (ReferenceEquals(regs[i].instance, instance))
                            {
                                regs[i].mod = mod;
                                if (!string.IsNullOrEmpty(path)) regs[i].path = path;
                                return;
                            }
                        }
                        Reg r = new Reg();
                        r.mod = mod;
                        r.instance = instance;
                        r.path = path;
                        regs.Add(r);
                    }
                }
                catch { }
            }
            // ---------- 对外的「应用设置」入口 ----------
            // id    : MOD 的 Id
            // toggle: true 时，改完内存后再把 MOD 关掉/打开一次，
            //         让只在 OnToggle 里应用设置的 MOD（例如 Overlayer）也生效
            public static string Apply(string id, bool toggle)
            {
                ModEntry mod = FindMod(id);
                if (mod == null)
                {
                    // MOD 还没加载完（例如刚开机就读了配置档案）：先记下来，
                    // 等加载器把 MOD 全部加载完之后再补一次。
                    Defer(id, toggle);
                    return "deferred";
                }

                List<string> files = ModSettingFiles(mod);
                int applied = 0;
                bool reloaded = false;

                // 1) 我们自己登记过的实例（最准，路径已知）
                List<Reg> mine = new List<Reg>();
                lock (gate)
                {
                    for (int i = 0; i < regs.Count; i++)
                        if (regs[i].mod == mod || (regs[i].mod != null && regs[i].mod.Info != null && regs[i].mod.Info.Id == id))
                            mine.Add(regs[i]);
                }
                for (int i = 0; i < mine.Count; i++)
                {
                    try
                    {
                        Reg r = mine[i];
                        string p = !string.IsNullOrEmpty(r.path) ? r.path : DefaultPath(mod);
                        if (r.instance is ModSettings && (!File.Exists(p) || ApplyFull(r.instance, p) == 0))
                            applied += ApplyScalars(r.instance, p);
                        else
                            applied += ApplyScalars(r.instance, p);
                    }
                    catch (Exception e) { UnityModManager.Logger.LogException("apply/reg", e); }
                }

                // 2) 反射扫描 MOD 程序集里的静态设置对象
                try { applied += ScanStatics(mod, files); }
                catch (Exception e) { UnityModManager.Logger.LogException("apply/scan", e); }

                // 3) 关掉再打开：让 MOD 自己重新读取并应用（语言/贴图之类）
                if (toggle && mod.Active)
                {
                    Dictionary<string, string> snapshot = Snapshot(files);
                    bool wasOpen = UI.IsOpen(mod);
                    try
                    {
                        mod.Active = false;
                        RestoreIfExists(snapshot);
                        mod.Active = true;
                        reloaded = true;
                    }
                    catch (Exception e) { UnityModManager.Logger.LogException("apply/toggle", e); }
                    if (wasOpen)
                    {
                        try { UI.Open(mod); } catch { }
                    }
                }

                mod.Logger.Log("Settings applied (" + applied + " field(s))" + (reloaded ? " + reload." : "."));
                if (applied > 0) return "ok:" + applied;
                // 没找到实时字段、但整只 MOD 已经重载过一次：MOD 会在 OnToggle 里
                // 自己重读设置文件（Overlayer 就是这样），对用户同样算应用成功，
                // 不能在界面上报「应用失败」。
                if (reloaded) return "ok:reload";
                return "err:no-live-settings";
            }

            // ---------- 延迟应用（MOD 尚未加载时）----------
            private class Pending
            {
                public string id;
                public bool toggle;
            }

            private static readonly List<Pending> pending = new List<Pending>();

            private static void Defer(string id, bool toggle)
            {
                try
                {
                    lock (gate)
                    {
                        for (int i = 0; i < pending.Count; i++)
                            if (pending[i].id == id) { pending[i].toggle = pending[i].toggle || toggle; return; }
                        Pending p = new Pending();
                        p.id = id; p.toggle = toggle;
                        pending.Add(p);
                    }
                }
                catch { }
            }

            // MOD 全部加载完之后由 Loader.Start() 调用
            public static void RunPending()
            {
                List<Pending> list;
                lock (gate)
                {
                    if (pending.Count == 0) return;
                    list = new List<Pending>(pending);
                    pending.Clear();
                }
                for (int i = 0; i < list.Count; i++)
                {
                    try { Apply(list[i].id, list[i].toggle); }
                    catch (Exception e) { UnityModManager.Logger.LogException("apply/pending", e); }
                }
            }

            // ---------- MOD 目录下看起来像设置文件的 *.xml / *.json ----------
            private static bool IsSetName(string name)
            {
                string low = name.ToLowerInvariant();
                if (low == "settings.xml" || low == "settings.json") return true;
                if (low == "config.xml" || low == "config.json") return true;
                if (low == "options.xml" || low == "options.json") return true;
                if (low == "preferences.xml" || low == "preferences.json") return true;
                if (low.Contains(".settings.")) return true;
                int dot = low.LastIndexOf('.');
                string stem = dot > 0 ? low.Substring(0, dot) : low;
                if (stem.EndsWith("settings") || stem.EndsWith("setting")) return true;
                if (stem.EndsWith("config") || stem.EndsWith("configuration")) return true;
                return false;
            }

            private static List<string> ModSettingFiles(ModEntry mod)
            {
                List<string> list = new List<string>();
                try
                {
                    string dir = mod.Path;
                    if (string.IsNullOrEmpty(dir) || !Directory.Exists(dir)) return list;
                    string[] all = Directory.GetFiles(dir, "*.xml");
                    string[] all2 = Directory.GetFiles(dir, "*.json");
                    for (int i = 0; i < all.Length; i++)
                        if (IsSetName(Path.GetFileName(all[i]))) list.Add(all[i]);
                    for (int i = 0; i < all2.Length; i++)
                        if (IsSetName(Path.GetFileName(all2[i]))) list.Add(all2[i]);
                }
                catch { }
                return list;
            }

            private static string DefaultPath(ModEntry mod)
            {
                string p = Path.Combine(mod.Path, "Settings.xml");
                if (File.Exists(p)) return p;
                p = Path.Combine(mod.Path, "Settings.json");
                if (File.Exists(p)) return p;
                return Path.Combine(mod.Path, "Settings.xml");
            }

            // ---------- 切开关前先留一份设置文件快照 ----------
            // 有些 MOD 在 OnToggle(false) 里会把自己内存里的旧设置写回文件，
            // 那样会把我们刚写进去的新值冲掉；这里在关掉之后再还原。
            private static Dictionary<string, string> Snapshot(List<string> files)
            {
                Dictionary<string, string> map = new Dictionary<string, string>();
                for (int i = 0; i < files.Count; i++)
                {
                    try
                    {
                        if (!File.Exists(files[i])) continue;
                        byte[] b = File.ReadAllBytes(files[i]);
                        map[files[i]] = Convert.ToBase64String(b);
                    }
                    catch { }
                }
                return map;
            }

            private static void RestoreIfExists(Dictionary<string, string> map)
            {
                foreach (KeyValuePair<string, string> kv in map)
                {
                    try
                    {
                        if (!File.Exists(kv.Key)) continue;    // 被 MOD 删掉的文件不要复活
                        File.WriteAllBytes(kv.Key, Convert.FromBase64String(kv.Value));
                    }
                    catch { }
                }
            }

            // ---------- 反射扫描 MOD 程序集里的静态设置对象 ----------
            private static int ScanStatics(ModEntry mod, List<string> files)
            {
                int n = 0;
                List<object> done = new List<object>();
                Assembly[] asms;
                try { asms = AppDomain.CurrentDomain.GetAssemblies(); }
                catch { return 0; }
                for (int a = 0; a < asms.Length; a++)
                {
                    Assembly asm = asms[a];
                    if (!IsModAssembly(asm, mod)) continue;
                    Type[] types;
                    try { types = asm.GetTypes(); }
                    catch (ReflectionTypeLoadException e) { types = e.Types; }
                    catch { continue; }
                    if (types == null) continue;
                    for (int i = 0; i < types.Length; i++)
                    {
                        Type t = types[i];
                        if (t == null) continue;
                        n += ScanType(mod, t, files, done);
                    }
                }
                return n;
            }

            private static bool IsModAssembly(Assembly asm, ModEntry mod)
            {
                if (asm == null) return false;
                string name = null;
                try { name = asm.GetName().Name; } catch { }
                string loc = null;
                try { loc = asm.Location; } catch { }
                string dir = mod.Path;
                if (!string.IsNullOrEmpty(loc) && !string.IsNullOrEmpty(dir))
                {
                    try
                    {
                        if (Path.GetFullPath(loc).StartsWith(Path.GetFullPath(dir), StringComparison.OrdinalIgnoreCase))
                            return true;
                    }
                    catch { }
                }
                // 兜底 1：程序集名 == Info.json 里的 AssemblyName
                try
                {
                    string want = mod.Info.AssemblyName;
                    if (!string.IsNullOrEmpty(want) && want.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
                        want = want.Substring(0, want.Length - 4);
                    if (!string.IsNullOrEmpty(want) && string.Equals(name, want, StringComparison.OrdinalIgnoreCase))
                        return true;
                }
                catch { }
                // 兜底 2：内存加载的程序集（Assembly.Load(byte[])）没有 Location，
                // 例如 Overlayer 的启动器就是这样把 Overlayer.dll 读进来的；
                // 用「MOD 目录下存在同名 dll」判定归属，否则永远找不到它的设置对象。
                if (!string.IsNullOrEmpty(name) && !string.IsNullOrEmpty(dir))
                {
                    try
                    {
                        if (File.Exists(Path.Combine(dir, name + ".dll"))) return true;
                    }
                    catch { }
                }
                return false;
            }

            private static int ScanType(ModEntry mod, Type t, List<string> files, List<object> done)
            {
                int n = 0;
                List<object> cand = new List<object>();
                List<Type> candType = new List<Type>();

                FieldInfo[] fs = null;
                PropertyInfo[] ps = null;
                try { fs = t.GetFields(BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic); } catch { }
                try { ps = t.GetProperties(BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic); } catch { }

                if (fs != null)
                {
                    for (int i = 0; i < fs.Length; i++)
                    {
                        Type vt = fs[i].FieldType;
                        if (!LooksLikeSettings(vt)) continue;
                        object v = null;
                        try { v = fs[i].GetValue(null); } catch { continue; }
                        if (v == null) continue;
                        cand.Add(v); candType.Add(vt);
                    }
                }
                if (ps != null)
                {
                    for (int i = 0; i < ps.Length; i++)
                    {
                        if (ps[i].GetIndexParameters().Length != 0) continue;
                        Type vt = ps[i].PropertyType;
                        if (!LooksLikeSettings(vt)) continue;
                        object v = null;
                        try { v = ps[i].GetValue(null, null); } catch { continue; }
                        if (v == null) continue;
                        cand.Add(v); candType.Add(vt);
                    }
                }

                for (int i = 0; i < cand.Count; i++)
                {
                    object obj = cand[i];
                    if (obj is UnityEngine.Object) continue;
                    bool dup = false;
                    for (int k = 0; k < done.Count; k++) if (ReferenceEquals(done[k], obj)) { dup = true; break; }
                    if (dup) continue;
                    done.Add(obj);

                    string path = null;
                    ModSettings ms = obj as ModSettings;
                    if (ms != null)
                    {
                        try { path = ms.GetPath(mod); } catch { }
                    }
                    if (string.IsNullOrEmpty(path) || !File.Exists(path))
                        path = FindBestFile(candType[i], files);
                    if (string.IsNullOrEmpty(path)) continue;

                    int c = ApplyFull(obj, path);
                    if (c == 0) c = ApplyScalars(obj, path);
                    n += c;
                }
                return n;
            }

            private static bool LooksLikeSettings(Type t)
            {
                if (t == null || t.IsPrimitive || t.IsEnum) return false;
                if (t == typeof(string) || t.IsArray || t.IsGenericType) return false;
                string n = t.Name.ToLowerInvariant();
                return n.Contains("setting") || n.Contains("config");
            }

            // ---------- 在若干候选设置文件里挑「键名最匹配」的那个 ----------
            private static string FindBestFile(Type t, List<string> files)
            {
                string best = null;
                int bestScore = 0;
                for (int i = 0; i < files.Count; i++)
                {
                    List<Item> items = ReadItems(files[i]);
                    int s = 0;
                    for (int k = 0; k < items.Count; k++)
                        if (HasMember(t, items[k].key)) s++;
                    if (s > bestScore) { bestScore = s; best = files[i]; }
                }
                return bestScore >= 2 ? best : null;
            }

            private static bool HasMember(Type t, string key)
            {
                try
                {
                    FieldInfo[] fs = t.GetFields(BindingFlags.Instance | BindingFlags.Public);
                    for (int i = 0; i < fs.Length; i++)
                        if (string.Equals(fs[i].Name, key, StringComparison.OrdinalIgnoreCase)) return true;
                    PropertyInfo[] ps = t.GetProperties(BindingFlags.Instance | BindingFlags.Public);
                    for (int i = 0; i < ps.Length; i++)
                        if (string.Equals(ps[i].Name, key, StringComparison.OrdinalIgnoreCase)) return true;
                }
                catch { }
                return false;
            }

            // ---------- 反序列化整对象（仅 ModSettings，用 MOD 自己的序列化器）----------
            private static int ApplyFull(object target, string path)
            {
                if (!(target is ModSettings)) return 0;
                try
                {
                    if (!File.Exists(path) || !path.ToLowerInvariant().EndsWith(".xml")) return 0;
                    XmlSerializer xs = new XmlSerializer(target.GetType());
                    object fresh;
                    using (FileStream fs = File.OpenRead(path)) fresh = xs.Deserialize(fs);
                    if (fresh == null) return 0;
                    int n = 0;
                    FieldInfo[] flds = target.GetType().GetFields(BindingFlags.Instance | BindingFlags.Public);
                    for (int i = 0; i < flds.Length; i++)
                    {
                        try { flds[i].SetValue(target, flds[i].GetValue(fresh)); n++; }
                        catch { }
                    }
                    return n;
                }
                catch { return 0; }
            }

            // ---------- 只把文件里的标量键覆盖到对象上 ----------
            private static int ApplyScalars(object target, string path)
            {
                if (target == null || string.IsNullOrEmpty(path) || !File.Exists(path)) return 0;
                List<Item> items = ReadItems(path);
                if (items.Count == 0) return 0;
                Type t = target.GetType();

                // 先校验匹配度：至少 2 个键名对得上，才动手（避免误伤）
                bool soft = target is ModSettings;
                int hits = 0;
                for (int i = 0; i < items.Count; i++) if (HasMember(t, items[i].key)) hits++;
                if (!soft && hits < 2) return 0;

                int n = 0;
                for (int i = 0; i < items.Count; i++)
                {
                    try { if (SetMember(target, t, items[i])) n++; }
                    catch { }
                }
                return n;
            }

            private static bool SetMember(object target, Type t, Item it)
            {
                if (it == null || it.isNull) return false;
                FieldInfo fi = null;
                PropertyInfo pi = null;
                FieldInfo[] fs = t.GetFields(BindingFlags.Instance | BindingFlags.Public);
                for (int i = 0; i < fs.Length; i++)
                    if (string.Equals(fs[i].Name, it.key, StringComparison.OrdinalIgnoreCase)) { fi = fs[i]; break; }
                if (fi == null)
                {
                    PropertyInfo[] ps = t.GetProperties(BindingFlags.Instance | BindingFlags.Public);
                    for (int i = 0; i < ps.Length; i++)
                        if (ps[i].CanWrite && ps[i].GetIndexParameters().Length == 0 &&
                            string.Equals(ps[i].Name, it.key, StringComparison.OrdinalIgnoreCase)) { pi = ps[i]; break; }
                }
                if (fi == null && pi == null) return false;
                Type mt = fi != null ? fi.FieldType : pi.PropertyType;
                object v;
                if (!ToValue(it, mt, out v)) return false;
                if (fi != null) fi.SetValue(target, v);
                else pi.SetValue(target, v, null);
                return true;
            }

            private static bool ToValue(Item it, Type mt, out object v)
            {
                v = null;
                if (mt == typeof(string)) { v = it.val; return true; }
                if (mt == typeof(bool))
                {
                    string s = it.val.Trim().ToLowerInvariant();
                    v = (s == "true" || s == "1" || s == "yes" || s == "on");
                    return true;
                }
                double d;
                if (mt == typeof(float)) { if (!Num(it.val, out d)) return false; v = (float)d; return true; }
                if (mt == typeof(double)) { if (!Num(it.val, out d)) return false; v = d; return true; }
                if (mt == typeof(int) || mt == typeof(uint)) { if (!Num(it.val, out d)) return false; v = (int)d; return true; }
                if (mt == typeof(long) || mt == typeof(ulong)) { if (!Num(it.val, out d)) return false; v = (long)d; return true; }
                if (mt == typeof(short) || mt == typeof(ushort)) { if (!Num(it.val, out d)) return false; v = (short)d; return true; }
                if (mt == typeof(byte) || mt == typeof(sbyte)) { if (!Num(it.val, out d)) return false; v = (byte)d; return true; }
                if (mt.IsEnum)
                {
                    try { v = Enum.Parse(mt, it.val.Trim(), true); return true; }
                    catch { }
                    if (Num(it.val, out d)) { try { v = Enum.ToObject(mt, (int)d); return true; } catch { } }
                    return false;
                }
                return false;
            }

            private static bool Num(string s, out double d)
            {
                return double.TryParse(s.Trim(), NumberStyles.Float, CultureInfo.InvariantCulture, out d);
            }

            // ---------- 读取设置文件里的「顶层标量」 ----------
            private static List<Item> ReadItems(string path)
            {
                List<Item> list = new List<Item>();
                try
                {
                    if (!File.Exists(path)) return list;
                    string text = File.ReadAllText(path);
                    if (path.ToLowerInvariant().EndsWith(".xml")) list = ParseXml(text);
                    else list = ParseJson(text);
                }
                catch { }
                return list;
            }

            private static void SkipWs(string s, ref int i)
            {
                while (i < s.Length && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
            }

            private static string ReadString(string s, ref int i)
            {
                StringBuilder sb = new StringBuilder();
                if (i < s.Length && s[i] == '"') i++;
                while (i < s.Length)
                {
                    char c = s[i++];
                    if (c == '\\' && i < s.Length)
                    {
                        char e = s[i++];
                        if (e == 'n') sb.Append('\n');
                        else if (e == 'r') sb.Append('\r');
                        else if (e == 't') sb.Append('\t');
                        else if (e == 'u' && i + 4 <= s.Length)
                        {
                            int code;
                            if (int.TryParse(s.Substring(i, 4), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out code))
                            {
                                sb.Append((char)code);
                                i += 4;
                            }
                        }
                        else sb.Append(e);
                        continue;
                    }
                    if (c == '"') break;
                    sb.Append(c);
                }
                return sb.ToString();
            }

            private static void SkipNested(string s, ref int i)
            {
                int depth = 0;
                while (i < s.Length)
                {
                    char c = s[i];
                    if (c == '"') { ReadString(s, ref i); continue; }
                    if (c == '{' || c == '[') depth++;
                    else if (c == '}' || c == ']')
                    {
                        depth--;
                        i++;
                        if (depth <= 0) return;
                        continue;
                    }
                    i++;
                }
            }

            private static List<Item> ParseJson(string text)
            {
                List<Item> list = new List<Item>();
                int n = text.Length, i = 0, depth = 0;
                while (i < n)
                {
                    char ch = text[i];
                    if (ch == '"' && depth == 1)
                    {
                        string key = ReadString(text, ref i);
                        SkipWs(text, ref i);
                        if (i < n && text[i] == ':') i++;
                        SkipWs(text, ref i);
                        if (i >= n) break;
                        char c = text[i];
                        if (c == '"')
                        {
                            string val = ReadString(text, ref i);
                            Item it = new Item();
                            it.key = key; it.val = val;
                            list.Add(it);
                        }
                        else if (c == '{' || c == '[')
                        {
                            SkipNested(text, ref i);
                        }
                        else
                        {
                            int st = i;
                            while (i < n && text[i] != ',' && text[i] != '}' && text[i] != '\r' && text[i] != '\n') i++;
                            string raw = text.Substring(st, i - st).Trim();
                            Item it = new Item();
                            it.key = key; it.val = raw;
                            if (raw == "null") it.isNull = true;
                            list.Add(it);
                        }
                        continue;
                    }
                    if (ch == '"') { ReadString(text, ref i); continue; }
                    if (ch == '{' || ch == '[') depth++;
                    else if (ch == '}' || ch == ']') depth--;
                    i++;
                }
                return list;
            }

            private static List<Item> ParseXml(string text)
            {
                List<Item> list = new List<Item>();
                if (string.IsNullOrEmpty(text)) return list;

                // 找根节点开始标签的结束位置
                int rootEnd = -1;
                int i = 0;
                while (i < text.Length)
                {
                    int lt = text.IndexOf('<', i);
                    if (lt < 0) break;
                    if (string.Compare(text, lt, "<!--", 0, 4, StringComparison.Ordinal) == 0)
                    {
                        int e = text.IndexOf("-->", lt, StringComparison.Ordinal);
                        i = e < 0 ? text.Length : e + 3;
                        continue;
                    }
                    if (string.Compare(text, lt, "<?", 0, 2, StringComparison.Ordinal) == 0)
                    {
                        int e = text.IndexOf("?>", lt, StringComparison.Ordinal);
                        i = e < 0 ? text.Length : e + 2;
                        continue;
                    }
                    int gt = text.IndexOf('>', lt);
                    if (gt < 0) break;
                    if (lt + 1 < text.Length && text[lt + 1] == '/') return list; // 空文档
                    rootEnd = gt + 1;
                    if (gt > 0 && text[gt - 1] == '/') return list;               // 自闭合根节点
                    break;
                }
                if (rootEnd < 0) return list;

                i = rootEnd;
                while (i < text.Length)
                {
                    int lt = text.IndexOf('<', i);
                    if (lt < 0) break;
                    if (string.Compare(text, lt, "<!--", 0, 4, StringComparison.Ordinal) == 0)
                    {
                        int e = text.IndexOf("-->", lt, StringComparison.Ordinal);
                        i = e < 0 ? text.Length : e + 3;
                        continue;
                    }
                    if (lt + 1 >= text.Length) break;
                    if (text[lt + 1] == '/') break;                               // 根节点结束
                    int gt = text.IndexOf('>', lt);
                    if (gt < 0) break;
                    string tag = text.Substring(lt + 1, gt - lt - 1);
                    int attrStart = tag.IndexOf(' ');
                    string name = attrStart > 0 ? tag.Substring(0, attrStart) : tag;
                    if (name.EndsWith("/", StringComparison.Ordinal)) { i = gt + 1; continue; }
                    if (string.IsNullOrEmpty(name)) { i = gt + 1; continue; }
                    if (attrStart > 0) { i = gt + 1; continue; }                  // 带属性的元素跳过
                    string closeTag = "</" + name + ">";
                    int close = text.IndexOf(closeTag, gt + 1, StringComparison.Ordinal);
                    if (close < 0) break;
                    string content = text.Substring(gt + 1, close - gt - 1);
                    int next = close + closeTag.Length;
                    if (content.IndexOf('<') >= 0) { i = next; continue; }        // 嵌套结构：整段跳过
                    Item it = new Item();
                    it.key = name;
                    it.val = content.Trim();
                    if (it.val.Length == 0) it.isNull = true;
                    list.Add(it);
                    i = next;
                }
                return list;
            }
        }
    }
}
