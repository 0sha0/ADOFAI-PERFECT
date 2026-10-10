// ============================================================
// ModManager.cpp — Mod Manager（UMM 规范 MOD 的管理页面）
//
//   逆向/参考：UnityModManager 的 MOD 目录与元数据约定
//     · MOD 目录：<MOD 根>\<文件夹>\Info.json（Id / DisplayName / Version /
//       Author / ManagerVersion / GameVersion / Requirements / HomePage / Repository）
//     · 启用状态：<游戏>\<游戏>_Data\Managed\UnityModManager\Params.xml
//         <Param><ModParams><Mod Id="X" Enabled="true" /></ModParams></Param>
//     · 每个 MOD 自己的设置文件（*.Settings.xml / Settings.xml / Settings.json /
//       Config.json …）由 MOD 自己读写；这里用自绘 IMGUI 编辑器打开它。
//
//   本文件与 4K/5K/6K/10K/16K/CATCH 引擎完全独立，只做 MOD 管理。
// ============================================================
#include "ModManager.h"
#include "GameDir.h"
#include "Lang.h"
#include "Log.h"
#include "UiKit.h"
#include "ModLoader.h"

#include "imgui.h"
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>

#pragma comment(lib, "shell32.lib")

namespace ModManager
{
    using namespace UiKit;

    // ============================================================
    //  文案（运行时注册，5 语言）
    // ============================================================
    enum MS
    {
        MS_TITLE, MS_DESC,
        MS_PATH_CARD, MS_GAMEDIR, MS_MODDIR, MS_MODDIR_HINT,
        MS_BROWSE, MS_APPLY, MS_OPEN, MS_DEFAULT, MS_REFRESH,
        MS_ENABLE, MS_DISABLE, MS_SETTINGS, MS_DELETE,
        MS_STARTUP, MS_ENABLE_ALL, MS_DISABLE_ALL, MS_COUNT,
        MS_EMPTY, MS_NODIR, MS_NOINFO, MS_MISSING,
        MS_DEL_TITLE, MS_DEL_ASK, MS_YES, MS_CANCEL, MS_DELETED, MS_DELFAIL,
        MS_SET_TITLE, MS_SET_FILE, MS_SET_SAVE, MS_SET_SAVED, MS_SET_NONE,
        MS_SET_NOFILE, MS_CLOSE, MS_OPENFOLDER, MS_META,
        MS_STATE_ON, MS_STATE_OFF, MS_STATE_DESC, MS_PROFILE_HINT,
        MS_LOADER, MS_LOADER_ON, MS_LOADER_OFF, MS_LOADER_PART, MS_LOADER_KERNEL,
        MS_LOADER_NOKERNEL, MS_LOADER_INSTALL, MS_LOADER_UNINSTALL, MS_LOADER_DONE,
        MS_LOADER_UNINSTALLED, MS_LOADER_FAIL, MS_OPENGAME,
        MS_INSTALL, MS_INSTALL_OK, MS_INSTALL_ERR,
        MS_LOG_TITLE, MS_LOG_SUM, MS_LOG_NOLOG, MS_LOG_TIME, MS_LOG_LOADED,
        MS_LOG_SKIPPED, MS_LOG_ERROR, MS_LOG_NOTRUN,
        MS_LD_READY, MS_LD_OFFLINE, MS_LD_LOADED, MS_LD_DISABLED, MS_LD_ERROR,
        MS_LD_LOADING, MS_LD_PENDING, MS_LD_UNSEEN, MS_LD_TIP, MS_LD_PASSIVE,
        MS_OPENUI, MS_CLOSEUI, MS_RELOAD, MS_LD_ERRTAG,
        MS_EXPAND,
        MS_SET_APPLIED, MS_SET_APPLYFAIL, MS_APPLYING,
        MS_N
    };

    static const char* T(int i)
    {
        static int t[MS_N];
        static bool init = false;
        if (!init)
        {
            init = true;
            for (int k = 0; k < MS_N; k++) t[k] = -1;
            t[MS_TITLE]     = I18N::Register("MOD 管理器", "MOD 管理器", "Mod Manager", "MOD マネージャー", "Mod Manager");
            t[MS_DESC]      = I18N::Register("管理 UnityModManager 规范的 MOD：启用 / 停用、打开设置、删除。",
                                             "管理 UnityModManager 規範的 MOD：啟用 / 停用、開啟設定、刪除。",
                                             "Manage UMM-spec mods: enable / disable, open settings, delete.",
                                             "UMM 規格 MOD の管理：有効/無効・設定・削除。",
                                             "Управление модами UMM: включить/выключить, настройки, удалить.");
            t[MS_PATH_CARD] = I18N::Register("目录", "目錄", "Folders", "フォルダ", "Папки");
            t[MS_GAMEDIR]   = I18N::Register("游戏目录", "遊戲目錄", "Game folder", "ゲームフォルダ", "Папка игры");
            t[MS_MODDIR]    = I18N::Register("MOD 目录", "MOD 目錄", "Mods folder", "MOD フォルダ", "Папка модов");
            t[MS_MODDIR_HINT] = I18N::Register("默认 <游戏目录>\\Mods；也可指向任意存放 MOD 文件夹的目录。",
                                               "預設 <遊戲目錄>\\Mods；也可指向任意存放 MOD 資料夾的目錄。",
                                               "Defaults to <game>\\Mods; can point at any folder holding mod folders.",
                                               "既定は <ゲーム>\\Mods。MOD フォルダを置いた任意の場所も指定できます。",
                                               "По умолчанию <игра>\\Mods; можно указать любую папку с модами.");
            t[MS_BROWSE]    = I18N::Register("浏览", "瀏覽", "Browse", "参照", "Обзор");
            t[MS_APPLY]     = I18N::Register("应用", "套用", "Apply", "適用", "Применить");
            t[MS_OPEN]      = I18N::Register("打开", "開啟", "Open", "開く", "Открыть");
            t[MS_DEFAULT]   = I18N::Register("恢复默认", "恢復預設", "Default", "既定に戻す", "По умолчанию");
            t[MS_REFRESH]   = I18N::Register("刷新", "重新整理", "Refresh", "更新", "Обновить");
            t[MS_ENABLE]    = I18N::Register("开启", "開啟", "Enable", "有効化", "Включить");
            t[MS_DISABLE]   = I18N::Register("关闭", "關閉", "Disable", "無効化", "Выключить");
            t[MS_SETTINGS]  = I18N::Register("设置", "設定", "Settings", "設定", "Настройки");
            t[MS_DELETE]    = I18N::Register("删除", "刪除", "Delete", "削除", "Удалить");
            t[MS_STARTUP]   = I18N::Register("写入启动配置", "寫入啟動設定",
                                             "Write to game config", "ゲーム設定へ書き込む",
                                             "Записать в конфиг игры");
            t[MS_ENABLE_ALL]= I18N::Register("全部开启", "全部開啟", "Enable all", "すべて有効", "Включить все");
            t[MS_DISABLE_ALL]= I18N::Register("全部关闭", "全部關閉", "Disable all", "すべて無効", "Выключить все");
            t[MS_COUNT]     = I18N::Register("已安装 %d 个 MOD（%d 个已开启）", "已安裝 %d 個 MOD（%d 個已開啟）",
                                             "%d mods installed (%d enabled)", "%d 個の MOD（%d 個が有効）",
                                             "Модов: %d (включено %d)");
            t[MS_EMPTY]     = I18N::Register("这个目录里没有找到 MOD（需要 <名称>\\Info.json）。",
                                             "這個目錄裡沒有找到 MOD（需要 <名稱>\\Info.json）。",
                                             "No mods found here (expects <name>\\Info.json).",
                                             "MOD が見つかりません（<名前>\\Info.json が必要）。",
                                             "Моды не найдены (нужен <имя>\\Info.json).");
            t[MS_NODIR]     = I18N::Register("目录不存在。", "目錄不存在。", "Folder does not exist.", "フォルダがありません。", "Папки нет.");
            t[MS_NOINFO]    = I18N::Register("该目录没有 Info.json（不是 UMM 规范 MOD）。",
                                             "該目錄沒有 Info.json（不是 UMM 規範 MOD）。",
                                             "No Info.json in this folder (not a UMM mod).",
                                             "Info.json がありません（UMM 規格ではありません）。",
                                             "Нет Info.json (не UMM-мод).");
            t[MS_MISSING]   = I18N::Register("未安装 UMM 加载器：开关已记入本工具配置，装好加载器后点下面按钮即可写入游戏。",
                                             "未安裝 UMM 載入器：開關已記入本工具設定，裝好載入器後點下方按鈕即可寫入遊戲。",
                                             "UMM loader not installed: switches are stored in this tool's profile; use the button below to push them once the loader exists.",
                                             "UMM ローダー未検出：状態は本ツールに保存済み。ローダー導入後に下のボタンで書き込みます。",
                                             "UMM не установлен: состояния сохранены в профиле; нажмите кнопку ниже после установки.");
            t[MS_DEL_TITLE] = I18N::Register("删除 MOD", "刪除 MOD", "Delete mod", "MOD を削除", "Удалить мод");
            t[MS_DEL_ASK]   = I18N::Register("确定要删除 %s 吗？此操作会永久删除该 MOD 文件夹，无法撤销。",
                                             "確定要刪除 %s 嗎？此操作會永久刪除該 MOD 資料夾，無法復原。",
                                             "Delete %s? This permanently removes the mod folder and cannot be undone.",
                                             "%s を削除しますか？フォルダごと完全に削除され、元に戻せません。",
                                             "Удалить %s? Папка мода будет удалена безвозвратно.");
            t[MS_YES]       = I18N::Register("删除", "刪除", "Delete", "削除する", "Удалить");
            t[MS_CANCEL]    = I18N::Register("取消", "取消", "Cancel", "キャンセル", "Отмена");
            t[MS_DELETED]   = I18N::Register("已删除 %s。", "已刪除 %s。", "Deleted %s.", "%s を削除しました。", "%s удалён.");
            t[MS_DELFAIL]   = I18N::Register("删除失败：%s", "刪除失敗：%s", "Delete failed: %s", "削除に失敗：%s", "Ошибка удаления: %s");
            t[MS_SET_TITLE] = I18N::Register("%s - 设置", "%s - 設定", "%s - Settings", "%s - 設定", "%s - Настройки");
            t[MS_SET_FILE]  = I18N::Register("设置文件", "設定檔", "Settings file", "設定ファイル", "Файл настроек");
            t[MS_SET_SAVE]  = I18N::Register("保存设置", "儲存設定", "Save settings", "設定を保存", "Сохранить");
            t[MS_SET_SAVED] = I18N::Register("已保存到 %s", "已儲存到 %s", "Saved to %s", "%s に保存しました", "Сохранено в %s");
            t[MS_SET_NONE]  = I18N::Register("这个 MOD 没有可编辑的设置文件。", "這個 MOD 沒有可編輯的設定檔。",
                                             "This mod has no editable settings file.", "編集できる設定ファイルがありません。",
                                             "У этого мода нет файла настроек.");
            t[MS_SET_NOFILE]= I18N::Register("文件不存在或无法读取。", "檔案不存在或無法讀取。", "File missing or unreadable.", "ファイルがありません。", "Файл недоступен.");
            t[MS_CLOSE]     = I18N::Register("关闭", "關閉", "Close", "閉じる", "Закрыть");
            t[MS_OPENFOLDER]= I18N::Register("打开 MOD 文件夹", "開啟 MOD 資料夾", "Open mod folder", "MOD フォルダを開く", "Открыть папку мода");
            t[MS_META]      = I18N::Register("信息", "資訊", "Info", "情報", "Инфо");
            t[MS_STATE_ON]  = I18N::Register("已开启", "已開啟", "Enabled", "有効", "Включён");
            t[MS_STATE_OFF] = I18N::Register("已关闭", "已關閉", "Disabled", "無効", "Выключен");
            t[MS_PROFILE_HINT] = I18N::Register(
                "MOD 开关状态会随「设置 → 配置档案」一起保存，读档时同步进 Params.xml。",
                "MOD 開關狀態會隨「設定 → 設定檔」一起保存，讀檔時同步進 Params.xml。",
                "Mod switches live in the Settings profile and are pushed to Params.xml when loaded.",
                "MOD のスイッチ状態は「設定 → プロファイル」と一緒に保存され、読込時に Params.xml へ同期します。",
                "Состояния модов хранятся в профиле и применяются в Params.xml при загрузке.");
            t[MS_LOADER]        = I18N::Register("UMM 加载器", "UMM 載入器", "UMM loader", "UMM ローダー", "Загрузчик UMM");
            t[MS_LOADER_ON]     = I18N::Register("已安装：游戏启动时会加载 MOD。",
                                                 "已安裝：遊戲啟動時會載入 MOD。",
                                                 "Installed: the game loads your mods on startup.",
                                                 "インストール済み：起動時に MOD が読み込まれます。",
                                                 "Установлен: моды загружаются при запуске игры.");
            t[MS_LOADER_OFF]    = I18N::Register("未安装：MOD 不会在游戏里生效。",
                                                 "未安裝：MOD 不會在遊戲裡生效。",
                                                 "Not installed: mods will not run in game.",
                                                 "未インストール：MOD はゲーム内で有効になりません。",
                                                 "Не установлен: моды не будут работать в игре.");
            t[MS_LOADER_PART]   = I18N::Register("安装不完整：点「安装 / 修复加载器」补全。",
                                                 "安裝不完整：點「安裝 / 修復載入器」補全。",
                                                 "Incomplete install: use Install / Repair.",
                                                 "不完全：インストール / 修復を実行してください。",
                                                 "Неполная установка: нажмите «Установить / Восстановить».");
            t[MS_LOADER_KERNEL] = I18N::Register("内核目录", "核心目錄", "Kernel folder", "カーネル", "Каталог ядра");
            t[MS_LOADER_NOKERNEL] = I18N::Register(
                "缺少内核文件：把 UnityModManager.dll / 0Harmony.dll / winhttp_x64.dll 放到 <本 DLL 目录>\\umm\\。",
                "缺少核心檔案：把 UnityModManager.dll / 0Harmony.dll / winhttp_x64.dll 放到 <本 DLL 目錄>\\umm\\。",
                "Kernel files missing: put UnityModManager.dll / 0Harmony.dll / winhttp_x64.dll into <DLL dir>\\umm\\.",
                "カーネルがありません：UnityModManager.dll / 0Harmony.dll / winhttp_x64.dll を <DLL フォルダ>\\umm\\ に置いてください。",
                "Нет файлов ядра: положите UnityModManager.dll / 0Harmony.dll / winhttp_x64.dll в <папка DLL>\\umm\\.");
            t[MS_LOADER_INSTALL] = I18N::Register("安装 / 修复加载器", "安裝 / 修復載入器", "Install / Repair", "インストール / 修復", "Установить / Восстановить");
            t[MS_LOADER_UNINSTALL] = I18N::Register("卸载加载器", "卸載載入器", "Uninstall loader", "アンインストール", "Удалить загрузчик");
            t[MS_LOADER_DONE] = I18N::Register("加载器已就绪：重启游戏后 MOD 才会加载。",
                                               "載入器已就緒：重啟遊戲後 MOD 才會載入。",
                                               "Loader ready: restart the game to load mods.",
                                               "準備完了：ゲームを再起動すると MOD が読み込まれます。",
                                               "Готово: перезапустите игру, чтобы загрузить моды.");
            t[MS_LOADER_UNINSTALLED] = I18N::Register("加载器已卸载。", "載入器已卸載。", "Loader uninstalled.", "アンインストールしました。", "Загрузчик удалён.");
            t[MS_LOADER_FAIL] = I18N::Register("操作失败：请确认游戏目录正确、且当前有写入权限。",
                                               "操作失敗：請確認遊戲目錄正確、且目前有寫入權限。",
                                               "Failed: check the game folder and write permission.",
                                               "失敗：ゲームフォルダと書き込み権限を確認してください。",
                                               "Ошибка: проверьте папку игры и права на запись.");
            t[MS_OPENGAME]      = I18N::Register("打开游戏目录", "開啟遊戲目錄", "Open game folder", "ゲームフォルダを開く", "Открыть папку игры");
            t[MS_INSTALL]       = I18N::Register("安装", "安裝", "Install", "インストール", "Установить");
            t[MS_INSTALL_OK]    = I18N::Register("已把 %s 安装到游戏：重启游戏后生效。",
                                                 "已把 %s 安裝到遊戲：重啟遊戲後生效。",
                                                 "Installed %s into the game; restart to apply.",
                                                 "%s をゲームにインストールしました。再起動で反映されます。",
                                                 "%s установлен в игру; перезапустите для применения.");
            t[MS_INSTALL_ERR]   = I18N::Register("安装 %s 失败。", "安裝 %s 失敗。", "Failed to install %s.", "%s のインストールに失敗。", "Не удалось установить %s.");
            t[MS_LOG_TITLE] = I18N::Register("上次运行结果（读游戏日志）", "上次執行結果（讀遊戲日誌）",
                                             "Last run (from game log)", "前回の実行結果（ログ）",
                                             "Прошлый запуск (лог игры)");
            t[MS_LOG_SUM]   = I18N::Register("上次游戏运行：%d / %d 个 MOD 加载成功", "上次遊戲執行：%d / %d 個 MOD 載入成功",
                                             "Last game run: %d / %d mods loaded", "前回の起動：%d / %d MOD 読み込み成功",
                                             "Прошлый запуск: загружено %d / %d модов");
            t[MS_LOG_NOLOG] = I18N::Register("还没跑过游戏（找不到 Log.txt）。", "還沒跑過遊戲（找不到 Log.txt）。",
                                             "No game run recorded yet (Log.txt missing).", "まだ起動していません（Log.txt なし）。",
                                             "Игра ещё не запускалась (нет Log.txt).");
            t[MS_LOG_TIME]  = I18N::Register("记录时间：%s", "記錄時間：%s", "Recorded: %s", "記録時刻：%s", "Записано: %s");
            t[MS_LOG_LOADED]  = I18N::Register("上次运行：已加载", "上次執行：已載入", "Last run: loaded", "前回：読み込み成功", "Прошлый запуск: загружен");
            t[MS_LOG_SKIPPED] = I18N::Register("上次运行：已跳过（停用）", "上次執行：已跳過（停用）", "Last run: skipped (disabled)", "前回：スキップ（無効）", "Прошлый запуск: пропущен");
            t[MS_LOG_ERROR]   = I18N::Register("上次运行：报错", "上次執行：報錯", "Last run: error", "前回：エラー", "Прошлый запуск: ошибка");
            t[MS_LOG_NOTRUN]  = I18N::Register("上次运行：未加载", "上次執行：未載入", "Last run: not loaded", "前回：読み込まれず", "Прошлый запуск: не загружен");
            t[MS_STATE_DESC]= I18N::Register("卡片最前方的圆点：绿色=启用，红色=停用；点「开启/关闭」立即切换并写入 Params.xml。",
                                             "卡片最前方的圓點：綠色=啟用，紅色=停用；點「開啟/關閉」立即切換並寫入 Params.xml。",
                                             "Leading dot: green = enabled, red = disabled. The toggle writes Params.xml immediately.",
                                             "先頭の点：緑=有効、赤=無効。ボタンで即時切替し Params.xml に書き込みます。",
                                             "Точка: зелёная = включён, красная = выключен. Переключатель пишет Params.xml.");

            t[MS_LD_READY]   = I18N::Register("加载器已就绪（游戏内实时状态）", "載入器已就緒（遊戲內即時狀態）",
                                              "Loader ready (live in-game state)", "ローダー準備完了（ゲーム内の状態）",
                                              "Загрузчик готов (состояние в игре)");
            t[MS_LD_PASSIVE] = I18N::Register("检测到原版 UnityModManager：已切换为复用模式（本工具不重复加载 MOD，开关与设置直接作用于原版加载的 MOD）",
                                              "檢測到原版 UnityModManager：已切換為復用模式（本工具不重複載入 MOD，開關與設置直接作用於原版載入的 MOD）",
                                              "Native UnityModManager detected: reuse mode (this tool no longer loads mods twice; toggles and settings act on the mods it loaded)",
                                              "ネイティブ UnityModManager を検出：再利用モード（本ツールは MOD を二重に読み込まず、切り替えや設定は元のローダーが読込んだ MOD に反映されます）",
                                              "Обнаружен оригинальный UnityModManager: режим повторного использования (моды не загружаются дважды; переключения и настройки применяются к модам оригинального загрузчика)");
            t[MS_LD_OFFLINE] = I18N::Register("加载器未运行：先启动游戏，MOD 才会真正被加载。",
                                              "載入器未執行：先啟動遊戲，MOD 才會真正被載入。",
                                              "Loader offline: launch the game so mods actually load.",
                                              "ローダー未起動：ゲームを起動すると MOD が読み込まれます。",
                                              "Загрузчик не запущен: запустите игру, чтобы моды загрузились.");
            t[MS_LD_LOADED]  = I18N::Register("已加载", "已載入", "Loaded", "読み込み済み", "Загружен");
            t[MS_LD_DISABLED]= I18N::Register("已禁用", "已停用", "Disabled", "無効", "Отключён");
            t[MS_LD_ERROR]   = I18N::Register("加载失败", "載入失敗", "Load error", "読み込み失敗", "Ошибка загрузки");
            t[MS_LD_LOADING] = I18N::Register("加载中…", "載入中…", "Loading...", "読み込み中…", "Загрузка...");
            t[MS_LD_PENDING] = I18N::Register("未加载", "未載入", "Not loaded", "未読み込み", "Не загружен");
            t[MS_LD_UNSEEN]  = I18N::Register("加载器未识别到该 MOD", "載入器未識別到該 MOD",
                                              "Loader did not see this mod", "ローダーが認識していません",
                                              "Загрузчик не видит этот мод");
            t[MS_LD_TIP]     = I18N::Register("卡片状态来自游戏内加载器的实时回报：绿=已加载，黄=未加载/加载中，红=失败，灰=已禁用。",
                                              "卡片狀態來自遊戲內載入器的即時回報：綠=已載入，黃=未載入/載入中，紅=失敗，灰=已停用。",
                                              "Card state is reported live by the in-game loader: green = loaded, amber = pending, red = failed, grey = disabled.",
                                              "カードの状態はゲーム内ローダーの報告：緑=読込済、黄=待機、赤=失敗、灰=無効。",
                                              "Состояние от загрузчика: зелёный = загружен, жёлтый = ожидание, красный = ошибка, серый = отключён.");
            t[MS_OPENUI]     = I18N::Register("打开界面", "開啟介面", "Open UI", "UI を開く", "Открыть UI");
            t[MS_CLOSEUI]    = I18N::Register("关闭界面", "關閉介面", "Close UI", "UI を閉じる", "Закрыть UI");
            t[MS_RELOAD]     = I18N::Register("重载", "重載", "Reload", "再読み込み", "Перезагрузить");
            t[MS_LD_ERRTAG]  = I18N::Register("（该 MOD 自带设置界面，用「打开界面」在游戏内修改）",
                                              "（該 MOD 自帶設定介面，用「開啟介面」在遊戲內修改）",
                                              "(This mod ships its own UI; use Open UI to edit it in-game.)",
                                              "（この MOD は独自 UI を持ちます。「UI を開く」で編集）",
                                              "(У мода свой интерфейс; используйте «Открыть UI».)");
            t[MS_EXPAND]     = I18N::Register("展开设置", "展開設定", "Expand settings", "設定を展開", "Развернуть настройки");
            t[MS_SET_APPLIED] = I18N::Register("已保存并应用到运行中的 MOD", "已儲存並套用到執行中的 MOD",
                                               "Saved and applied to the running mod", "保存して実行中の MOD に適用しました",
                                               "Сохранено и применено к моду");
            t[MS_SET_APPLYFAIL] = I18N::Register("已写入文件，但没能应用到 MOD（MOD 会在下次启动时读取）",
                                                 "已寫入檔案，但沒能套用到 MOD（MOD 將於下次啟動時讀取）",
                                                 "Written to file, but not applied to the mod (it will read it on next launch)",
                                                 "ファイルには保存しましたが MOD に適用できませんでした（次回起動時に反映）",
                                                 "Записано в файл, но не применено к моду (применится при следующем запуске)");
            t[MS_APPLYING]   = I18N::Register("正在应用到 MOD……", "正在套用到 MOD……", "Applying to the mod...",
                                              "MOD に適用中...", "Применение к моду...");
        }
        if (i < 0 || i >= MS_N) return "";
        return I18N::Tr(t[i]);
    }

    // ============================================================
    //  小工具
    // ============================================================
    namespace
    {
        bool FileExists(const std::string& p)
        {
            DWORD a = GetFileAttributesA(p.c_str());
            return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
        }
        bool DirExists(const std::string& p)
        {
            DWORD a = GetFileAttributesA(p.c_str());
            return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
        }
        bool PathEquals(const std::string& a, const std::string& b)
        {
            return _stricmp(a.c_str(), b.c_str()) == 0;
        }
        std::string JoinPath(const std::string& a, const std::string& b)
        {
            if (a.empty()) return b;
            std::string r = a;
            if (r.back() != '\\' && r.back() != '/') r += '\\';
            return r + b;
        }
        std::string ToLower(std::string s)
        {
            for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            return s;
        }
        bool Contains(const std::string& s, const char* sub)
        {
            return s.find(sub) != std::string::npos;
        }
        std::string ReadAll(const std::string& path, bool* ok = nullptr)
        {
            std::string out;
            FILE* f = nullptr;
            if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) { if (ok) *ok = false; return out; }
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0)
            {
                out.resize((size_t)n);
                size_t got = fread(&out[0], 1, (size_t)n, f);
                out.resize(got);
            }
            fclose(f);
            if (ok) *ok = true;
            return out;
        }
        bool WriteAll(const std::string& path, const std::string& data)
        {
            FILE* f = nullptr;
            if (fopen_s(&f, path.c_str(), "wb") != 0 || !f) return false;
            size_t w = data.empty() ? 0 : fwrite(data.data(), 1, data.size(), f);
            fclose(f);
            return w == data.size();
        }
        bool RemoveDirRecursive(const std::string& dir)
        {
            std::string pat = dir + "\\*";
            WIN32_FIND_DATAA fd{};
            HANDLE h = FindFirstFileA(pat.c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE)
            {
                do
                {
                    std::string nm = fd.cFileName;
                    if (nm == "." || nm == "..") continue;
                    std::string full = dir + "\\" + nm;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                    {
                        if (!RemoveDirRecursive(full)) { FindClose(h); return false; }
                    }
                    else
                    {
                        SetFileAttributesA(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                        if (!DeleteFileA(full.c_str())) { FindClose(h); return false; }
                    }
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
            return RemoveDirectoryA(dir.c_str()) != 0;
        }
        std::vector<std::string> ListDirs(const std::string& root)
        {
            std::vector<std::string> out;
            std::string pat = root + "\\*";
            WIN32_FIND_DATAA fd{};
            HANDLE h = FindFirstFileA(pat.c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return out;
            do
            {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                std::string nm = fd.cFileName;
                if (nm == "." || nm == "..") continue;
                out.push_back(nm);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
            std::sort(out.begin(), out.end());
            return out;
        }
        std::vector<std::string> ListFiles(const std::string& root, const char* ext)
        {
            std::vector<std::string> out;
            std::string pat = root + "\\*";
            WIN32_FIND_DATAA fd{};
            HANDLE h = FindFirstFileA(pat.c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return out;
            do
            {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                std::string nm = fd.cFileName;
                std::string low = ToLower(nm);
                size_t dot = low.find_last_of('.');
                if (dot == std::string::npos) continue;
                if (low.substr(dot) != ext) continue;
                out.push_back(nm);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
            std::sort(out.begin(), out.end());
            return out;
        }
    } // namespace
    // ============================================================
    //  Info.json / 设置文件的小型解析（不引入第三方 JSON 库）
    // ============================================================
    namespace
    {
        // --- JSON：取顶层字符串字段（如 "Id": "xxx"） ---
        std::string JsonStrField(const std::string& js, const char* key)
        {
            std::string pat = std::string("\"") + key + "\"";
            size_t p = 0;
            while ((p = js.find(pat, p)) != std::string::npos)
            {
                size_t q = p + pat.size();
                while (q < js.size() && (js[q] == ' ' || js[q] == '\t' || js[q] == '\r' || js[q] == '\n')) q++;
                if (q >= js.size() || js[q] != ':') { p = q; continue; }
                q++;
                while (q < js.size() && (js[q] == ' ' || js[q] == '\t' || js[q] == '\r' || js[q] == '\n')) q++;
                if (q < js.size() && js[q] == '"')
                {
                    q++;
                    std::string out;
                    while (q < js.size() && js[q] != '"')
                    {
                        if (js[q] == '\\' && q + 1 < js.size())
                        {
                            char e = js[q + 1];
                            if (e == 'n') out += '\n';
                            else if (e == 't') out += '\t';
                            else out += e;
                            q += 2;
                        }
                        else out += js[q++];
                    }
                    return out;
                }
                p = q;
            }
            return std::string();
        }

        // --- JSON：取顶层布尔字段（如 "enabled": true） ---
        bool JsonBoolField(const std::string& js, const char* key)
        {
            std::string pat = std::string("\"") + key + "\"";
            size_t p = 0;
            while ((p = js.find(pat, p)) != std::string::npos)
            {
                size_t q = p + pat.size();
                while (q < js.size() && (js[q] == ' ' || js[q] == '\t' || js[q] == '\r' || js[q] == '\n')) q++;
                if (q >= js.size() || js[q] != ':') { p = q; continue; }
                q++;
                while (q < js.size() && (js[q] == ' ' || js[q] == '\t' || js[q] == '\r' || js[q] == '\n')) q++;
                return js.compare(q, 4, "true") == 0;
            }
            return false;
        }

        // --- 游戏内加载器回报的状态数组（数组里每个对象一项） ---
        struct LdState
        {
            std::string id, name, version, author, assembly;
            bool enabled = false, active = false, loaded = false;
            bool error = false, gui = false, open = false;
        };

        // 逐个对象切开 "[{...},{...}]"（跳过字符串内的花括号）
        void ParseLdStates(const std::string& js, std::vector<LdState>& out)
        {
            out.clear();
            bool inStr = false;
            int  depth = 0;
            size_t start = 0;
            for (size_t i = 0; i < js.size(); i++)
            {
                char c = js[i];
                if (inStr)
                {
                    if (c == '\\') { i++; continue; }
                    if (c == '"') inStr = false;
                    continue;
                }
                if (c == '"') { inStr = true; continue; }
                if (c == '{')
                {
                    if (depth == 0) start = i;
                    depth++;
                }
                else if (c == '}')
                {
                    if (--depth == 0)
                    {
                        std::string obj = js.substr(start, i - start + 1);
                        LdState s;
                        s.id       = JsonStrField(obj, "id");
                        if (s.id.empty()) continue;
                        s.name     = JsonStrField(obj, "name");
                        s.version  = JsonStrField(obj, "version");
                        s.author   = JsonStrField(obj, "author");
                        s.assembly = JsonStrField(obj, "assembly");
                        s.enabled  = JsonBoolField(obj, "enabled");
                        s.active   = JsonBoolField(obj, "active");
                        s.loaded   = JsonBoolField(obj, "loaded");
                        s.error    = JsonBoolField(obj, "error");
                        s.gui      = JsonBoolField(obj, "gui");
                        s.open     = JsonBoolField(obj, "open");
                        out.push_back(s);
                    }
                    if (depth < 0) depth = 0;
                }
            }
        }

        // --- JSON：取顶层字符串数组（如 "Requirements": ["A","B"]） ---
        std::vector<std::string> JsonStrArray(const std::string& js, const char* key)
        {
            std::vector<std::string> out;
            std::string pat = std::string("\"") + key + "\"";
            size_t p = js.find(pat);
            if (p == std::string::npos) return out;
            size_t q = js.find('[', p);
            if (q == std::string::npos) return out;
            size_t e = js.find(']', q);
            if (e == std::string::npos) return out;
            std::string body = js.substr(q + 1, e - q - 1);
            size_t i = 0;
            while (i < body.size())
            {
                size_t a = body.find('"', i);
                if (a == std::string::npos) break;
                size_t b = body.find('"', a + 1);
                if (b == std::string::npos) break;
                out.push_back(body.substr(a + 1, b - a - 1));
                i = b + 1;
            }
            return out;
        }

        // 设置项：type 0=bool 1=number 2=string
        struct SetItem
        {
            std::string key;
            std::string val;   // 纯文本（不含引号）
            int         type = 2;
            char        edit[512] = { 0 };   // 编辑框缓冲
        };
        struct SetFile
        {
            std::string path;
            std::string name;
            bool        xml = true;
            std::vector<SetItem> items;
        };

        // --- XML：解析根节点下的直接子元素（只取标量，跳过嵌套结构） ---
        void ParseXmlScalars(const std::string& text, std::vector<SetItem>& out)
        {
            out.clear();
            size_t rootEnd = 0;
            bool inRootTag = false;
            // 跳过 <?xml ... ?> 与注释，找到根节点开始标签的结束
            for (size_t i = 0; i < text.size(); i++)
            {
                if (text[i] == '<')
                {
                    if (text.compare(i, 4, "<!--") == 0)
                    {
                        size_t e = text.find("-->", i);
                        i = (e == std::string::npos) ? text.size() : e + 2;
                        continue;
                    }
                    if (text.compare(i, 2, "<?") == 0)
                    {
                        size_t e = text.find("?>", i);
                        i = (e == std::string::npos) ? text.size() : e + 1;
                        continue;
                    }
                    size_t e = text.find('>', i);
                    if (e == std::string::npos) return;
                    if (!inRootTag && text[i + 1] != '/')
                    {
                        inRootTag = true;
                        rootEnd = e + 1;
                        // 自闭合的根节点：没有子元素
                        if (text[e - 1] == '/') return;
                    }
                    break;
                }
            }
            if (!inRootTag) return;

            size_t i = rootEnd;
            while (i < text.size())
            {
                size_t lt = text.find('<', i);
                if (lt == std::string::npos) break;
                if (text.compare(lt, 4, "<!--") == 0)
                {
                    size_t e = text.find("-->", lt);
                    i = (e == std::string::npos) ? text.size() : e + 3;
                    continue;
                }
                if (text[lt + 1] == '/') break;         // 根节点结束
                size_t gt = text.find('>', lt);
                if (gt == std::string::npos) break;
                if (text.compare(lt, 2, "<?") == 0) { i = gt + 1; continue; }
                std::string tag = text.substr(lt + 1, gt - lt - 1);
                if (tag.empty() || tag.back() == '/') { i = gt + 1; continue; }
                if (tag.find(' ') != std::string::npos) { i = gt + 1; continue; } // 带属性的元素：跳过
                std::string closeTag = "</" + tag + ">";
                size_t close = text.find(closeTag, gt + 1);
                if (close == std::string::npos) break;
                std::string content = text.substr(gt + 1, close - gt - 1);
                i = close + closeTag.size();
                if (content.find('<') != std::string::npos) continue;   // 嵌套结构：不当作标量
                // 去空白
                size_t a = content.find_first_not_of(" \t\r\n");
                size_t b = content.find_last_not_of(" \t\r\n");
                SetItem it;
                it.key = tag;
                it.val = (a == std::string::npos) ? std::string() : content.substr(a, b - a + 1);
                std::string low = ToLower(it.val);
                if (low == "true" || low == "false") it.type = 0;
                else
                {
                    char* endp = nullptr;
                    strtod(it.val.c_str(), &endp);
                    it.type = (endp && *endp == '\0' && !it.val.empty()) ? 1 : 2;
                }
                out.push_back(it);
            }
        }

        // --- JSON：解析顶层标量 ---
        void ParseJsonScalars(const std::string& text, std::vector<SetItem>& out)
        {
            out.clear();
            int depth = 0;
            bool inStr = false;
            size_t i = 0;
            const size_t n = text.size();
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
                    // 读取 key
                    size_t a = i + 1, b = a;
                    while (b < n)
                    {
                        if (text[b] == '\\') { b += 2; continue; }
                        if (text[b] == '"') break;
                        b++;
                    }
                    if (b >= n) break;
                    std::string key = text.substr(a, b - a);
                    size_t c = b + 1;
                    while (c < n && (text[c] == ' ' || text[c] == '\t' || text[c] == '\r' || text[c] == '\n')) c++;
                    if (c >= n || text[c] != ':') { i = b + 1; continue; }
                    c++;
                    while (c < n && (text[c] == ' ' || text[c] == '\t' || text[c] == '\r' || text[c] == '\n')) c++;
                    if (c >= n) break;
                    SetItem it;
                    it.key = key;
                    if (text[c] == '"')
                    {
                        size_t d = c + 1, e = d;
                        while (e < n)
                        {
                            if (text[e] == '\\') { e += 2; continue; }
                            if (text[e] == '"') break;
                            e++;
                        }
                        it.val = text.substr(d, e - d);
                        it.type = 2;
                        out.push_back(it);
                        i = e + 1;
                        continue;
                    }
                    if (text[c] == '{' || text[c] == '[')
                    {
                        // 嵌套结构：整段跳过（括号配对，考虑字符串）
                        int d2 = 0;
                        bool s2 = false;
                        size_t e = c;
                        for (; e < n; e++)
                        {
                            char x = text[e];
                            if (s2) { if (x == '\\') { e++; continue; } if (x == '"') s2 = false; continue; }
                            if (x == '"') { s2 = true; continue; }
                            if (x == '{' || x == '[') d2++;
                            else if (x == '}' || x == ']') { d2--; if (d2 == 0) break; }
                        }
                        i = e + 1;
                        continue;
                    }
                    // 裸标量：number / true / false / null
                    size_t e = c;
                    while (e < n && text[e] != ',' && text[e] != '}' && text[e] != '\r' && text[e] != '\n') e++;
                    std::string raw = text.substr(c, e - c);
                    while (!raw.empty() && (raw.back() == ' ' || raw.back() == '\t')) raw.pop_back();
                    it.val = raw;
                    std::string low = ToLower(raw);
                    it.type = (low == "true" || low == "false") ? 0 : 1;
                    if (low == "null") it.type = 2;
                    out.push_back(it);
                    i = e;
                    continue;
                }
                if (ch == '{' || ch == '[') depth++;
                else if (ch == '}' || ch == ']') depth--;
                i++;
            }
        }

        // 把值写回文件（就地替换，保留其余内容/注释）
        bool PatchXml(const std::string& path, const SetItem& it)
        {
            bool ok = false;
            std::string text = ReadAll(path, &ok);
            if (!ok) return false;
            std::string open = "<" + it.key + ">";
            size_t p = text.find(open);
            if (p == std::string::npos) return false;
            size_t cs = p + open.size();
            std::string close = "</" + it.key + ">";
            size_t ce = text.find(close, cs);
            if (ce == std::string::npos) return false;
            text = text.substr(0, cs) + it.val + text.substr(ce);
            return WriteAll(path, text);
        }
        bool PatchJson(const std::string& path, const SetItem& it)
        {
            bool ok = false;
            std::string text = ReadAll(path, &ok);
            if (!ok) return false;
            std::string pat = "\"" + it.key + "\"";
            size_t p = text.find(pat);
            if (p == std::string::npos) return false;
            size_t c = text.find(':', p + pat.size());
            if (c == std::string::npos) return false;
            c++;
            while (c < text.size() && (text[c] == ' ' || text[c] == '\t')) c++;
            if (c >= text.size()) return false;
            std::string newVal;
            if (it.type == 2) newVal = "\"" + it.val + "\"";
            else              newVal = it.val;
            size_t e = c;
            if (text[c] == '"')
            {
                e = c + 1;
                while (e < text.size())
                {
                    if (text[e] == '\\') { e += 2; continue; }
                    if (text[e] == '"') break;
                    e++;
                }
                e++;
            }
            else
            {
                while (e < text.size() && text[e] != ',' && text[e] != '}' && text[e] != '\r' && text[e] != '\n') e++;
                size_t t = e;
                while (t > c && (text[t - 1] == ' ' || text[t - 1] == '\t')) t--;
                e = t;
            }
            text = text.substr(0, c) + newVal + text.substr(e);
            return WriteAll(path, text);
        }
    } // namespace

    // ============================================================
    //  路径解析
    // ============================================================
    namespace
    {
        std::string FindDataDir(const std::string& gameDir)
        {
            std::string pat = gameDir + "\\*_Data";
            WIN32_FIND_DATAA fd{};
            HANDLE h = FindFirstFileA(pat.c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return std::string();
            std::string found = fd.cFileName;
            FindClose(h);
            return found;
        }
        std::string ParamsXmlPath()
        {
            // Params.xml 属于「自研加载器」自己那份（与它的 DLL 同目录）
            std::string d = ModLoader::LoaderDirPath();
            return d.empty() ? std::string() : JoinPath(d, "Params.xml");
        }
        // 原版 UMM 的 Params.xml（Managed\UnityModManager\Params.xml，若它也装着）
        std::string ForeignParamsXmlPath()
        {
            // 我们加载器在 <Managed>\AdofPerfectUmm，原版在同级 <Managed>\UnityModManager
            std::string ld = ModLoader::LoaderDirPath();
            if (ld.empty()) return std::string();
            size_t sep = ld.find_last_of("\\/");
            if (sep == std::string::npos) return std::string();
            std::string managed = ld.substr(0, sep);
            std::string d = JoinPath(managed, "UnityModManager");
            return DirExists(d) ? JoinPath(d, "Params.xml") : std::string();
        }
        std::string DefaultModsDir()
        {
            std::string g = GameDir::Get();
            if (DirExists(JoinPath(g, "Mods"))) return JoinPath(g, "Mods");
            if (DirExists(JoinPath(g, "mods"))) return JoinPath(g, "mods");
            return JoinPath(g, "Mods");
        }
        std::string ResolveModsDir()
        {
            char b[MAX_PATH * 2] = { 0 };
            I18N::Prefs::GetStr("mod.dir", b, sizeof(b), "");
            if (b[0] && DirExists(b)) return b;
            return DefaultModsDir();
        }
    } // namespace

    // ============================================================
    //  游戏侧路径 / UMM 加载器（统一 DoorstopProxy 方式安装）
    //   参考 UnityModManager 0.32.x：
    //     · 游戏根目录：winhttp.dll（UnityDoorstop 代理）+ doorstop_config.ini
    //     · <游戏>_Data\Managed\UnityModManager\：
    //         UnityModManager.dll / 0Harmony.dll / dnlib.dll / UnityModManager.xml / Config.xml
    //   内核文件随本工具分发，位置：<本 DLL 目录>\umm\
    // ============================================================
    namespace
    {
        std::string DllDir()
        {
            static std::string s;
            static bool once = false;
            if (!once)
            {
                once = true;
                HMODULE self = nullptr;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)(const void*)&DllDir, &self) && self)
                {
                    char p[MAX_PATH * 2] = { 0 };
                    DWORD n = GetModuleFileNameA(self, p, (DWORD)sizeof(p));
                    if (n > 0 && n < sizeof(p))
                    {
                        std::string full(p, n);
                        size_t k = full.find_last_of("\\/");
                        s = (k == std::string::npos) ? std::string(".\\") : full.substr(0, k + 1);
                    }
                }
                if (s.empty()) s = ".\\";
            }
            return s;
        }

        std::string KernelDir()
        {
            const char* subs[] = { "umm", "UmmLoader", "UMM", "umm\\kernel" };
            for (int i = 0; i < 4; i++)
            {
                std::string d = DllDir() + subs[i] + "\\";
                if (FileExists(d + "UnityModManager.dll") && FileExists(d + "0Harmony.dll"))
                    return d;
            }
            return std::string();
        }

        std::string GameModsDir()
        {
            std::string g = GameDir::Get();
            if (g.empty() || !DirExists(g)) return std::string();
            if (DirExists(JoinPath(g, "Mods"))) return JoinPath(g, "Mods");
            if (DirExists(JoinPath(g, "mods"))) return JoinPath(g, "mods");
            return JoinPath(g, "Mods");
        }
        std::string LoaderDir()
        {
            return ModLoader::LoaderDirPath();
        }
        std::string WinhttpPath()
        {
            std::string g = GameDir::Get();
            return g.empty() ? std::string() : JoinPath(g, "winhttp.dll");
        }
        std::string DoorstopIniPath()
        {
            std::string g = GameDir::Get();
            return g.empty() ? std::string() : JoinPath(g, "doorstop_config.ini");
        }
        bool LoaderFilesOk()
        {
            std::string d = LoaderDir();
            if (d.empty()) return false;
            return FileExists(JoinPath(d, "UnityModManager.dll")) &&
                   FileExists(JoinPath(d, "0Harmony.dll"));
        }
        bool LoaderInstalled()
        {
            return LoaderFilesOk() && FileExists(WinhttpPath()) && FileExists(DoorstopIniPath());
        }
        bool LoaderPartial()
        {
            if (LoaderInstalled()) return false;
            std::string g = GameDir::Get();
            if (g.empty() || !DirExists(g)) return false;
            return LoaderFilesOk() || FileExists(WinhttpPath()) ||
                   FileExists(DoorstopIniPath()) || DirExists(LoaderDir());
        }
        // ---- UMM 运行日志（Log.txt）：判断 MOD 是否真的被游戏加载的事实依据 ----
        std::string LoaderLogPath() { return JoinPath(LoaderDir(), "Log.txt"); }

        int         g_logOk = -1, g_logTotal = -1;   // -1 = 没有日志
        std::string g_logTime;
        unsigned long long g_logStamp = 0;

        unsigned long long FileStamp(const std::string& path)
        {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
            return ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32) |
                   (unsigned long long)fad.ftLastWriteTime.dwLowDateTime;
        }
        std::string FileTimeStr(const std::string& path)
        {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad)) return std::string();
            SYSTEMTIME st{};
            if (!FileTimeToSystemTime(&fad.ftLastWriteTime, &st)) return std::string();
            char b[64];
            snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
            return std::string(b);
        }

        // 解析 UMM 日志：states = Id -> 状态（1=加载成功 2=跳过 3=报错），errs = Id -> 错误摘要
        void ParseLoaderLog(std::vector<std::pair<std::string, int>>& states,
                            std::vector<std::pair<std::string, std::string>>& errs)
        {
            bool ok = false;
            std::string t = ReadAll(LoaderLogPath(), &ok);
            if (!ok) return;
            std::vector<std::pair<std::string, int>> flags;   // tag -> 位标记 1=Active 2=跳过 4=报错
            size_t p = 0;
            while (p < t.size())
            {
                size_t e = t.find('\n', p);
                if (e == std::string::npos) e = t.size();
                std::string line = t.substr(p, e - p);
                p = e + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
                if (line.size() < 3 || line[0] != '[') continue;
                size_t rb = line.find(']');
                if (rb == std::string::npos) continue;
                std::string tag = line.substr(1, rb - 1);
                if (tag == "Manager")
                {
                    // 重复安装检测： [Manager] [Error] Id 'X' already uses another mod.
                    size_t q = line.find("Id '");
                    if (q != std::string::npos && Contains(line, "already uses another mod"))
                    {
                        size_t q2 = line.find('\'', q + 4);
                        if (q2 != std::string::npos)
                        {
                            std::string dup = line.substr(q + 4, q2 - (q + 4));
                            if (!dup.empty())
                            {
                                flags.push_back(std::make_pair(dup, 4));
                                errs.push_back(std::make_pair(dup, std::string("duplicate")));
                            }
                        }
                    }
                    continue;
                }
                std::string rest = line.substr(rb + 1);
                while (!rest.empty() && rest[0] == ' ') rest.erase(rest.begin());
                if (rest.compare(0, 7, "Version") == 0) continue;   // "... Version 'x'. Loading."
                int st = 0;
                std::string err;
                if (rest.compare(0, 7, "Active.") == 0) st = 1;
                else if (rest.compare(0, 14, "Unsuccessfully") == 0) st = 3;   // 自研加载器的失败标记
                else if (Contains(rest, "To skip")) st = 2;
                else if (Contains(rest, "[Exception]") || Contains(rest, "[Error]"))
                {
                    st = 3;
                    err = rest;
                    size_t b1 = err.find(']');
                    if (b1 != std::string::npos) err = err.substr(b1 + 1);
                    while (!err.empty() && err[0] == ' ') err.erase(err.begin());
                }
                if (!st) continue;
                int bit = (st == 1) ? 1 : (st == 2 ? 2 : 4);
                bool merged = false;
                for (auto& kv : flags)
                    if (kv.first == tag) { kv.second |= bit; merged = true; break; }
                if (!merged) flags.push_back(std::make_pair(tag, bit));
                if (st == 3 && !err.empty()) errs.push_back(std::make_pair(tag, err));
            }
            // 判定优先级：加载成功 > 报错 > 跳过（网络/补丁类 [Error] 不影响“已加载”）
            for (auto& kv : flags)
            {
                int state = (kv.second & 1) ? 1 : ((kv.second & 4) ? 3 : 2);
                states.push_back(std::make_pair(kv.first, state));
            }
            size_t f = t.rfind("LOADED ");
            if (f != std::string::npos)
            {
                int a = -1, b = -1;
                if (sscanf(t.c_str() + f + 7, "%d/%d", &a, &b) == 2) { g_logOk = a; g_logTotal = b; }
            }
        }

        bool CopyFileTo(const std::string& src, const std::string& dst)
        {
            if (!FileExists(src)) return false;
            SetFileAttributesA(dst.c_str(), FILE_ATTRIBUTE_NORMAL);
            return CopyFileA(src.c_str(), dst.c_str(), FALSE) != 0;
        }
        void MakeDirsFor(const std::string& dir)
        {
            for (size_t i = 3; i < dir.size(); i++)
                if (dir[i] == '\\') CreateDirectoryA(dir.substr(0, i).c_str(), nullptr);
            CreateDirectoryA(dir.c_str(), nullptr);
        }
        bool CopyDirInto(const std::string& src, const std::string& dst)
        {
            MakeDirsFor(dst);
            std::string pat = src + "\\*";
            WIN32_FIND_DATAA fd{};
            HANDLE h = FindFirstFileA(pat.c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) return false;
            bool ok = true;
            do
            {
                std::string nm = fd.cFileName;
                if (nm == "." || nm == "..") continue;
                std::string s = src + "\\" + nm, d = dst + "\\" + nm;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    if (!CopyDirInto(s, d)) ok = false;
                }
                else
                {
                    SetFileAttributesA(d.c_str(), FILE_ATTRIBUTE_NORMAL);
                    if (!CopyFileA(s.c_str(), d.c_str(), FALSE)) ok = false;
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
            return ok;
        }

        std::string G_ldMsg;
        bool        G_ldMsgBad = false;

        bool InstallLoader()
        {
            std::string g = GameDir::Get();
            std::string k = KernelDir();
            if (g.empty() || !DirExists(g)) { G_ldMsg = T(MS_LOADER_FAIL);     G_ldMsgBad = true; return false; }
            if (k.empty())                  { G_ldMsg = T(MS_LOADER_NOKERNEL); G_ldMsgBad = true; return false; }
            std::string ld = LoaderDir();
            if (ld.empty())                 { G_ldMsg = T(MS_LOADER_FAIL);     G_ldMsgBad = true; return false; }
            MakeDirsFor(ld);

            const char* files[] = { "UnityModManager.dll", "0Harmony.dll", "dnlib.dll",
                                    "UnityModManager.xml" };
            for (int i = 0; i < 4; i++) CopyFileTo(k + files[i], JoinPath(ld, files[i]));
            if (!FileExists(JoinPath(ld, "Config.xml")))
                CopyFileTo(k + "Config.xml", JoinPath(ld, "Config.xml"));

            std::string wh = WinhttpPath(), ini = DoorstopIniPath();
            if (FileExists(wh) && !FileExists(ini) && !FileExists(wh + ".adofperfect-backup"))
                CopyFileTo(wh, wh + ".adofperfect-backup");
            std::string wsrc = k + "winhttp_x64.dll";
            if (!FileExists(wsrc)) wsrc = k + "winhttp_x86.dll";
            if (!CopyFileTo(wsrc, wh)) { G_ldMsg = T(MS_LOADER_NOKERNEL); G_ldMsgBad = true; return false; }

            std::string data = FindDataDir(g);
            std::string target = data + "\\Managed\\UnityModManager\\UnityModManager.dll";
            std::string doc = "[General]\nenabled = true\ntarget_assembly = " + target + "\n";
            if (!WriteAll(ini, doc)) { G_ldMsg = T(MS_LOADER_FAIL); G_ldMsgBad = true; return false; }

            G_ldMsg = T(MS_LOADER_DONE);
            G_ldMsgBad = false;
            Log::Printf("[mod] UMM loader installed (kernel=%s loader=%s)", k.c_str(), ld.c_str());
            return true;
        }

        bool UninstallLoader()
        {
            std::string g = GameDir::Get();
            if (g.empty()) { G_ldMsg = T(MS_LOADER_FAIL); G_ldMsgBad = true; return false; }
            std::string wh = WinhttpPath(), ini = DoorstopIniPath();
            if (FileExists(ini)) { SetFileAttributesA(ini.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileA(ini.c_str()); }
            if (FileExists(wh))  { SetFileAttributesA(wh.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileA(wh.c_str()); }
            if (FileExists(wh + ".adofperfect-backup")) CopyFileTo(wh + ".adofperfect-backup", wh);
            std::string ld = LoaderDir();
            if (DirExists(ld)) RemoveDirRecursive(ld);
            G_ldMsg = T(MS_LOADER_UNINSTALLED);
            G_ldMsgBad = false;
            Log::Printf("[mod] UMM loader uninstalled");
            return true;
        }
    } // namespace

    // ============================================================
    //  MOD 数据
    // ============================================================
    namespace
    {
        struct ModInfo
        {
            std::string id, name, version, author, gameVersion, managerVersion;
            std::string homePage, repository;
            std::string assemblyName;        // Info.json 的 AssemblyName（运行时 Id 变化时用它兜底匹配）
            std::vector<std::string> requirements;
            std::string folder, folderPath;
            bool        enabled = true;
            bool        inGame  = false;     // 是否已存在于游戏的 mods 目录
            int         logState = 0;        // 0=未运行 1=已加载 2=跳过 3=报错
            std::string logErr;
            // ---- 游戏内加载器的实时状态（来自 ModLoader::StateJson）----
            bool        ldSeen   = false;    // 加载器扫描到了这个 MOD
            bool        ldEnabled = false;   // 加载器侧记录的启用状态
            bool        ldActive = false;    // 已加载进程序集
            bool        ldLoaded = false;    // 加载流程正常结束
            bool        ldError  = false;    // 加载报错
            bool        ldGui    = false;    // MOD 自带 OnGUI 界面
            bool        ldOpen   = false;    // 自带界面的窗口当前是否打开
            std::vector<SetFile> settings;
        };

        std::vector<ModInfo> g_mods;
        int   g_enabledCount = 0;
        bool  g_stateLoaded = false;
        std::string g_modsDirCache;
        std::string g_msg;          // 顶部状态提示
        bool  g_msgBad = false;
        DWORD g_lastScan = 0;
        // 展开的 MOD 卡片（Card Expand）：按 Id 记录，重扫 / 刷新后仍保持展开
        std::string g_expandId;

        // 读档时暂存的 MOD 设置项：mod.set.<id>|<设置文件>|<键>
        struct PendingSet { std::string id, file, key, val; };
        std::vector<PendingSet> g_pendingSets;

        // ---- 游戏内加载器的实时回报（渲染线程只读缓存） ----
        std::vector<LdState> g_ld;
        bool  g_ldReady = false;
        std::string g_ldSig;       // 加载器识别到的 MOD 集合签名（变化时补扫）
        std::string g_ldErr;
        DWORD g_ldPull  = 0;
        bool  g_ldFirst = true;    // 首次拿到 ready 时给个提示

        void RefreshLoaderStates(bool force)
        {
            DWORD now = GetTickCount();
            if (!force && now - g_ldPull < 400) return;
            g_ldPull = now;
            g_ldReady = ModLoader::Ready();
            g_ldErr   = ModLoader::LastError();
            ParseLdStates(ModLoader::StateJson(), g_ld);
        }

        // 把加载器状态并到本页的 MOD 列表上
        void ApplyLoaderStates()
        {
            for (auto& m : g_mods)
            {
                m.ldSeen = false;
                for (auto& s : g_ld)
                {
                    bool hit = (s.id == m.id);
                    // 有的 MOD 会在运行时改写 Info.Id（例如 TogetherBootstrap -> Together），
                    // 这时用程序集名兜底匹配，避免误报「加载器未识别到该 MOD」。
                    if (!hit && !s.assembly.empty() && !m.assemblyName.empty())
                        hit = (ToLower(s.assembly) == ToLower(m.assemblyName));
                    if (!hit) continue;
                    m.ldSeen    = true;
                    m.ldEnabled = s.enabled;
                    m.ldActive  = s.active;
                    m.ldLoaded  = s.loaded;
                    m.ldError   = s.error;
                    m.ldGui     = s.gui;
                    m.ldOpen    = s.open;
                    break;
                }
            }
        }

        // ---- 启用状态持久化：本工具 prefs（+ Params.xml 镜像） ----
        std::string StateKey(const std::string& id) { return std::string("mod.state.") + id; }

        bool PrefHasState(const std::string& id)
        {
            return I18N::Prefs::GetInt(StateKey(id).c_str(), -1) >= 0;
        }
        bool PrefGetState(const std::string& id, bool def)
        {
            int v = I18N::Prefs::GetInt(StateKey(id).c_str(), -1);
            return v < 0 ? def : (v != 0);
        }
        void PrefSetState(const std::string& id, bool on)
        {
            I18N::Prefs::SetInt(StateKey(id).c_str(), on ? 1 : 0);
            I18N::Prefs::Save();
        }

        // ---- Params.xml ----
        bool ParamsReadEnabled(const std::string& xml, const std::string& id, bool* out)
        {
            size_t p = 0;
            std::string needle = "Id=\"" + id + "\"";
            while ((p = xml.find(needle, p)) != std::string::npos)
            {
                size_t tag = xml.rfind("<Mod", p);
                if (tag == std::string::npos) { p += needle.size(); continue; }
                size_t end = xml.find('>', p);
                if (end == std::string::npos) return false;
                std::string body = xml.substr(tag, end - tag);
                size_t en = body.find("Enabled=\"");
                if (en != std::string::npos)
                {
                    std::string v = body.substr(en + 9, 5);
                    *out = (ToLower(v).compare(0, 4, "true") == 0);
                    return true;
                }
                p = end;
            }
            return false;
        }

        std::string ParamsUpsert(const std::string& xml, const std::string& id, bool on)
        {
            const char* val = on ? "true" : "false";
            std::string needle = "Id=\"" + id + "\"";
            size_t p = xml.find(needle);
            if (p != std::string::npos)
            {
                size_t tag = xml.rfind("<Mod", p);
                size_t end = xml.find('>', p);
                if (tag != std::string::npos && end != std::string::npos)
                {
                    std::string body = xml.substr(tag, end - tag);
                    size_t en = body.find("Enabled=\"");
                    if (en != std::string::npos)
                    {
                        size_t q = body.find('"', en + 9);
                        if (q != std::string::npos)
                        {
                            std::string before = xml.substr(0, tag + en + 9);
                            std::string after = xml.substr(tag + q);
                            return before + val + after;
                        }
                    }
                    // 没有 Enabled 属性：补一个
                    std::string before = xml.substr(0, end);
                    std::string after = xml.substr(end);
                    return before + " Enabled=\"" + val + "\"" + after;
                }
            }
            // 未找到：插到 <ModParams> 之后
            size_t mp = xml.find("<ModParams>");
            if (mp != std::string::npos)
            {
                std::string ins = "\n    <Mod Id=\"" + id + "\" Enabled=\"" + val + "\" />";
                return xml.substr(0, mp + 11) + ins + xml.substr(mp + 11);
            }
            // 没有 ModParams：整体重建（保留原 Param 根）
            std::string doc = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<Param>\n  <ModParams>";
            doc += "\n    <Mod Id=\"" + id + "\" Enabled=\"" + val + "\" />";
            doc += "\n  </ModParams>\n</Param>\n";
            return doc;
        }

        bool ParamsWriteEnabled(const std::string& id, bool on)
        {
            std::string path = ParamsXmlPath();
            if (path.empty()) return false;
            std::string dir = path.substr(0, path.find_last_of('\\'));
            // 逐级创建
            for (size_t i = 3; i < dir.size(); i++)
                if (dir[i] == '\\') CreateDirectoryA(dir.substr(0, i).c_str(), nullptr);
            CreateDirectoryA(dir.c_str(), nullptr);

            bool ok = false;
            std::string xml = ReadAll(path, &ok);
            if (!ok || xml.empty())
            {
                // 自己还没有：从原版 UMM 的 Params.xml 接手（保留用户已有启用状态）
                std::string fp = ForeignParamsXmlPath();
                if (!fp.empty()) xml = ReadAll(fp, &ok);
            }
            if (!ok || xml.empty())
            {
                xml = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<Param>\n  <ModParams>\n  </ModParams>\n</Param>\n";
            }
            std::string out = ParamsUpsert(xml, id, on);
            // 生态迁移：原版 UMM 在场时把同一份状态镜像进它的 Params.xml，
            // 它的 GUI / 下次 doorstop 启动读到的启用状态与本工具一致
            std::string fp2 = ForeignParamsXmlPath();
            if (!fp2.empty() && !PathEquals(fp2, path)) WriteAll(fp2, out);
            return WriteAll(path, out);
        }

        // ---- 设置文件识别 ----
        // 除了 settings/config，也要认 <X>Settings.xml、<X>Config.json 这类
        // 「名字里含 settings/setting/config/configuration」的文件
        // （例如 AdofaiTweaks 的 GlobalSettings.xml、ShowBPM 的 Setting.xml）。
        bool StemHasSettings(const std::string& low)
        {
            size_t dot = low.rfind('.');
            std::string stem = (dot == std::string::npos) ? low : low.substr(0, dot);
            auto ends = [&](const char* suf) {
                size_t n = strlen(suf);
                return stem.size() >= n && stem.compare(stem.size() - n, n, suf) == 0;
            };
            if (ends("settings") || ends("setting")) return true;
            if (ends("config") || ends("configuration")) return true;
            return false;
        }
        bool IsSettingsFileName(const std::string& low)
        {
            if (low == "settings.xml" || low == "config.xml") return true;
            if (low == "settings.json" || low == "config.json") return true;
            if (low == "options.xml" || low == "options.json") return true;
            if (low == "preferences.xml" || low == "preferences.json") return true;
            if (low.size() > 12 && low.compare(low.size() - 12, 12, ".settings.xml") == 0) return true;
            if (low.size() > 13 && low.compare(low.size() - 13, 13, ".settings.json") == 0) return true;
            return StemHasSettings(low);
        }
        void CollectSettings(ModInfo& m)
        {
            m.settings.clear();
            for (auto& f : ListFiles(m.folderPath, ".xml"))
            {
                std::string low = ToLower(f);
                if (!IsSettingsFileName(low)) continue;
                SetFile sf;
                sf.name = f;
                sf.path = JoinPath(m.folderPath, f);
                sf.xml = true;
                m.settings.push_back(sf);
            }
            for (auto& f : ListFiles(m.folderPath, ".json"))
            {
                std::string low = ToLower(f);
                if (!IsSettingsFileName(low)) continue;
                SetFile sf;
                sf.name = f;
                sf.path = JoinPath(m.folderPath, f);
                sf.xml = false;
                m.settings.push_back(sf);
            }
            // 默认编辑「settings.xml」——那是原版 UMM 与本加载器共用的规范
            // 设置文件（生态迁移后两边读写同一家）。其余设置文件排后面。
            for (size_t i = 0; i < m.settings.size(); i++)
            {
                if (ToLower(m.settings[i].name) == "settings.xml" && i > 0)
                {
                    SetFile first = m.settings[i];
                    m.settings.erase(m.settings.begin() + i);
                    m.settings.insert(m.settings.begin(), first);
                    break;
                }
            }
        }

        void ApplyLogStates()
        {
            std::vector<std::pair<std::string, int>> states;
            std::vector<std::pair<std::string, std::string>> errs;
            ParseLoaderLog(states, errs);
            for (auto& m : g_mods)
            {
                m.logState = 0;
                m.logErr.clear();
                if (m.id.empty()) continue;
                for (auto& kv : states) if (kv.first == m.id) { m.logState = kv.second; break; }
                for (auto& kv : errs)   if (kv.first == m.id) { m.logErr = kv.second;   break; }
            }
        }

        void RefreshLogStatus(bool force)
        {
            std::string p = LoaderLogPath();
            unsigned long long st = FileStamp(p);
            if (!force && st == g_logStamp) return;
            g_logStamp = st;
            g_logTime  = st ? FileTimeStr(p) : std::string();
            g_logOk    = -1;
            g_logTotal = -1;
            ApplyLogStates();
        }

        // 在 <游戏>\Mods 下按 Id / 程序集名找对应文件夹名；找不到就用 Id 兜底。
        std::string GameModFolder(const std::string& id, const std::string& asmName)
        {
            std::string gm = GameModsDir();
            if (DirExists(gm))
            {
                for (auto& folder : ListDirs(gm))
                {
                    std::string info = JoinPath(JoinPath(gm, folder), "Info.json");
                    if (!FileExists(info)) info = JoinPath(JoinPath(gm, folder), "info.json");
                    if (!FileExists(info)) continue;
                    bool ok = false;
                    std::string js = ReadAll(info, &ok);
                    if (!ok) continue;
                    std::string mid = JsonStrField(js, "Id");
                    if (mid.empty()) mid = JsonStrField(js, "ID");
                    std::string masm = JsonStrField(js, "AssemblyName");
                    if (mid == id) return folder;
                    if (!asmName.empty() && !masm.empty() && ToLower(masm) == ToLower(asmName)) return folder;
                }
            }
            return id;
        }

        void ScanMods()
        {
            g_lastScan = GetTickCount();
            g_mods.clear();
            g_enabledCount = 0;
            g_modsDirCache = ResolveModsDir();
            if (!DirExists(g_modsDirCache)) return;

            std::string paramsXml;
            {
                bool ok = false;
                std::string pp = ParamsXmlPath();
                if (!pp.empty()) paramsXml = ReadAll(pp, &ok);
                if (!ok || paramsXml.empty())
                {
                    // 自己还没有：接原版 UMM 已经写好的启用状态（生态迁移）
                    std::string fp = ForeignParamsXmlPath();
                    if (!fp.empty()) paramsXml = ReadAll(fp, &ok);
                }
            }

            for (auto& folder : ListDirs(g_modsDirCache))
            {
                std::string dir = JoinPath(g_modsDirCache, folder);
                std::string info = JoinPath(dir, "Info.json");
                if (!FileExists(info)) info = JoinPath(dir, "info.json");
                if (!FileExists(info)) continue;
                bool ok = false;
                std::string js = ReadAll(info, &ok);
                if (!ok) continue;

                ModInfo m;
                m.id = JsonStrField(js, "Id");
                if (m.id.empty()) m.id = JsonStrField(js, "ID");
                if (m.id.empty()) m.id = folder;
                m.name = JsonStrField(js, "DisplayName");
                if (m.name.empty()) m.name = JsonStrField(js, "Name");
                if (m.name.empty()) m.name = m.id;
                m.version = JsonStrField(js, "Version");
                m.author = JsonStrField(js, "Author");
                m.gameVersion = JsonStrField(js, "GameVersion");
                m.managerVersion = JsonStrField(js, "ManagerVersion");
                m.homePage = JsonStrField(js, "HomePage");
                m.repository = JsonStrField(js, "Repository");
                m.assemblyName = JsonStrField(js, "AssemblyName");
                m.requirements = JsonStrArray(js, "Requirements");
                m.folder = folder;
                m.folderPath = dir;

                bool en = true;
                bool fromParams = ParamsReadEnabled(paramsXml, m.id, &en);
                if (!fromParams) en = PrefGetState(m.id, true);
                m.enabled = en;
                if (m.enabled) g_enabledCount++;

                CollectSettings(m);
                {
                    std::string gm = GameModsDir();
                    m.inGame = !gm.empty() &&
                               (ToLower(gm) == ToLower(g_modsDirCache) ||
                                DirExists(JoinPath(gm, m.folder)) ||
                                (!m.id.empty() && DirExists(JoinPath(gm, m.id))));
                }
                g_mods.push_back(std::move(m));
            }

            // 加载器还认识别的目录（通常是 <游戏>\Mods）里的 MOD：一并列出来，
            // 这样“加载器到底看到了哪些 MOD”在本页一目了然（它们同样能开关 / 改设置）。
            if (g_ldReady)
            {
                for (auto& s : g_ld)
                {
                    if (s.id.empty()) continue;
                    bool dup = false;
                    for (auto& m : g_mods)
                        if (m.id == s.id ||
                            (!m.assemblyName.empty() && ToLower(m.assemblyName) == ToLower(s.assembly)))
                        { dup = true; break; }
                    if (dup) continue;

                    ModInfo em;
                    em.id = s.id;
                    em.name = s.name.empty() ? s.id : s.name;
                    em.version = s.version;
                    em.author = s.author;
                    em.assemblyName = s.assembly;
                    em.folder = GameModFolder(s.id, s.assembly);
                    em.folderPath = JoinPath(GameModsDir(), em.folder);
                    em.inGame = true;
                    em.enabled = s.enabled;
                    if (em.enabled) g_enabledCount++;
                    CollectSettings(em);
                    g_mods.push_back(std::move(em));
                }
            }

            std::sort(g_mods.begin(), g_mods.end(), [](const ModInfo& a, const ModInfo& b) {
                return ToLower(a.name) < ToLower(b.name);
            });
            Log::Printf("[mod] scanned %d mods in %s", (int)g_mods.size(), g_modsDirCache.c_str());
            RefreshLogStatus(true);
        }

        std::string Fmt(const char* f, ...);   // 定义在下方 UI 段

        // 把仓库里的 MOD 复制进游戏的 mods 目录（已存在则不动，避免覆盖现场配置）
        bool InstallModToGame(int idx)
        {
            if (idx < 0 || idx >= (int)g_mods.size()) return false;
            ModInfo& m = g_mods[idx];
            std::string gm = GameModsDir();
            if (gm.empty()) { g_msg = T(MS_LOADER_FAIL); g_msgBad = true; return false; }
            std::string dst = JoinPath(gm, m.folder);
            if (!DirExists(dst) && !CopyDirInto(m.folderPath, dst))
            {
                g_msg = Fmt(T(MS_INSTALL_ERR), m.name.c_str());
                g_msgBad = true;
                Log::Printf("[mod] install '%s' -> %s FAILED", m.id.c_str(), dst.c_str());
                return false;
            }
            m.inGame = true;
            g_msg = Fmt(T(MS_INSTALL_OK), m.name.c_str());
            g_msgBad = false;
            Log::Printf("[mod] install '%s' -> %s ok", m.id.c_str(), dst.c_str());
            return true;
        }

        void EnsureScanned()
        {
            if (!g_stateLoaded) { g_stateLoaded = true; ScanMods(); return; }
            if (g_mods.empty()) return;
        }
    } // namespace
    // ============================================================
    //  页面 UI 状态
    // ============================================================
    namespace
    {
        enum { MD_NONE = 0, MD_DELETE, MD_SETTINGS };

        int   s_modal = MD_NONE;
        int   s_delIndex = -1;
        int   s_setIndex = -1;
        int   s_setFileIdx = 0;
        bool  s_setDirty = false;
        std::string s_setStatus;
        bool  s_rescan = false;

        char  s_gameBuf[MAX_PATH * 2] = { 0 };
        char  s_modBuf[MAX_PATH * 2] = { 0 };
        bool  s_pathInit = false;
        int   s_pendingPick = 0;   // 1=游戏目录 2=MOD 目录

        std::string Fmt(const char* f, ...)
        {
            char b[1024];
            va_list ap;
            va_start(ap, f);
            vsnprintf(b, sizeof(b), f, ap);
            va_end(ap);
            return std::string(b);
        }

        void InitPathsOnce()
        {
            if (s_pathInit) return;
            s_pathInit = true;
            snprintf(s_gameBuf, sizeof(s_gameBuf), "%s", GameDir::Get());
            snprintf(s_modBuf, sizeof(s_modBuf), "%s", ResolveModsDir().c_str());
        }

        void ReloadPaths()
        {
            snprintf(s_gameBuf, sizeof(s_gameBuf), "%s", GameDir::Get());
            snprintf(s_modBuf, sizeof(s_modBuf), "%s", ResolveModsDir().c_str());
        }

        void RecomputeEnabledCount()
        {
            g_enabledCount = 0;
            for (auto& m : g_mods) if (m.enabled) g_enabledCount++;
        }

        void SetEnabled(int idx, bool on)
        {
            if (idx < 0 || idx >= (int)g_mods.size()) return;
            auto& m = g_mods[idx];
            m.enabled = on;
            PrefSetState(m.id, on);
            bool wrote = ParamsWriteEnabled(m.id, on);
            // 游戏内加载器：立刻热切换（异步排队，主线程执行）
            ModLoader::SetEnabled(m.id, on);
            RecomputeEnabledCount();
            Log::Printf("[mod] %s -> %d (params=%d)", m.id.c_str(), (int)on, (int)wrote);
            g_msg = on ? (std::string(T(MS_STATE_ON)) + " : " + m.name)
                       : (std::string(T(MS_STATE_OFF)) + " : " + m.name);
            g_msgBad = false;
        }

        void DeleteMod(int idx)
        {
            if (idx < 0 || idx >= (int)g_mods.size()) return;
            ModInfo m = g_mods[idx];   // 拷贝：删除后原元素失效
            if (!RemoveDirRecursive(m.folderPath))
            {
                g_msg = Fmt(T(MS_DELFAIL), m.folderPath.c_str());
                g_msgBad = true;
                return;
            }
            I18N::Prefs::SetInt(StateKey(m.id).c_str(), -1);
            I18N::Prefs::Save();
            g_msg = Fmt(T(MS_DELETED), m.name.c_str());
            g_msgBad = false;
            ScanMods();
        }



        void EnsureSettingsLoaded(int idx);   // 前置声明
        bool SaveSetFile(SetFile& sf);        // 前置声明
        void LoadSetFileItems(SetFile& sf);   // 前置声明
        float DrawModBody(int i, float w, float x, float y);   // 卡片展开区（Card Expand）

        // 卡片状态点 + 文案：优先用游戏内加载器的实时回报；
        // 加载器没在跑（游戏没开）时，退回读上一次的 Log.txt。
        void ModStatus(const ModInfo& m, ImU32* col, std::string* text)
        {
            if (g_ldReady && m.ldSeen)
            {
                if (m.ldError)         { *col = Th().bad;  *text = T(MS_LD_ERROR); }
                else if (!m.enabled)   { *col = Th().textFaint; *text = T(MS_LD_DISABLED); }
                else if (m.ldLoaded)   { *col = Th().good; *text = T(MS_LD_LOADED); }
                else if (m.ldActive)   { *col = Th().warn; *text = T(MS_LD_LOADING); }
                else                   { *col = Th().warn; *text = T(MS_LD_PENDING); }
                if (m.ldError && !m.logErr.empty()) *text += "  ·  " + m.logErr;
                return;
            }
            if (g_ldReady && !m.ldSeen)
            {
                *col = Th().warn; *text = T(MS_LD_UNSEEN);
                return;
            }
            if (!m.enabled) { *col = Th().textFaint; *text = T(MS_LD_DISABLED); return; }
            *col = Th().textFaint;
            *text = T(MS_LOG_NOTRUN);
            if (g_logTotal < 0) return;
            if (m.logState == 1)      { *col = Th().good; *text = T(MS_LOG_LOADED); }
            else if (m.logState == 2) { *col = Th().warn; *text = T(MS_LOG_SKIPPED); }
            else if (m.logState == 3)
            {
                *col = Th().bad; *text = T(MS_LOG_ERROR);
                if (!m.logErr.empty()) *text += "  ·  " + m.logErr;
            }
        }

        // ---- 单个 MOD 卡片 ----
        // ---- 卡片展开区（Card Expand）：信息 + 设置参数编辑（本工具自绘 ImGui 风格） ----
        // 返回展开区底部的屏幕 Y，供外层推进光标。
        float DrawModBody(int i, float w, float x, float y)
        {
            ModInfo& m = g_mods[i];
            ImDrawList* dl = ImGui::GetWindowDrawList();

            EnsureSettingsLoaded(i);
            const int nFiles = (int)m.settings.size();

            const float padX = 14.f, padY = 10.f;
            const float lineH = 18.f, comboH = 30.f, listH = 172.f, btnH = 30.f;
            float bh = padY + lineH;
            if (nFiles > 1) bh += comboH + 5.f;
            if (nFiles > 0) bh += listH + 6.f;
            if (!s_setStatus.empty()) bh += lineH;
            bh += btnH + padY;

            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + bh), Th().cardBg, 6.f);
            dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + bh), A(Th().line, 0.55f), 6.f);

            const float innerW = w - padX * 2.f;
            float cy = y + padY;

            ImGui::SetCursorScreenPos(ImVec2(x + padX, cy));
            TextFaint("%s", m.folderPath.c_str());
            cy += lineH;

            if (nFiles == 0)
            {
                ImGui::SetCursorScreenPos(ImVec2(x + padX, cy));
                TextFaint("%s", T(MS_SET_NONE));
                cy += lineH;
            }
            else
            {
                if (nFiles > 1)
                {
                    const char* names[64];
                    int n = nFiles > 64 ? 64 : nFiles;
                    for (int k = 0; k < n; k++) names[k] = m.settings[k].name.c_str();
                    ImGui::SetCursorScreenPos(ImVec2(x + padX, cy));
                    int cur = s_setFileIdx;
                    if (Combo("##body_file", &cur, names, n, innerW * 0.55f))
                    {
                        s_setFileIdx = cur;
                        s_setDirty = false;
                        s_setStatus.clear();
                        EnsureSettingsLoaded(i);
                    }
                    cy += comboH + 5.f;
                }

                SetFile& sf = m.settings[(s_setFileIdx >= 0 && s_setFileIdx < nFiles) ? s_setFileIdx : 0];
                ImGui::SetCursorScreenPos(ImVec2(x + padX, cy));
                ImGui::BeginChild("##body_list", ImVec2(innerW, listH),
                                  ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, 0);
                if (sf.items.empty())
                {
                    TextFaint("%s", T(MS_SET_NOFILE));
                }
                else
                {
                    const float colX = LabelCol({ "XXXXXXXXXXXXXXXXXXXXXXXX" }, 140.f);
                    for (int k = 0; k < (int)sf.items.size(); k++)
                    {
                        SetItem& it = sf.items[k];
                        ImGui::PushID(k);
                        if (it.type == 0)
                        {
                            bool v = (ToLower(it.val) == "true");
                            if (CheckRow("##b", it.key.c_str(), &v))
                            {
                                it.val = v ? "true" : "false";
                                s_setDirty = true;
                            }
                        }
                        else
                        {
                            LabelRow(it.key.c_str(), colX);
                            float ww = ImGui::GetContentRegionAvail().x;
                            if (ww < 80.f) ww = 80.f;
                            if (InputText("##v", it.edit, sizeof(it.edit), nullptr, ww))
                            {
                                it.val = it.edit;
                                s_setDirty = true;
                            }
                        }
                        Space(3.f);
                        ImGui::PopID();
                    }
                }
                ImGui::EndChild();
                cy += listH + 6.f;
            }

            if (!s_setStatus.empty())
            {
                ImGui::SetCursorScreenPos(ImVec2(x + padX, cy));
                TextCol(s_setDirty ? Th().warn : Th().good, "%s", s_setStatus.c_str());
                cy += lineH;
            }

            {
                const float sp = ImGui::GetStyle().ItemSpacing.x;
                float bw = (innerW - sp * 2.f) / 3.f;
                ImGui::SetCursorScreenPos(ImVec2(x + padX, cy));
                if (nFiles > 0)
                {
                    if (Button("##body_save", T(MS_SET_SAVE), ImVec2(bw, btnH), BTN_PRIMARY))
                    {
                        SetFile& sf2 = m.settings[(s_setFileIdx >= 0 && s_setFileIdx < nFiles) ? s_setFileIdx : 0];
                        s_setIndex = i;
                        bool ok = SaveSetFile(sf2);
                        if (ok)
                        {
                            s_setDirty = false;
                            // 光写文件 MOD 是看不到的（它只认加载时读进内存的那份），
                            // 立刻让加载器把新值热应用进运行中的 MOD。
                            ModLoader::ApplySettings(m.id, true);
                            s_setStatus = std::string(Fmt(T(MS_SET_SAVED), sf2.name.c_str())) + "  ·  " + T(MS_APPLYING);
                        }
                        else s_setStatus = T(MS_SET_NOFILE);
                        g_msg = s_setStatus;
                        g_msgBad = !ok;
                    }
                    ImGui::SameLine();
                    if (Button("##body_reload", T(MS_RELOAD), ImVec2(bw, btnH)))
                    {
                        SetFile& sf3 = m.settings[(s_setFileIdx >= 0 && s_setFileIdx < nFiles) ? s_setFileIdx : 0];
                        sf3.items.clear();
                        LoadSetFileItems(sf3);
                        s_setIndex = i;
                        s_setDirty = false;
                        s_setStatus.clear();
                    }
                    ImGui::SameLine();
                    if (Button("##body_open", T(MS_OPENFOLDER), ImVec2(bw, btnH)))
                        ShellAsync::Open(m.folderPath.c_str());
                }
                else
                {
                    if (Button("##body_open2", T(MS_OPENFOLDER), ImVec2(innerW * 0.5f, btnH)))
                        ShellAsync::Open(m.folderPath.c_str());
                }
                cy += btnH;
            }

            return y + bh;
        }

        void DrawModCard(int i)
        {
            ModInfo& m = g_mods[i];
            ImGui::PushID(i);
            const float h = 76.f;
            float w = ImGui::GetContentRegionAvail().x;
            if (w < 260.f) w = 260.f;
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const bool expanded = (!g_expandId.empty() && g_expandId == m.id);

            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Th().rowBg, 6.f);
            dl->AddRect(p, ImVec2(p.x + w, p.y + h), A(Th().line, 0.85f), 6.f);

            // 状态点
            ImVec2 dot(p.x + 20.f, p.y + h * 0.5f);
            ImU32 dotCol = Th().textFaint;
            std::string dotTxt;
            ModStatus(m, &dotCol, &dotTxt);
            dl->AddCircleFilled(dot, 8.f, A(dotCol, 0.20f));
            dl->AddCircleFilled(dot, 5.f, dotCol);

            const float bw1 = 82.f, bw2 = 78.f, bw3 = 62.f, gap = 6.f;
            const float btnBlock = bw1 + gap + bw2 + gap + bw3;
            const float textW = w - (dot.x - p.x) - btnBlock - 40.f;

            ImFont* f = ImGui::GetFont();
            dl->PushClipRect(ImVec2(p.x + 36.f, p.y + 6.f),
                             ImVec2(p.x + 36.f + textW, p.y + h - 6.f), true);
            dl->AddText(f, 17.f, ImVec2(p.x + 36.f, p.y + 9.f), Th().text, m.name.c_str());
            std::string sub = m.id;
            if (!m.version.empty()) sub += "  ·  v" + m.version;
            if (!m.author.empty())  sub += "  ·  " + m.author;
            dl->AddText(ImVec2(p.x + 36.f, p.y + 31.f), Th().textFaint, sub.c_str());
            dl->AddText(ImVec2(p.x + 36.f, p.y + 52.f), dotCol, dotTxt.c_str());
            dl->PopClipRect();

            // 展开箭头（Card Expand）
            {
                ImVec2 c(p.x + w - btnBlock - 26.f, p.y + h * 0.5f);
                ImU32 cc = expanded ? Th().accent : A(Th().textFaint, 1.f);
                const float r = 4.5f;
                if (expanded)
                {
                    dl->AddLine(ImVec2(c.x - r, c.y + r * 0.4f), ImVec2(c.x, c.y - r * 0.7f), cc, 1.7f);
                    dl->AddLine(ImVec2(c.x + r, c.y + r * 0.4f), ImVec2(c.x, c.y - r * 0.7f), cc, 1.7f);
                }
                else
                {
                    dl->AddLine(ImVec2(c.x - r * 0.7f, c.y - r), ImVec2(c.x + r * 0.4f, c.y), cc, 1.7f);
                    dl->AddLine(ImVec2(c.x - r * 0.7f, c.y + r), ImVec2(c.x + r * 0.4f, c.y), cc, 1.7f);
                }
            }

            // 头部点击区：卡片主体（右侧按钮之外）都能点开 / 收起设置
            {
                float hdrW = w - btnBlock - 18.f;
                if (hdrW < 60.f) hdrW = 60.f;
                ImGui::SetCursorScreenPos(p);
                if (ImGui::InvisibleButton("##card_hdr", ImVec2(hdrW, h)))
                {
                    if (expanded) g_expandId.clear();
                    else
                    {
                        g_expandId = m.id;
                        s_setIndex = i;
                        s_setFileIdx = 0;
                        s_setDirty = false;
                        s_setStatus.clear();
                        EnsureSettingsLoaded(i);
                    }
                }
                if (ImGui::IsItemHovered())
                    dl->AddRect(p, ImVec2(p.x + w, p.y + h), A(Th().accent, 0.55f), 6.f);
            }

            const float by = p.y + (h - 26.f) * 0.5f;
            float bx = p.x + w - btnBlock - 12.f;

            ImGui::SetCursorScreenPos(ImVec2(bx, by));
            {
                // 加载器直接读「MOD 目录」，不需要先复制进游戏目录：
                // 这里就是纯粹的启用 / 停用（写入 Params.xml + 热切换）。
                const char* lab = m.enabled ? T(MS_DISABLE) : T(MS_ENABLE);
                int var = m.enabled ? BTN_GHOST : BTN_PRIMARY;
                if (Button("##tog", lab, ImVec2(bw1, 26.f), var))
                    SetEnabled(i, !m.enabled);
            }
            bx += bw1 + gap;
            ImGui::SetCursorScreenPos(ImVec2(bx, by));
            if (m.ldGui)
            {
                // MOD 自带界面：直接开关它在游戏里的窗口（用 MOD 自己的 UI）
                const bool wasOpen = m.ldOpen;
                if (Button("##set", wasOpen ? T(MS_CLOSEUI) : T(MS_OPENUI),
                           ImVec2(bw2, 26.f), wasOpen ? BTN_GHOST : BTN_PRIMARY))
                {
                    if (wasOpen) ModLoader::CloseSettings(m.id);
                    else         ModLoader::OpenSettings(m.id);
                    g_msg = std::string(wasOpen ? T(MS_CLOSEUI) : T(MS_OPENUI)) + " : " + m.name;
                    g_msgBad = false;
                }
            }
            else if (Button("##set", T(MS_SETTINGS), ImVec2(bw2, 26.f)))
            {
                // 没有自带界面：展开卡片，直接在本工具自绘的 ImGui 风格里改设置
                if (expanded) g_expandId.clear();
                else
                {
                    g_expandId = m.id;
                    s_setIndex = i;
                    s_setFileIdx = 0;
                    s_setDirty = false;
                    s_setStatus.clear();
                    EnsureSettingsLoaded(i);
                }
            }

            bx += bw2 + gap;
            ImGui::SetCursorScreenPos(ImVec2(bx, by));
            if (Button("##del", T(MS_DELETE), ImVec2(bw3, 26.f), BTN_DANGER))
            {
                s_modal = MD_DELETE;
                s_delIndex = i;
            }

            float cardBottom = p.y + h;
            if (expanded) cardBottom = DrawModBody(i, w, p.x, p.y + h + 3.f);

            ImGui::SetCursorScreenPos(ImVec2(p.x, cardBottom + 6.f));
            ImGui::PopID();
        }
    } // namespace
    // ============================================================
    //  设置文件加载 / 保存
    // ============================================================
    namespace
    {
        void EnsureSettingsLoaded(int idx)
        {
            if (idx < 0 || idx >= (int)g_mods.size()) return;
            ModInfo& m = g_mods[idx];
            if (m.settings.empty()) return;
            if (s_setFileIdx < 0 || s_setFileIdx >= (int)m.settings.size()) s_setFileIdx = 0;
            LoadSetFileItems(m.settings[s_setFileIdx]);
        }

        // 读取一个设置文件的标量项（带缓存：已读过的直接返回）
        void LoadSetFileItems(SetFile& sf)
        {
            if (!sf.items.empty()) return;
            bool ok = false;
            std::string text = ReadAll(sf.path, &ok);
            if (!ok) return;
            if (sf.xml) ParseXmlScalars(text, sf.items);
            else        ParseJsonScalars(text, sf.items);
            for (auto& it : sf.items)
                snprintf(it.edit, sizeof(it.edit), "%s", it.val.c_str());
        }

        bool SaveSetFile(SetFile& sf)
        {
            if (!FileExists(sf.path)) return false;
            bool any = false;
            for (auto& it : sf.items)
            {
                if (it.type == 0) it.val = (ToLower(it.val) == "true") ? "true" : "false";
                if (sf.xml) any |= PatchXml(sf.path, it);
                else        any |= PatchJson(sf.path, it);
            }
            return any;
        }
    } // namespace

    // ============================================================
    //  页面主体
    // ============================================================
    void DrawPage()
    {
        InitPathsOnce();
        if (s_rescan) { s_rescan = false; ScanMods(); }
        EnsureScanned();
        {
            static int s_logTick = 0;
            if ((s_logTick++ % 120) == 0) RefreshLogStatus(false);
        }
        // 游戏内加载器的实时状态（节流轮询由 ModLoader::Tick 负责）
        RefreshLoaderStates(false);
        ApplyLoaderStates();

        // 「设置是否真的应用进 MOD」的异步回报（apply 命令的结果）
        {
            std::string aid, ares;
            if (ModLoader::TakeApplyResult(aid, ares))
            {
                std::string nm = aid;
                for (auto& m : g_mods) if (m.id == aid) { nm = m.name; break; }
                if (ares.compare(0, 8, "deferred") == 0)
                {
                    g_msg = nm + " : " + T(MS_APPLYING);
                    g_msgBad = false;
                }
                else if (ares.compare(0, 2, "ok") == 0)
                {
                    g_msg = nm + " : " + T(MS_SET_APPLIED);
                    g_msgBad = false;
                }
                else
                {
                    g_msg = nm + " : " + T(MS_SET_APPLYFAIL) + "  (" + ares + ")";
                    g_msgBad = true;
                }
            }
        }

        // 调试钩子：ADOFAI_PERFECT_MODOPEN=<ModId> 启动后自动打开该 MOD 的自带界面；
        // ADOFAI_PERFECT_MODAPPLY=<ModId> 再顺手把它的设置热应用一次。
        {
            static bool s_modOpenDone = false;
            static DWORD s_modReadyAt = 0;
            static std::vector<std::string> s_applyMods;
            static size_t s_applyIdx = 0;
            static DWORD s_applyDelay = 0;
            static DWORD s_applyAt = 0;
            if (!s_modOpenDone && g_ldReady)
            {
                s_modReadyAt = GetTickCount();
                s_modOpenDone = true;
                char v[512] = { 0 };
                if (GetEnvironmentVariableA("ADOFAI_PERFECT_MODOPEN", v, sizeof(v)) > 0 && v[0])
                {
                    ModLoader::OpenSettings(v);
                    Log::Printf("[mod] debug open '%s'", v);
                }
                memset(v, 0, sizeof(v));
                if (GetEnvironmentVariableA("ADOFAI_PERFECT_MODAPPLY", v, sizeof(v)) > 0 && v[0])
                {
                    // 支持逗号分隔的多个 MOD Id：一次把多个 MOD 的设置都热应用一遍
                    std::string all = v, cur;
                    for (size_t i = 0; i <= all.size(); i++)
                    {
                        if (i == all.size() || all[i] == ',')
                        {
                            if (!cur.empty()) s_applyMods.push_back(cur);
                            cur.clear();
                        }
                        else if (all[i] != ' ') cur += all[i];
                    }
                    char d[16] = { 0 };
                    s_applyDelay = GetEnvironmentVariableA("ADOFAI_PERFECT_MODAPPLY_DELAY", d, sizeof(d)) > 0
                                   ? (DWORD)atoi(d) : 0;
                    s_applyAt = s_modReadyAt + s_applyDelay;
                }
            }
            if (s_applyIdx < s_applyMods.size() && s_applyAt != 0 && GetTickCount() >= s_applyAt)
            {
                const std::string id = s_applyMods[s_applyIdx++];
                ModLoader::ApplySettings(id, true);
                Log::Printf("[mod] debug apply '%s' (%d/%d)", id.c_str(),
                            (int)s_applyIdx, (int)s_applyMods.size());
                s_applyAt = GetTickCount() + 150;   // 间隔一点，逐个回报，便于读日志
            }
        }

        // 把工具当前皮肤强调色推给游戏内的 MOD 窗口，保持同一套配色
        if (g_ldReady)
        {
            ImU32 ac = Th().accent;
            char hex[16];
            snprintf(hex, sizeof(hex), "#%02X%02X%02X",
                     (int)(ac & 0xFF), (int)((ac >> 8) & 0xFF), (int)((ac >> 16) & 0xFF));
            ModLoader::SetTheme(hex);
        }

        // 加载器识别到的 MOD 集合变化时（首次就绪 / 增减）补扫一次
        {
            std::string sig;
            for (auto& s : g_ld) sig += s.id + "|";
            if (sig != g_ldSig)
            {
                g_ldSig = sig;
                if (g_ldReady) { ScanMods(); ApplyLoaderStates(); }
            }
        }

        const float sp = ImGui::GetStyle().ItemSpacing.x;

        // 操作反馈（启用/停用、打开界面、应用目录……）
        if (!g_msg.empty())
        {
            TextCol(g_msgBad ? Th().bad : Th().good, "%s", g_msg.c_str());
            Space(7.f);
        }

        // 目录选择返回
        {
            char pick[MAX_PATH * 2] = { 0 };
            if (ShellAsync::TakeFolder(pick, sizeof(pick)))
            {
                if (s_pendingPick == 1)      snprintf(s_gameBuf, sizeof(s_gameBuf), "%s", pick);
                else if (s_pendingPick == 2) snprintf(s_modBuf, sizeof(s_modBuf), "%s", pick);
                s_pendingPick = 0;
            }
        }

        // ---------------- 目录卡片 ----------------
        BeginCard("##mod_paths");
        CardTitle(T(MS_PATH_CARD), nullptr);

        const float colX = LabelCol({ T(MS_GAMEDIR), T(MS_MODDIR) }, 96.f);

        LabelRow(T(MS_GAMEDIR), colX);
        InputText("##mod_gdir", s_gameBuf, sizeof(s_gameBuf), nullptr,
                  ImGui::GetContentRegionAvail().x);
        Space(4.f);
        LabelRow(T(MS_MODDIR), colX);
        {
            float w = ImGui::GetContentRegionAvail().x;
            InputText("##mod_mdir", s_modBuf, sizeof(s_modBuf), nullptr, w - 86.f);
            ImGui::SameLine();
            if (Button("##mod_mbrowse", T(MS_BROWSE), ImVec2(80.f, 26.f)))
                ShellAsync::PickFolder(T(MS_MODDIR));
        }
        TextFaint("%s", T(MS_MODDIR_HINT));
        Space(4.f);
        {
            float bw = (ImGui::GetContentRegionAvail().x - sp * 3.f) / 4.f;
            if (Button("##mod_apply", T(MS_APPLY), ImVec2(bw, 27.f), BTN_PRIMARY))
            {
                // 应用：游戏目录 + MOD 目录一起落地，并让游戏内加载器改用新目录
                if (s_gameBuf[0]) GameDir::Set(s_gameBuf);
                I18N::Prefs::SetStr("mod.dir", s_modBuf);
                I18N::Prefs::Save();
                ModLoader::SetModsDir(s_modBuf);
                ReloadPaths();
                ScanMods();
            }
            ImGui::SameLine();
            if (Button("##mod_refresh", T(MS_REFRESH), ImVec2(bw, 27.f)))
                ScanMods();
            ImGui::SameLine();
            if (Button("##mod_allon", T(MS_ENABLE_ALL), ImVec2(bw, 27.f)))
            {
                for (auto& m : g_mods)
                {
                    m.enabled = true;
                    I18N::Prefs::SetInt(StateKey(m.id).c_str(), 1);
                    ParamsWriteEnabled(m.id, true);
                    ModLoader::SetEnabled(m.id, true);
                }
                I18N::Prefs::Save();
                RecomputeEnabledCount();
            }
            ImGui::SameLine();
            if (Button("##mod_alloff", T(MS_DISABLE_ALL), ImVec2(bw, 27.f)))
            {
                for (auto& m : g_mods)
                {
                    m.enabled = false;
                    I18N::Prefs::SetInt(StateKey(m.id).c_str(), 0);
                    ParamsWriteEnabled(m.id, false);
                    ModLoader::SetEnabled(m.id, false);
                }
                I18N::Prefs::Save();
                RecomputeEnabledCount();
            }
        }
        Space(4.f);
        TextFaint("%s", T(MS_PROFILE_HINT));
        EndCard();

        Space(9.f);

        // ---------------- UMM 加载器（真实加载 MOD 的前提） ----------------
        BeginCard("##mod_loader");
        CardTitle(T(MS_LOADER), nullptr);
        {
            // 第一行：加载器是否已经在游戏里跑起来 —— 这才是“MOD 真的被加载”的依据
            const bool ready = g_ldReady;
            if (ModLoader::Passive())
            {
                // 原版 UMM 在场：显式告诉用户现在是复用模式，不会二次加载
                TextCol(Th().accent, "%s", T(MS_LD_PASSIVE));
            }
            else
            {
                TextCol(ready ? Th().good : Th().warn, "%s",
                        ready ? T(MS_LD_READY) : T(MS_LD_OFFLINE));
            }
            if (!ready && !ModLoader::Passive())
                TextFaint("%s", T(MS_LD_TIP));
            else if (!g_ldErr.empty())
                TextCol(Th().bad, "%s", g_ldErr.c_str());
            Space(3.f);
            {
                const bool files = ModLoader::Installed();
                TextCol(files ? Th().good : Th().bad, "%s",
                        files ? T(MS_LOADER_ON) : T(MS_LOADER_OFF));
            }
            TextFaint("%s  %s", T(MS_MODDIR), ResolveModsDir().c_str());
            if (g_logTotal >= 0)
            {
                TextCol(Th().good, "%s", Fmt(T(MS_LOG_SUM), g_logOk, g_logTotal).c_str());
                if (!g_logTime.empty())
                    TextFaint("%s", Fmt(T(MS_LOG_TIME), g_logTime.c_str()).c_str());
            }
            else
            {
                TextFaint("%s", T(MS_LOG_NOLOG));
            }
            if (!G_ldMsg.empty())
            {
                Space(2.f);
                TextCol(G_ldMsgBad ? Th().bad : Th().good, "%s", G_ldMsg.c_str());
            }
            Space(7.f);
            float bw = (ImGui::GetContentRegionAvail().x - sp * 2.f) / 3.f;
            if (Button("##ld_install", T(MS_LOADER_INSTALL), ImVec2(bw, 27.f), BTN_PRIMARY))
            {
                std::string msg;
                bool ok = ModLoader::Install(&msg);
                G_ldMsg = std::string(ok ? T(MS_LOADER_DONE) : T(MS_LOADER_FAIL)) + "  " + msg;
                G_ldMsgBad = !ok;
            }
            ImGui::SameLine();
            if (Button("##ld_reload", T(MS_RELOAD), ImVec2(bw, 27.f)))
                ModLoader::Reload();
            ImGui::SameLine();
            if (Button("##ld_open", T(MS_OPENGAME), ImVec2(bw, 27.f)))
                ShellAsync::Open(GameDir::Get());
        }
        EndCard();
        Space(9.f);

        CardHint(T(MS_LD_TIP));
        Space(7.f);

        // ---------------- MOD 卡片列表（可无限向下） ----------------
        if (g_mods.empty())
        {
            BeginCard("##mod_empty");
            TextCol(Th().warn, "%s", DirExists(g_modsDirCache) ? T(MS_EMPTY) : T(MS_NODIR));
            EndCard();
        }
        else
        {
            for (int i = 0; i < (int)g_mods.size(); i++)
            {
                DrawModCard(i);
                Space(7.f);
            }
        }
    }

    // ============================================================
    //  模态层
    // ============================================================
    bool ModalActive() { return s_modal != MD_NONE; }

    // ---------------- 对话框外壳（自绘标题栏 + 右上角 X） ----------------
    namespace
    {
        bool DlgCloseX(const char* id, float size)
        {
            ImVec2 p = ImGui::GetCursorScreenPos();
            bool clicked = ImGui::InvisibleButton(id, ImVec2(size, size));
            if (clicked)   // 幽灵点击过滤：按下点必须落在这个按钮里
            {
                const ImGuiIO& io = ImGui::GetIO();
                const ImVec2 c = io.MouseClickedPos[0];
                clicked = io.MouseClickedTime[0] > 0.f &&
                          c.x >= p.x - 3.f && c.x <= p.x + size + 3.f &&
                          c.y >= p.y - 3.f && c.y <= p.y + size + 3.f;
            }
            bool hov = ImGui::IsItemHovered();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (hov) dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), IM_COL32(178, 74, 82, 140), 4.f);
            float r = size * 0.27f;
            ImVec2 c(p.x + size * 0.5f, p.y + size * 0.5f);
            ImU32 col = hov ? IM_COL32(255, 255, 255, 240) : A(Th().textDim, 1.f);
            dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), col, 1.7f);
            dl->AddLine(ImVec2(c.x + r, c.y - r), ImVec2(c.x - r, c.y + r), col, 1.7f);
            return clicked;
        }

        // 对话框外壳：全屏压暗 + 居中面板 + 标题栏（标题 + X）。
        // 返回 false = 用户点了 X；进入后本帧必须调用 DlgEnd()。
        bool DlgBegin(const char* bodyId, const char* title, float cw, float ch)
        {
            ImGuiIO& io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2(0.f, 0.f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
            ImGuiWindowFlags fl = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoCollapse |
                                  ImGuiWindowFlags_NoSavedSettings |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
            ImGui::Begin("##mm_dlg_root", nullptr, fl);
            ImGui::PopStyleVar(2);

            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(ImVec2(0.f, 0.f), io.DisplaySize, IM_COL32(4, 4, 7, 168));
            ImGui::SetCursorPos(ImVec2(0.f, 0.f));
            ImGui::InvisibleButton("##mm_dlg_block", io.DisplaySize);

            float x = (io.DisplaySize.x - cw) * 0.5f;
            float y = (io.DisplaySize.y - ch) * 0.5f;
            if (x < 8.f) x = 8.f;
            if (y < 8.f) y = 8.f;
            ImVec2 p0(x, y), p1(x + cw, y + ch);
            dl->AddRectFilled(p0, p1, Th().winBg, 10.f);
            dl->AddRect(p0, p1, A(Th().line, 0.9f), 10.f);
            dl->AddRectFilled(p0, ImVec2(p1.x, p0.y + 36.f), Th().headBg, 10.f,
                              ImDrawFlags_RoundCornersTop);
            dl->AddLine(ImVec2(p0.x, p0.y + 36.f), ImVec2(p1.x, p0.y + 36.f), A(Th().line, 0.9f));

            ImFont* f = ImGui::GetFont();
            dl->AddText(f, 16.f, ImVec2(p0.x + 16.f, p0.y + 10.f), Th().text, title);

            ImGui::SetCursorPos(ImVec2(x + cw - 32.f, y + 8.f));
            bool open = true;
            if (DlgCloseX("##mm_dlg_x", 20.f)) open = false;

            ImGui::SetCursorPos(ImVec2(x + 16.f, y + 46.f));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.f, 9.f));
            ImGui::BeginChild(bodyId, ImVec2(cw - 32.f, ch - 58.f), 0, 0);
            return open;
        }

        void DlgEnd()
        {
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::End();
        }
    } // namespace

    void DrawModal()
    {
        if (s_modal == MD_NONE) return;

        // ---------------- 删除确认 ----------------
        if (s_modal == MD_DELETE)
        {
            const float cw = 470.f, ch = 190.f;
            bool open = DlgBegin("##dlg_del_body", T(MS_DEL_TITLE), cw, ch);
            const char* nm = (s_delIndex >= 0 && s_delIndex < (int)g_mods.size())
                                 ? g_mods[s_delIndex].name.c_str() : "";
            TextWrapCol(Th().textDim, T(MS_DEL_ASK), nm);
            Space(16.f);
            const float sp = ImGui::GetStyle().ItemSpacing.x;
            float bw = (ImGui::GetContentRegionAvail().x - sp) * 0.5f;
            bool doDel = false;
            if (Button("##del_no", T(MS_CANCEL), ImVec2(bw, 30.f))) open = false;
            ImGui::SameLine();
            if (Button("##del_yes", T(MS_YES), ImVec2(bw, 30.f), BTN_DANGER)) { doDel = true; open = false; }
            DlgEnd();
            if (doDel)       { int idx = s_delIndex; s_modal = MD_NONE; s_delIndex = -1; DeleteMod(idx); }
            else if (!open)  { s_modal = MD_NONE; s_delIndex = -1; }
            return;
        }

        // ---------------- MOD 设置 ----------------
        if (s_setIndex < 0 || s_setIndex >= (int)g_mods.size()) { s_modal = MD_NONE; return; }
        ModInfo& m = g_mods[s_setIndex];

        const float cw = 700.f, ch = 500.f;
        bool open = DlgBegin("##dlg_set_body",
                             Fmt(T(MS_SET_TITLE), m.name.c_str()).c_str(), cw, ch);

        {
            std::string meta = m.id;
            if (!m.version.empty()) meta += "  ·  v" + m.version;
            if (!m.author.empty())  meta += "  ·  " + m.author;
            if (!m.managerVersion.empty()) meta += "  ·  UMM " + m.managerVersion;
            TextFaint("%s", meta.c_str());
        }
        Space(8.f);

        if (m.settings.empty())
        {
            TextCol(Th().warn, "%s", T(MS_SET_NONE));
            Space(12.f);
            const float sp = ImGui::GetStyle().ItemSpacing.x;
            float bw = (ImGui::GetContentRegionAvail().x - sp) * 0.5f;
            if (Button("##set_openfolder", T(MS_OPENFOLDER), ImVec2(bw, 28.f)))
                ShellAsync::Open(m.folderPath.c_str());
            ImGui::SameLine();
            if (Button("##set_close0", T(MS_CLOSE), ImVec2(bw, 28.f), BTN_PRIMARY))
                open = false;
            DlgEnd();
            if (!open) s_modal = MD_NONE;
            return;
        }

        // 设置文件选择
        {
            const char* names[64];
            int n = (int)m.settings.size();
            if (n > 64) n = 64;
            for (int k = 0; k < n; k++) names[k] = m.settings[k].name.c_str();
            LabelRow(T(MS_SET_FILE), 96.f);
            int cur = s_setFileIdx;
            if (Combo("##set_file", &cur, names, n, ImGui::GetContentRegionAvail().x))
            {
                s_setFileIdx = cur;
                s_setDirty = false;
                s_setStatus.clear();
                EnsureSettingsLoaded(s_setIndex);
            }
        }
        Space(7.f);

        {
            SetFile& sf = m.settings[(s_setFileIdx >= 0 && s_setFileIdx < (int)m.settings.size())
                                         ? s_setFileIdx : 0];
            float listH = ImGui::GetContentRegionAvail().y - 42.f;
            if (listH < 90.f) listH = 90.f;
            ImGui::BeginChild("##set_list", ImVec2(-1.f, listH),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, 0);
            if (sf.items.empty())
            {
                TextFaint("%s", T(MS_SET_NOFILE));
            }
            else
            {
                const float colX = LabelCol({ "XXXXXXXXXXXXXXXXXXXXXXXX" }, 120.f);
                for (int k = 0; k < (int)sf.items.size(); k++)
                {
                    SetItem& it = sf.items[k];
                    ImGui::PushID(k);
                    if (it.type == 0)
                    {
                        bool v = (ToLower(it.val) == "true");
                        if (CheckRow("##b", it.key.c_str(), &v))
                        {
                            it.val = v ? "true" : "false";
                            s_setDirty = true;
                        }
                    }
                    else
                    {
                        LabelRow(it.key.c_str(), colX);
                        float w = ImGui::GetContentRegionAvail().x;
                        if (w < 80.f) w = 80.f;
                        if (InputText("##v", it.edit, sizeof(it.edit), nullptr, w))
                        {
                            it.val = it.edit;
                            s_setDirty = true;
                        }
                    }
                    Space(3.f);
                    ImGui::PopID();
                }
            }
            ImGui::EndChild();
        }

        Space(8.f);
        if (!s_setStatus.empty())
        {
            TextCol(s_setDirty ? Th().warn : Th().good, "%s", s_setStatus.c_str());
            Space(4.f);
        }
        {
            const float sp = ImGui::GetStyle().ItemSpacing.x;
            float bw = (ImGui::GetContentRegionAvail().x - sp * 2.f) / 3.f;
            if (Button("##set_save", T(MS_SET_SAVE), ImVec2(bw, 28.f), BTN_PRIMARY))
            {
                SetFile& sf2 = m.settings[(s_setFileIdx >= 0 && s_setFileIdx < (int)m.settings.size())
                                              ? s_setFileIdx : 0];
                bool ok = SaveSetFile(sf2);
                if (ok)
                {
                    s_setDirty = false;
                    ModLoader::ApplySettings(m.id, true);   // 热应用进运行中的 MOD
                    s_setStatus = std::string(Fmt(T(MS_SET_SAVED), sf2.name.c_str())) + "  ·  " + T(MS_APPLYING);
                }
                else s_setStatus = T(MS_SET_NOFILE);
            }
            ImGui::SameLine();
            if (Button("##set_openf2", T(MS_OPENFOLDER), ImVec2(bw, 28.f)))
                ShellAsync::Open(m.folderPath.c_str());
            ImGui::SameLine();
            if (Button("##set_close2", T(MS_CLOSE), ImVec2(bw, 28.f)))
                open = false;
        }

        DlgEnd();
        if (!open) s_modal = MD_NONE;
    }

    // ============================================================
    //  配置档案 / 启动应用
    // ============================================================
    void ApplyStartupStates()
    {
        // 让游戏内加载器也切到同一个 MOD 目录 / 同一套启用状态
        ModLoader::SetModsDir(ResolveModsDir());
        for (auto& m : g_mods)
        {
            int v = I18N::Prefs::GetInt(StateKey(m.id).c_str(), -1);
            bool on = (v < 0) ? m.enabled : (v != 0);
            m.enabled = on;
            ParamsWriteEnabled(m.id, on);
            ModLoader::SetEnabled(m.id, on);
        }
        RecomputeEnabledCount();
        Log::Printf("[mod] startup states applied (%d mods, params=%s)",
                    (int)g_mods.size(), ParamsXmlPath().c_str());
    }

    void ProfileWrite(FILE* f)
    {
        if (!f) return;
        fprintf(f, "mod.dir=%s\n", ResolveModsDir().c_str());
        for (auto& m : g_mods)
        {
            fprintf(f, "mod.state.%s=%d\n", m.id.c_str(), m.enabled ? 1 : 0);
            // MOD 自身的设置项一并写进档案：下次读档可整体还原
            for (auto& sf : m.settings)
            {
                LoadSetFileItems(sf);
                for (auto& it : sf.items)
                {
                    std::string v = it.val;
                    for (size_t k = 0; k < v.size(); k++)
                        if (v[k] == '\r' || v[k] == '\n') v[k] = ' ';
                    fprintf(f, "mod.set.%s|%s|%s=%s\n", m.id.c_str(), sf.name.c_str(),
                            it.key.c_str(), v.c_str());
                }
            }
        }
    }

    bool ProfileReadKey(const std::string& key, const std::string& val)
    {
        if (key == "mod.dir")
        {
            I18N::Prefs::SetStr("mod.dir", val.c_str());
            s_pathInit = false;
            s_rescan = true;
            return true;
        }
        if (key.rfind("mod.state.", 0) == 0)
        {
            I18N::Prefs::SetInt(key.c_str(), atoi(val.c_str()));
            s_rescan = true;
            return true;
        }
        if (key.rfind("mod.set.", 0) == 0)
        {
            // mod.set.<MOD Id>|<设置文件>|<键>=<值>
            std::string body = key.substr(8);
            size_t p1 = body.find('|');
            if (p1 == std::string::npos) return true;
            size_t p2 = body.find('|', p1 + 1);
            if (p2 == std::string::npos) return true;
            PendingSet ps;
            ps.id   = body.substr(0, p1);
            ps.file = body.substr(p1 + 1, p2 - p1 - 1);
            ps.key  = body.substr(p2 + 1);
            ps.val  = val;
            g_pendingSets.push_back(ps);
            return true;
        }
        return false;
    }

    // 把档案里的 MOD 设置值写回各自的设置文件（读档时调用一次）
    void ApplyPendingSets()
    {
        if (g_pendingSets.empty()) return;
        std::vector<std::string> touched;
        for (auto& ps : g_pendingSets)
        {
            for (auto& m : g_mods)
            {
                if (m.id != ps.id) continue;
                bool hit = false;
                for (auto& sf : m.settings)
                {
                    if (!ps.file.empty() && sf.name != ps.file) continue;
                    LoadSetFileItems(sf);
                    for (auto& it : sf.items)
                    {
                        if (it.key != ps.key) continue;
                        it.val = ps.val;
                        snprintf(it.edit, sizeof(it.edit), "%s", it.val.c_str());
                        SaveSetFile(sf);
                        hit = true;
                        break;
                    }
                }
                if (hit)
                {
                    bool dup = false;
                    for (auto& id : touched) if (id == m.id) { dup = true; break; }
                    if (!dup) touched.push_back(m.id);
                }
            }
        }
        // 文件写完了，同样让加载器把值热应用进 MOD（MOD 还没加载时会自动补应用）
        for (auto& id : touched) ModLoader::ApplySettings(id, false);
        g_pendingSets.clear();
    }

    void OnProfileApplied()
    {
        I18N::Prefs::Save();
        ScanMods();
        ApplyStartupStates();
        ApplyPendingSets();
    }
} // namespace ModManager
