/** @file src/sandstorm.h Random sandstorms, drawn over the viewport. */

#ifndef SANDSTORM_H
#define SANDSTORM_H

#include <stdbool.h>

extern bool g_debugTestSandstorm; /* From --test-sandstorm: one storm at full strength, a screenshot, then quit. */
extern bool g_debugSandstormNow;  /* From --sandstorm-now: a storm at full strength that stays, for a look. */
extern bool g_sandstorms;         /* [graphics] sandstorms (default on). */

extern void Sandstorm_Draw(void);
extern void Sandstorm_Tick(void);
extern float Sandstorm_GetStrength(void);   /* 0 when there is no storm, up to 1. */
extern void Sandstorm_GetWind(float *x, float *y);  /* The storm's wind on the screen, a unit vector (kept after it ends). */

#endif /* SANDSTORM_H */
