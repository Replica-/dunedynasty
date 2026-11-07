/** @file src/tithe.h The Sardaukar tithe. */

#ifndef TITHE_H
#define TITHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "types.h"

struct Unit;

extern bool g_tithe;                 /* [enhancement] sardaukar_tithe (default on). */
extern bool g_debugTestTithe;        /* From --test-tithe: short timers, logs to tithe_test.log, then quits. */

extern void Tithe_Tick(void);                           /* Server logic. */
extern bool Tithe_ControlsUnit(const struct Unit *u);   /* The tithe frigate flies by itself, with no script. */
extern bool Tithe_IsCollectorTank(const struct Unit *u);   /* The tank that fetches the money: it has no turret. */

/* The game times (g_timerGame) of the spice tax pickups due within horizon ticks from
 * now, soonest first, at most max of them.  visiting is true while the frigate is here.
 */
extern int Tithe_GetPickups(int64_t *when, int max, int64_t horizon, bool *visiting);

/* "Spice Tax: <credits>" for the top bar, or false when there is none.  alert is
 * true when the player should worry.
 */
extern bool Tithe_GetStatusText(char *buf, size_t size, bool *alert);

#endif /* TITHE_H */
