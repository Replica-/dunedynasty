/** @file src/remains.c What is left of the dead.
 *
 * A destroyed vehicle leaves its burnt-out hull, and fallen infantry their bodies,
 * where the game would soon have cleared them away.  They stay for a couple of
 * minutes, then wear away into the sand a pixel at a time, in a random order,
 * until nothing is left.
 *
 * Only for show: the game knows nothing of them.
 */

#include <allegro5/allegro.h>
#include <math.h>
#include <stdlib.h>
#include "os/math.h"
#include <string.h>

#include "remains.h"

#include "animation.h"
#include "gui/widget.h"
#include "explosion.h"
#include "map.h"
#include "newui/viewport.h"
#include "sprites.h"
#include "timer/timer.h"
#include "tools/coord.h"
#include "pool/pool.h"
#include "pool/pool_unit.h"
#include "unit.h"
#include "video/video_a5.h"

#define MAX_REMAINS 64
#define STAY_TICKS (g_debugTestRemains ? 4 * 60 : 120 * 60)       /* Whole, for two minutes of game time... */
#define DISSOLVE_TICKS (g_debugTestRemains ? 8 * 60 : 40 * 60)   /* ... then gone, a pixel at a time, over this. */
#define MAX_PIXELS (32 * 32)
#define BURN_TICKS (g_debugTestRemains ? 3 * 60 : 40 * 60)   /* A wreck burns this long when it is hit... */
#define FLAMES_CATCH_TICKS 90       /* The flames take this long to catch, */
#define FLAMES_DIE_TICKS (4 * 60)   /* and this long to die down once it has gone; */
#define SMOKE_TICKS (g_debugTestRemains ? 5 * 60 : 12 * 60)  /* the smoke this long. */

typedef struct Remains {
	bool used;
	ALLEGRO_BITMAP *bmp;
	tile32 position;
	int flip;                       /* ALLEGRO_FLIP_* */
	ALLEGRO_COLOR tint;
	int64_t born;
	int pixels;                     /* Opaque pixels to take away, in the order of order[]. */
	int taken;
	uint16 *order;                  /* Pixel indices, shuffled. */
	bool burns;                     /* A wreck: it burns as it crumbles. */
	uint32 seed;
} Remains;

static Remains s_remains[MAX_REMAINS];
static int64_t s_scenarioStart = -1;
static uint32 s_rng = 0x2545F491u;

static uint32
Remains_Rand(void)
{
	s_rng ^= s_rng << 13;
	s_rng ^= s_rng >> 17;
	s_rng ^= s_rng << 5;
	return s_rng;
}

static void
Remains_Free(Remains *r)
{
	if (r->bmp != NULL)
		al_destroy_bitmap(r->bmp);
	free(r->order);
	memset(r, 0, sizeof(*r));
}

static void
Remains_CheckScenario(void)
{
	if (s_scenarioStart == g_tickScenarioStart)
		return;

	s_scenarioStart = g_tickScenarioStart;
	for (int i = 0; i < MAX_REMAINS; i++)
		Remains_Free(&s_remains[i]);
}

/* The opaque pixels of the copy, in a random order: the order they wear away in. */
static bool
Remains_Shuffle(Remains *r)
{
	const int w = al_get_bitmap_width(r->bmp), h = al_get_bitmap_height(r->bmp);

	if (w * h > MAX_PIXELS)
		return false;

	r->order = malloc(sizeof(r->order[0]) * w * h);
	if (r->order == NULL)
		return false;

	ALLEGRO_LOCKED_REGION *lock = al_lock_bitmap(r->bmp, ALLEGRO_PIXEL_FORMAT_ANY, ALLEGRO_LOCK_READONLY);

	if (lock == NULL)
		return false;

	r->pixels = 0;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			unsigned char cr, cg, cb, ca;

			al_unmap_rgba(al_get_pixel(r->bmp, x, y), &cr, &cg, &cb, &ca);
			if (ca > 0)
				r->order[r->pixels++] = (uint16)(y * w + x);
		}
	}
	al_unlock_bitmap(r->bmp);

	for (int i = r->pixels - 1; i > 0; i--) {
		const int j = (int)(Remains_Rand() % (uint32)(i + 1));
		const uint16 t = r->order[i];

		r->order[i] = r->order[j];
		r->order[j] = t;
	}

	return r->pixels > 0;
}

static void
Remains_Add(ALLEGRO_BITMAP *bmp, tile32 position, int flip, ALLEGRO_COLOR tint, bool burns)
{
	if (bmp == NULL)
		return;

	Remains_CheckScenario();

	/* A free slot, or else the oldest goes. */
	Remains *r = NULL;

	for (int i = 0; i < MAX_REMAINS; i++) {
		if (!s_remains[i].used) {
			r = &s_remains[i];
			break;
		}
		if (r == NULL || s_remains[i].born < r->born)
			r = &s_remains[i];
	}

	Remains_Free(r);

	r->used = true;
	r->bmp = bmp;
	r->position = position;
	r->flip = flip;
	r->tint = tint;
	r->born = g_timerGame;
	r->burns = burns;
	r->seed = Remains_Rand();

	if (!Remains_Shuffle(r))
		Remains_Free(r);
}

void
Remains_AddVehicle(const Unit *u)
{
	uint16 shapeID;
	int flip;

	if (u == NULL)
		return;

	Viewport_UnitBodySprite(u, &shapeID, &flip);

	/* Burnt black, the house colours scorched off. */
	Remains_Add(VideoA5_CopyShape(shapeID, Unit_GetHouseID(u)), u->o.position,
			((flip & 1) ? ALLEGRO_FLIP_HORIZONTAL : 0) | ((flip & 2) ? ALLEGRO_FLIP_VERTICAL : 0),
			al_map_rgb_f(0.34f, 0.30f, 0.27f), true);
}

void
Remains_AddIcon(uint16 iconID, enum HouseType houseID, tile32 position)
{
	Remains_Add(VideoA5_CopyIcon(iconID, houseID), Tile_Center(position), 0, al_map_rgb_f(1.0f, 1.0f, 1.0f), false);
}

/* Takes away the pixels that should be gone by now. */
static void
Remains_Wear(Remains *r, int target)
{
	if (target <= r->taken)
		return;

	const int w = al_get_bitmap_width(r->bmp);

	if (al_lock_bitmap(r->bmp, ALLEGRO_PIXEL_FORMAT_ANY, ALLEGRO_LOCK_READWRITE) == NULL)
		return;

	ALLEGRO_STATE state;

	al_store_state(&state, ALLEGRO_STATE_TARGET_BITMAP);
	al_set_target_bitmap(r->bmp);

	for (; r->taken < target && r->taken < r->pixels; r->taken++) {
		const uint16 p = r->order[r->taken];

		al_put_pixel(p % w, p / w, al_map_rgba(0, 0, 0, 0));
	}

	al_restore_state(&state);
	al_unlock_bitmap(r->bmp);
}

void
Remains_Draw(void)
{
	Remains_CheckScenario();

	if (g_debugTestRemains)
		Remains_DebugShots();

	for (int i = 0; i < MAX_REMAINS; i++) {
		Remains *r = &s_remains[i];

		if (!r->used)
			continue;

		const int64_t age = g_timerGame - r->born;
		const int64_t gone = STAY_TICKS + DISSOLVE_TICKS;

		if (age > gone + (r->burns ? SMOKE_TICKS : 0)) {
			Remains_Free(r);
			continue;
		}

		if (age > STAY_TICKS)
			Remains_Wear(r, (int)(min(age - STAY_TICKS, (int64_t)DISSOLVE_TICKS) * r->pixels / DISSOLVE_TICKS));

		if (!Map_IsUnveiledToHouse(g_playerHouseID, Tile_PackTile(r->position)))
			continue;

		int x, y;

		if (!Map_IsPositionInViewport(r->position, &x, &y))
			continue;

		x += g_widgetProperties[WINDOWID_VIEWPORT].xBase;
		y += g_widgetProperties[WINDOWID_VIEWPORT].yBase;

		const int w = al_get_bitmap_width(r->bmp), h = al_get_bitmap_height(r->bmp);

		if (r->taken < r->pixels)
			al_draw_tinted_bitmap(r->bmp, r->tint, x - w / 2, y - h / 2, r->flip);

		/* A wreck burns as it crumbles away: the flames catch, cover it while it
		 * goes, and die down when it has gone; the smoke lingers a while after.
		 */
		if (r->burns) {
			/* It burns as soon as it is hit, and goes out after a while... */
			const float t0 = (float)age;
			float flames = fminf(fminf(t0 / 20.0f, 1.0f), fmaxf(1.0f - (t0 - BURN_TICKS) / FLAMES_DIE_TICKS, 0.0f));
			float smoke = fminf(fminf(t0 / 20.0f, 1.0f), 1.0f - (t0 - BURN_TICKS) / SMOKE_TICKS);

			/* ... and burns again as it crumbles away at the end. */
			if (age > STAY_TICKS) {
				const float t = (float)(age - STAY_TICKS);
				const float catchUp = t / FLAMES_CATCH_TICKS;

				flames = fmaxf(flames, fminf(fminf(catchUp, 1.0f), fmaxf(1.0f - (t - DISSOLVE_TICKS) / FLAMES_DIE_TICKS, 0.0f)));
				smoke = fmaxf(smoke, fminf(catchUp, 1.0f - (t - DISSOLVE_TICKS) / SMOKE_TICKS));
			}

			if (flames > 0.0f || smoke > 0.0f)
				Explosion_DrawFire(x, y, fminf(smoke, 1.0f), flames, r->seed);
		}
	}
}

void
Remains_AddAnimation(const AnimationCommandStruct *script, uint16 iconGroup, enum HouseType houseID, tile32 position)
{
	int last = -1;

	/* The body the animation leaves last is the one to keep. */
	for (int i = 0; i < 8 && script[i].command != ANIMATION_STOP; i++) {
		if (script[i].command == ANIMATION_SET_OVERLAY_SPRITE)
			last = script[i].parameter;
	}

	if (last >= 0)
		Remains_AddIcon(g_iconMap[g_iconMap[iconGroup] + last], houseID, position);
}

/*--------------------------------------------------------------*/

bool g_debugTestRemains = false;

/* --test-remains: some of the player's units near the base are destroyed; the
 * remains last a few seconds only.  Screenshots of them whole and half worn away.
 */
void
Remains_DebugTest(void)
{
	static int stage = 0;
	static int64_t start;

	if (stage == 0) {
		PoolFindStruct find;
		int killed = 0;

		start = g_timerGame;

		for (Unit *u = Unit_FindFirst(&find, g_playerHouseID, UNIT_INVALID); u != NULL && killed < 6; u = Unit_FindNext(&find)) {
			const UnitInfo *ui = &g_table_unitInfo[u->o.type];

			if (u->o.flags.s.isNotOnMap || !ui->flags.isNormalUnit || ui->movementType == MOVEMENT_WINGER)
				continue;

			if (killed == 0)
				Map_CentreViewport(u->o.position.x >> 4, u->o.position.y >> 4);

			Unit_Damage(u, 2000, 0);
			killed++;
		}

		stage = 1;
		return;
	}

	if (g_timerGame - start > 60 * 18)
		exit(0);
}

void
Remains_DebugShots(void)
{
	static int shots = 0;
	static int64_t start = -1;

	if (start < 0)
		start = g_timerGame;

	/* Whole (once the fires are out), then half worn away. */
	if ((shots == 0 && g_timerGame - start > 60 * 5) || (shots == 1 && g_timerGame - start > 60 * 10)) {
		shots++;
		VideoA5_CaptureScreenshot();
	}
}
