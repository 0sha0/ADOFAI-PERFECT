#pragma once
// ============================================================
// SkinMsp.h — Malody MSP 皮肤包（.msp，ZIP 容器）解析
//
//   · info.asm  = Google.Protobuf 的 SkinFile 消息（逆向自 MalodyV
//     Il2CppDumper 枚举：SkinModuleUsage / SkinModuleSubType /
//     SkinModuleScene / SkinModuleTrigger）
//   · 解析结果是一张"角色表"：每个渲染槽位对应的贴图文件名（相对包目录）
//   · 旧格式（只有 info.json + key-note-*.png 命名约定）也能生成角色表
//   · .msp 导入 = 解压到 <运行目录>\skin\<包名>\（tar 内建，失败时 PowerShell 兜底）
// ============================================================

namespace SkinMsp
{
    constexpr int kLanes      = 10;
    constexpr int kFxMax      = 16;   // 打击特效帧上限
    constexpr int kJudgeBarN  = 4;    // 判定闪条档位
    constexpr int kJudgePopN  = 5;    // 中央判定图（scene Judge=0..4）

    // ---- MSP 模块表（整屏渲染用） ----
    // 逆向依据：Malody.Scene.Composer.fky::ApplyBasicParam / ApplyImageSize
    //   · x/y/dx/dy 各有单位：0=Percent(父宽/父高的百分比) 1=Unit(1080p 像素×缩放) 2=Px(原始像素)
    //   · w/h 同理（ModuleParamImage 的 wu/hu），0 = 用贴图原始尺寸
    //   · pivot 0..8 = 左/中/右 × 上/中/下（CreatePivot）
    //   · Percent 原点在父物体左下：x=0 左边缘、y=0 下边缘、y=100 上边缘
    constexpr int kModMax = 160;
    struct RoleCond
    {
        int  source = 0, flag = 0;
        float val = 0.f;
        char valstr[48]{};      // ModuleCondition 5（字符串条件值；Lua/编辑器用）
    };
    // 模块内置动画（SkinFile.Module field 20 = ModuleAnimation 重复字段）。
    // 逆向 dump：1 type · 2 startTime · 3 endTime · 4 fromVal0 · 5 fromVal1 ·
    //            6 toVal0 · 7 toVal1 · 8 repeat · 9 repeatType · 10 delay · 11 ease · 12 nid
    // type = Malody.Composer.AnimateType（None/MoveX/MoveY/Move/SizeW/SizeH/Size/ScaleX/ScaleY/Scale/Alpha/RotateZ）
    struct RoleAnim
    {
        int   type = 0;
        int   startMs = 0, endMs = 0;
        float from0 = 0, from1 = 0, to0 = 0, to1 = 0;
        int   repeat = 0, repeatType = 0, delay = 0, ease = 0, nid = 0;
    };
    struct RoleMod
    {
        char  srcFile[80]{};                        // 源文件名（Lua Module:Find 对应的贴图名）
        int   usage = 0, type = 0;                  // SkinModuleUsage / SubType
        char  desc[40]{};                           // meta.desc（皮肤作者命名）
        char  img[80]{};                            // 5000/5002 图片（或帧序列首帧）
        char  base[80]{};                           // 5002 filebase
        char  color[16]{};                          // "#RRGGBB"
        char  text[80]{};                           // 5001 文本模板
        char  numFile[80]{}, numBase[48]{}, numText[64]{};   // 5003 数字
        float x = 0, y = 0, dx = 0, dy = 0;
        float w = 0, h = 0;
        float size = 0;                             // 文本字号（%屏高）
        int   font = 0;                             // SkinModuleFont：0=None(不渲染) 1=Normal 2=Demi 3=Bold
        float numH = 0, numPad = 0;                 // 数字高（%屏高）/ 字距
        int   xu = 0, yu = 0, dxu = 0, dyu = 0, wu = 0, hu = 0;
        int   pivot = 4, layer = 0, order = 0, alpha = 100;
        bool  alphaSet = false;                     // ModuleParamBasic.alpha(field 6) 是否声明：
                                                    //  protobuf 缺省 = 0 = 不可见（证据见 ChartCore LoadSkin）
        int   rotate = 0;                           // 本地 Z 旋转（度，逆向自 ModuleParam.Rotate 字段 3）
        int   frames = 0, fps = 0, start = 0;
        int   flipx = 0, flipy = 0;                 // ModuleParamImage 8/9
        // ModuleParamImage 10 = slice（repeated int，Unity 9-slice 边框：左/下/右/上，单位=贴图像素）。
        // 逆向证据：fky::ConvertToSlice(Sprite, RepeatedField<int> slice, float scale)。
        int   slice[4] = { 0, 0, 0, 0 };
        int   sliceN = 0;
        float fill = 0.f;                           // ModuleParamImage 13 旧写法（float 比例）
        int   fillDir = 0;                          // ModuleParamFill：0 FromLeft 1 FromRight
        int   valint = 0;                           // ModuleParamImage 14（帧索引/图集序号）
        int   hide = 0, blend = 0, nobreak = 0;     // ModuleParamImage 16/19/20
        int   res = 0;                              // ModuleParamImage 2（SkinModuleRes：封面/头像）
        int   coexist = 0;                          // ModuleParam 16（同组模块共存排它）
        int   arrow = -1;                           // scene Arrow=11（1=Left/2=Up/3=Right/0=Static；-1=未声明）
        int   repeat = 0;                           // ModuleParamImage 5（帧序列重复次数，0=循环播放）
        int   numValint = 0;                        // ModuleParamNumber 7（数字图集序号）
        // ModuleMeta（Module field 3）：1 creator · 2 tags · 3 desc · 4 client · 5 time · 6 disabled · 7 hideInEditor
        char  metaCreator[48]{}, metaTags[64]{}, metaClient[48]{};
        long long metaTime = 0;
        int   metaDisabled = 0, metaHide = 0;
        // ScriptValue（usage=98 / type=4700..4702）载荷：valF/valS/valB + 范围
        float valF = 0.f, valMin = 0.f, valMax = 0.f;
        char  valS[64]{};
        int   valB = 0;
        // Sound（field 13 = ModuleParamSound）
        char  snd[64]{};
        int   sndLoop = 0, sndVol = 0, sndHit = 0;
        RoleAnim anims[8]; int animN = 0;           // field 20
        int   lane = -1, judgeScene = -1, noteType = -1;
        RoleCond scene[4]; int sceneN = 0;          // scenes（NoteX/Judge/...）
        RoleCond trig[6];  int trigN = 0;           // triggers（Press/Judge/Note/Fast/Slow/...）
        char  noteImg[6][64]{};                     // usage=1 音符图（head/body/tail...）
        int   noteN = 0;
        bool  anchorNote = false;
        // Lua Value（0..1）：覆盖 4900 填充条的引擎填充量（Cynosure kpsbar/hp）
        bool  fillSet = false;
        float fillVal = 0.f;
    };

    struct Roles
    {
        char title[128]{};
        char creator[128]{};
        char modeLabel[32]{};
        int  modeId = -1;
        // Meta.Key（逆向 dump.cs MetaModeKey：4=Keys 5=Scale 8=JudgePos）
        // 判定区宽度 = Scale × 屏宽。实测 info.asm：Phigros≈0.995 近全屏 /
        // Rurudo 4K≈0.421 / Gazer=0.7 / CrinoBaka 5K≈0.444 / Mango 4K≈0.35。
        // 0 = 旧格式无 Meta.Key（渲染端兜底整屏）。
        float keyScale = 0.f;
        int   keyKeys = 0;
        int   keyJudgePos = 0;
        // Meta.Mode.Key 其余字段（dump.cs MetaModeKey）：
        //   1 locked · 2 angle · 3 use3D · 6 lock3D · 7 lockScale · 8 judgePos
        //   angle = 轨道倾角（3D 时用于透视；2D 皮肤也用它给 Lua 查询）
        float keyAngle = 0.f;
        int   keyUse3D = 0, keyLock3D = 0, keyLockScale = 0;
        // Meta.Mode.Ring：2 dis（圆环半径，Ring 皮肤判定圈尺寸的来源）
        float ringDis = 0.f;
        // Meta.Mode.Taiko：1 scale · 2 judgePos · 3 judgeY
        float taikoScale = 0.f, taikoJudgeY = 0.f;
        int   taikoJudgePos = 0;
        char  script[96]{};                         // Meta field 23（皮肤 Lua 入口）
        char  cover[96]{};                          // Meta field 4（封面；ModuleParamImage.res=1 封面资源用）
        char  desc[128]{};                          // Meta field 2
        int   client = 0;                           // Meta field 6（引擎客户端标识）
        // Meta 24=minver · 25=feature · 26=free · 27=skinid · 28=updated
        int   minver = 0, feature = 0, freeFlag = 0, skinid = 0;
        long long updated = 0;
        // SkinFile 顶层：2=Disabled(重复 int，编辑器元数据) · 4=Settings(ComposerSetting) ·
        // 5=Defs(ModuleDef：1 type · 2 param)。全部解析入库，保证"每个字段都能解析"。
        int   disabledN = 0;
        int   setHideDef = 0, setHideImg = 0, setUsageFilter = 0;
        int   defsN = 0, defsType[16] = {};
        int   defsPivot[16] = {}, defsRotate[16] = {}, defsAlpha[16] = {},
              defsLayer[16] = {}, defsOrder[16] = {};
        float defsX[16] = {}, defsY[16] = {};
        int   keyLocked = 0;                        // MetaModeKey 1
        int   ringLockDis = 0;                      // MetaModeRing 1

        // 音符 / 长条 / 按键光（按轨；空串回退到内置皮肤）
        char note[kLanes][96]{};
        char holdHead[kLanes][96]{};
        char holdBody[kLanes][96]{};
        char holdTail[kLanes][96]{};
        char press[kLanes][96]{};

        // 打击特效（Frames 模块：hits-1.png + filebase=hits- + frames=9）
        char hitFxSeed[96]{};
        char hitFxBase[96]{};
        int  hitFxStart = 0, hitFxCount = 0, hitFxFps = 0;

        // 判定：静态图 = 闪条；动画帧 = 中央判定提示动画
        char judgeBar[kJudgeBarN][96]{};
        char judgePop[kJudgePopN][96]{};
        char judgeAnimSeed[kJudgeBarN][96]{};
        char judgeAnimBase[kJudgeBarN][96]{};
        int  judgeAnimCount[kJudgeBarN]{};
        int  judgeAnimStart[kJudgeBarN]{};   // 动画首帧编号（filebase + start）
        int  judgeAnimFps[kJudgeBarN]{};

        // 数字字型（Number 模块：filebase + filestart）
        char comboBase[64]{}; int comboStart = 0; float comboHeight = 0.f;
        char accBase[64]{};   int accStart = 0;   float accHeight = 0.f;

        // 场景元素
        char line[96]{};        // 判定线（noteinex.png）
        char grid[96]{};        // 轨道网格（bggrid.png）
        char hitBg[96]{};       // 判定底光（notehitbg.png）
        char judgeColor[96]{};  // 彩虹条（judgercolor.png）
        char avatar[96]{};      // 头像（progressrrd.png）
        char logo[96]{};        // 标识（m5logo.png）
        char bg[96]{};          // 背景（bg.png）
        char charImg[96]{};     // 右侧常驻小人（rurudokey.png / rrdbg）
        char charPress[4][96]{};// 小人打击姿势（Press 触发，按轨）
        char cheek[96]{};       // 右下托腮小人（scene Judge=0）

        bool valid = false;

        // ---- 整屏渲染模块表（info.asm 新格式才有） ----
        static const int kMaxMods = kModMax;
        RoleMod mods[kModMax]{};
        int     modCount = 0;
        bool    hasModules = false;
    };

    // 目录是否像皮肤（info.asm / info.json / 旧版 key-note-*.png / roles.ini）
    bool LooksLikeSkin(const char* dir);

    // 解析皮肤目录 → 角色表。支持：info.asm 新格式、旧版命名约定。
    bool Load(const char* dir, Roles* out);

    // 只读标题/作者/模式（皮肤列表用；不解析模块）
    void ReadMeta(const char* dir, char* title, int tn, char* creator, int cn, int* modeId);

    // 导入 .msp：解压到 <运行目录>\skin\<包名>\，返回皮肤目录
    bool ImportArchive(const wchar_t* mspPath, char* outDir, int outDirLen, char* err, int errLen);

    // 运行目录（DLL/EXE 同级）\skin
    void DefaultRoot(char* out, int n);
}
