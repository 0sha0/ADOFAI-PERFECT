--=== 常量配置区 ==============================================================
local ANIM_CONFIG = {          -- 动画缓动配置
    mtextureDoMoveY_ease =  {p1 = 0.42, p2 = 0, p3 = 1, p4 = 1},
    ROTATE_ANIM_ease = {p1=0.2, p2=0.55, p3=0.25, p4=1},
    breath_ease = {p1 = 0.42,p2 = 0,p3 = 0.58,p4 = 1},
    DoAlpha_ease = {p1=0.86, p2=0, p3=0.59, p4=-0.11},
    combochar_move_ease = {p1=0.25, p2=0.84, p3=0.25, p4=1}
}

--========= 音频模块 =========--

local state = {
    hasEarlyPlayed = false,
    hasMissPlayed = false,
    hasFinalPlayed = false,
    lastMissTime = -200
}

-- 结算音频阈值配置表
local FINAL_THRESHOLDS = {
    { value = 100,    file = "m5w.wav" },
    { value = 99.6,   file = "996w.wav" },
    { value = 99,     file = "99w.wav" },
    { value = 98,     file = "98w.wav" },
    { value = 97,     file = "97w.wav" },
    { value = 96,     file = "96w.wav" },
    { value = 88,     file = "95w.wav" },
    { value = -math.huge, file = "bakaw.wav" }  -- 默认项
}
-- 结算音频匹配函数
local function PlayFinalAudio(macc)
    -- 查找匹配的音频文件
    local targetFile = FINAL_THRESHOLDS[#FINAL_THRESHOLDS].file  -- 默认值
    for _, v in ipairs(FINAL_THRESHOLDS) do
        if macc >= v.value then
            targetFile = v.file
            break
        end
    end
    -- 加载并播放
    local res = FINAL_THRESHOLDS[targetFile]
    if res then
        Audio:Play(res, volume_value)
    end
end
-- 随机播放miss音频函数
local function PlayMissSound(currentTime)
    -- 短路返回：音频未启用时直接退出
    if not audio_on or state.hasMissPlayed then return end

    local res = audioResources_miss[math.random(1,2)]
    Audio:Play(res, volume_value)
    state.hasMissPlayed = true
    state.lastMissTime = currentTime
end




--========= 倒计时 =========--

-- 时间格式化函数（毫秒转 MM.SS）
local function FormatTime(milliseconds)
    local total_seconds = math.floor(milliseconds / 1000)
    local minutes = math.floor(total_seconds / 60)
    local seconds = total_seconds % 60
    return string.format("%02d.%02d", minutes, seconds) -- MM.SS 格式
end

function Init()
    --=== MOD解析 ===============================================================

    local mod = Module:Find("mod")
    speedMod = 1  -- 默认速度
    for mod in string.gmatch(mod.Text, "[%a+]+") do
        if mod == "Dash" then speedMod = 1.2
        elseif mod == "Rush" then speedMod = 1.5
        elseif mod == "Slow" then speedMod = 0.8 end
    end

    --=== 环境参数获取 ===============================================================

    local INFINITE_LOOP = 114514  -- 无限循环标识值
    local screenWidth = Game:Width()
    startTime = Game:StartTime()
    endTime = Game:AudioLength()

    --=== 模块初始化 ===============================================================

    local modules_m5logo = Module:Find("m5logo")
    local modules_cout = Module:Find("cout")
    local modules_cin = Module:Find("cin")
    local modules_cyellow = Module:Find("cyellow")
    local modules_countdownbg = Module:Find("countdownbg")
    local modules_pause = Module:Find("pause")
    local modules_title = Module:Find("title")
    local modules_mbpm = Module:Find("bpm")
    local modules_ver = Module:Find("ver")
    local modules_progresslight = Module:Find("progresslight")
    local modules_progress = Module:Find("progress")
    local modules_progressrrd = Module:Find("progressrrd")
    if Game:FieldMeta("Angle") ~=0 then
        local wangge = Module:Find("wangge")
        wangge.RotateX = -Game:FieldMeta("Angle")
        wangge.Width = 3456
        wangge.Height = 3456
    end
    modules_mcombo = Module:Find("combok")
    mjudge = Module:Find("judge")

    --=== 偏移指示器 =========================================================

    indicator = Module:Find("indicator")
    offm={}
    for i=0,3 do
        offm[i]=Module:Find("offm"..i)
    end
    offvalue = 0

    --=== KPS 直方图初始化 =====================================================

    -- [1] 基础配置获取
    kps_histogram_on = Module:GetBool("kps图开关")  -- 功能开关状态
    barx = Module:Find("bar1")                     -- 基础条形元件
    maxkps = Module:Find("maxkps")                 -- 最大值显示元件
    currentkps = Module:Find("currentkps")         -- 当前值显示元件
    currentkps.Text = 0                            -- 初始化显示文本
    
    -- [2] 时间窗口数据结构初始化
    kpstime = {{}, {}, {}, {}}                     -- 击打时间原始数据
    kpsvalue = {0, 0, 0, 0, 0}                    -- 单轨/总KPS缓存
    total_kps = 0                                  -- 总KPS计算值
    last_clean_time = 0                            -- 清理计时器
    
    -- [3] 环形队列初始化
    kps_windows = {}  -- 轨道时间窗口(优化内存结构)
    for i = 1, 4 do  -- 4个独立轨道
        kps_windows[i] = {
            buffer = {},    -- 时间戳环形缓冲区(容量1000)
            head = 1,       -- 有效数据头指针
            tail = 1,       -- 下次写入位置
            count = 0       -- 当前有效计数
        }
    end
    
    -- [4] 时间分段配置
    note_duration = endTime - startTime           -- 总持续时间(毫秒)
    local seconds = math.ceil(note_duration / 1000)  -- 转换为秒数
    slot_count = math.min(seconds, 180)           -- 时间段数(最大180)
    slot_length = note_duration / slot_count      -- 单段时长(毫秒)
    
    -- [5] 直方图条形初始化
    barAHS = 600 / slot_count  -- 自适应条形高度
    bar = {}                    -- 条形元件容器
    max_kps_per_slot = {}       -- 时段最大KPS记录
    
    -- 创建并配置所有条形
    for i = 1, slot_count do
        max_kps_per_slot[i] = 0  -- 初始化时段记录

        -- 克隆基础条形并定位
        bar[i] = Module:Clone(barx, "kps_bar_"..i)
        bar[i].Height = barAHS                      -- 动态高度
        bar[i].Y = barAHS * (i - 1)                 -- 垂直排列
        bar[i]:SetColor(255, 255, 255)              -- 默认白色
        bar[i].Width = 0                            -- 初始宽度
        bar[i].Alpha = 100                          -- 保持可见
    end
    
    -- [6] 状态控制变量
    maxvalue = 0            -- 历史最大KPS值
    lastBarUpdateTime = 0   -- 最后更新时间戳
    isBarDataDirty = false  -- 数据更新标记
    cached_total_kps = 0    -- 当前KPS缓存值


    --=== 音频模块 ===========================================================

    audio_on = Module:GetBool("人声开关")
    volume_value= Module:GetNumber("人声音量")
    luaacc= Module:Find("luaacc")
    audioResources_miss={}
    -- 音频预加载
    if audio_on then
        audioResources_early = Audio:Load("senow.wav")
        audioResources_miss[1] = Audio:Load("missuew.wav")
        audioResources_miss[2] = Audio:Load("misskiraiw.wav")
        for _, v in ipairs(FINAL_THRESHOLDS) do
        FINAL_THRESHOLDS[v.file] = Audio:Load(v.file)
        end
    end

    --=== 连击提示图 =========================================================

    luacombo = Module:Find("luacombo")
    rrdcombo_trigger_value= Module:GetNumber("连击提示图触发值 (默认每100combo)")
    rrdcombo_on= Module:GetBool("连击提示图开关")
    char = {}
    animating = false  -- 防止重复触发
    -- 根据开关状态预加载
    if rrdcombo_on then
        for i = 1, 4 do
            char[i] = Module:Find("rrdcb"..i)
        end
        
    else
        local char = Module:Find("rrdcb"..math.random(1,4))
        -- 确保元件可见并初始化动画开始位置
        char.Alpha = 100
        char.Y = -20
        -- 呼吸动画
        char:DoMoveY({
            start = 0,
            finish = 2500 * speedMod,
            from = -20,
            to = 0,
            repeats = INFINITE_LOOP,
            repeatType = 2,
            custom = ANIM_CONFIG.breath_ease
        })
    end

    --=== 彩判定结果记数 =========================================================

    best = Module:Find("best")
    marv = Module:Find("marv")
    marvvalue= 0
    bestvalue= 0
    is_marv=0
    is_best=0

    --=== 倒计时 =========================================================

    countdown = Module:Find("countdown")
    countdown.Text = FormatTime(endTime)
    --倒计时入场动画
    countdown:DoMoveY({
        start = startTime -  1800,
        finish = startTime - 1000,
        from = 240,
        to = -39,
        custom = mtextureDoMoveY_ease
    })

    --=== 动画效果 ===============================================================

    --========== 进度条动画 ==========--
    
    modules_progresslight:DoMoveX({
        start = 0,
        finish = endTime,
        from = 0,
        to = screenWidth
    })
    modules_progress:DoWidth({
        start = 0,
        finish = endTime,
        from = 0,
        to = screenWidth
    })
    --底部rurudo小人
    modules_progressrrd:DoMoveX({
        start = 0,
        finish = endTime,
        from = 0,
        to = screenWidth
    })
    --左侧kps
    currentkps:DoMoveY({
        start = startTime,
        finish = endTime,
        from = 0,
        to = 600
    })
    maxkps:DoMoveY({
        start = startTime,
        finish = endTime,
        from = 0,
        to = 600
    })




    --==========  轨道文字呼吸效果 ==========--
    rurudotext = Module:Find("rurudotext")
    rurudotext.RotateX = -15  -- 额外旋转
    rurudotext:DoAlpha({
        start = startTime - 1200,
        finish = startTime + 200*speedMod,
        from = 80,
        to = 20,
        repeats = INFINITE_LOOP,
        repeatType = 2,
        custom = ANIM_CONFIG.breath_ease
    })
    

    --========== 暂停与信息部件动画 ==========--

    local mtexture = {
        modules_cyellow, modules_m5logo, modules_cout, modules_cin, modules_pause,
        modules_title, modules_mbpm, modules_ver, modules_countdownbg,
        countdown
    }
    
    --===== 渐显动画 =====--

    for i = 2, 10 do
        mtexture[i]:DoAlpha({
            start = startTime - 1600,
            finish = startTime - 1400,
            from = 50,
            to = 100
        })
    end

    --===== 暂停部件入场动画 =====--

    for i = 1, 5 do
        mtexture[i]:DoMoveY({
            start = startTime - 1700,
            finish = startTime - 1500,
            from = 240,
            to = -87,
            custom =mtextureDoMoveY_ease
        })
    end
    
    --===== 信息部件入场动画 =====--

    
    modules_title:DoMoveY({
        start = startTime -  1800,
        finish = startTime - 1100,
        from = 240,
        to = -81,
        custom =mtextureDoMoveY_ease
    })
    modules_mbpm:DoMoveY({
        start = startTime -  1800,
        finish = startTime - 1200,
        from = 240,
        to = -112.5,
        custom =mtextureDoMoveY_ease
    })
    modules_ver:DoMoveY({
        start = startTime -  1800,
        finish = startTime - 1300,
        from = 240,
        to =  -139.5,
        custom =mtextureDoMoveY_ease
    })
    modules_countdownbg:DoMoveY({
        start = startTime - 1600,
        finish = startTime - 1500,
        from = 240,
        to = -39,
        custom =mtextureDoMoveY_ease
    })

    --===== logo动画 =====--

    -- 橙色圆环光效动画 --
    modules_cyellow:DoAlpha({
        start = startTime - 1000,
        finish = startTime + 500,
        from = 0,
        to = 90
    })
    modules_cyellow:DoAlpha({
        start = startTime + 500,
        finish = startTime + 2000,
        from = 90,
        to = 40,
        repeats = INFINITE_LOOP,
        repeatType = 2,
        custom = ANIM_CONFIG.breath_ease
    })
    -- 蓝色圆环旋转动画 --
    modules_cout:DoRotate({
        start = startTime - 1350,
        finish = startTime - 350,
        from = 0,
        to = 360,
        repeats = INFINITE_LOOP,
        delay = 2000*speedMod,
        custom = ANIM_CONFIG.ROTATE_ANIM_ease
    })
    -- malodyv logo旋转动画 --
    modules_m5logo:DoRotate({
        start = startTime - 1300,
        finish = startTime - 1000,
        from = 0,
        to = 360,
        repeats = INFINITE_LOOP,
        delay = 2000*speedMod,
        custom = ANIM_CONFIG.ROTATE_ANIM_ease
    })
end



function OnInput()
    if not kps_histogram_on then return end
    
    local input = Game:InputEvent()
    local hitx = input:HitX()
    local inputType = input:Type()
    local currentTime = Game:Time()

    -- 仅处理有效轨道（1-4）的按下事件
    if hitx >= 1 and hitx <= 4 and inputType == 1 then
        local window = kps_windows[hitx]
        
        -- 写入新时间戳（覆盖最旧数据）
        window.buffer[window.tail] = currentTime
        window.tail = (window.tail % 1000) + 1
        
        -- 如果缓冲区已满，移动头指针
        if window.tail == window.head then
            window.head = (window.head % 1000) + 1
        else
            window.count = window.count + 1  -- 仅当未覆盖有效数据时增加计数
        end
    end
end




function Update()
    local currentTime = Game:Time()
    local start_time = startTime
    local end_time = endTime
    

    --=== KPS 直方图 =====================================================
    if kps_histogram_on and currentTime >= start_time then
        local last_clean = last_clean_time
        if currentTime - last_clean >= 100 then
            last_clean_time = currentTime
            local cleanup_threshold = currentTime - 1000
            -- 并行清理轨道数据
            local total = 0
            for i = 1, 4 do
                local window = kps_windows[i]
                local head = window.head
                local tail = window.tail
                
                -- 清理过期数据（指针操作优化）
                while head ~= tail do
                    local ts = window.buffer[head]
                    if ts and ts < cleanup_threshold then
                        head = (head % 1000) + 1
                        window.count = window.count - 1
                    else
                        break
                    end
                end
                window.head = head
                total = total + window.count
            end
            
            -- 更新 KPS 显示
            cached_total_kps = total
            currentkps.Text = total
            
            -- 颜色梯度预计算
            local factor = math.min(total, 28)
            local g = 255 - factor * 7
            local b = 252 - factor * 9
            currentkps:SetColor(255, g, b)
            
            -- 更新最大值
            if total > maxvalue then
                maxvalue = total
                maxkps.Text = "/"..total
            end
            
            -- 槽位计算
            local elapsed = currentTime - start_time
            local slot_len = slot_length
            local raw_slot = (elapsed + slot_len - 1) // slot_len
            local current_slot = raw_slot < 1 and 1 or (raw_slot > slot_count and slot_count or raw_slot)
            
            if current_slot <= slot_count then
                -- 更新槽位数据
                if total > max_kps_per_slot[current_slot] then
                    max_kps_per_slot[current_slot] = total
                    isBarDataDirty = true
                end
                
                -- 批量更新直方图
                if isBarDataDirty then
                    local max_val = maxvalue
                    local max_factor = math.min(max_val, 28)
                    local r_ratio = max_factor * 3.5
                    local b_ratio = max_factor * 5.5
                    
                    for i = 1, slot_count do
                        local bar_item = bar[i]
                        local slot_kps = max_kps_per_slot[i]
                        local is_peak = slot_kps == max_val
                        
                        -- 颜色计算
                        local g = is_peak and (255 - max_factor*7) or (255 - (slot_kps/max_val)*r_ratio)
                        local b = is_peak and (255 - max_factor*9) or (255 - (slot_kps/max_val)*b_ratio)
                        bar_item:SetColor(255, math.floor(g), math.floor(b))
                        
                        -- 宽度计算
                        local width = (slot_kps > 0) and (is_peak and 70 or (slot_kps/max_val*70)) or 0
                        bar_item.Width = math.floor(width)
                    end
                    isBarDataDirty = false
                end
            end
        end
    end

    --=== 音频模块 =====================================================
    if audio_on then
        -- 开场音频
        local early_trigger = start_time - 2000
        if not state.hasEarlyPlayed and currentTime >= early_trigger then
            Audio:Play(audioResources_early, volume_value)
            state.hasEarlyPlayed = true
        end
        
        -- 结算音频
        local final_trigger = end_time + 50
        if not state.hasFinalPlayed and currentTime >= final_trigger then
            local acc_text = luaacc.Text
            local macc = tonumber(acc_text:match("%d+%.?%d*")) or 0
            PlayFinalAudio(macc)
            state.hasFinalPlayed = true
        end
    end

    --=== 倒计时 =====================================================
    if currentTime < end_time then
        local remaining = end_time - currentTime
        if remaining % 100 < 16 then
            countdown.Text = FormatTime(remaining)
            local red = remaining <= 10000
            countdown:SetColor(255, red and 50 or 255, red and 50 or 255)
        end
    end
end



function OnHit()
    local event = Game:HitEvent()
    local judgeResult = event:JudgeResult()
    local hitOffset = event:Offset()
    local abs_offset = math.abs(hitOffset)
    local currentTime = Game:Time()
    

    --=== 音频模块 ===========================================================

    if judgeResult == 4 then
        PlayMissSound(currentTime)
    elseif state.hasMissPlayed and currentTime - state.lastMissTime > 400 then
        state.hasMissPlayed = false
    end

    --=== 偏移指示器 =========================================================

    if judgeResult <= 3 then
        -- 缓存元件引用
        local offvalue = -hitOffset
        local indicator_x = indicator.X
        -- 位移动画
        indicator:DoMoveX({
            start = currentTime,
            finish = currentTime + 500 * speedMod,
            from = indicator_x,
            to = offvalue
        })
        
        local effect_index = (abs_offset <= 16) and 0 or judgeResult
        local shadow = Module:Shadow(offm[effect_index], 3000 * speedMod)
        shadow.X = offvalue
        shadow:DoAlpha({
            start = currentTime,
            finish = currentTime + 3000 * speedMod,
            from = 70,
            to = 0,
            custom =ANIM_CONFIG.DoAlpha_ease
        })
    

        
    --=== 连击动画 ===========================================================

        modules_mcombo:DoMoveY({start=currentTime, finish=currentTime+100*speedMod, from=2.1, to=0, ease=2})
        modules_mcombo:DoAlpha({start=currentTime, finish=currentTime+100*speedMod, from=60, to=20})

    end




    --=== 连击提示图 =========================================================

    if rrdcombo_on then
        local current_combo = tonumber(luacombo.Text) or 0
        local combo_trigger = current_combo % rrdcombo_trigger_value
        if combo_trigger <= 3 and current_combo >= rrdcombo_trigger_value and not animating then
            animating = true
            local target_id = math.random(1,4)
            local combo_char = char[target_id]
            
            combo_char:DoAlpha{start=currentTime, finish=currentTime+100*speedMod, from=0, to=100}
            combo_char:DoMoveX{
                start=currentTime, 
                finish=currentTime+1100*speedMod, 
                from=520, 
                to=0, 
                custom={p1=0.25,p2=0.84,p3=0.25,p4=1}
            }
            combo_char:DoAlpha{
                start=currentTime+500*speedMod, 
                finish=currentTime+950*speedMod, 
                from=100, 
                to=0
            }
        end
        
        if judgeResult == 4 or combo_trigger > 3 then
            animating = false
        end
    end
    --=== 判定动画 ===========================================================

    if judgeResult >= 2 and judgeResult <= 4 then
        mjudge:DoResize(
            {start=currentTime, finish=currentTime+100*speedMod, from=105, to=73.5},
            {start=currentTime, finish=currentTime+100*speedMod, from=30, to=21}
        )
        mjudge:DoAlpha({
            start=currentTime,
            finish=currentTime+100*speedMod,
            from=100,
            to=0,
            custom=ANIM_CONFIG.DoAlpha_ease
        })

    --=== 彩判定计数 =========================================================

    elseif judgeResult == 1 then
        
        is_marv = (abs_offset <= 16)
        marvvalue = marvvalue + (is_marv and 1 or 0)
        bestvalue = bestvalue + (is_marv and 0 or 1)
        marv.Text = marvvalue
        best.Text = bestvalue
    end 
    
    
    
end


function OnRetry()
    --=== 音频模块状态重置 ===--
    state.hasEarlyPlayed = false
    state.hasMissPlayed = false
    state.hasFinalPlayed = false
    state.lastMissTime = -200

    --=== 连击提示图状态重置 ===--
    animating = false  -- 解除动画锁定
        -- 连击提示图元件状态重置
    for i = 1, 4 do
        if char[i] then
            char[i].Alpha = 0
            char[i]:CancelAnimate()  -- 停止动画
            char[i].X = 520          -- 重置初始位置
        end
    end

    --=== KPS直方图模块重置 ===--
    -- 重置轨道时间窗口
    for i = 1, 4 do
        kps_windows[i] = {
            buffer = {},
            head = 1,
            tail = 1,
            count = 0
        }
    end

    -- 重置模块状态
    last_clean_time = 0
    cached_total_kps = 0
    maxvalue = 0
    isBarDataDirty = true

    -- 重置显示元件
    currentkps.Text = "0"
    maxkps.Text = "/0"
    currentkps:SetColor(255, 255, 255)

    -- 批量重置直方图条形
    for i = 1, slot_count do
        max_kps_per_slot[i] = 0
        bar[i].Width = 0
        bar[i]:SetColor(255, 255, 255)  -- 恢复默认白色
    end

    --=== 判定计数重置 ===--
    marvvalue = 0
    bestvalue = 0
    marv.Text = "0"
    best.Text = "0"

    --=== 倒计时重置 ===--
    countdown.Text = FormatTime(endTime)
    countdown:SetColor(255, 255, 255)

    --=== 其他动画状态重置 ===--
    mjudge.Alpha = 0
    indicator.X = 0
end

