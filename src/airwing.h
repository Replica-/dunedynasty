/** @file src/airwing.h The player's ornithopters and idle harvesters: a row of icons to pick them out, and patrols. */

#ifndef AIRWING_H
#define AIRWING_H

#include <stdbool.h>
#include <stdint.h>

struct Unit;

extern int  g_debugSpawnOrnithopters;   /* From --spawn-ornithopters N: N of them for the player at the start. */
extern bool g_debugTestAirwing;         /* From --test-ornithopters: logs to ornithopter_test.log, then quits. */
extern bool g_debugTestAirDefence;      /* From --test-air-defence: enemy troopers under a patrol; logs, then quits. */

extern void Airwing_Tick(void);         /* Server logic. */
extern void Airwing_OrderGiven(const struct Unit *u);  /* The player gave it an order: a patrol starts afresh. */

/* For a patrolling ornithopter: the target it may go for, and how fast it may fly.
 * False when the unit is not on patrol and the game should do as it always does.
 */
extern bool Airwing_OverrideTarget(const struct Unit *unit, uint16_t *encoded);
extern bool Airwing_LimitSpeed(const struct Unit *unit, uint16_t *speed);

/* Patrol takes a place on the map, for an ornithopter: the area to patrol. */
extern bool Airwing_ActionNeedsTarget(const struct Unit *u, int action);
extern void Airwing_SetPatrol(struct Unit *u, uint16_t encoded);
extern void Airwing_SetAttack(struct Unit *u, uint16_t encoded);   /* An attack order for an ornithopter: that target only. */

/* The player's ornithopters are always moving: an order cannot wait for them to stop. */
extern bool Airwing_ActsAtOnce(const struct Unit *u);

/* A missile fired at an aircraft aims where it will be: at launch, then as it flies. */
extern void Airwing_AimBullet(struct Unit *bullet, bool launch);

extern void Airwing_DrawPatrolAreas(void);  /* Viewport: the area of each selected patrolling ornithopter. */
extern void Airwing_DrawIcons(void);        /* In game, after the interface. */
extern bool Airwing_Click(int mouseX, int mouseY, bool add);   /* Screen pixels: true when it hit an icon. */
extern struct Unit *Airwing_UnitAtViewport(int x, int y);   /* The player's ornithopter at that point of the viewport, or NULL. */

#endif /* AIRWING_H */
