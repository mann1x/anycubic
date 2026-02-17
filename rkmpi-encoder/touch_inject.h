/*
 * Touch Event Injection
 *
 * Inject touch events via /dev/input/event0 for LCD interaction.
 * Transforms coordinates based on display orientation.
 */

#ifndef TOUCH_INJECT_H
#define TOUCH_INJECT_H

/* Inject a tap at (x, y) on the web display coordinate system.
 * Coordinates are transformed to touch panel coordinates based on
 * the printer's display orientation.
 *
 * x, y: coordinates on the web display
 * duration_ms: touch duration (0 = single tap ~50ms)
 *
 * Returns: 0 on success, -1 on error */
int touch_inject(int x, int y, int duration_ms);

#endif /* TOUCH_INJECT_H */
