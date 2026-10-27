/*
 * pds-nx: raise the CPU/GPU clocks while the game runs, restore them on exit.
 *
 * Uses clkrst on firmware 8.0.0+ and pcv on older firmware. A value of 0
 * leaves that clock alone. If sys-clk is running with its own profile for
 * this title, sys-clk wins, since it reapplies its settings periodically.
 *
 * This file is part of pds-nx and is licensed under the GNU GPL v2 or later.
 */
#include <switch.h>

#include "nx_clock.h"
#include "nx_log.h"

static int use_clkrst;
static int ready;
static u32 orig_cpu, orig_gpu;
static int set_cpu, set_gpu;
static int want_cpu, want_gpu;

static u32 get_rate(PcvModuleId id, PcvModule old)
{
    u32 hz = 0;
    if (use_clkrst) {
        ClkrstSession s;
        if (R_SUCCEEDED(clkrstOpenSession(&s, id, 3))) {
            clkrstGetClockRate(&s, &hz);
            clkrstCloseSession(&s);
        }
    } else {
        pcvGetClockRate(old, &hz);
    }
    return hz;
}

static Result set_rate(PcvModuleId id, PcvModule old, u32 hz)
{
    Result rc;
    if (use_clkrst) {
        ClkrstSession s;
        rc = clkrstOpenSession(&s, id, 3);
        if (R_SUCCEEDED(rc)) {
            rc = clkrstSetClockRate(&s, hz);
            clkrstCloseSession(&s);
        }
    } else {
        rc = pcvSetClockRate(old, hz);
    }
    return rc;
}

void nx_clock_apply(int cpu_mhz, int gpu_mhz)
{
    if (!ready) {
        use_clkrst = hosversionAtLeast(8, 0, 0);
        Result rc = use_clkrst ? clkrstInitialize() : pcvInitialize();
        if (R_FAILED(rc)) {
            nxlog("clock: service init failed: 0x%x", rc);
            return;
        }
        ready = 1;
        orig_cpu = get_rate(PcvModuleId_CpuBus, PcvModule_CpuBus);
        orig_gpu = get_rate(PcvModuleId_GPU, PcvModule_GPU);
        nxlog("clock: was CPU %u MHz, GPU %u MHz", orig_cpu / 1000000, orig_gpu / 1000000);
    }
    want_cpu = cpu_mhz;
    want_gpu = gpu_mhz;
    if (cpu_mhz > 0) {
        Result rc = set_rate(PcvModuleId_CpuBus, PcvModule_CpuBus, (u32)cpu_mhz * 1000000u);
        set_cpu = R_SUCCEEDED(rc);
        nxlog("clock: CPU -> %d MHz (%s, now %u MHz)", cpu_mhz, set_cpu ? "ok" : "failed",
              get_rate(PcvModuleId_CpuBus, PcvModule_CpuBus) / 1000000);
    }
    if (gpu_mhz > 0) {
        Result rc = set_rate(PcvModuleId_GPU, PcvModule_GPU, (u32)gpu_mhz * 1000000u);
        set_gpu = R_SUCCEEDED(rc);
        nxlog("clock: GPU -> %d MHz (%s, now %u MHz)", gpu_mhz, set_gpu ? "ok" : "failed",
              get_rate(PcvModuleId_GPU, PcvModule_GPU) / 1000000);
    }
}

/* The system can reset clocks (e.g. after a visit to the HOME menu). Called
 * once a second; puts our clocks back if they changed. */
void nx_clock_maintain(void)
{
    if (!ready)
        return;
    if (set_cpu && get_rate(PcvModuleId_CpuBus, PcvModule_CpuBus) != (u32)want_cpu * 1000000u)
        set_rate(PcvModuleId_CpuBus, PcvModule_CpuBus, (u32)want_cpu * 1000000u);
    if (set_gpu && get_rate(PcvModuleId_GPU, PcvModule_GPU) != (u32)want_gpu * 1000000u)
        set_rate(PcvModuleId_GPU, PcvModule_GPU, (u32)want_gpu * 1000000u);
}

void nx_clock_restore(void)
{
    if (!ready)
        return;
    if (set_cpu && orig_cpu)
        set_rate(PcvModuleId_CpuBus, PcvModule_CpuBus, orig_cpu);
    if (set_gpu && orig_gpu)
        set_rate(PcvModuleId_GPU, PcvModule_GPU, orig_gpu);
    if (use_clkrst)
        clkrstExit();
    else
        pcvExit();
    ready = set_cpu = set_gpu = 0;
}
