/** @file src/daynight.h Day and night, with shadows and headlights. */

#ifndef DAYNIGHT_H
#define DAYNIGHT_H

#include <stdbool.h>

extern bool g_day_night;            /* [graphics] day_night (default on). */
extern float g_debugTimeOfDay;      /* From --time-of-day <0-1>: freezes the clock.  Negative: normal. */
extern bool g_debugTestDayNight;    /* From --test-daynight: a screenshot after a few seconds, then quit. */

/* One full day and night is this many seconds of game time. */
#define DAYNIGHT_CYCLE_SECONDS (24 * 60)   /* 20% longer than 20 minutes. */

extern bool DayNight_Active(void);
extern float DayNight_Phase(void);              /* 0 to 1 through the cycle. */
extern float DayNight_Daylight(void);           /* 1 in full day, 0 in full night. */
extern const char *DayNight_Name(void);         /* "Dawn", "Day", "Dusk" or "Night". */
extern bool DayNight_IsNight(void);

/* While it is night and a sandstorm blows, units see no further than this many
 * tiles.  0 when there is no such limit.
 */
extern int DayNight_VisionLimit(void);

/* How far units and buildings see compared with the usual: a third further in daylight, shrinking
 * through dusk to well under what they usually see at night.  1 when there is no day and night.
 */
extern float DayNight_VisionScale(void);

extern void DayNight_Tick(void);                /* Server logic: night turns fog of war on. */
extern void DayNight_RestoreFog(void);          /* Gives back the player's own fog of war setting. */

extern void DayNight_DrawShadows(void);         /* Viewport, after the tiles, before the units. */
extern void DayNight_DrawDarkness(void);        /* Viewport, over everything, with the lights. */
extern void DayNight_DrawIndicator(int x, int y); /* Sun or moon with the position in the day, in the top bar. */

#endif /* DAYNIGHT_H */
