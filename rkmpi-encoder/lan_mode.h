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

#endif /* LAN_MODE_H */
