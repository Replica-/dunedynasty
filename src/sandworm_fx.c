/** @file src/sandworm_fx.c A sandworm moving under the sand.
 *
 * With the hardware effects on, a sandworm is not drawn as a smear of sprites.  Under
 * the sand there is a mound that shakes, with cracks and ripples spreading from it.
 * Sand is thrown up from it and falls back.  As it travels it makes a furrow: a
 * trough with a ridge of sand at each side, which sinks back and fills in over a few
 * seconds, so that the path closes up behind it.
 *
 * Everything is drawn with the same chunky pixels as the rest of the game.
 */

#include <math.h>
#include <stdint.h>

#include "sandworm_fx.h"

#include "gui/widget.h"
#include "map.h"
#include "pool/pool_unit.h"
#include "table/widgetinfo.h"
#include "timer/timer.h"
#include "tools/coord.h"
#include "unit.h"
#include "video/prim.h"
#include "video/video_a5.h"

#define TICKS_PER_SECOND 60

#define MAX_WORMS 128           /* Indexed by the unit's index. */
#define MAX_TRAIL 56
#define TRAIL_SECONDS 8.0f
#define TRAIL_SPACING 70        /* Distance between points of the furrow, 256 to a tile. */
#define MAX_GRAINS 160
#define GRAVITY 300.0f

typedef struct TrailPoint {
	tile32 pos;
	float t;                    /* When it was made, in seconds. */
	float hx, hy;               /* The way the worm was going. */
} TrailPoint;

typedef struct Grain {
	float ox, oy;               /* Pixels from where it was thrown. */
	float vx, vy, vz;
	float z;                    /* Height above the ground. */
	tile32 base;
	uint8_t rgb[3];
	uint8_t size;
	bool alive;
} Grain;

typedef struct Worm {
	bool used;
	tile32 last;
	float lastTime;
	float spawn;                /* Grains owed. */
	int trailCount, trailHead;
	TrailPoint trail[MAX_TRAIL];
	Grain grains[MAX_GRAINS];
	uint8_t palette[6][3];      /* Pixels of the sand it is under. */
	int grabFrame;
	float hx, hy;
} Worm;

static Worm s_worms[MAX_WORMS];
static uint32_t s_rng = 0x9E3779B9u;

static uint32_t
Fx_Next(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}

static float
Fx_Rand(void)
{
	return (Fx_Next() & 0xFFFF) / 65535.0f;
}

static float
Fx_Clamp01(float v)
{
	return (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
}

/*--------------------------------------------------------------*/

typedef struct View {
	int ox, oy, w, h;
} View;

static void
Fx_Rect(const View *v, float x1, float y1, float x2, float y2, unsigned char r, unsigned char g, unsigned char b, float alpha)
{
	int ix1 = (int)floorf(x1), iy1 = (int)floorf(y1), ix2 = (int)floorf(x2), iy2 = (int)floorf(y2);
	const int a = (int)(255.0f * Fx_Clamp01(alpha));

	if (ix1 < 0) ix1 = 0;
	if (iy1 < 0) iy1 = 0;
	if (ix2 > v->w) ix2 = v->w;
	if (iy2 > v->h) iy2 = v->h;
	if (ix1 >= ix2 || iy1 >= iy2 || a <= 0)
		return;

	Prim_FillRect_RGBA(v->ox + ix1, v->oy + iy1, v->ox + ix2, v->oy + iy2, r, g, b, a);
}

/* A disc, squashed to look flat on the ground, a row at a time. */
static void
Fx_Disc(const View *v, float cx, float cy, float rx, float ry, unsigned char r, unsigned char g, unsigned char b, float alpha)
{
	const int rows = (int)(ry * 2.0f + 0.5f);

	for (int row = 0; row < rows; row++) {
		const float dy = (row + 0.5f - ry) / ry;
		const float half = rx * sqrtf(fmaxf(0.0f, 1.0f - dy * dy));

		if (half >= 0.5f)
			Fx_Rect(v, cx - half, floorf(cy - ry) + row, cx + half, floorf(cy - ry) + row + 1, r, g, b, alpha);
	}
}

/*--------------------------------------------------------------*/

/* Takes a few pixels of the sand the worm is under, so the sand it throws is the same colour. */
static void
Fx_GrabPalette(Worm *w, const View *v, float x, float y)
{
	static const unsigned char fallback[3] = { 214, 170, 104 };

	for (int i = 0; i < 6; i++) {
		for (int c = 0; c < 3; c++)
			w->palette[i][c] = fallback[c];
	}

	if (!Prim_GrabBegin(v->ox, v->oy, v->w, v->h))
		return;

	for (int i = 0; i < 6; i++) {
		unsigned char rgb[3];
		const float px = x + (Fx_Rand() - 0.5f) * 36.0f, py = y + (Fx_Rand() - 0.5f) * 26.0f;

		if (px >= 0 && py >= 0 && px < v->w && py < v->h && Prim_GrabPixel(v->ox + px + 0.5f, v->oy + py + 0.5f, rgb)) {
			for (int c = 0; c < 3; c++) {
				const int val = (int)(rgb[c] * 1.15f) + 6;
				w->palette[i][c] = (uint8_t)((val > 255) ? 255 : val);
			}
		}
	}

	Prim_GrabEnd();
}

static void
Fx_AddTrail(Worm *w, tile32 pos, float now)
{
	TrailPoint *p = &w->trail[w->trailHead];

	p->pos = pos;
	p->t = now;
	p->hx = w->hx;
	p->hy = w->hy;

	w->trailHead = (w->trailHead + 1) % MAX_TRAIL;
	if (w->trailCount < MAX_TRAIL)
		w->trailCount++;
}

/* The furrow.  The oldest first, so the newest is on top. */
static void
Fx_DrawTrail(const Worm *w, const View *v, float now)
{
	for (int n = 0; n < w->trailCount; n++) {
		const TrailPoint *p = &w->trail[(w->trailHead - w->trailCount + n + MAX_TRAIL) % MAX_TRAIL];
		const float age = now - p->t;

		if (age < 0.0f || age >= TRAIL_SECONDS)
			continue;

		if (g_mapVisible[Tile_PackTile(p->pos)].fogOverlayBits == 0xF)
			continue;

		int x, y;

		if (!Map_IsPositionInViewport(p->pos, &x, &y))
			continue;

		/* It sinks back: the furrow shrinks and fades as the sand slides in. */
		const float life = 1.0f - age / TRAIL_SECONDS;
		const float fresh = Fx_Clamp01(age * 6.0f);                  /* Opens out as the worm passes. */
		const float size = (0.35f + 0.65f * life) * (0.55f + 0.45f * fresh);
		const float side = 4.0f + 3.0f * size;

		/* Across the way it went. */
		const float px = -p->hy, py = p->hx;

		/* The ridges of sand pushed up to each side, lit from the top left. */
		Fx_Disc(v, x + px * side - 1.0f, y + py * side * 0.7f - 1.0f, 2.5f * size + 0.5f, 2.0f * size + 0.5f,
				236, 200, 140, 0.30f * life);
		Fx_Disc(v, x - px * side + 1.0f, y - py * side * 0.7f + 1.0f, 2.5f * size + 0.5f, 2.0f * size + 0.5f,
				70, 42, 20, 0.22f * life);

		/* The trough. */
		Fx_Disc(v, (float)x, (float)y, 4.5f * size + 0.5f, 3.0f * size + 0.5f, 60, 36, 16, 0.34f * life);
	}
}

/* Sand thrown up from the worm, falling back. */
static void
Fx_Grains(Worm *w, const View *v, float x, float y, float dt, bool moving, tile32 pos)
{
	w->spawn += (moving ? 55.0f : 20.0f) * dt;

	while (w->spawn >= 1.0f) {
		w->spawn -= 1.0f;

		for (int i = 0; i < MAX_GRAINS; i++) {
			Grain *g = &w->grains[i];

			if (g->alive)
				continue;

			const float a = Fx_Rand() * 6.2831853f;
			const float speed = 10.0f + 30.0f * Fx_Rand();
			const uint8_t *rgb = w->palette[Fx_Next() % 6];

			g->base = pos;
			g->ox = (Fx_Rand() - 0.5f) * 10.0f;
			g->oy = (Fx_Rand() - 0.5f) * 6.0f;
			g->vx = cosf(a) * speed - (moving ? w->hx * 30.0f : 0.0f);
			g->vy = sinf(a) * speed * 0.6f - (moving ? w->hy * 18.0f : 0.0f);
			g->vz = 20.0f + 24.0f * Fx_Rand();
			g->z = 2.0f;
			g->rgb[0] = rgb[0]; g->rgb[1] = rgb[1]; g->rgb[2] = rgb[2];
			g->size = (Fx_Rand() < 0.3f) ? 2 : 1;
			g->alive = true;
			break;
		}
	}

	for (int i = 0; i < MAX_GRAINS; i++) {
		Grain *g = &w->grains[i];

		if (!g->alive)
			continue;

		g->ox += g->vx * dt;
		g->oy += g->vy * dt;
		g->z += g->vz * dt;
		g->vz -= GRAVITY * dt;

		if (g->z < 0.0f) {
			g->alive = false;
			continue;
		}

		int gx, gy;

		if (!Map_IsPositionInViewport(g->base, &gx, &gy))
			continue;

		const float px = gx + g->ox, py = gy + g->oy - g->z;

		Fx_Rect(v, px, py, px + g->size, py + g->size, g->rgb[0], g->rgb[1], g->rgb[2], 0.7f);
	}

	(void)x;
	(void)y;
}


void
SandwormFx_Draw(const Unit *u)
{
	if (u->o.index >= MAX_WORMS)
		return;

	const WidgetInfo *wi = &g_table_gameWidgetInfo[GAME_WIDGET_VIEWPORT];
	const View v = { g_widgetProperties[WINDOWID_VIEWPORT].xBase, g_widgetProperties[WINDOWID_VIEWPORT].yBase, wi->width, wi->height };
	Worm *w = &s_worms[u->o.index];
	const float now = (float)Timer_GetTicks() / TICKS_PER_SECOND;

	if (!w->used || now < w->lastTime || now - w->lastTime > 5.0f) {
		/* A new worm, or the game has been away: start afresh. */
		*w = (Worm){ 0 };
		w->used = true;
		w->last = u->o.position;
		w->lastTime = now;
		w->hx = 1.0f;
		w->hy = 0.0f;
		w->grabFrame = 100;
	}

	float dt = now - w->lastTime;

	if (dt > 0.25f)
		dt = 0.016f;
	w->lastTime = now;

	/* Which way it is going, and the furrow it leaves. */
	const int moved = Tile_GetDistance(w->last, u->o.position);
	const bool moving = moved > 1 || (u->speed != 0);

	if (moved >= TRAIL_SPACING) {
		const float dx = (float)u->o.position.x - (float)w->last.x, dy = (float)u->o.position.y - (float)w->last.y;
		const float len = sqrtf(dx * dx + dy * dy);

		if (len > 0.0f) {
			w->hx = dx / len;
			w->hy = dy / len;
		}

		Fx_AddTrail(w, u->o.position, now);
		w->last = u->o.position;
	}

	/* The furrow stays where it was made, and fills in even while the worm is out of sight. */
	Fx_DrawTrail(w, &v, now);

	if (g_mapVisible[Tile_PackTile(u->o.position)].fogOverlayBits == 0xF)
		return;

	int x, y;

	if (!Map_IsPositionInViewport(u->o.position, &x, &y))
		return;

	if (++w->grabFrame >= 20) {
		w->grabFrame = 0;
		Fx_GrabPalette(w, &v, (float)x, (float)y);
	}

	Fx_Grains(w, &v, (float)x, (float)y, dt, moving, u->o.position);
}
