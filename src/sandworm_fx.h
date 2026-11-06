/** @file src/sandworm_fx.h A sandworm moving under the sand. */

#ifndef SANDWORM_FX_H
#define SANDWORM_FX_H

struct Unit;

/* Draws a worm: the shaking mound, the sand it throws, and the furrow it leaves.
 * Once a frame for each worm, in the viewport, over the ground.
 */
extern void SandwormFx_Draw(const struct Unit *u);

#endif /* SANDWORM_FX_H */
