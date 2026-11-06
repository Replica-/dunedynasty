/** @file src/fog.h Fog of war drawn by the graphics card. */

#ifndef FOG_H
#define FOG_H

#include <stdbool.h>

/* True when the fog is drawn as one smooth layer instead of with sprites: the
 * hardware effects are on.
 */
extern bool Fog_Active(void);

extern void Fog_Reset(void);    /* A new map: the fog starts out as it should be, with no creeping. */
extern void Fog_Update(void);   /* Every frame: moves the fog, and sets what the game sees as hidden. */
extern void Fog_Draw(void);     /* Viewport, after the tiles and before the units. */
extern void Fog_DrawBackground(void);   /* Viewport, before the tiles: the ground that has not been seen is the colour of the fog. */

#endif /* FOG_H */
