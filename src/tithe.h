/** @file src/tithe.h The Sardaukar tithe. */

#ifndef TITHE_H
#define TITHE_H

#include <stdbool.h>
#include <stddef.h>

#include "types.h"

struct Unit;

extern bool g_tithe;                 /* [enhancement] sardaukar_tithe (default on). */
extern bool g_debugTestTithe;        /* From --test-tithe: short timers, logs to tithe_test.log, then quits. */

extern void Tithe_Tick(void);                           /* Server logic. */
extern bool Tithe_ControlsUnit(const struct Unit *u);   /* The tithe frigate flies by itself, with no script. */
extern bool Tithe_IsCollectorTank(const struct Unit *u);   /* The tank that fetches the money: it has no turret. */

/* The text for the top bar, or false when there is nothing to show.  alert is
 * true when the player should worry.
 */
extern bool Tithe_GetStatusText(char *buf, size_t size, bool *alert);

#endif /* TITHE_H */
