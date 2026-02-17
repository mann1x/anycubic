/*
 * CPU Monitor
 *
 * System and per-process CPU usage tracking via /proc/stat.
 */

#ifndef CPU_MONITOR_H
#define CPU_MONITOR_H

#include <sys/types.h>
#include <stdint.h>

/* Per-process delta tracking */
#define CPU_MONITOR_MAX_PROCS 8

typedef struct {
    pid_t pid;                      /* 0 = unused slot */
    unsigned long prev_utime;
    unsigned long prev_stime;
    unsigned long prev_total;       /* system-wide total jiffies at last sample */
    float cpu_pct;                  /* last computed CPU % */
} ProcCPU;

typedef struct {
    /* Previous /proc/stat totals */
    unsigned long prev_total;
    unsigned long prev_idle;
    /* Computed CPU % */
    float total_cpu;
    /* Per-process delta tracking */
    ProcCPU procs[CPU_MONITOR_MAX_PROCS];
} CPUMonitor;

/* Initialize CPU monitor (reads initial baseline) */
void cpu_monitor_init(CPUMonitor *mon);

/* Update CPU measurements (call periodically, e.g. every 1-2 seconds) */
void cpu_monitor_update(CPUMonitor *mon);

/* Get total system CPU usage (0-100) */
float cpu_monitor_get_total(const CPUMonitor *mon);

/* Get delta-based CPU usage for a specific process (0-100%).
 * Tracks previous values internally per PID. Call after cpu_monitor_update().
 * Returns -1 on error (process gone), 0 on first call (no delta yet). */
float cpu_monitor_get_process(CPUMonitor *mon, pid_t pid);

#endif /* CPU_MONITOR_H */
