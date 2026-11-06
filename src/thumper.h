/** @file src/thumper.h Thumpers: infantry plant one in the sand to call the worms. */

#ifndef THUMPER_H
#define THUMPER_H

#include <stdbool.h>
#include <stdint.h>

struct Unit;

extern bool g_debugTestThumper;     /* From --test-thumper: a worm, a soldier and a thumper; logs to thumper_test.log, then quits. */

extern bool Thumper_IsDropper(const struct Unit *u);   /* Atreides and Fremen foot soldiers: their Retreat button plants a thumper. */
extern bool Thumper_Drop(struct Unit *u);              /* Server: plant one where the unit is, for 100 credits. */
extern uint16_t Thumper_WormTarget(const struct Unit *worm);   /* The thumper a worm hears (an encoded tile), or 0. */
extern bool Thumper_Eat(uint16_t packed);              /* A worm swallows the thumper on that tile, if there is one. */
extern void Thumper_Draw(void);                        /* Viewport, over the ground. */
extern void Thumper_DebugTest(void);                   /* Server logic, with --test-thumper. */

#endif /* THUMPER_H */
