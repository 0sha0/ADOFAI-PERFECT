// ============================================================
//  Apply.cs — 设置热应用引擎（Sidecar 版，纯反射、只面向官方 UMM）
//
//  与 umm/host/src/SettingsSync.cs 同一套语义：
//  工具改完 <MOD>\Settings.xml 后，把文件里的新值就地推进
//  官方 UMM 加载的那份设置对象上；必要时把 MOD 关掉再打开，
//  让只在 OnToggle 里应用设置的 MOD（如 Overlayer）重新读取。
// ============================================================
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;

namespace AdofPerfectUmm
{
    internal static class Apply
    {
        private class Item
        {
            public string key;
            public string val;
            public bool isNull;
        }

        // id：MOD 的 Id；toggle：true 时关→开一次
        public static string Run(string id, bool toggle)
        {
            object fe = Foreign.Entry(id);
            if (fe == null) return "err:not-found";
            string dir = Foreign.PathOf(fe);
            string asmName = Foreign.AssemblyNameOf(fe);

            List<string> files = DirSettingFiles(dir);
            int applied = 0;
            bool reloaded = false;

            // 1) 反射扫描 MOD 程序集里的静态设置对象
            try { applied += ScanStatics(dir, asmName, fe, files); }
            catch (Exception e) { SideLogger.LogException("apply/scan", e); }

            // 2) 关掉再打开：让 MOD 自己重新读取并应用（语言/贴图之类）
            if (toggle && Foreign.ActiveOf(fe))
            {
                Dictionary<string, string> snapshot = Snapshot(files);
                try
                {
                    Foreign.SetEnabled(id, false);
                    RestoreIfExists(snapshot);
                    Foreign.SetEnabled(id, true);
                    reloaded = true;
                }
                catch (Exception e) { SideLogger.LogException("apply/toggle", e); }
            }

            SideLogger.Log("[" + id + "] Settings applied (" + applied + " field(s))" + (reloaded ? " + reload." : "."));
            if (applied > 0) return "ok:" + applied;
            if (reloaded) return "ok:reload";
            return "err:no-live-settings";
        }

        // ---------- 设置文件识别 ----------
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

        private static List<string> DirSettingFiles(string dir)
        {
            List<string> list = new List<string>();
            try
            {
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

        // ---------- 快照 / 还原（防止 OnToggle(false) 把旧值写回文件） ----------
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
                    if (!File.Exists(kv.Key)) continue;
                    File.WriteAllBytes(kv.Key, Convert.FromBase64String(kv.Value));
                }
                catch { }
            }
        }

        // ---------- 扫描 MOD 程序集里的静态设置对象 ----------
        private static int ScanStatics(string dir, string asmName, object foreignEntry, List<string> files)
        {
            int n = 0;
            List<object> done = new List<object>();
            Assembly[] asms;
            try { asms = AppDomain.CurrentDomain.GetAssemblies(); }
            catch { return 0; }
            for (int a = 0; a < asms.Length; a++)
            {
                Assembly asm = asms[a];
                if (!IsModAssembly(asm, dir, asmName)) continue;
                Type[] types;
                try { types = asm.GetTypes(); }
                catch (ReflectionTypeLoadException e) { types = e.Types; }
                catch { continue; }
                if (types == null) continue;
                for (int i = 0; i < types.Length; i++)
                {
                    Type t = types[i];
                    if (t == null) continue;
                    n += ScanType(t, foreignEntry, files, done);
                }
            }
            return n;
        }

        private static bool IsModAssembly(Assembly asm, string dir, string asmName)
        {
            if (asm == null) return false;
            string name = null;
            try { name = asm.GetName().Name; } catch { }
            string loc = null;
            try { loc = asm.Location; } catch { }
            if (!string.IsNullOrEmpty(loc) && !string.IsNullOrEmpty(dir))
            {
                try
                {
                    if (Path.GetFullPath(loc).StartsWith(Path.GetFullPath(dir), StringComparison.OrdinalIgnoreCase))
                        return true;
                }
                catch { }
            }
            try
            {
                string want = asmName;
                if (!string.IsNullOrEmpty(want) && want.EndsWith(".dll", StringComparison.OrdinalIgnoreCase))
                    want = want.Substring(0, want.Length - 4);
                if (!string.IsNullOrEmpty(want) && string.Equals(name, want, StringComparison.OrdinalIgnoreCase))
                    return true;
            }
            catch { }
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

        private static int ScanType(Type t, object foreignEntry, List<string> files, List<object> done)
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
                bool dup = false;
                for (int k = 0; k < done.Count; k++) if (ReferenceEquals(done[k], obj)) { dup = true; break; }
                if (dup) continue;
                done.Add(obj);

                string path = SettingsObjPath(obj, candType[i], foreignEntry);
                if (string.IsNullOrEmpty(path) || !File.Exists(path))
                    path = FindBestFile(candType[i], files);
                if (string.IsNullOrEmpty(path)) continue;

                int c = ApplyScalars(obj, path);
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

        // 官方 ModSettings.GetPath(modEntry) —— 反射调用
        private static string SettingsObjPath(object obj, Type objType, object foreignEntry)
        {
            try
            {
                MethodInfo gp = objType.GetMethod("GetPath", BindingFlags.Public | BindingFlags.Instance);
                if (gp != null && foreignEntry != null)
                    return gp.Invoke(obj, new object[] { foreignEntry }) as string;
            }
            catch { }
            return null;
        }

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

        // ---------- 只把文件里的标量键覆盖到对象上 ----------
        private static int ApplyScalars(object target, string path)
        {
            if (target == null || string.IsNullOrEmpty(path) || !File.Exists(path)) return 0;
            List<Item> items = ReadItems(path);
            if (items.Count == 0) return 0;
            Type t = target.GetType();

            int hits = 0;
            for (int i = 0; i < items.Count; i++) if (HasMember(t, items[i].key)) hits++;
            if (hits < 2) return 0;

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
                if (lt + 1 < text.Length && text[lt + 1] == '/') return list;
                rootEnd = gt + 1;
                if (gt > 0 && text[gt - 1] == '/') return list;
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
                if (text[lt + 1] == '/') break;
                int gt = text.IndexOf('>', lt);
                if (gt < 0) break;
                string tag = text.Substring(lt + 1, gt - lt - 1);
                int attrStart = tag.IndexOf(' ');
                string name = attrStart > 0 ? tag.Substring(0, attrStart) : tag;
                if (name.EndsWith("/", StringComparison.Ordinal)) { i = gt + 1; continue; }
                if (string.IsNullOrEmpty(name)) { i = gt + 1; continue; }
                if (attrStart > 0) { i = gt + 1; continue; }
                string closeTag = "</" + name + ">";
                int close = text.IndexOf(closeTag, gt + 1, StringComparison.Ordinal);
                if (close < 0) break;
                string content = text.Substring(gt + 1, close - gt - 1);
                int next = close + closeTag.Length;
                if (content.IndexOf('<') >= 0) { i = next; continue; }
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
