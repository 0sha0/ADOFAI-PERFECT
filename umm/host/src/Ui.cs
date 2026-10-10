// ============================================================
//  Ui.cs — 我们自己实现的 UMM 兼容加载器（3/4）
//
//  UI 是游戏内的窗口宿主 MonoBehaviour：
//    · Update/LateUpdate/FixedUpdate 逐帧派发各 MOD 的回调
//    · OnGUI 为每个「打开界面」的 MOD 画一个窗口（自绘标题栏 + 右上角 X）：
//        - MOD 自带 OnGUI  → 原样使用 MOD 自己的界面
//        - MOD 只有设置项  → 用本工具的暗色 / 强调色风格自绘设置编辑器
//  强调色由工具侧（Mod Manager 页）推送，和工具本体保持同一套皮肤。
//  Overlayer 会反射读取 UI.Instance 上的 mWindowRect / mScrollPosition /
//  tabId，因此这些字段的名字与类型必须保持一致。
// ============================================================
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Xml;
using UnityEngine;

namespace UnityModManagerNet
{
    public partial class UnityModManager
    {
        public class UI : MonoBehaviour
        {
            public static UI Instance { get; private set; }

            public int tabId;

            private Rect mWindowRect;
            private Vector2[] mScrollPosition = new Vector2[64];

            private float mUIScale = 1f;

            // 原版 UMM 的 UI 暴露了这两个静态缩放函数（Iridium / YCH 等 MOD 会
            // 直接调用 UnityModManagerNet.UnityModManager.UI.Scale(...)）。
            public static int Scale(int value)
            {
                UI ui = Instance;
                if (ui == null) return value;
                return (int)(value * ui.mUIScale);
            }

            public static float Scale(float value)
            {
                UI ui = Instance;
                if (ui == null) return value;
                return value * ui.mUIScale;
            }

            // ---------------- 主题色 ----------------
            internal static Color AccentColor = new Color(149f / 255f, 173f / 255f, 181f / 255f, 1f);
            private static Color sBuiltAccent = new Color(0f, 0f, 0f, 0f);

            public static void SetAccent(float r, float g, float b)
            {
                AccentColor = new Color(Mathf.Clamp01(r), Mathf.Clamp01(g), Mathf.Clamp01(b), 1f);
            }

            internal readonly List<ModEntry> opened = new List<ModEntry>();
            private readonly Dictionary<string, Rect> rects = new Dictionary<string, Rect>();
            private readonly Dictionary<string, Vector2> scrolls = new Dictionary<string, Vector2>();

            private bool stylesReady;
            private Texture2D texWin, texHead, texCard, texRow, texField, texBtn, texBtnHover, texAccent, texLine, texBad;
            private GUIStyle winStyle, titleStyle, subStyle, secStyle, hintStyle, labelStyle;
            private GUIStyle fieldStyle, btnStyle, btnPrimaryStyle, closeStyle;

            private void Awake()
            {
                Instance = this;
                float scale = Params != null ? Params.UIScale : 1f;
                if (scale < 0.5f) scale = 0.5f;
                if (scale > 5f) scale = 5f;
                mUIScale = scale;
            }

            private void Update()
            {
                float dt = Time.unscaledDeltaTime;
                for (int i = 0; i < modEntries.Count; i++)
                {
                    ModEntry entry = modEntries[i];
                    if (!entry.Active) continue;
                    try { if (entry.OnUpdate != null) entry.OnUpdate(entry, dt); }
                    catch (Exception e) { entry.Logger.LogException("OnUpdate", e); }
                }
            }

            private void LateUpdate()
            {
                float dt = Time.unscaledDeltaTime;
                for (int i = 0; i < modEntries.Count; i++)
                {
                    ModEntry entry = modEntries[i];
                    if (!entry.Active) continue;
                    try { if (entry.OnLateUpdate != null) entry.OnLateUpdate(entry, dt); }
                    catch (Exception e) { entry.Logger.LogException("OnLateUpdate", e); }
                }
            }

            private void FixedUpdate()
            {
                float dt = Time.fixedDeltaTime;
                for (int i = 0; i < modEntries.Count; i++)
                {
                    ModEntry entry = modEntries[i];
                    if (!entry.Active) continue;
                    try { if (entry.OnFixedUpdate != null) entry.OnFixedUpdate(entry, dt); }
                    catch (Exception e) { entry.Logger.LogException("OnFixedUpdate", e); }
                }
            }
            // ============================================================
            //  样式（暗色 + 强调色，与工具本体一致）
            // ============================================================
            private static Color C(int r, int g, int b, float a)
            {
                return new Color(r / 255f, g / 255f, b / 255f, a);
            }

            private static Texture2D Solid(Color c)
            {
                Texture2D t = new Texture2D(1, 1);
                t.SetPixel(0, 0, c);
                t.Apply();
                t.hideFlags = HideFlags.HideAndDontSave;
                return t;
            }

            private void ReleaseTex()
            {
                Texture2D[] all = new Texture2D[] { texWin, texHead, texCard, texRow, texField,
                                                    texBtn, texBtnHover, texAccent, texLine, texBad };
                for (int i = 0; i < all.Length; i++)
                    if (all[i] != null) UnityEngine.Object.Destroy(all[i]);
            }

            private static GUIStyle Clone(GUIStyle src)
            {
                return src != null ? new GUIStyle(src) : new GUIStyle();
            }

            private void EnsureStyles()
            {
                if (stylesReady && sBuiltAccent == AccentColor) return;
                if (stylesReady) ReleaseTex();
                stylesReady = true;
                sBuiltAccent = AccentColor;

                texWin      = Solid(C(12, 12, 16, 0.92f));
                texHead     = Solid(C(17, 17, 19, 0.97f));
                texCard     = Solid(C(11, 12, 13, 0.72f));
                texRow      = Solid(C(24, 25, 27, 0.96f));
                texField    = Solid(C(31, 32, 35, 1f));
                texBtn      = Solid(C(34, 35, 37, 1f));
                texBtnHover = Solid(C(48, 49, 52, 1f));
                texAccent   = Solid(AccentColor);
                texLine     = Solid(C(52, 52, 52, 1f));
                texBad      = Solid(C(178, 74, 82, 240f / 255f));

                winStyle = new GUIStyle(GUIStyle.none);
                winStyle.normal.background = null;
                winStyle.padding = new RectOffset(0, 0, 0, 0);
                winStyle.margin = new RectOffset(0, 0, 0, 0);
                winStyle.border = new RectOffset(0, 0, 0, 0);

                titleStyle = Clone(GUI.skin.label);
                titleStyle.fontStyle = FontStyle.Bold;
                titleStyle.alignment = TextAnchor.MiddleLeft;
                titleStyle.normal.textColor = Color.white;

                subStyle = Clone(GUI.skin.label);
                subStyle.alignment = TextAnchor.MiddleLeft;
                subStyle.normal.textColor = C(150, 150, 150, 255);

                secStyle = Clone(GUI.skin.label);
                secStyle.fontStyle = FontStyle.Bold;
                secStyle.alignment = TextAnchor.MiddleLeft;
                secStyle.normal.textColor = AccentColor;

                hintStyle = Clone(GUI.skin.label);
                hintStyle.wordWrap = true;
                hintStyle.normal.textColor = C(128, 128, 128, 255);

                labelStyle = Clone(GUI.skin.label);
                labelStyle.alignment = TextAnchor.MiddleLeft;
                labelStyle.normal.textColor = C(175, 175, 175, 255);

                fieldStyle = Clone(GUI.skin.textField);
                fieldStyle.normal.background = texField;
                fieldStyle.hover.background = texField;
                fieldStyle.focused.background = texField;
                fieldStyle.normal.textColor = Color.white;
                fieldStyle.padding = new RectOffset(7, 7, 4, 4);
                fieldStyle.border = new RectOffset(0, 0, 0, 0);

                btnStyle = Clone(GUI.skin.button);
                btnStyle.normal.background = texBtn;
                btnStyle.normal.textColor = C(210, 210, 210, 255);
                btnStyle.hover.background = texBtnHover;
                btnStyle.hover.textColor = Color.white;
                btnStyle.active.background = texBtnHover;
                btnStyle.active.textColor = Color.white;
                btnStyle.border = new RectOffset(0, 0, 0, 0);
                btnStyle.padding = new RectOffset(6, 6, 4, 4);

                btnPrimaryStyle = Clone(btnStyle);
                btnPrimaryStyle.normal.background = texAccent;
                btnPrimaryStyle.normal.textColor = C(16, 16, 18, 255);
                btnPrimaryStyle.hover.background = texAccent;
                btnPrimaryStyle.hover.textColor = C(0, 0, 0, 255);

                closeStyle = Clone(btnStyle);
                closeStyle.normal.background = null;
                closeStyle.normal.textColor = C(170, 170, 170, 255);
                closeStyle.hover.background = texBad;
                closeStyle.hover.textColor = Color.white;
                closeStyle.active.background = texBad;
                closeStyle.active.textColor = Color.white;
                closeStyle.fontStyle = FontStyle.Bold;
            }
            // ============================================================
            //  窗口
            // ============================================================
            private void OnGUI()
            {
                if (opened.Count == 0) return;
                EnsureStyles();

                for (int i = opened.Count - 1; i >= 0; i--)
                {
                    ModEntry entry = opened[i];
                    if (entry == null) { opened.RemoveAt(i); continue; }
                    string key = entry.Info.Id;
                    Rect rect;
                    if (!rects.TryGetValue(key, out rect))
                        rect = new Rect(280f, 150f, Scale(580f), Scale(420f));
                    rect.width  = Scale(580f);
                    rect.height = Scale(420f);
                    if (rect.x > Screen.width - 80f) rect.x = Screen.width - 80f;
                    if (rect.y > Screen.height - 60f) rect.y = Screen.height - 60f;
                    if (rect.x < 0f) rect.x = 0f;
                    if (rect.y < 0f) rect.y = 0f;
                    mWindowRect = rect;
                    rects[key] = GUILayout.Window(WindowId(key), rect, DrawModWindow, GUIContent.none, winStyle);
                }

                Event e = Event.current;
                if (e != null && e.type == EventType.KeyDown && e.keyCode == KeyCode.Escape)
                {
                    ModEntry last = opened[opened.Count - 1];
                    Close(last);
                    e.Use();
                }
            }

            internal static int WindowId(string key)
            {
                return (key.GetHashCode() & 0x7fffffff) | 0x10000000;
            }

            private void DrawModWindow(int windowId)
            {
                string id = null;
                foreach (KeyValuePair<string, Rect> kv in rects)
                    if (WindowId(kv.Key) == windowId) { id = kv.Key; break; }
                ModEntry entry = id != null ? FindMod(id) : null;
                if (entry == null) { GUI.DragWindow(); return; }

                Rect rect = rects[id];
                float w = rect.width, h = rect.height;
                float pad = Scale(12f), titleH = Scale(32f);

                GUI.DrawTexture(new Rect(0f, 0f, w, h), texWin);
                GUI.DrawTexture(new Rect(0f, 0f, w, titleH), texHead);
                GUI.DrawTexture(new Rect(0f, titleH - 1f, w, 1f), texLine);

                string title = string.IsNullOrEmpty(entry.Info.DisplayName) ? entry.Info.Id : entry.Info.DisplayName;
                GUI.Label(new Rect(pad, 2f, w - titleH - pad * 2f, titleH - 4f),
                          title + "   v" + entry.Info.Version, titleStyle);
                if (GUI.Button(new Rect(w - titleH - 2f, 3f, titleH - 6f, titleH - 6f), "X", closeStyle))
                {
                    Close(entry);
                    return;
                }
                GUI.DragWindow(new Rect(0f, 0f, w - titleH - 6f, titleH));

                GUILayout.Space(titleH + Scale(4f));
                GUILayout.BeginHorizontal();
                GUILayout.Space(pad);
                GUILayout.BeginVertical();
                {
                    Vector2 scroll;
                    if (!scrolls.TryGetValue(id, out scroll)) scroll = Vector2.zero;
                    scroll = GUILayout.BeginScrollView(scroll, false, true, GUILayout.ExpandHeight(true));
                    try
                    {
                        if (entry.OnGUI != null) entry.OnGUI(entry);
                        else DrawSettingsEditor(entry);
                    }
                    catch (Exception e) { entry.Logger.LogException("OnGUI", e); }
                    GUILayout.EndScrollView();
                    scrolls[id] = scroll;
                    mScrollPosition[0] = scroll;
                }
                GUILayout.EndVertical();
                GUILayout.Space(pad);
                GUILayout.EndHorizontal();
            }
            // ============================================================
            //  MOD 自带界面的缺失时的自绘设置编辑器（本工具风格）
            // ============================================================
            private readonly Dictionary<string, List<SetFile>> setCache = new Dictionary<string, List<SetFile>>();
            private readonly Dictionary<string, SetFile> setSel = new Dictionary<string, SetFile>();
            private readonly Dictionary<string, string> setStatus = new Dictionary<string, string>();

            private void DrawSettingsEditor(ModEntry entry)
            {
                GUILayout.Label("Id: " + entry.Info.Id + "     " + (entry.Info.Author ?? ""), subStyle);
                GUILayout.Label("Path: " + entry.Path, hintStyle);
                GUILayout.Space(Scale(6f));

                List<SetFile> files = GetSetFiles(entry);
                if (files.Count == 0)
                {
                    GUILayout.Label("该 MOD 没有独立的设置文件。", labelStyle);
                    GUILayout.Space(Scale(4f));
                    GUILayout.Label("可以在 MOD 管理器里开启 / 停用，或用「打开 MOD 文件夹」查看它的配置。", hintStyle);
                    return;
                }

                SetFile sel;
                if (!setSel.TryGetValue(entry.Info.Id, out sel) || sel == null) sel = files[0];

                if (files.Count > 1)
                {
                    GUILayout.BeginHorizontal();
                    GUILayout.Label("设置文件", labelStyle, GUILayout.Width(Scale(72f)));
                    for (int i = 0; i < files.Count; i++)
                    {
                        bool on = (files[i] == sel);
                        if (GUILayout.Toggle(on, files[i].name, btnStyle) != on && !on)
                        {
                            sel = files[i];
                            setSel[entry.Info.Id] = sel;
                        }
                    }
                    GUILayout.EndHorizontal();
                    GUILayout.Space(Scale(4f));
                }

                EnsureSetFileLoaded(sel);
                GUILayout.Label(sel.name, secStyle);
                GUILayout.Space(Scale(3f));

                if (sel.items.Count == 0)
                {
                    GUILayout.Label("文件不存在或没有可编辑的标量项。", hintStyle);
                }
                else
                {
                    for (int i = 0; i < sel.items.Count; i++)
                    {
                        SetItem it = sel.items[i];
                        if (it.type == 0)
                        {
                            bool v = string.Equals(it.val, "true", StringComparison.OrdinalIgnoreCase);
                            bool nv = GUILayout.Toggle(v, "  " + it.key, GUI.skin.toggle);
                            if (nv != v) it.val = nv ? "true" : "false";
                        }
                        else
                        {
                            GUILayout.BeginHorizontal();
                            GUILayout.Label(it.key, labelStyle, GUILayout.Width(Scale(180f)));
                            it.edit = GUILayout.TextField(it.edit ?? string.Empty, fieldStyle);
                            if (!string.Equals(it.edit, it.val, StringComparison.Ordinal)) it.val = it.edit;
                            GUILayout.EndHorizontal();
                        }
                        GUILayout.Space(Scale(3f));
                    }
                }

                GUILayout.Space(Scale(8f));
                string st;
                if (setStatus.TryGetValue(entry.Info.Id, out st) && !string.IsNullOrEmpty(st))
                {
                    GUIStyle ss = Clone(labelStyle);
                    ss.normal.textColor = C(150, 205, 155, 255);
                    GUILayout.Label(st, ss);
                    GUILayout.Space(Scale(3f));
                }

                GUILayout.BeginHorizontal();
                if (GUILayout.Button("保存设置", btnPrimaryStyle, GUILayout.Height(Scale(26f))))
                {
                    bool ok = SaveSetFile(sel);
                    if (ok)
                    {
                        // 文件改完了，但 MOD 内存里还是旧值 —— 立刻热应用一次
                        string res = SettingsSync.Apply(entry.Info.Id, true);
                        setStatus[entry.Info.Id] = "已保存到 " + sel.name +
                            (res.StartsWith("ok", StringComparison.Ordinal) ? "，并已应用到运行中的 MOD" : "（MOD 需重新加载后生效）");
                    }
                    else setStatus[entry.Info.Id] = "保存失败（文件不可写）";
                    if (!ok) entry.Logger.Error("SaveSettings failed: " + sel.path);
                }
                if (GUILayout.Button("重新载入", btnStyle, GUILayout.Height(Scale(26f))))
                {
                    sel.items.Clear();
                    sel.loaded = false;
                    EnsureSetFileLoaded(sel);
                    setStatus.Remove(entry.Info.Id);
                }
                if (GUILayout.Button("打开文件夹", btnStyle, GUILayout.Height(Scale(26f))))
                {
                    try { Application.OpenURL("file://" + entry.Path.TrimEnd('\\', '/')); } catch { }
                }
                GUILayout.EndHorizontal();
            }
            // ---------------- 设置文件：枚举 / 解析 / 写回 ----------------
            private class SetItem
            {
                public string key;
                public string val;
                public string edit;
                public int type;   // 0=bool 1=number 2=string
            }

            private class SetFile
            {
                public string name;
                public string path;
                public bool xml;
                public bool loaded;
                public readonly List<SetItem> items = new List<SetItem>();
            }

            private static bool IsSetName(string low)
            {
                if (low == "settings.xml" || low == "config.xml") return true;
                if (low == "settings.json" || low == "config.json") return true;
                if (low == "options.xml" || low == "options.json") return true;
                if (low == "preferences.xml" || low == "preferences.json") return true;
                if (low.EndsWith(".settings.xml", StringComparison.Ordinal)) return true;
                if (low.EndsWith(".settings.json", StringComparison.Ordinal)) return true;
                int dot = low.LastIndexOf('.');
                string stem = dot > 0 ? low.Substring(0, dot) : low;
                if (stem.EndsWith("settings", StringComparison.Ordinal) ||
                    stem.EndsWith("setting", StringComparison.Ordinal)) return true;
                if (stem.EndsWith("config", StringComparison.Ordinal) ||
                    stem.EndsWith("configuration", StringComparison.Ordinal)) return true;
                return false;
            }

            private List<SetFile> GetSetFiles(ModEntry entry)
            {
                List<SetFile> list;
                if (setCache.TryGetValue(entry.Info.Id, out list)) return list;
                list = new List<SetFile>();
                try
                {
                    string dir = entry.Path;
                    if (Directory.Exists(dir))
                    {
                        string[] xmls = Directory.GetFiles(dir, "*.xml");
                        for (int i = 0; i < xmls.Length; i++)
                        {
                            string low = Path.GetFileName(xmls[i]).ToLowerInvariant();
                            if (!IsSetName(low)) continue;
                            SetFile f = new SetFile();
                            f.name = Path.GetFileName(xmls[i]);
                            f.path = xmls[i];
                            f.xml = true;
                            list.Add(f);
                        }
                        string[] jsons = Directory.GetFiles(dir, "*.json");
                        for (int i = 0; i < jsons.Length; i++)
                        {
                            string low = Path.GetFileName(jsons[i]).ToLowerInvariant();
                            if (!IsSetName(low)) continue;
                            SetFile f = new SetFile();
                            f.name = Path.GetFileName(jsons[i]);
                            f.path = jsons[i];
                            f.xml = false;
                            list.Add(f);
                        }
                    }
                }
                catch (Exception e) { entry.Logger.LogException("GetSetFiles", e); }
                setCache[entry.Info.Id] = list;
                return list;
            }

            private static int Classify(string v)
            {
                if (string.Equals(v, "true", StringComparison.OrdinalIgnoreCase)) return 0;
                if (string.Equals(v, "false", StringComparison.OrdinalIgnoreCase)) return 0;
                if (!string.IsNullOrEmpty(v))
                {
                    double d;
                    if (double.TryParse(v, NumberStyles.Float, CultureInfo.InvariantCulture, out d)) return 1;
                }
                return 2;
            }

            private void EnsureSetFileLoaded(SetFile sf)
            {
                if (sf.loaded) return;
                sf.loaded = true;
                try
                {
                    if (!File.Exists(sf.path)) return;
                    string text = File.ReadAllText(sf.path);
                    if (sf.xml) ParseXml(text, sf.items);
                    else ParseJson(text, sf.items);
                }
                catch (Exception e) { UnityModManager.Logger.LogException("ReadSettings", e); }
            }

            private static void AddItem(List<SetItem> outItems, string key, string val)
            {
                if (string.IsNullOrEmpty(key)) return;
                SetItem it = new SetItem();
                it.key = key;
                it.val = val ?? string.Empty;
                it.edit = it.val;
                it.type = Classify(it.val);
                outItems.Add(it);
            }
            private static void ParseXml(string text, List<SetItem> outItems)
            {
                outItems.Clear();
                try
                {
                    XmlDocument doc = new XmlDocument();
                    doc.LoadXml(text);
                    XmlElement root = doc.DocumentElement;
                    if (root == null) return;
                    foreach (XmlNode node in root.ChildNodes)
                    {
                        if (node.NodeType != XmlNodeType.Element) continue;
                        if (node.ChildNodes.Count > 1) continue;
                        if (node.ChildNodes.Count == 1 && node.ChildNodes[0].NodeType != XmlNodeType.Text) continue;
                        string key = node.Name;
                        if (node.Attributes != null)
                        {
                            XmlAttribute na = node.Attributes["Name"];
                            if (na != null && !string.IsNullOrEmpty(na.Value)) key = na.Value;
                        }
                        AddItem(outItems, key, node.InnerText.Trim());
                    }
                }
                catch { }
            }

            private static void ParseJson(string text, List<SetItem> outItems)
            {
                outItems.Clear();
                int depth = 0;
                bool inStr = false;
                int i = 0;
                int n = text.Length;
                while (i < n)
                {
                    char ch = text[i];
                    if (inStr)
                    {
                        if (ch == '\\') { i += 2; continue; }
                        if (ch == '"') inStr = false;
                        i++;
                        continue;
                    }
                    if (ch == '"' && depth == 1)
                    {
                        int a = i + 1, b = a;
                        while (b < n)
                        {
                            if (text[b] == '\\') { b += 2; continue; }
                            if (text[b] == '"') break;
                            b++;
                        }
                        if (b >= n) break;
                        string key = text.Substring(a, b - a);
                        int c = b + 1;
                        while (c < n && char.IsWhiteSpace(text[c])) c++;
                        if (c >= n || text[c] != ':') { i = b + 1; continue; }
                        c++;
                        while (c < n && char.IsWhiteSpace(text[c])) c++;
                        if (c >= n) break;
                        if (text[c] == '"')
                        {
                            int d = c + 1, e = d;
                            while (e < n)
                            {
                                if (text[e] == '\\') { e += 2; continue; }
                                if (text[e] == '"') break;
                                e++;
                            }
                            AddItem(outItems, key, text.Substring(d, e - d));
                            i = e + 1;
                            continue;
                        }
                        if (text[c] == '{' || text[c] == '[')
                        {
                            int d2 = 0;
                            bool s2 = false;
                            int e = c;
                            for (; e < n; e++)
                            {
                                char x = text[e];
                                if (s2)
                                {
                                    if (x == '\\') { e++; continue; }
                                    if (x == '"') s2 = false;
                                    continue;
                                }
                                if (x == '"') { s2 = true; continue; }
                                if (x == '{' || x == '[') d2++;
                                else if (x == '}' || x == ']') { d2--; if (d2 == 0) break; }
                            }
                            i = e + 1;
                            continue;
                        }
                        int f = c;
                        while (f < n && text[f] != ',' && text[f] != '}' && text[f] != ']' &&
                               text[f] != '\r' && text[f] != '\n') f++;
                        AddItem(outItems, key, text.Substring(c, f - c).Trim());
                        i = f;
                        continue;
                    }
                    if (ch == '{' || ch == '[') depth++;
                    else if (ch == '}' || ch == ']') depth--;
                    i++;
                }
            }
            private static bool PatchXml(string text, SetItem it, out string result)
            {
                result = text;
                string open = "<" + it.key + ">";
                int p = text.IndexOf(open, StringComparison.Ordinal);
                if (p < 0) return false;
                int cs = p + open.Length;
                string close = "</" + it.key + ">";
                int ce = text.IndexOf(close, cs, StringComparison.Ordinal);
                if (ce < 0) return false;
                result = text.Substring(0, cs) + it.val + text.Substring(ce);
                return true;
            }

            private static bool PatchJson(string text, SetItem it, out string result)
            {
                result = text;
                string pat = "\"" + it.key + "\"";
                int p = text.IndexOf(pat, StringComparison.Ordinal);
                while (p >= 0)
                {
                    int q = p + pat.Length;
                    while (q < text.Length && char.IsWhiteSpace(text[q])) q++;
                    if (q >= text.Length || text[q] != ':')
                    {
                        p = text.IndexOf(pat, q, StringComparison.Ordinal);
                        continue;
                    }
                    q++;
                    while (q < text.Length && char.IsWhiteSpace(text[q])) q++;
                    if (q >= text.Length) return false;
                    string nv;
                    if (it.type == 0)
                        nv = string.Equals(it.val, "true", StringComparison.OrdinalIgnoreCase) ? "true" : "false";
                    else if (it.type == 1) nv = it.val;
                    else nv = "\"" + it.val.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
                    if (text[q] == '"')
                    {
                        int e = q + 1;
                        while (e < text.Length)
                        {
                            if (text[e] == '\\') { e += 2; continue; }
                            if (text[e] == '"') break;
                            e++;
                        }
                        if (e >= text.Length) return false;
                        result = text.Substring(0, q) + nv + text.Substring(e + 1);
                        return true;
                    }
                    int f = q;
                    while (f < text.Length && text[f] != ',' && text[f] != '}' && text[f] != ']' &&
                           text[f] != '\r' && text[f] != '\n') f++;
                    int end = f;
                    while (end > q && char.IsWhiteSpace(text[end - 1])) end--;
                    result = text.Substring(0, q) + nv + text.Substring(end);
                    return true;
                }
                return false;
            }

            private static bool SaveSetFile(SetFile sf)
            {
                try
                {
                    if (!File.Exists(sf.path)) return false;
                    string work = File.ReadAllText(sf.path);
                    bool any = false;
                    for (int i = 0; i < sf.items.Count; i++)
                    {
                        string res;
                        bool ok = sf.xml ? PatchXml(work, sf.items[i], out res)
                                         : PatchJson(work, sf.items[i], out res);
                        if (ok) { work = res; any = true; }
                    }
                    if (!any) return false;
                    File.WriteAllText(sf.path, work);
                    return true;
                }
                catch (Exception e)
                {
                    UnityModManager.Logger.LogException("SaveSettings", e);
                    return false;
                }
            }

            // ============================================================
            //  开关 / 查询
            // ============================================================
            public static void Open(ModEntry entry)
            {
                if (entry == null || Instance == null) return;
                if (!Instance.opened.Contains(entry))
                {
                    Instance.opened.Add(entry);
                    try { if (entry.OnShowGUI != null) entry.OnShowGUI(entry); }
                    catch (Exception e) { entry.Logger.LogException("OnShowGUI", e); }
                }
            }

            public static void Close(ModEntry entry)
            {
                if (entry == null || Instance == null) return;
                if (Instance.opened.Remove(entry))
                {
                    try { if (entry.OnHideGUI != null) entry.OnHideGUI(entry); }
                    catch (Exception e) { entry.Logger.LogException("OnHideGUI", e); }
                    try { if (entry.OnSaveGUI != null) entry.OnSaveGUI(entry); }
                    catch (Exception e) { entry.Logger.LogException("OnSaveGUI", e); }
                }
            }

            public static void CloseAll()
            {
                if (Instance == null) return;
                while (Instance.opened.Count > 0) Close(Instance.opened[Instance.opened.Count - 1]);
            }

            public static bool IsOpen(ModEntry entry)
            {
                return Instance != null && entry != null && Instance.opened.Contains(entry);
            }

            public static bool HasWindow
            {
                get { return Instance != null && Instance.opened.Count > 0; }
            }
        }
    }
}
