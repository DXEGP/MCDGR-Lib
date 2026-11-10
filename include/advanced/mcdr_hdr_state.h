// mcdr_hdr_state.h — HDR 的进程级状态，定义在普通 TU（surface 模块单元与 DLSS 侧
// 都读它；模块 purview 里的定义无法被普通 TU 链接）。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 0 = auto（按 DXGI 输出当前色彩空间判定），1 = 强制开，2 = 强制关。越界夹到 0。
void mcdr_hdr_state_set_mode(int32_t mode);
int32_t mcdr_hdr_state_mode(void);

// 最近一次创建的 surface 是否真的以 scRGB 呈现（0/1）。
void mcdr_hdr_state_set_active(int32_t active);
int32_t mcdr_hdr_state_active(void);

// 最近一次建链时实际用掉的模式（0/1/2）：换格式必须重建链，JVM 侧拿它判断要不要
// 提示重启。初值 -1 = 还没建过链。
void mcdr_hdr_state_set_applied_mode(int32_t mode);
int32_t mcdr_hdr_state_applied_mode(void);

#ifdef __cplusplus
}
#endif
