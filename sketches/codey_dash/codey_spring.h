// sketches/codey_dash/codey_spring.h — 帧率无关的临界阻尼弹簧插值。
// 替代逐处手写、和帧率绑死的 `v += (target - v) * 0.15f` 衰减系数(见开发文档「显示驱动与配网技术选型」§4.2)。
// 纯头文件,无外部依赖。
#pragma once
#include <math.h>

// half-life(ms):值追到与目标差距减半所需的时间——描述"追多快"而不是每帧系数,
// 帧率/loop 节奏变化时手感不变。update() 传相邻两次调用的时间差(ms)。
struct Spring1D {
  float value = 0.0f, target = 0.0f;
  float halfLifeMs = 120.0f;

  void set(float v) { value = target = v; }
  void to(float t)  { target = t; }

  void update(float dtMs) {
    if (dtMs <= 0.0f) return;
    float decay = exp2f(-dtMs / halfLifeMs);
    value = target + (value - target) * decay;
  }
};
