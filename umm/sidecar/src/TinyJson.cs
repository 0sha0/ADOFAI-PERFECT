// ============================================================
//  TinyJson.cs — 随本加载器一同分发的 JSON 读写实现
//
//  JALib / JAMod 的引导程序在编译期引用了 UnityModManager.dll 里的
//  TinyJson.JSONParser.FromJson<T>()，因此这套实现必须与它们的调用
//  约定完全一致：
//    · 对象映射只用 “公开实例字段 / 属性”，大小写不敏感；
//    · 用 GetUninitializedObject 构造，不要求目标类型有默认构造函数；
//    · JSON 里没有的成员保持原样，JSON 里多出来的成员直接忽略；
//    · 解析前整体去掉字符串外的空白。
// ============================================================
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.Reflection;
using System.Runtime.Serialization;
using System.Text;

namespace TinyJson
{
    [AttributeUsage(AttributeTargets.Field | AttributeTargets.Property)]
    public class IgnoreJsonAttribute : Attribute { }

    public static class JSONParser
    {
        [ThreadStatic] private static Stack<List<string>> splitArrayPool;
        [ThreadStatic] private static StringBuilder stringBuilder;
        [ThreadStatic] private static Dictionary<Type, Dictionary<string, FieldInfo>> fieldInfoCache;
        [ThreadStatic] private static Dictionary<Type, Dictionary<string, PropertyInfo>> propertyInfoCache;

        public static T FromJson<T>(this string json)
        {
            if (propertyInfoCache == null) propertyInfoCache = new Dictionary<Type, Dictionary<string, PropertyInfo>>();
            if (fieldInfoCache == null) fieldInfoCache = new Dictionary<Type, Dictionary<string, FieldInfo>>();
            if (stringBuilder == null) stringBuilder = new StringBuilder();
            if (splitArrayPool == null) splitArrayPool = new Stack<List<string>>();

            stringBuilder.Length = 0;
            for (int i = 0; i < json.Length; i++)
            {
                char c = json[i];
                if (c == '"') i = AppendUntilStringEnd(true, i, json);
                else if (!char.IsWhiteSpace(c)) stringBuilder.Append(c);
            }
            return (T)ParseValue(typeof(T), stringBuilder.ToString());
        }

        private static int AppendUntilStringEnd(bool appendEscapeCharacter, int startIdx, string json)
        {
            stringBuilder.Append(json[startIdx]);
            for (int i = startIdx + 1; i < json.Length; i++)
            {
                if (json[i] == '\\')
                {
                    if (appendEscapeCharacter) stringBuilder.Append(json[i]);
                    stringBuilder.Append(json[i + 1]);
                    i++;
                }
                else
                {
                    if (json[i] == '"') { stringBuilder.Append(json[i]); return i; }
                    stringBuilder.Append(json[i]);
                }
            }
            return json.Length - 1;
        }

        private static List<string> Split(string json)
        {
            List<string> list = splitArrayPool.Count > 0 ? splitArrayPool.Pop() : new List<string>();
            list.Clear();
            if (json.Length == 2) return list;
            int depth = 0;
            stringBuilder.Length = 0;
            for (int i = 1; i < json.Length - 1; i++)
            {
                switch (json[i])
                {
                    case '[':
                    case '{':
                        depth++;
                        break;
                    case ']':
                    case '}':
                        depth--;
                        break;
                    case '"':
                        i = AppendUntilStringEnd(true, i, json);
                        continue;
                    case ',':
                    case ':':
                        if (depth == 0) { list.Add(stringBuilder.ToString()); stringBuilder.Length = 0; continue; }
                        break;
                }
                stringBuilder.Append(json[i]);
            }
            list.Add(stringBuilder.ToString());
            return list;
        }

        internal static object ParseValue(Type type, string json)
        {
            if (type == typeof(string))
            {
                if (json.Length <= 2) return string.Empty;
                StringBuilder sb = new StringBuilder(json.Length);
                for (int i = 1; i < json.Length - 1; i++)
                {
                    if (json[i] == '\\' && i + 1 < json.Length - 1)
                    {
                        int idx = "\"\\nrtbf/".IndexOf(json[i + 1]);
                        if (idx >= 0) { sb.Append("\"\\\n\r\t\b\f/"[idx]); i++; continue; }
                        if (json[i + 1] == 'u' && i + 5 < json.Length - 1)
                        {
                            uint hex;
                            if (uint.TryParse(json.Substring(i + 2, 4), NumberStyles.AllowHexSpecifier, null, out hex))
                            {
                                sb.Append((char)hex); i += 5; continue;
                            }
                        }
                    }
                    sb.Append(json[i]);
                }
                return sb.ToString();
            }
            if (type.IsPrimitive) return Convert.ChangeType(json, type, CultureInfo.InvariantCulture);
            if (type == typeof(decimal))
            {
                decimal d;
                decimal.TryParse(json, NumberStyles.Float, CultureInfo.InvariantCulture, out d);
                return d;
            }
            if (json == "null") return null;
            if (type.IsEnum)
            {
                if (json.Length > 0 && json[0] == '"') json = json.Substring(1, json.Length - 2);
                try { return Enum.Parse(type, json, false); }
                catch { return 0; }
            }
            if (type.IsArray)
            {
                Type elementType = type.GetElementType();
                if (json.Length == 0 || json[0] != '[' || json[json.Length - 1] != ']') return null;
                List<string> parts = Split(json);
                Array array = Array.CreateInstance(elementType, parts.Count);
                for (int i = 0; i < parts.Count; i++) array.SetValue(ParseValue(elementType, parts[i]), i);
                splitArrayPool.Push(parts);
                return array;
            }
            if (type.IsGenericType && type.GetGenericTypeDefinition() == typeof(List<>))
            {
                Type elementType = type.GetGenericArguments()[0];
                if (json.Length == 0 || json[0] != '[' || json[json.Length - 1] != ']') return null;
                List<string> parts = Split(json);
                IList list = (IList)type.GetConstructor(new Type[] { typeof(int) }).Invoke(new object[] { parts.Count });
                for (int i = 0; i < parts.Count; i++) list.Add(ParseValue(elementType, parts[i]));
                splitArrayPool.Push(parts);
                return list;
            }
            if (type.IsGenericType && type.GetGenericTypeDefinition() == typeof(Dictionary<,>))
            {
                Type[] args = type.GetGenericArguments();
                Type keyType = args[0];
                Type valueType = args[1];
                if (keyType != typeof(string)) return null;
                if (json.Length == 0 || json[0] != '{' || json[json.Length - 1] != '}') return null;
                List<string> parts = Split(json);
                if (parts.Count % 2 != 0) return null;
                IDictionary dict = (IDictionary)type.GetConstructor(new Type[] { typeof(int) }).Invoke(new object[] { parts.Count / 2 });
                for (int i = 0; i < parts.Count; i += 2)
                {
                    if (parts[i].Length > 2)
                        dict.Add(parts[i].Substring(1, parts[i].Length - 2), ParseValue(valueType, parts[i + 1]));
                }
                return dict;
            }
            if (type == typeof(object)) return ParseAnonymousValue(json);
            if (json.Length > 0 && json[0] == '{' && json[json.Length - 1] == '}') return ParseObject(type, json);
            return null;
        }

        private static object ParseAnonymousValue(string json)
        {
            if (json.Length == 0) return null;
            if (json[0] == '{' && json[json.Length - 1] == '}')
            {
                List<string> parts = Split(json);
                if (parts.Count % 2 != 0) return null;
                Dictionary<string, object> dict = new Dictionary<string, object>(parts.Count / 2);
                for (int i = 0; i < parts.Count; i += 2)
                    dict.Add(parts[i].Substring(1, parts[i].Length - 2), ParseAnonymousValue(parts[i + 1]));
                return dict;
            }
            if (json[0] == '[' && json[json.Length - 1] == ']')
            {
                List<string> parts = Split(json);
                List<object> list = new List<object>(parts.Count);
                for (int i = 0; i < parts.Count; i++) list.Add(ParseAnonymousValue(parts[i]));
                return list;
            }
            if (json[0] == '"' && json[json.Length - 1] == '"') return json.Substring(1, json.Length - 2).Replace("\\", string.Empty);
            if (char.IsDigit(json[0]) || json[0] == '-')
            {
                if (json.Contains("."))
                {
                    double d;
                    double.TryParse(json, NumberStyles.Float, CultureInfo.InvariantCulture, out d);
                    return d;
                }
                int n;
                int.TryParse(json, out n);
                return n;
            }
            if (json == "true") return true;
            if (json == "false") return false;
            return null;
        }

        private static Dictionary<string, T> CreateMemberNameDictionary<T>(T[] members) where T : MemberInfo
        {
            Dictionary<string, T> dict = new Dictionary<string, T>(StringComparer.OrdinalIgnoreCase);
            foreach (T member in members)
            {
                if (member.GetCustomAttributes(typeof(IgnoreJsonAttribute), false).Length == 0) dict[member.Name] = member;
            }
            return dict;
        }

        private static object ParseObject(Type type, string json)
        {
            object instance = FormatterServices.GetUninitializedObject(type);
            List<string> parts = Split(json);
            if (parts.Count % 2 != 0) return instance;

            Dictionary<string, FieldInfo> fields;
            if (!fieldInfoCache.TryGetValue(type, out fields))
            {
                fields = CreateMemberNameDictionary(type.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.FlattenHierarchy));
                fieldInfoCache.Add(type, fields);
            }
            Dictionary<string, PropertyInfo> props;
            if (!propertyInfoCache.TryGetValue(type, out props))
            {
                props = CreateMemberNameDictionary(type.GetProperties(BindingFlags.Instance | BindingFlags.Public | BindingFlags.FlattenHierarchy));
                propertyInfoCache.Add(type, props);
            }

            for (int i = 0; i < parts.Count; i += 2)
            {
                if (parts[i].Length <= 2) continue;
                string key = parts[i].Substring(1, parts[i].Length - 2);
                string raw = parts[i + 1];
                FieldInfo field;
                PropertyInfo prop;
                if (fields.TryGetValue(key, out field)) field.SetValue(instance, ParseValue(field.FieldType, raw));
                else if (props.TryGetValue(key, out prop) && prop.CanWrite) prop.SetValue(instance, ParseValue(prop.PropertyType, raw), null);
            }
            return instance;
        }
    }

    public static class JSONWriter
    {
        public static string ToJson(this object item)
        {
            StringBuilder sb = new StringBuilder();
            AppendValue(sb, item);
            return sb.ToString();
        }

        private static void AppendValue(StringBuilder sb, object item)
        {
            if (item == null) { sb.Append("null"); return; }
            Type type = item.GetType();

            if (type == typeof(string))
            {
                sb.Append('"');
                string text = (string)item;
                for (int i = 0; i < text.Length; i++)
                {
                    if (text[i] < ' ' || text[i] == '"' || text[i] == '\\')
                    {
                        sb.Append('\\');
                        int idx = "\"\\\n\r\t\b\f".IndexOf(text[i]);
                        if (idx >= 0) sb.Append("\"\\nrtbf"[idx]);
                        else sb.AppendFormat("u{0:X4}", (uint)text[i]);
                    }
                    else sb.Append(text[i]);
                }
                sb.Append('"');
                return;
            }
            if (type == typeof(byte) || type == typeof(int) || type == typeof(long) || type == typeof(short))
            {
                sb.Append(item.ToString());
                return;
            }
            if (type == typeof(float)) { sb.Append(((float)item).ToString(CultureInfo.InvariantCulture)); return; }
            if (type == typeof(double)) { sb.Append(((double)item).ToString(CultureInfo.InvariantCulture)); return; }
            if (type == typeof(bool)) { sb.Append(((bool)item) ? "true" : "false"); return; }
            if (type.IsEnum) { sb.Append('"').Append(item.ToString()).Append('"'); return; }
            if (item is IList)
            {
                sb.Append('[');
                IList list = (IList)item;
                for (int i = 0; i < list.Count; i++)
                {
                    if (i > 0) sb.Append(',');
                    AppendValue(sb, list[i]);
                }
                sb.Append(']');
                return;
            }
            if (type.IsGenericType && type.GetGenericTypeDefinition() == typeof(Dictionary<,>))
            {
                if (type.GetGenericArguments()[0] != typeof(string)) { sb.Append("{}"); return; }
                sb.Append('{');
                IDictionary dict = (IDictionary)item;
                bool first = true;
                foreach (object key in dict.Keys)
                {
                    if (!first) sb.Append(',');
                    first = false;
                    sb.Append('"').Append((string)key).Append("\":");
                    AppendValue(sb, dict[key]);
                }
                sb.Append('}');
                return;
            }

            sb.Append('{');
            bool firstMember = true;
            FieldInfo[] fields = type.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.FlattenHierarchy);
            foreach (FieldInfo field in fields)
            {
                if (field.GetCustomAttributes(typeof(IgnoreJsonAttribute), false).Length != 0) continue;
                object value = field.GetValue(item);
                if (value == null) continue;
                if (!firstMember) sb.Append(',');
                firstMember = false;
                sb.Append('"').Append(field.Name).Append("\":");
                AppendValue(sb, value);
            }
            PropertyInfo[] props = type.GetProperties(BindingFlags.Instance | BindingFlags.Public | BindingFlags.FlattenHierarchy);
            foreach (PropertyInfo prop in props)
            {
                if (prop.GetCustomAttributes(typeof(IgnoreJsonAttribute), false).Length != 0) continue;
                if (!prop.CanRead) continue;
                object value = prop.GetValue(item, null);
                if (value == null) continue;
                if (!firstMember) sb.Append(',');
                firstMember = false;
                sb.Append('"').Append(prop.Name).Append("\":");
                AppendValue(sb, value);
            }
            sb.Append('}');
        }
    }
}
