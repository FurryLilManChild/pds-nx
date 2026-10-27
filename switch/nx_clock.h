/* pds-nx: CPU/GPU clock control. GPL v2 or later. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void nx_clock_apply(int cpu_mhz, int gpu_mhz); /* 0 = leave that clock alone */
void nx_clock_maintain(void);
void nx_clock_restore(void);
#ifdef __cplusplus
}
#endif
