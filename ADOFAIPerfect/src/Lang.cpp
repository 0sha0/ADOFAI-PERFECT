// ============================================================
// Lang.cpp — i18n 实现（表驱动）
//   每行一条：{ 简中, 繁中, English, 日本語, Русский }
//   后续更新只需在 g_table 里按同名枚举顺序追加，或用 I18N::Register 动态注册。
// ============================================================
#include "Lang.h"
#include "Log.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <cstdlib>

namespace I18N
{
    static_assert((int)BUILTIN_N > 0, "strings");

    struct Row { const char* t[LANG_N]; };
    static const Row g_table[] = {
        /* TAB_* */
        { { "功能", "功能", "Features", "機能", "Функции" } },
        { { "状态", "狀態", "Status", "状態", "Статус" } },
        { { "读谱", "讀譜", "Sightread", "譜面読み", "Чтение" } },
        { { "4K", "4K", "4K", "4K", "4K" } },
        { { "5K", "5K", "5K", "5K", "5K" } },
        { { "6K", "6K", "6K", "6K", "6K" } },
        { { "10K", "10K", "10K", "10K", "10K" } },
        { { "\u6309\u952e", "\u6309\u9375", "Keys", "\u30ad\u30fc", "\u041a\u043b\u0430\u0432\u0438\u0448\u0438" } },
        { { "设置", "設定", "Settings", "設定", "Настройки" } },
        { { "MOD 加载器", "MOD 載入器", "Mod Loader", "MODローダー", "Загрузчик MOD" } },
        { { "Lua 脚本", "Lua 腳本", "Lua Scripts", "Luaスクリプト", "Скрипты Lua" } },
        /* 功能页 */
        { { "不死模式", "不死模式", "No Death", "無敵モード", "Бессмертие" } },
        { { "自动连击", "自動連擊", "Auto Combo", "自動コンボ", "Авто-комбо" } },
        { { "卸载模组并还原", "卸載模組並還原", "Unload & Restore", "アンロードして復元", "Выгрузить и восстановить" } },
        { { "重置", "重設", "Reset", "リセット", "Сброс" } },
        /* 通用标签 */
        { { "流速", "流速", "Speed", "速度", "Скорость" } },
        { { "延迟", "延遲", "Offset", "オフセット", "Задержка" } },
        { { "转换风格", "轉換風格", "Conversion", "変換スタイル", "Стиль" } },
        { { "键位", "鍵位", "Keys", "キー", "Клавиши" } },
        { { "打击特效", "打擊特效", "Hit FX", "ヒット演出", "Эффекты удара" } },
        { { "判定提示", "判定提示", "Judgement", "判定表示", "Оценка" } },
        { { "游戏画面小窗（左下角）", "遊戲畫面小窗（左下角）", "Game preview (bottom-left)", "ゲーム画面小窓（左下）", "Окно игры (слева внизу)" } },
        { { "位置", "位置", "Position", "位置", "Позиция" } },
        { { "大小", "大小", "Size", "サイズ", "Размер" } },
        { { "微调", "微調", "Fine tune", "微調整", "Точная настройка" } },
        { { "不透明度", "不透明度", "Opacity", "不透明度", "Прозрачность" } },
        /* 辅助读谱 */
        { { "辅助读谱（无轨）", "輔助讀譜（無軌）", "Sightread Assist (no lanes)", "譜面読み補助（レーンなし）", "Помощь чтению (без дорожек)" } },
        { { "不改谱、不代打：按「间隔 = 1 + 转角/180° 拍」把无轨谱面标成拍数谱，配行星角度盘 + 转向圆弧 + 特殊砖标记，专治大回转 / 换向 / 多押 / 中旋 / 变速读不出来。", "不改譜、不代打：按「間隔 = 1 + 轉角/180° 拍」把無軌譜面標成拍數譜，配行星角度盤 + 轉向圓弧 + 特殊磚標記，專治大迴轉 / 換向 / 多押 / 中旋 / 變速讀不出來。", "Read-only assist: marks the lane-less chart in beats (interval = 1 + turn/180°), with a live planet angle dial, turn arcs and special-tile marks. No chart edits, no auto-play.", "譜面を書き換えず代打もしない：間隔＝1＋回転角/180°拍で拍数譜を表示し、惑星角度盤＋回転アーク＋特殊タイル標記で読みやすくする。", "Только чтение: разметка в долях (интервал = 1 + угол/180°), диск угла планеты, дуги поворотов и метки плиток. Без правок чарта и автоплея." } },
        { { "布局", "佈局", "Layout", "レイアウト", "Раскладка" } },
        { { "侧栏面板", "側欄面板", "Side panel", "サイドパネル", "Боковая панель" } },
        { { "底部横条（打歌时推荐）", "底部橫條（打歌時推薦）", "Bottom bar (recommended)", "下部バー（推奨）", "Нижняя полоса (реком.)" } },
        { { "顶部横条", "頂部橫條", "Top bar", "上部バー", "Верхняя полоса" } },
        { { "预告块数", "預告塊數", "Lookahead tiles", "予告タイル数", "Плиток вперёд" } },
        { { "时间密度", "時間密度", "Time density", "時間密度", "Плотность времени" } },
        { { "拍点网格", "拍點網格", "Beat grid", "拍グリッド", "Сетка бита" } },
        { { "转向轨迹盘", "轉向軌跡盤", "Turn dial", "回転ダイヤル", "Диск поворота" } },
        { { "间隔分数", "間隔分數", "Rhythm ratio", "リズム比", "Ритм-доля" } },
        { { "特殊砖标记", "特殊磚標記", "Tile marks", "特殊タイル表示", "Метки плиток" } },
        { { "技术提示", "技術提示", "Trick hint", "技術ヒント", "Подсказка" } },
        { { "延迟校准", "延遲校準", "Calibration", "遅延調整", "Калибровка" } },
        { { "未读到谱面：进入任意关卡（含自定义 / 额外关卡）后自动解析。", "未讀到譜面：進入任意關卡（含自訂 / 額外關卡）後自動解析。", "No chart yet: enter any level (custom / extra supported) to parse.", "譜面未検出：レベル開始後に自動解析します（カスタム対応）。", "Чарт не найден: войдите в уровень (в т.ч. кастомный)." } },
        /* KeyViewer */
        { { "按键反馈", "按鍵回饋", "Key feedback", "キー入力表示", "Отображение клавиш" } },
        { { "竖排", "豎排", "Vertical", "縦並び", "Вертикально" } },
        { { "显示 KPS", "顯示 KPS", "Show KPS", "KPS 表示", "Показ KPS" } },
        { { "显示次数", "顯示次數", "Show count", "回数表示", "Показ счётчика" } },
        { { "显示总 KPS", "顯示總 KPS", "Show total KPS", "合計 KPS", "Общий KPS" } },
        { { "添加按键", "新增按鍵", "Add key", "キー追加", "Добавить клавишу" } },
        { { "恢复默认", "恢復預設", "Restore default", "既定に戻す", "По умолчанию" } },
        { { "按下新键…", "按下新鍵…", "Press a key...", "新しいキーを押す…", "Нажмите клавишу..." } },
        { { "删除", "刪除", "Delete", "削除", "Удалить" } },
        { { "像网上的 KeyViewer 一样实时反馈按键：按键亮起、次数、KPS；键位可自定义。", "像網上的 KeyViewer 一樣即時回饋按鍵：按鍵亮起、次數、KPS；鍵位可自訂。", "Real-time key feedback like the classic KeyViewer: highlights, counts, KPS, custom keys.", "定番 KeyViewer と同じ入力表示：ハイライト・回数・KPS、キーは自由に設定可。", "Реальное время: подсветка, счётчики, KPS, свои клавиши." } },
        /* 设置页 */
        { { "设置", "設定", "Settings", "設定", "Настройки" } },
        { { "语言 / Language", "語言 / Language", "Language", "言語 / Language", "Язык" } },
        { { "打开配置文件夹", "開啟設定資料夾", "Open config folder", "設定フォルダを開く", "Открыть папку конфигурации" } },
        { { "关于", "關於", "About", "このアプリについて", "О программе" } },
        { { "ADOFAI-PERFECT · 4K/5K/6K/10K 下坠辅助 + 无轨读谱 + KeyViewer。仅供学习与个人练习使用。", "ADOFAI-PERFECT · 4K/5K/6K/10K 下墜輔助 + 無軌讀譜 + KeyViewer。僅供學習與個人練習使用。", "ADOFAI-PERFECT: 4K/5K/6K/10K falling assist + sightread + KeyViewer. For personal practice only.", "ADOFAI-PERFECT：4K/5K/6K/10K 落下補助＋譜面読み＋KeyViewer。個人練習用。", "ADOFAI-PERFECT: помощник 4K/5K/6K/10K + чтение + KeyViewer. Только для личной практики." } },
        { { "语言修改立即生效；后续版本可直接用 I18N::Register 追加新字符串。", "語言修改立即生效；後續版本可直接用 I18N::Register 追加新字串。", "Language applies instantly; future builds can append strings via I18N::Register.", "言語変更は即時反映。今後の更新は I18N::Register で文字列追加可能。", "Язык применяется сразу; новые строки добавляются через I18N::Register." } },
        { { "打歌时建议用「底部横条」：判定区视线不用离开，横条按真实时间从左往右滚动。", "打歌時建議用「底部橫條」：判定區視線不用離開，橫條按真實時間由左往右捲動。", "Use the bottom bar while playing: it scrolls left-to-right by real time, staying near your focus.", "プレイ中は下部バー推奨：視線を外さず、実時間で左から右へ流れます。", "В игре используйте нижнюю полосу: она движется слева направо по реальному времени." } },
        { { "关卡", "關卡", "Level", "レベル", "Уровень" } },
        { { "状态", "狀態", "State", "状態", "Состояние" } },
        { { "进度", "進度", "Progress", "進捗", "Прогресс" } },
        { { "精准度", "精準度", "Accuracy", "精度", "Точность" } },
        { { "死亡", "死亡", "Deaths", "死亡", "Смерти" } },
        { { "检查点", "檢查點", "Checkpoints", "チェックポイント", "Чекпойнты" } },
        { { "正在连接游戏...", "正在連線遊戲...", "Connecting to game...", "ゲームに接続中...", "Подключение к игре..." } },
        { { "未检测到关卡，请进入任意关卡", "未檢測到關卡，請進入任意關卡", "No level detected, enter any level", "レベルが未検出。任意のレベルへ", "Уровень не найден" } },
        { { "提示：开关已在后台生效，进入关卡后自动应用。", "提示：開關已在後台生效，進入關卡後自動應用。", "Toggles apply in the background and take effect when you enter a level.", "ヒント：スイッチはバックグラウンドで有効、レベル開始時に適用。", "Переключатели применяются в уровне." } },
        { { "游戏中", "遊戲中", "In game", "プレイ中", "В игре" } },
        { { "（未知）", "（未知）", "(unknown)", "（不明）", "(неизвестно)" } },
        { { "Insert 显隐 · End 卸载 · 可拖动标题栏", "Insert 顯隱 · End 卸載 · 可拖動標題列", "Insert toggle · End unload · drag the title bar", "Insert 表示 · End アンロード · タイトルバーで移動", "Insert показ · End выгрузка · тяните заголовок" } },
        { { "底板", "底板", "Board", "背景ボード", "Фон" } },
        { { "修改", "修改", "Edit", "編集", "Изменить" } },
        { { "确定", "確定", "OK", "OK", "OK" } },
        { { "取消", "取消", "Cancel", "キャンセル", "Отмена" } },
        { { "恢复默认", "恢復預設", "Restore defaults", "既定に戻す", "По умолчанию" } },
        { { "点击槽位后按新键", "點擊槽位後按新鍵", "Click a slot, then press a key", "スロットをクリックしてキーを押す", "Нажмите ячейку, затем клавишу" } },
        { { "应用", "套用", "Apply", "適用", "Применить" } },
        { { "键位重复", "鍵位重複", "Duplicate keys", "キーが重複", "Дублирование клавиш" } },
        { { "请按新键...", "請按新鍵...", "Press a key...", "新しいキーを押してください...", "Нажмите клавишу..." } },
        { { "缩放", "縮放", "Scale", "拡大縮小", "Масштаб" } },
        { { "编辑键位", "編輯鍵位", "Edit keys", "キー編集", "Изменить клавиши" } },
        { { "打歌时按键反馈自动显示在游戏画面上（不受菜单显隐影响）。", "打歌時按鍵回饋自動顯示在遊戲畫面上（不受選單顯隱影響）。", "During play the key feedback overlays the game automatically (independent of menu visibility).", "プレイ中は自動でゲーム画面に重ねて表示されます。", "Во время игры оверлей показывается автоматически." } },
        { { "左上", "左上", "Top-left", "左上", "Сверху слева" } },
        { { "右上", "右上", "Top-right", "右上", "Сверху справа" } },
        { { "左下", "左下", "Bottom-left", "左下", "Снизу слева" } },
        { { "右下", "右下", "Bottom-right", "右下", "Снизу справа" } },
        { { "总击键", "總擊鍵", "Total presses", "合計キー", "Всего нажатий" } },
        { { "按键雨", "按鍵雨", "Key rain", "キーレイン", "Дождь клавиш" } },
        { { "自动换行", "自動換行", "Auto wrap", "自動改行", "Перенос строк" } },
        { { "每行键数", "每行鍵數", "Keys per row", "1行のキー数", "Клавиш в строке" } },
        { { "重置统计", "重置統計", "Reset stats", "統計リセット", "Сбросить статистику" } },
        { { "快速预设", "快速預設", "Presets", "プリセット", "Пресеты" } },
        { { "最高", "最高", "Max", "最高", "Макс" } },
        { { "平均", "平均", "Avg", "平均", "Среднее" } },
        /* 判定宽度 / 偏差校准 / KeyViewer 面板 */
        { { "判定宽度", "判定寬度", "Judgment", "判定幅", "Окно суда" } },
        { { "严判", "嚴判", "Strict", "厳格", "Строгий" } },
        { { "标准", "標準", "Normal", "標準", "Обычный" } },
        { { "宽判", "寬判", "Lenient", "寛容", "Мягкий" } },
        { { "命中偏差", "命中偏差", "Hit bias", "判定の偏り", "Смещение" } },
        { { "校准", "校準", "Calibrate", "キャリブレーション", "Калибровка" } },
        { { "快", "快", "Early", "早い", "Рано" } },
        { { "慢", "慢", "Late", "遅い", "Поздно" } },
        { { "开启菜单后可拖动调整位置", "開啟選單後可拖動調整位置", "Drag to reposition (menu open)", "メニュー表示中はドラッグで移動", "Перетащите при открытом меню" } },
        { { "背景面板", "背景面板", "Panel", "背景パネル", "Панель" } },
        { { "上隐", "上隐", "Upper hide", "上隠し", "Скрыть верх" } },
        { { "下隐", "下隐", "Lower hide", "下隠し", "Скрыть низ" } },
        { { "自动调整延迟", "自動調整延遲", "Auto offset", "自動遅延調整", "Автосмещение" } },
        /* MOD 加载器 / Lua 脚本页 */
        { { "Lua 脚本", "Lua 腳本", "Lua Scripts", "Luaスクリプト", "Скрипты Lua" } },
        { { "随工具启动", "隨工具啟動", "Autorun", "自動実行", "Автозапуск" } },
        { { "未找到脚本：把 .lua 放进 mods\\scripts\\ 目录。", "未找到腳本：把 .lua 放進 mods\\scripts\\ 目錄。", "No scripts: put .lua files into mods\\scripts\\.", ".lua を mods\\scripts\\ に置いてください。", "Нет скриптов: положите .lua в mods\\scripts\\." } },
        { { "控制台", "控制台", "Console", "コンソール", "Консоль" } },
        { { "MOD 加载器", "MOD 載入器", "Mod Loader", "MODローダー", "Загрузчик MOD" } },
        { { "把 Mod 放进 mods\\<名称>\\，用 mod.json 描述；支持原生 DLL 与 Lua 脚本模组。", "把 Mod 放進 mods\\<名稱>\\，用 mod.json 描述；支援原生 DLL 與 Lua 腳本模組。", "Put mods in mods\\<name>\\ described by mod.json; native DLL and Lua mods supported.", "mods\\<名前>\\ に mod.json を置きます。ネイティブ DLL と Lua に対応。", "Mods в mods\\<имя>\\ с mod.json; поддерживаются DLL и Lua." } },
        { { "启用", "啟用", "Enabled", "有効", "Включён" } },
        { { "未找到 Mod。", "未找到 Mod。", "No mods found.", "MODが見つかりません。", "Моды не найдены." } },
        { { "原生 Mod 导出 AdofMod_Load/Unload/OnImGui/OnUpdate；也可用 GetProcAddress(\"AdofPerfect_GetApi\") 取到同一份 API。", "原生 Mod 匯出 AdofMod_Load/Unload/OnImGui/OnUpdate；也可用 GetProcAddress(\"AdofPerfect_GetApi\") 取得同一份 API。", "Native mods export AdofMod_Load/Unload/OnImGui/OnUpdate; GetProcAddress(\"AdofPerfect_GetApi\") returns the same API.", "ネイティブ Mod は AdofMod_Load/Unload/OnImGui/OnUpdate を公開。AdofPerfect_GetApi も利用可。", "Нативные моды экспортируют AdofMod_Load/Unload/OnImGui/OnUpdate; AdofPerfect_GetApi доступен." } },
        { { "刷新", "重新整理", "Refresh", "更新", "Обновить" } },
        { { "打开目录", "開啟目錄", "Open folder", "フォルダを開く", "Открыть папку" } },
        { { "全部运行", "全部執行", "Run all", "すべて実行", "Запустить все" } },
        { { "全部停止", "全部停止", "Stop all", "すべて停止", "Остановить все" } },
        { { "清空", "清空", "Clear", "クリア", "Очистить" } },
        { { "清屏", "清屏", "Clear log", "ログ消去", "Очистить лог" } },
        { { "运行", "執行", "Run", "実行", "Запуск" } },
        { { "停止", "停止", "Stop", "停止", "Стоп" } },
        { { "重载", "重載", "Reload", "再読み込み", "Перезапуск" } },
        { { "执行", "執行", "Exec", "実行", "Выполнить" } },
        { { "已停止", "已停止", "Stopped", "停止中", "Остановлен" } },
        { { "运行中", "執行中", "Running", "実行中", "Работает" } },
        { { "加载失败", "載入失敗", "Load failed", "読み込み失敗", "Ошибка загрузки" } },
        /* UMM 规范 MOD */
        { { "设置界面", "設定介面", "Settings", "設定", "Настройки" } },
        { { "加载中…", "載入中…", "Loading...", "読み込み中…", "Загрузка…" } },
        { { "把社区 UMM 规范 MOD（含 Info.json 的目录）直接放进 mods 目录即可加载：CheryTools / Iridium / Creplay / Together / YqlossClientHarmony 等。",
            "把社群 UMM 規範 MOD（含 Info.json 的目錄）直接放進 mods 目錄即可載入：CheryTools / Iridium / Creplay / Together / YqlossClientHarmony 等。",
            "UMM-spec mods (folders with Info.json) load straight from the mods folder: CheryTools / Iridium / Creplay / Together / YqlossClientHarmony.",
            "UMM規格のMOD（Info.json 入りフォルダ）は mods フォルダに入れるだけで読み込めます：CheryTools / Iridium / Creplay / Together / YqlossClientHarmony など。",
            "Моды формата UMM (папка с Info.json) читаются прямо из папки mods: CheryTools / Iridium / Creplay / Together / YqlossClientHarmony и др." } },
        { { "缺少 UMM 兼容层：请确认 <DLL目录>\\umm\\UnityModManager.dll 与 0Harmony.dll 存在。",
            "缺少 UMM 相容層：請確認 <DLL目錄>\\umm\\UnityModManager.dll 與 0Harmony.dll 存在。",
            "UMM runtime missing: ensure <DLL dir>\\umm\\UnityModManager.dll and 0Harmony.dll exist.",
            "UMM互換ランタイムがありません：<DLLフォルダ>\\umm に UnityModManager.dll と 0Harmony.dll を置いてください。",
            "Нет среды UMM: проверьте <папка DLL>\\umm\\UnityModManager.dll и 0Harmony.dll." } },
        /* ---- 新增字符串（顺序必须与 Lang.h 枚举完全一致）---- */
        { { "皮肤", "皮膚", "Skins", "スキン", "Скины" } },                                   // TAB_SKIN
        { { "游戏目录", "遊戲目錄", "Game folder", "ゲームフォルダ", "Папка игры" } },              // ST_GAMEDIR
        { { "冰与火之舞的安装目录（用于定位 mods 与皮肤）。留空则使用当前进程目录。",
            "冰與火之舞的安裝目錄（用於定位 mods 與皮膚）。留空則使用目前行程目錄。",
            "A Dance of Fire and Ice install folder (locates mods and skins). Empty = process folder.",
            "A Dance of Fire and Ice のインストール先（mods とスキンの場所）。空欄ならプロセスフォルダを使用。",
            "Папка установки A Dance of Fire and Ice (для mods и skins). Пусто = папка процесса." } },  // ST_GAMEDIR_DESC
        { { "浏览…", "瀏覽…", "Browse...", "参照...", "Обзор..." } },                            // ST_BROWSE
        { { "应用", "套用", "Apply", "適用", "Применить" } },                                   // ST_APPLY
        { { "打开", "開啟", "Open", "開く", "Открыть" } },                                      // ST_OPEN
        { { "已应用游戏目录", "已套用遊戲目錄", "Game folder applied", "ゲームフォルダを適用しました", "Папка игры применена" } },  // ST_GAMEDIR_OK
        { { "无效目录：未找到 A Dance of Fire and Ice_Data", "無效目錄：未找到 A Dance of Fire and Ice_Data",
            "Invalid folder: A Dance of Fire and Ice_Data not found", "無効なフォルダ：A Dance of Fire and Ice_Data がありません",
            "Неверная папка: A Dance of Fire and Ice_Data не найдена" } },                       // ST_GAMEDIR_BAD
        { { "配置档案", "設定檔", "Config profiles", "設定プロファイル", "Профили настроек" } },   // ST_CFG_TITLE
        { { "保存 / 加载 4K、5K、6K、10K 模式、键位与 MOD 加载器的整套配置。",
            "儲存 / 載入 4K、5K、6K、10K 模式、鍵位與 MOD 載入器的整套設定。",
            "Save / load the full 4K, 5K, 6K, 10K mode, key and mod-loader configuration.",
            "4K / 5K / 6K / 10K モード・キー・MODローダー設定をまとめて保存 / 読み込みできます。",
            "Сохраняйте / загружайте настройки режимов 4K, 5K, 6K, 10K, клавиш и загрузчика модов." } },  // ST_CFG_DESC
        { { "档案名", "設定檔名", "Profile name", "プロファイル名", "Имя профиля" } },            // ST_CFG_NAME
        { { "保存", "儲存", "Save", "保存", "Сохранить" } },                                    // ST_CFG_SAVE
        { { "加载", "載入", "Load", "読み込み", "Загрузить" } },                                 // ST_CFG_LOAD
        { { "删除", "刪除", "Delete", "削除", "Удалить" } },                                    // ST_CFG_DEL
        { { "打开配置目录", "開啟設定目錄", "Open config folder", "設定フォルダを開く", "Открыть папку конфигураций" } },  // ST_CFG_OPEN
        { { "已保存", "已儲存", "Saved", "保存しました", "Сохранено" } },                        // ST_CFG_SAVED
        { { "已加载", "已載入", "Loaded", "読み込みました", "Загружено" } },                      // ST_CFG_LOADED
        { { "操作失败", "操作失敗", "Failed", "失敗しました", "Ошибка" } },                       // ST_CFG_FAIL
        { { "选择档案", "選擇設定檔", "Select profile", "プロファイルを選択", "Выбрать профиль" } }, // ST_CFG_SELECT
        { { "皮肤", "皮膚", "Skin", "スキン", "Скин" } },                                       // ST_SKIN_TITLE
        { { "选择下坠谱面使用的皮肤（目录内需包含 rurudokey.png）。把皮肤文件夹复制到 skin 目录即可新增。",
            "選擇下墜譜面使用的皮膚（目錄內需包含 rurudokey.png）。把皮膚資料夾複製到 skin 目錄即可新增。",
            "Pick the skin used by the falling playfield (folder must contain rurudokey.png). Drop new skin folders into the skin directory.",
            "落下譜面用スキンを選択（フォルダに rurudokey.png が必要）。skin フォルダに追加するだけで増やせます。",
            "Выберите скин для дорожек (в папке должен быть rurudokey.png). Новые скины просто копируются в папку skin." } },  // ST_SKIN_DESC
        { { "当前皮肤", "目前皮膚", "Current skin", "現在のスキン", "Текущий скин" } },           // ST_SKIN_CURRENT
        { { "使用", "使用", "Use", "使用", "Использовать" } },                                  // ST_SKIN_APPLY
        { { "打开皮肤目录", "開啟皮膚目錄", "Open skin folder", "スキンフォルダを開く", "Открыть папку скинов" } },  // ST_SKIN_OPEN
        { { "重新扫描", "重新掃描", "Rescan", "再スキャン", "Обновить" } },                      // ST_SKIN_RESCAN
        { { "未找到可用皮肤：目录内需包含 rurudokey.png", "未找到可用皮膚：目錄內需包含 rurudokey.png",
            "No usable skin found: folder must contain rurudokey.png", "使えるスキンがありません：rurudokey.png が必要です",
            "Скин не найден: в папке должен быть rurudokey.png" } },                            // ST_SKIN_NONE
        { { "已切换皮肤", "已切換皮膚", "Skin applied", "スキンを適用しました", "Скин применён" } }, // ST_SKIN_APPLIED
        { { "使用中", "使用中", "In use", "使用中", "Используется" } },                          // ST_SKIN_INUSE
        { { "皮肤目录", "皮膚目錄", "Skin folder", "スキンフォルダ", "Папка скинов" } },          // ST_SKIN_DIR
        { { "导入 MSP 皮肤…", "匯入 MSP 皮膚…", "Import MSP skin…", "MSPスキンを導入…", "Импорт скина MSP…" } },  // ST_SKIN_IMPORT
        { { "已导入皮肤：", "已匯入皮膚：", "Skin imported: ", "スキンを導入しました：", "Скин импортирован: " } },  // ST_SKIN_IMPORTED
        { { "导入失败", "匯入失敗", "Import failed", "導入失敗", "Ошибка импорта" } },  // ST_SKIN_IMPORT_FAIL
        { { "内置皮肤（推荐）", "內建皮膚（推薦）", "Built-in skin (recommended)", "内蔵スキン（推奨）", "Встроенный скин (рекоменд.)" } },  // ST_SKIN_BUILTIN
        { { "与 Malody 4K 皮肤同款固定版式：轨道、判定、连击、精准度、小人、托腮角色齐全；贴图取当前皮肤目录。",
            "與 Malody 4K 皮膚同款固定版式：軌道、判定、連擊、精準度、小人、托腮角色齊全；貼圖取目前皮膚目錄。",
            "Fixed Malody 4K-style layout: track, judge, combo, accuracy, character and resting mascot; textures come from the active skin folder.",
            "Malody 4K 風の固定レイアウト：トラック・判定・コンボ・精度・キャラ・頬杖キャラを表示。テクスチャは現在のスキンフォルダから取得。",
            "Фиксированная раскладка в стиле Malody 4K: дорожки, судья, комбо, точность, персонажи; текстуры берутся из активной папки скина." } },  // ST_SKIN_BUILTIN_DESC
        { { "皮肤模式", "皮膚模式", "Skin mode", "スキンモード", "Режим скина" } },  // ST_SKIN_MODE
        { { "布局", "佈局", "Layout", "レイアウト", "Раскладка" } },                             // RD_SEC_LAYOUT
        { { "显示", "顯示", "Display", "表示", "Отображение" } },                                // RD_SEC_VIEW
        { { "时间与校准", "時間與校準", "Time & Calibration", "時間と較正", "Время и калибровка" } }, // RD_SEC_TUNE
        { { "信息标记", "資訊標記", "Info markers", "情報マーカー", "Маркеры" } },                // RD_SEC_SIGNAL
        { { "实时状态", "即時狀態", "Live status", "リアルタイム状態", "Текущее состояние" } },    // RD_LIVE
        { { "安装 MOD…", "安裝 MOD…", "Install mod…", "MOD を導入…", "Установить мод…" } },  // MOD_INSTALL
        { { "安装完成", "安裝完成", "Installed", "導入が完了しました", "Установлено" } },  // MOD_INSTALL_OK
        { { "安装失败：ZIP 中未找到 Info.json（标准 MOD 包应包含 名称/Info.json）",
            "安裝失敗：ZIP 中未找到 Info.json（標準 MOD 包應包含 名稱/Info.json）",
            "Install failed: no Info.json found in the ZIP (a standard mod archive contains <name>/Info.json)",
            "導入失敗：ZIP に Info.json が見つかりません（標準的な MOD は 名前/Info.json を含みます）",
            "Ошибка: в ZIP не найден Info.json (стандартный архив содержит <имя>/Info.json)" } },  // MOD_INSTALL_FAIL
        { { "同名 MOD 已存在，是否覆盖？", "同名 MOD 已存在，是否覆蓋？", "A mod with the same folder name already exists. Overwrite?", "同名 MOD が已に存在します。上書きしますか？", "Мод с таким именем уже установлен. Перезаписать?" } },  // MOD_OVERWRITE
        { { "主页", "主頁", "Homepage", "ホームページ", "Сайт" } },  // MOD_HOMEPAGE
        { { "仓库", "倉庫", "Repository", "リポジトリ", "Репозиторий" } },  // MOD_REPO
        { { "缺少前置", "缺少前置", "Missing deps", "依存関係不足", "Нет зависимостей" } },  // MOD_REQ_MISSING
        { { "角度判定条（实时判定角）", "角度判定條（即時判定角）", "Angle judge bar (live)", "角度判定バー（リアルタイム）", "Полоса угловой оценки" } },  // RD_ANGLE
        { { "判定窗口分带（纯/准/有效）", "判定視窗分帶（純/準/有效）", "Judge window bands", "判定ウィンドウ帯（純/良/有効）", "Зоны окна оценки" } },  // RD_WINDOWS
        { { "高速自动简化", "高速自動簡化", "Auto-simplify at high speed", "高速時の自動簡略化", "Авто-упрощение на скорости" } },  // RD_ADAPT
        /* ---- 宏模式页 / 自动打歌 / 自动录制 ---- */
        { { "宏模式", "巨集模式", "Macro", "マクロ", "Макрос" } },                                              // TAB_MACRO
        { { "自动打歌", "自動打歌", "Auto play", "自動演奏", "Авто-игра" } },                                   // LBL_AUTOPLAY
        { { "宏打歌", "巨集打歌", "Macro play", "マクロ演奏", "Макро-игра" } },                                 // LBL_MACRO
        { { "录制", "錄製", "Record", "録画", "Запись" } },                                                     // LBL_RECORD
        { { "宏打歌（拟人化代打）", "巨集打歌（擬人化代打）", "Macro play (humanized)", "マクロ演奏（人間風）", "Макро (очеловеченный)" } },  // MACRO_TITLE
        { { "用内部虚拟按键自动帮你打 4K/5K/6K/10K 下坠谱：按目标精准度给每个音加拟人抖动（慢漂移 + 偶发手滑），长按自动保持。不注入系统键鼠、不改游戏内存，可随时开关；配合「一键自动校准」把判定偏移自动拉回 0。",
            "用內部虛擬按鍵自動幫你打 4K/5K/6K/10K 下墜譜：依目標精準度對每個音加擬人抖動（慢漂移 + 偶發手滑），長按自動保持。不注入系統鍵鼠、不改遊戲記憶體，可隨時開關；配合「一鍵自動校準」把判定偏移自動拉回 0。",
            "Internal virtual keys play the 4K/5K/6K/10K falling chart for you: per-note humanized jitter (slow drift + occasional slips) toward the target accuracy, holds auto-sustained. No system input injection, no game memory writes; toggle anytime. Use one-click calibration to pull the judge offset back to 0.",
            "内部仮想キーで4K/5K/6K/10K譜面を自動演奏：目標精度に合わせて各音に人間風の揺らぎ（ドリフト＋たまのミス）を付与、長押しは自動保持。システム入力の注入やメモリ書き換えは行いません。ワンクリック較正で判定オフセットを0に戻せます。",
            "Внутренние виртуальные клавиши играют чарт 4K/5K/6K/10K за вас: дрожание в человеческом стиле (дрейф + редкие промахи) под целевую точность, удержания автоматически. Без инъекции ввода и записи в память игры; калибровка возвращает смещение к 0." } },  // MACRO_DESC
        { { "目标精准度", "目標精準度", "Target accuracy", "目標精度", "Целевая точность" } },                 // MACRO_ACC
        { { "拟人程度", "擬人程度", "Humanness", "人間らしさ", "Человечность" } },                              // MACRO_HUMAN
        { { "一键自动校准", "一鍵自動校準", "One-click calibrate", "ワンクリック較正", "Авто-калибровка" } },   // MACRO_CALIB
        { { "开启「自动调整延迟」后，宏打歌会持续把实测偏差写回判定偏移（每 16 次命中微调一次）。",
            "開啟「自動調整延遲」後，巨集打歌會持續把實測偏差寫回判定偏移（每 16 次命中微調一次）。",
            "With auto-offset enabled, macro play keeps writing the measured bias back into the judge offset (adjusted every 16 hits).",
            "「遅延自動調整」を有効にすると、マクロ演奏が実測バイアスを判定オフセットへ書き戻します（16ヒットごと）。",
            "При включённой авто-подстройке макро-игра записывает измеренное смещение в офсет судьи (каждые 16 попаданий)." } },  // MACRO_CALIBHINT
        { { "参与模式", "參與模式", "Active modes", "対象モード", "Режимы" } },                                  // MACRO_MODES
        { { "自动录制（游戏原生画面）", "自動錄製（遊戲原生畫面）", "Auto record (native game view)", "自動録画（ゲーム原生画面）", "Автозапись (нативный вид)" } },  // REC_TITLE
        { { "直接录制游戏窗口的原生画面（不含辅助工具覆盖层），H.264/MP4 硬件编码，输出到下方目录。帧率/码率可调；掉帧时自动丢弃而不卡游戏。",
            "直接錄製遊戲視窗的原生畫面（不含輔助工具覆蓋層），H.264/MP4 硬體編碼，輸出到下方目錄。影格率/位元率可調；掉幀時自動丟棄而不卡遊戲。",
            "Records the native game window (no overlay), H.264/MP4 with hardware encoding, saved to the folder below. FPS/bitrate adjustable; frames are dropped instead of stalling the game.",
            "ゲームウィンドウの原生画面（オーバーレイなし）をH.264/MP4でハードウェアエンコードして下のフォルダへ保存。FPS/ビットレート調整可、遅延時はフレーム破棄でゲームを止めません。",
            "Запись нативного окна игры (без оверлея), H.264/MP4 с аппаратным кодированием, в папку ниже. FPS/битрейт настраиваются; при перегрузке кадры отбрасываются." } },  // REC_DESC
        { { "录制开关", "錄製開關", "Recording", "録画", "Запись" } },                                          // REC_ON
        { { "跟随游戏自动开始/停止", "跟隨遊戲自動開始/停止", "Auto start/stop with gameplay", "ゲームに追従して自動開始/停止", "Авто-старт/стоп с игрой" } },  // REC_AUTO
        { { "输出目录", "輸出目錄", "Output folder", "出力フォルダ", "Папка вывода" } },                        // REC_DIR
        { { "打开目录", "開啟目錄", "Open folder", "フォルダを開く", "Открыть папку" } },                       // REC_OPEN
        { { "预览", "預覽", "Preview", "プレビュー", "Просмотр" } },                                            // REC_PREVIEW
        { { "帧率", "影格率", "FPS", "FPS", "FPS" } },                                                          // REC_FPS
        { { "码率", "位元率", "Bitrate", "ビットレート", "Битрейт" } },                                        // REC_MBPS
        { { "状态", "狀態", "Status", "状態", "Статус" } },                                                     // REC_STATS
        { { "录制的是游戏原生画面，因此不会出现辅助工具的判定线/特效；清晰度取决于上面的码率设置。",
            "錄製的是遊戲原生畫面，因此不會出現輔助工具的判定線/特效；清晰度取決於上面的位元率設定。",
            "Recording captures the native game view, so the assist overlay (judge line/FX) is not included; quality follows the bitrate above.",
            "録画はゲーム原生画面のため、補助ツールの判定線/演出は入りません。画質は上のビットレートに依存します。",
            "Запись идёт с нативного вида игры, поэтому оверлей (линия/эффекты) в видео не попадает; качество зависит от битрейта выше." } },  // REC_HINT
        /* ---- 关于页 ---- */
        { { "版本", "版本", "Version", "バージョン", "Версия" } },                                              // ABOUT_VER
        { { "打开演示视频", "開啟示範影片", "Open demo video", "デモ動画を開く", "Открыть демо-видео" } },       // ABOUT_OPEN
        { { "GitHub 仓库", "GitHub 倉庫", "GitHub repo", "GitHub リポジトリ", "Репозиторий GitHub" } },         // ABOUT_GITHUB
        { { "进入本页会自动打开演示视频；若浏览器没有弹出，点上面的按钮即可。",
            "進入本頁會自動開啟示範影片；若瀏覽器沒有彈出，點上面的按鈕即可。",
            "Entering this page opens the demo video automatically; click the button above if it did not open.",
            "このページを開くとデモ動画が自動で開きます。開かない場合は上のボタンを押してください。",
            "При входе на страницу демо-видео открывается автоматически; если нет — нажмите кнопку выше." } },  // ABOUT_HINT
        { { "由 SHASHEN4404 制作 · MIT License", "由 SHASHEN4404 製作 · MIT License", "Made by SHASHEN4404 · MIT License", "制作: SHASHEN4404 · MIT License", "Автор: SHASHEN4404 · MIT License" } },  // ABOUT_MADE
        /* ---- 录制页（独立 PAGE） / 冰与火宏模式 ---- */
        { { "录制", "錄製", "Recording", "録画", "Запись" } },   // TAB_RECORD
        { { "冰与火（原生关卡）", "冰與火（原生關卡）", "Fire & Ice (native level)", "氷と炎（ネイティブ）", "A Dance of Fire and Ice" } },  // MACRO_FIRE
        { { "冰与火模式：宏直接在游戏原生关卡里按键打歌，手感拟人（高精准但非 100%）。开启时会自动打开不死、暂时停用自动连击，关闭后自动恢复。",
            "冰與火模式：巨集直接在遊戲原生關卡裡按鍵打歌，手感擬人（高精準但非 100%）。開啟時會自動開啟不死、暫時停用自動連擊，關閉後自動恢復。",
            "Fire & Ice mode: the macro presses keys in the native level with human-like timing (high but not 100% accuracy). While enabled it forces no-fail on and temporarily disables auto-combo; both are restored when disabled.",
            "氷と炎モード：マクロがネイティブステージで人間らしいタイミングでキーを押します（高精度・非100%）。オン中は不死を強制し、自動コンボを一時的に無効化します。",
            "Режим A Dance of Fire and Ice: макрос жмёт клавиши в самой игре с человеческим таймингом. Пока включён — принудительно no-fail и временно отключено авто-комбо." } },  // MACRO_FIRE_HINT
        { { "开始录制", "開始錄製", "Start", "開始", "Старт" } },        // REC_START
        { { "暂停录制", "暫停錄製", "Pause", "一時停止", "Пауза" } },    // REC_PAUSE
        { { "继续录制", "繼續錄製", "Resume", "再開", "Продолжить" } },  // REC_RESUME
        { { "暂停会结束当前分段，继续录制会写入新文件。", "暫停會結束目前分段，繼續錄製會寫入新檔案。", "Pausing finalizes the current segment; resuming starts a new file.", "一時停止で現在のセグメントを確定し、再開で新しいファイルに書き込みます。", "Пауза завершает текущий сегмент, продолжение пишет новый файл." } },  // REC_PAUSED_HINT
        { { "小窗录制：把游戏原生画面（不含本工具覆盖层）录成 H.264/MP4 文件。", "小窗錄製：把遊戲原生畫面（不含本工具覆蓋層）錄成 H.264/MP4 檔案。", "Window recording: captures the native game frame (without this tool's overlay) into H.264/MP4.", "ウィンドウ録画：ゲーム本来の画面（本ツールのオーバーレイなし）を H.264/MP4 に保存します。", "Запись окна: нативное изображение игры (без оверлея) в H.264/MP4." } },  // REC_PAGEDESC
        /* ---- 本轮新增 ---- */
        { { "16K", "16K", "16K", "16K", "16K" } },                                                              // TAB_16K
        { { "居中", "置中", "Center", "中央", "По центру" } },                                                  // LBL_POS_CT
        { { "伪双押优化", "偽雙押優化", "Merge near-simultaneous", "疑似同時押し補正", "Слияние псевдо-дублей" } },  // LBL_PSEUDO2
        { { "16K（PAD）：Malody Pad 式 4×4 方形面板；音符在对应方块上收缩，缩到边界时按下该方块的键。每个方块可单独设键。", "16K（PAD）：Malody Pad 式 4×4 方形面板；音符在對應方塊上收縮，縮到邊界時按下該方塊的鍵。每個方塊可單獨設鍵。", "16K (PAD): a Malody Pad-style 4x4 square panel; notes shrink on their pad and you hit when the shrinking square meets the border. Every pad has its own key.", "16K（PAD）：Malody Pad 風の4×4パネル。音符は対応パッド上で縮小し、枠に重なった瞬間にそのパッドのキーを押します。各パッドに個別キーを設定できます。", "16K (PAD): панель 4x4 в стиле Malody Pad; нота сжимается на своём квадрате, нажатие — когда квадрат совпадёт с рамкой. У каждого квадрата своя клавиша." } },  // LBL_PAD_HINT
        { { "CATCH", "CATCH", "CATCH", "CATCH", "CATCH" } },  // TAB_CATCH
        { { "拖尾", "拖尾", "Tail", "トレイル", "Шлейф" } },  // KV_TRAIL
        { { "一键接：无需按键，用鼠标左右移动底部接盘；雨点落线瞬间被接盘盖住即自动接住（正中 MARVELOUS / 靠边 GOOD），没盖住判 MISS；判定按同一档位同步给冰与火本体（PERFECT→PERFECT、GOOD→GOOD 并显示快/慢、MISS→本体也 MISS 并断连击）。打歌算法为本模式独立实现（与冰与火宏不共用代码）。MISS 不会自动返回或重开，与 PERFECT/GOOD 一样照常显示判定、继续游玩；本页还可切换多种生成手法（经典 / 走 / 冲 / 超冲 / 边冲 / 阶梯）。", "一鍵接：無需按鍵，用滑鼠左右移動底部接盤；雨點落線瞬間被接盤蓋住即自動接住（正中 MARVELOUS / 靠邊 GOOD），沒蓋住判 MISS；判定按同一檔位同步給冰與火本體（PERFECT→PERFECT、GOOD→GOOD 並顯示快/慢、MISS→本體也 MISS 並斷連擊）。打歌演算法為本模式獨立實作（與冰與火巨集不共用程式碼）。MISS 不會自動返回或重開，與 PERFECT/GOOD 一樣照常顯示判定、繼續遊玩；本頁還可切換多種生成手法（經典 / 走 / 衝 / 超衝 / 邊衝 / 階梯）。", "Mouse catcher: no keys needed - move the bottom catcher with the mouse. A drop is caught the moment it reaches the line if the catcher covers it (center = MARVELOUS, edge = GOOD); otherwise MISS. The same grade is pushed to the native Fire&Ice level (PERFECT->PERFECT, GOOD->GOOD with a FAST/SLOW tag, MISS->native MISS and combo break) by this mode own standalone play algorithm (no code shared with the Fire&Ice macro). A MISS never auto-restarts: it shows a normal judgment and play continues just like PERFECT/GOOD. Several generation styles (Classic / Walk / Dash / Hyper / Edge / Stair) can be picked on this page.", "マウスキャッチャー：キー不要。マウスで下部のキャッチャーを左右に動かします。雨がラインに達した瞬間キャッチャーが覆っていれば自動キャッチ（中央=MARVELOUS / 端=GOOD）、覆っていなければ MISS。同じ判定が Fire&Ice 本体にも同期されます（PERFECT→PERFECT、GOOD→GOOD と FAST/SLOW 表示、MISS→本体も MISS でコンボ切れ）。打鍵アルゴリズムは本モード専用の独立実装です（Fire&Ice マクロとコードを共有しません）。MISS でも自動リトライせず、PERFECT/GOOD と同様に判定を表示してそのまま続行します。本ページで複数の生成手法（クラシック / ウォーク / ダッシュ / ハイパー / エッジ / ステア）を選べます。", "Мышиный ловец: нажатия не нужны — двигайте нижний ловец мышью. Капля ловится в момент достижения линии, если ловец её накрывает (центр = MARVELOUS, край = GOOD); иначе MISS. Та же оценка передаётся нативной Fire&Ice (PERFECT→PERFECT, GOOD→GOOD с меткой FAST/SLOW, MISS→MISS и обрыв комбо) отдельным алгоритмом этого режима (общий код с макросом Fire&Ice не используется). MISS не перезапускает уровень автоматически: показывается обычная оценка, игра продолжается как при PERFECT/GOOD. На этой странице можно выбрать несколько стилей генерации (классика / шаг / рывок / гипер / край / лестница)." } },  // LBL_CATCH_HINT
        { { "方块键位", "方塊鍵位", "Pad keys", "パッドキー", "Клавиши пада" } },  // LBL_PAD_KEYS
        { { "接盘键位", "接盤鍵位", "Catcher keys", "キャッチャーキー", "Клавиши ловца" } },  // LBL_CATCH_KEYS
        { { "硬抗 BPM（越低越偏轮指）", "硬抗 BPM（越低越偏輪指）", "Hard-resist BPM (lower = more rolls)", "硬抗BPM（低いほどロール寄り）", "Жёсткий BPM (ниже = больше роллов)" } },  // LBL_HARDRESIST
        { { "内轮指（J K / F D）", "內輪指（J K / F D）", "Inner roll (J K / F D)", "内ロール（J K / F D）", "Внутренний ролл (J K / F D)" } },  // LBL_INNERROLL
        { { "16K（PAD）仅支持内置皮肤：4×4 面板是 Malody Pad 布局，外部 MSP 轨道皮肤不适用于该模式。", "16K（PAD）僅支援內建面板：4×4 面板是 Malody Pad 佈局，外部 MSP 軌道面板不適用於該模式。", "16K (PAD) supports the built-in skin only: the 4x4 panel is a Malody Pad layout, so external MSP track skins do not apply.", "16K（PAD）は内蔵スキンのみ対応：4×4 パネルは Malody Pad レイアウトのため、外部 MSP レーンスキンは適用されません。", "16K (PAD) поддерживает только встроенный скин: панель 4x4 — это раскладка Malody Pad, внешние MSP-скины дорожек не подходят." } },  // LBL_16K_BUILTIN_ONLY
        { { "选中某个模式会同时启用该模式的辅助（四模式互斥）；再次点击取消。",
            "選中某個模式會同時啟用該模式的輔助（四模式互斥）；再次點擊取消。",
            "Selecting a mode also enables that mode's assist (the four modes are mutually exclusive); click again to turn it off.",
            "モードを選ぶとそのモードの補助も有効になります（4モードは排他）。もう一度押すと解除。",
            "Выбор режима включает и вспомогательный слой этого режима (режимы взаимоисключающие); нажмите снова, чтобы выключить." } },  // MACRO_MODES_HINT
        { { "冰与火原生钩子：已装好，代打生效。", "冰與火原生鉤子：已裝好，代打生效。", "Fire & Ice native hook: installed — auto-play is active.", "氷と炎ネイティブフック：導入済み — 代打有効。", "Нативный хук Fire & Ice: установлен — автопрогон работает." } },  // MACRO_HOOK_ON
        { { "冰与火原生钩子：未装好，代打无法生效（请看日志或重启游戏）。", "冰與火原生鉤子：未裝好，代打無法生效（請看日誌或重啟遊戲）。", "Fire & Ice native hook: NOT installed — auto-play will not work (check the log or restart the game).", "氷と炎ネイティブフック：未導入 — 代打は機能しません（ログ確認かゲーム再起動）。", "Нативный хук Fire & Ice: не установлен — автопрогон не работает (см. лог или перезапустите игру)." } },  // MACRO_HOOK_OFF
        { { "自适应窗口", "自適應窗口", "Adaptive window", "自動調整窓", "Адаптивное окно" } },  // LBL_PSEUDO2_ADAPT
        { { "漏音即死", "漏音即死", "Miss = death", "ミス即死", "Промах = смерть" } },  // LBL_CATCH_KILL
        { { "接盘宽度", "接盤寬度", "Catcher width", "キャッチャー幅", "Ширина ловца" } },  // LBL_CATCH_PLATEW
        { { "漏音，判定失败", "漏音，判定失敗", "Missed — failed", "ミス — 失敗", "Промах — провал" } },  // LBL_CATCH_DEAD
        { { "打歌精准度", "打歌精準度", "Playback accuracy", "演奏精度", "Точность игры" } },  // LBL_CATCH_PLAYACC
        // ---- 直播模式（防采集覆盖层）----
        { { "直播模式", "直播模式", "Stream mode", "配信モード", "Режим стрима" } },  // TAB_LIVE
        { { "直播模式（防采集）", "直播模式（防擷取）", "Stream mode (capture-proof)", "配信モード（キャプチャ対策）", "Режим стрима" } },  // LIVE_TITLE
        { { "开启后这些元素在直播 / 录像里看不到，但本机屏幕上照常显示；左上角会带 ADOFAI-PERFECT 水印。", "開啟後這些元素在直播 / 錄影裡看不到，但本機螢幕上照常顯示；左上角會帶 ADOFAI-PERFECT 浮水印。", "While enabled these elements are hidden from capture (OBS etc.) but still visible on your own screen; an ADOFAI-PERFECT watermark is drawn on the game.", "有効にするとこれらの要素は配信/録画に映らず、自分の画面には表示されたままです。左上に ADOFAI-PERFECT の透かしが出ます。", "Включив это, элементы не попадут в стрим/запись, но останутся видны на вашем экране; слева вверху — водяной знак ADOFAI-PERFECT." } },  // LIVE_DESC
        { { "不显示 IMGUI 界面", "不顯示 IMGUI 介面", "Hide IMGUI menu", "IMGUI 画面を隠す", "Скрыть меню IMGUI" } },  // LIVE_HIDE_MENU
        { { "不显示一般轨道辅助", "不顯示一般軌道輔助", "Hide track assist", "通常トラック補助を隠す", "Скрыть дорожки" } },  // LIVE_HIDE_TRACK
        { { "不显示辅助读谱", "不顯示輔助讀譜", "Hide sightread", "譜面補助を隠す", "Скрыть чтение" } },  // LIVE_HIDE_READ
        { { "不显示按键反馈", "不顯示按鍵回饋", "Hide key feedback", "キー表示を隠す", "Скрыть клавиши" } },  // LIVE_HIDE_KV
        { { "直播推送软件（OBS / 直播姬 / 各种录像机）里看不到被勾选的元素；人眼在显示器上仍然能看到。", "直播推送軟體（OBS / 直播姬 / 各種錄影機）裡看不到被勾選的元素；人眼在螢幕上仍然能看到。", "Capture software cannot see the checked elements, but your own eyes still can.", "配信ソフト（OBS 等）にはチェックした要素が映りませんが、自分の目には見えたままです。", "Программы захвата не увидят отмеченные элементы, но вы их по-прежнему видите." } },  // LIVE_HINT
        { { "覆盖窗口不可用（显卡/驱动不支持），直播模式暂时停用；已自动回退为普通显示。", "覆蓋視窗不可用（顯示卡/驅動不支援），直播模式暫時停用；已自動回退為普通顯示。", "Overlay window unavailable (GPU/driver); stream mode is inactive and fell back to normal drawing.", "オーバーレイウィンドウが利用できません（GPU/ドライバ）；通常描画にフォールバックしました。", "Оверлей недоступен (GPU/драйвер); режим отключён, обычная отрисовка." } },  // LIVE_UNAVAIL
        { { "覆盖层状态", "覆蓋層狀態", "Overlay", "オーバーレイ状態", "Оверлей" } },  // LIVE_STATUS
        // ---- CATCH 宏代打 / 快慢 ----
        { { "宏代打（自动接盘）", "巨集代打（自動接盤）", "Macro auto-catch", "マクロ自動キャッチ", "Макрос-ловля" } },  // LBL_CATCH_AUTO
        { { "宏精准度", "巨集精準度", "Macro accuracy", "マクロ精度", "Точность макроса" } },  // LBL_CATCH_MACC
    { { "快", "快", "FAST", "早い", "РАНО" } },        // LBL_CATCH_FAST
    { { "慢", "慢", "SLOW", "遅い", "ПОЗДНО" } },      // LBL_CATCH_SLOW
    // ---- 8K / OSU（戳泡泡）模式页 ----
    { { "8K", "8K", "8K", "8K", "8K" } },  // TAB_8K
    { { "OSU!", "OSU!", "OSU!", "OSU!", "OSU!" } },  // TAB_OSU
    { { "鼠标移动瞄准泡泡，判定环收缩到与圈外沿重合时左/右键（或 Z / X）点击。判定窗按 OD 换算：300 = 80-6*OD · 100 = 140-8*OD · 50 = 200-10*OD，超窗 MISS。",
        "滑鼠移動瞄準泡泡，判定環收縮到與圈外沿重合時左/右鍵（或 Z / X）點擊。判定窗按 OD 換算：300 = 80-6*OD · 100 = 140-8*OD · 50 = 200-10*OD，超窗 MISS。",
        "Aim with the mouse; click left/right button (or Z / X) when the approach ring meets the circle edge. Windows follow OD: 300 = 80-6*OD, 100 = 140-8*OD, 50 = 200-10*OD, else MISS.",
        "マウスで照準し、判定サークルが重なった瞬間に左/右クリック（または Z / X）。判定幅は OD 換算：300 = 80-6*OD · 100 = 140-8*OD · 50 = 200-10*OD、外れは MISS。",
        "Наводите мышью и щёлкайте левой/правой кнопкой (или Z / X), когда кольцо сойдётся с кругом. Окна по OD: 300 = 80-6*OD · 100 = 140-8*OD · 50 = 200-10*OD, иначе MISS." } },  // LBL_OSU_CAL_HINT
    };
    static_assert(sizeof(g_table) / sizeof(g_table[0]) == (size_t)BUILTIN_N, "table/enum mismatch");

    static std::vector<Row>  g_extra;
    static Lang              g_lang = ZH_CN;
    static char              g_cfg[MAX_PATH] = { 0 };
    static bool              g_cfgTried = false;

    static void CfgPath()
    {
        if (g_cfgTried)
            return;
        g_cfgTried = true;
        HMODULE hm = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)&CfgPath, &hm);
        char path[MAX_PATH] = { 0 };
        if (hm && GetModuleFileNameA(hm, path, MAX_PATH))
        {
            char* slash = strrchr(path, '\\');
            if (slash)
                *(slash + 1) = 0;
        }
        snprintf(g_cfg, sizeof(g_cfg), "%sadofai_perfect.cfg", path);
    }

    // ---------------- Prefs：adofai_perfect.cfg 键值存储 ----------------
    namespace Prefs
    {
        static std::vector<std::pair<std::string, std::string>> g_kv;   // 保持写入顺序
        static bool g_loaded = false;

        static std::string* Find(const char* key)
        {
            if (!key) return nullptr;
            for (auto& kv : g_kv)
                if (kv.first == key) return &kv.second;
            return nullptr;
        }

        const char* Dir()
        {
            CfgPath();
            static char dir[MAX_PATH] = { 0 };
            if (!dir[0])
            {
                strncpy_s(dir, g_cfg, _TRUNCATE);
                char* slash = strrchr(dir, '\\');
                if (slash) *(slash + 1) = 0;
            }
            return dir;
        }
        const char* File() { CfgPath(); return g_cfg; }

        void Load()
        {
            g_kv.clear();
            g_loaded = true;
            CfgPath();

            std::string txt;
            FILE* f = nullptr;
            if (fopen_s(&f, g_cfg, "rb") == 0 && f)
            {
                char buf[4096];
                size_t n;
                while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                    txt.append(buf, n);
                fclose(f);
            }
            // 旧版格式：文件内容只有语言编号（纯数字）
            {
                size_t a = 0, b = txt.size();
                while (a < b && (txt[a] == ' ' || txt[a] == '\r' || txt[a] == '\n' || txt[a] == '\t')) a++;
                while (b > a && (txt[b - 1] == ' ' || txt[b - 1] == '\r' || txt[b - 1] == '\n' || txt[b - 1] == '\t')) b--;
                bool numeric = (b > a);
                for (size_t i = a; i < b; i++)
                    if (txt[i] < '0' || txt[i] > '9') { numeric = false; break; }
                if (numeric)
                {
                    int v = atoi(txt.substr(a, b - a).c_str());
                    if (v >= 0 && v < LANG_N) g_lang = (Lang)v;
                    return;
                }
            }

            size_t p = 0;
            while (p < txt.size())
            {
                size_t e = txt.find('\n', p);
                if (e == std::string::npos) e = txt.size();
                std::string line = txt.substr(p, e - p);
                p = e + 1;
                if (!line.empty() && line[0] == '#') continue;
                size_t eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string k = line.substr(0, eq), v2 = line.substr(eq + 1);
                while (!k.empty() && (k.back() == ' ' || k.back() == '\r' || k.back() == '\t')) k.pop_back();
                while (!v2.empty() && (v2.back() == ' ' || v2.back() == '\r' || v2.back() == '\t')) v2.pop_back();
                while (!v2.empty() && v2[0] == ' ') v2.erase(0, 1);
                if (k.empty()) continue;
                if (k == "lang")
                {
                    int v = atoi(v2.c_str());
                    if (v >= 0 && v < LANG_N) g_lang = (Lang)v;
                    continue;
                }
                if (std::string* cur = Find(k.c_str())) *cur = v2;
                else g_kv.emplace_back(k, v2);
            }
        }

        void Save()
        {
            if (!g_loaded) Load();
            CfgPath();
            FILE* f = nullptr;
            if (fopen_s(&f, g_cfg, "wb") == 0 && f)
            {
                fprintf(f, "# ADOFAI-PERFECT settings\n");
                fprintf(f, "lang=%d\n", (int)g_lang);
                for (auto& kv : g_kv)
                    fprintf(f, "%s=%s\n", kv.first.c_str(), kv.second.c_str());
                fclose(f);
            }
        }

        int GetInt(const char* key, int def)
        {
            if (!g_loaded) Load();
            std::string* v = Find(key);
            return v ? atoi(v->c_str()) : def;
        }
        void SetInt(const char* key, int value)
        {
            if (!g_loaded) Load();
            char b[32];
            snprintf(b, sizeof(b), "%d", value);
            if (std::string* v = Find(key)) *v = b;
            else g_kv.emplace_back(key, b);
        }
        void GetStr(const char* key, char* out, int n, const char* def)
        {
            if (!out || n <= 0) return;
            if (!g_loaded) Load();
            std::string* v = Find(key);
            snprintf(out, (size_t)n, "%s", v ? v->c_str() : (def ? def : ""));
        }
        void SetStr(const char* key, const char* value)
        {
            if (!g_loaded) Load();
            std::string v = value ? value : "";
            if (std::string* cur = Find(key)) *cur = v;
            else g_kv.emplace_back(key, v);
        }
    }

    void Load() { Prefs::Load(); }

    void Save()
    {
        Prefs::SetInt("lang", (int)g_lang);
        Prefs::Save();
    }

    void        SetLang(Lang l) { if (l >= 0 && l < LANG_N) { g_lang = l; Save(); } }
    Lang        GetLang()       { return g_lang; }

    const char* TrL(int id, Lang l)
    {
        if (l < 0 || l >= LANG_N) l = g_lang;
        if (id < 0) return "";
        if (id < (int)BUILTIN_N) return g_table[id].t[l];
        int k = id - (int)BUILTIN_N;
        if (k < (int)g_extra.size()) return g_extra[k].t[l];
        return "";
    }

    const char* Tr(int id) { return TrL(id, g_lang); }

    int Register(const char* zhCN, const char* zhTW, const char* en, const char* ja, const char* ru)
    {
        Row r{};
        r.t[ZH_CN] = zhCN ? zhCN : ""; r.t[ZH_TW] = zhTW ? zhTW : "";
        r.t[EN] = en ? en : ""; r.t[JA] = ja ? ja : ""; r.t[RU] = ru ? ru : "";
        g_extra.push_back(r);
        return (int)BUILTIN_N + (int)g_extra.size() - 1;
    }

    const char* LangName(Lang l)
    {
        switch (l)
        {
        case ZH_CN: return "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87";     // 简体中文
        case ZH_TW: return "\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87";     // 繁體中文
        case EN:    return "English";
        case JA:    return "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";                     // 日本語
        case RU:    return "\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9"; // Русский
        default:    return "?";
        }
    }
}
