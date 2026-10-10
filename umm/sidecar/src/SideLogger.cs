// ============================================================
//  SideLogger.cs — Sidecar 自带的最小日志
//
//  Sidecar 不加载任何 MOD，只做「官方 UMM ↔ 工具」的反射桥，
//  所以它的日志只写自己的 Log.txt（落在 AdofPerfectUmm 目录）；
//  MOD / 官方加载器的日志仍归官方 UnityModManager 的 Log.txt。
// ============================================================
using System;
using System.Collections.Generic;
using System.IO;

namespace AdofPerfectUmm
{
    internal static class SideLogger
    {
        private static readonly string filepath = Path.Combine(
            Path.GetDirectoryName(typeof(SideLogger).Assembly.Location), "BridgeLog.txt");

        private static readonly List<string> buffer = new List<string>(200);
        private static readonly object gate = new object();

        public static string[] History
        {
            get { lock (gate) return buffer.ToArray(); }
        }

        public static void Log(string str)
        {
            string line = DateTime.Now.ToString("HH:mm:ss.fff") + " [Bridge] " + str;
            lock (gate)
            {
                buffer.Add(line);
                while (buffer.Count > 200) buffer.RemoveAt(0);
                try { File.AppendAllText(filepath, line + Environment.NewLine); } catch { }
            }
        }

        public static void LogException(string key, Exception e)
        {
            Log(key + (e != null ? e.ToString() : "null"));
        }
    }
}
