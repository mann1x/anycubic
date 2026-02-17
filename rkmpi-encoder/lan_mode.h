/*
 * LAN Mode Management
 *
 * Query and enable LAN print mode via RPC to gkapi (port 18086).
 * Uses one-shot TCP connections with JSON-RPC + ETX delimiter.
 */

#ifndef LAN_MODE_H
#define LAN_MODE_H

/* Query LAN mode status.
 * Returns: 1=enabled, 0=disabled, -1=error */
int lan_mode_query(void);

/* Enable LAN mode.
 * Returns: 0=success (or already enabled), -1=error */
int lan_mode_enable(void);

/* Fix wlan0 route priority when eth1 and wlan0 share a subnet.
 * Re-adds wlan0 routes with metric 100 so eth1 is preferred.
 * Returns: 1=fixed/not-needed, 0=failed (retry later) */
int wifi_fix_route_priority(void);

/* Optimize RTL8723DS WiFi driver for lower CPU usage.
 * Enables A-MSDU aggregation and disables power management.
 * Returns: 1=done/not-applicable, 0=failed (retry later) */
int wifi_optimize_driver(void);

#endif /* LAN_MODE_H */
