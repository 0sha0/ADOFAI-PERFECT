#pragma once
// ============================================================
// GameDetour.h — 用 Detours 挂钩游戏 JIT 编译后的原生方法
//
// scrPlayer.DieByHitbox(string) 是游戏中唯一 hitbox 死亡入口
// （内部调用 Die(hitbox:true)，而 hitbox 死亡不走 noFail 存活分支）。
// 不死模式开启时直接吞掉这次死亡：星球不爆炸、音乐不停、继续游玩。
// ============================================================
namespace GameDetour
{
    bool Attach();   // 需在 GameBridge::Init 之后调用
    void Detach();
}
