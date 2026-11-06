/** @file src/remains.h What is left of the dead: hulls and bodies that wear away into the sand. */

#ifndef REMAINS_H
#define REMAINS_H

#include "enumeration.h"
#include "types.h"

struct Unit;

extern void Remains_AddVehicle(const struct Unit *u);  /* A vehicle destroyed: its burnt hull. */
extern void Remains_AddIcon(uint16 iconID, enum HouseType houseID, tile32 position);  /* A body, as the game's overlay icon. */
struct AnimationCommandStruct;
extern void Remains_AddAnimation(const struct AnimationCommandStruct *script, uint16 iconGroup, enum HouseType houseID, tile32 position);  /* The body an infantry death animation ends with. */
extern void Remains_Draw(void);     /* Viewport, after the ground and before the fog. */

extern bool g_debugTestRemains;     /* From --test-remains: units destroyed, short-lived remains, screenshots, then quit. */
extern void Remains_DebugTest(void);   /* Server logic, with --test-remains. */
extern void Remains_DebugShots(void);

#endif /* REMAINS_H */
