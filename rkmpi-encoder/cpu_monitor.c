/*
 * CPU Monitor
 *
 * Reads /proc/stat for system-wide CPU usage.
 * Reads /proc/PID/stat for per-process CPU usage.
 */

#include "cpu_monitor.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* Read /proc/stat first line and extract total/idle jiffies */
static int read_proc_stat(unsigned long *total, unsigned long *idle) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;

    char line[256];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    /* Format: cpu user nice system idle iowait irq softirq steal guest guest_nice */
    unsigned long user, nice, system, idle_v, iowait, irq, softirq, steal;
    int n = sscanf(line, "cpu %lu %lu %lu %lu %lu %lu %lu %lu",
                   &user, &nice, &system, &idle_v, &iowait, &irq, &softirq, &steal);
    if (n < 4) return -1;

    *idle = idle_v + (n >= 5 ? iowait : 0);
    *total = user + nice + system + idle_v;
    if (n >= 5) *total += iowait;
    if (n >= 6) *total += irq;
    if (n >= 7) *total += softirq;
    if (n >= 8) *total += steal;

    return 0;
}

void cpu_monitor_init(CPUMonitor *mon) {
    memset(mon, 0, sizeof(*mon));
    read_proc_stat(&mon->prev_total, &mon->prev_idle);
}

void cpu_monitor_update(CPUMonitor *mon) {
    unsigned long total, idle;
    if (read_proc_stat(&total, &idle) < 0) return;

    unsigned long total_diff = total - mon->prev_total;
    unsigned long idle_diff = idle - mon->prev_idle;

    if (total_diff > 0) {
        mon->total_cpu = 100.0f * (1.0f - (float)idle_diff / (float)total_diff);
    }

    mon->prev_total = total;
    mon->prev_idle = idle;
}

float cpu_monitor_get_total(const CPUMonitor *mon) {
    return mon->total_cpu;
}

/* Read utime + stime for a process from /proc/PID/stat */
static int read_proc_pid_stat(pid_t pid, unsigned long *utime, unsigned long *stime) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);

    FILE *f = fopen(path, "r");
    if (!f) return -1;

    char line[512];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    /* Skip past comm field (may contain spaces/parens) */
    char *p = strrchr(line, ')');
    if (!p) return -1;
    p += 2; /* skip ") " */

    /* Fields after comm: state, ppid, pgrp, session, tty_nr, tpgid,
     * flags, minflt, cminflt, majflt, cmajflt, utime, stime */
    int n = sscanf(p, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
                   utime, stime);
    return (n == 2) ? 0 : -1;
}

/* Find or allocate a ProcCPU slot for the given PID */
static ProcCPU *find_proc_slot(CPUMonitor *mon, pid_t pid) {
    ProcCPU *empty = NULL;
    for (int i = 0; i < CPU_MONITOR_MAX_PROCS; i++) {
        if (mon->procs[i].pid == pid)
            return &mon->procs[i];
        if (!empty && mon->procs[i].pid == 0)
            empty = &mon->procs[i];
    }
    if (empty) {
        memset(empty, 0, sizeof(*empty));
        empty->pid = pid;
    }
    return empty;
}

float cpu_monitor_get_process(CPUMonitor *mon, pid_t pid) {
    if (pid <= 0) return -1.0f;

    unsigned long utime, stime;
    if (read_proc_pid_stat(pid, &utime, &stime) < 0) return -1.0f;

    ProcCPU *pc = find_proc_slot(mon, pid);
    if (!pc) return -1.0f;  /* no free slots */

    unsigned long cur_total = mon->prev_total;  /* use latest system total */
    unsigned long proc_delta = (utime + stime) - (pc->prev_utime + pc->prev_stime);
    unsigned long total_delta = cur_total - pc->prev_total;

    if (pc->prev_total > 0 && total_delta > 0) {
        pc->cpu_pct = 100.0f * (float)proc_delta / (float)total_delta;
    }

    /* Store current values for next delta */
    pc->prev_utime = utime;
    pc->prev_stime = stime;
    pc->prev_total = cur_total;

    return pc->cpu_pct;
}
