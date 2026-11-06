/** @file src/sandstorm.c Random sandstorms.
 *
 * A storm is only a visual effect, drawn over the viewport with the same
 * chunky pixels as the rest of the game: tinted haze in bands, soft dust
 * clouds, and streaks of sand blown across by the wind.  It builds up, rages
 * for a while, and dies down again.  Storms come at random, a few minutes
 * apart, and are part of the hardware effects.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "sandstorm.h"

#include "audio/audio.h"
#include "gui/widget.h"
#include "map.h"
#include "house.h"
#include "tile.h"
#include "pool/pool.h"
#include "pool/pool_structure.h"
#include "structure.h"
#include "table/widgetinfo.h"
#include "timer/timer.h"
#include "tools/coord.h"
#include "tools/random_xorshift.h"
#include "video/prim.h"
#include "video/video_a5.h"

bool g_debugTestSandstorm = false;
bool g_debugSandstormNow = false;
bool g_sandstorms = true;

#define TICKS_PER_SECOND 60
#define WEAR_SECONDS 3       /* How often a storm wears down the buildings. */

enum StormState {
	STORM_IDLE,
	STORM_BUILD,
	STORM_PEAK,
	STORM_FADE
};

#define BUILD_SECONDS 10
#define FADE_SECONDS 15
#define TURN_SECONDS 10     /* The wind changes direction this often, */
#define TURNING_SECONDS 2   /* turning round over this long. */
#define MIN_WAIT_SECONDS 150
#define MAX_WAIT_SECONDS 330

static enum StormState s_state = STORM_IDLE;
static int64_t s_scenarioStart = -1;
static int64_t s_nextStorm = 0;     /* Game time. */
static int64_t s_phaseStart = 0;
static int64_t s_peakTicks = 0;
static float s_strength = 1.0f;     /* How fierce this storm is at its peak. */
static float s_windX = 1.0f;        /* The wind, a unit vector: picked when the storm starts, and it turns as it blows. */
static float s_windY = 0.0f;
static float s_turnFrom = 0.0f;     /* The wind turns from this angle to s_turnTo, from s_turnStart on. */
static float s_turnTo = 0.0f;
static int64_t s_turnStart = 0;
static uint32_t s_seed = 1;

static uint32_t
Storm_Hash(uint32_t a, uint32_t b)
{
	uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu;

	h ^= h >> 15; h *= 0x2C1B3C6Du;
	h ^= h >> 12; h *= 0x297A2D39u;
	h ^= h >> 15;
	return h;
}

/* A stable random number in [0, 1] for item i, property k. */
static float
Storm_Rand(uint32_t i, uint32_t k)
{
	return (Storm_Hash(s_seed + k * 0x1000193u, i) & 0xFFFF) / 65535.0f;
}

static float
Storm_Clamp01(float v)
{
	return (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
}

static void
Storm_Schedule(int64_t now)
{
	const int seconds = MIN_WAIT_SECONDS + (Random_Xorshift_256() * (MAX_WAIT_SECONDS - MIN_WAIT_SECONDS)) / 255;

	s_nextStorm = now + (int64_t)seconds * TICKS_PER_SECOND;
}

static void
Storm_Start(int64_t now)
{
	s_state = STORM_BUILD;
	s_phaseStart = now;
	s_peakTicks = (int64_t)(50 + Random_Xorshift_256() % 41) * TICKS_PER_SECOND;
	s_strength = 0.6f + 0.4f * (Random_Xorshift_256() / 255.0f);
	{
		/* Any direction at all. */
		const float angle = 6.2831853f * (Random_Xorshift_256() * 256u + Random_Xorshift_256()) / 65535.0f;

		s_turnFrom = s_turnTo = angle;
		s_turnStart = now;
		s_windX = cosf(angle);
		s_windY = sinf(angle);
	}
	s_seed = Random_Xorshift_256() * 65599u + Random_Xorshift_256();
}

/* Moves the storm along, and returns its strength from 0 to 1. */
static float
Storm_UpdateRaw(int64_t now)
{
	if (s_scenarioStart != g_tickScenarioStart) {
		s_scenarioStart = g_tickScenarioStart;
		s_state = STORM_IDLE;
		Storm_Schedule(now);
		s_nextStorm += 60 * TICKS_PER_SECOND; /* Not in the first minutes. */
	}

	/* While a storm blows, the wind changes direction every so often, turning
	 * round smoothly rather than all at once.
	 */
	if (s_state != STORM_IDLE) {
		if (now - s_turnStart >= TURN_SECONDS * TICKS_PER_SECOND) {
			const float swing = 1.75f * ((Random_Xorshift_256() / 255.0f) * 2.0f - 1.0f);  /* Up to 100 degrees. */

			s_turnFrom = s_turnTo;
			s_turnTo = s_turnFrom + swing;
			s_turnStart = now;
		}

		const float t = Storm_Clamp01((float)(now - s_turnStart) / (TURNING_SECONDS * TICKS_PER_SECOND));
		const float angle = s_turnFrom + (s_turnTo - s_turnFrom) * t * t * (3.0f - 2.0f * t);

		s_windX = cosf(angle);
		s_windY = sinf(angle);
	}

	const int64_t age = now - s_phaseStart;

	switch (s_state) {
		case STORM_IDLE:
			if (now >= s_nextStorm)
				Storm_Start(now);
			return 0.0f;

		case STORM_BUILD: {
			const float t = Storm_Clamp01((float)age / (BUILD_SECONDS * TICKS_PER_SECOND));

			if (age >= BUILD_SECONDS * TICKS_PER_SECOND) {
				s_state = STORM_PEAK;
				s_phaseStart = now;
			}
			return s_strength * t * t * (3.0f - 2.0f * t);
		}

		case STORM_PEAK:
			if (age >= s_peakTicks) {
				s_state = STORM_FADE;
				s_phaseStart = now;
			}
			/* Gusts: the strength swells and slackens a little. */
			return s_strength * (0.88f + 0.12f * sinf(age * 0.011f) * sinf(age * 0.0037f + 1.0f));

		case STORM_FADE: {
			const float t = Storm_Clamp01((float)age / (FADE_SECONDS * TICKS_PER_SECOND));

			if (age >= FADE_SECONDS * TICKS_PER_SECOND) {
				s_state = STORM_IDLE;
				Storm_Schedule(now);
			}
			return s_strength * (1.0f - t * t * (3.0f - 2.0f * t));
		}
	}

	return 0.0f;
}

static float s_level = 0.0f;        /* The strength at the last update, 0 to 1. */

static float
Storm_Update(int64_t now)
{
	s_level = Storm_UpdateRaw(now);
	return s_level;
}

float
Sandstorm_GetStrength(void)
{
	return s_level;
}

/*--------------------------------------------------------------*/

static void
Storm_Rect(int ox, int oy, int w, int h, float x, float y, int rw, int rh, const unsigned char *c, float alpha)
{
	int x1 = (int)floorf(x), y1 = (int)floorf(y);
	int x2 = x1 + rw, y2 = y1 + rh;
	const int a = (int)(255.0f * Storm_Clamp01(alpha));

	if (x1 < 0) x1 = 0;
	if (y1 < 0) y1 = 0;
	if (x2 > w) x2 = w;
	if (y2 > h) y2 = h;
	if (x1 >= x2 || y1 >= y2 || a <= 0)
		return;

	Prim_FillRect_RGBA(ox + x1, oy + y1, ox + x2, oy + y2, c[0], c[1], c[2], a);
}

/* A streak of sand with its head at x,y, trailing len pixels back along the wind. */
static void
Storm_Streak(int ox, int oy, int w, int h, float x, float y, int len, int thick, const unsigned char *c, float alpha)
{
	const float ax = fabsf(s_windX), ay = fabsf(s_windY);

	if (ay < 0.38f) {
		Storm_Rect(ox, oy, w, h, (s_windX > 0.0f) ? x - len : x, y, len, thick, c, alpha);
	} else if (ax < 0.38f) {
		Storm_Rect(ox, oy, w, h, x, (s_windY > 0.0f) ? y - len : y, thick, len, c, alpha);
	} else {
		for (int i = 0; i < len; i++)
			Storm_Rect(ox, oy, w, h, x - s_windX * i, y - s_windY * i, thick, thick, c, alpha);
	}
}


/*--------------------------------------------------------------*/

/* The sand.  Every so often a few random spots of the viewport are looked at,
 * and where the ground there is sand, the pixel that is actually on the screen
 * is picked up and blown away in the direction of the wind, keeping its colour.
 * So the dust is made of the sand that is really there.  Where the sand meets
 * concrete or a building, far more is picked up, as the wind catches on the edge.
 * The grains live on the map, so they stay put when the view scrolls.
 */

#define MAX_GRAINS 1000
#define GRAB_EVERY 6        /* Frames between looking at the screen (each look stalls the graphics card). */
#define MAX_PICKS 96
#define MIN_PICK_SPACING 12 /* Pixels apart that grains picked up at one look must be, so they spread out. */

typedef struct Grain {
	float x, y;             /* Map pixels. */
	float vx, vy;           /* Pixels per second. */
	float age, life;        /* Seconds. */
	uint8_t rgb[3];         /* The colour of the pixel it was. */
	uint8_t size;           /* Length along the wind. */
} Grain;

static Grain s_grains[MAX_GRAINS];
static float s_grainTime = 0.0f;
static uint32_t s_grainRng = 0x2545F491u;
static int s_grabFrame = 0;

static uint32_t
Storm_NextRandom(void)
{
	s_grainRng ^= s_grainRng << 13;
	s_grainRng ^= s_grainRng >> 17;
	s_grainRng ^= s_grainRng << 5;
	return s_grainRng;
}

static float
Storm_Random01(void)
{
	return (Storm_NextRandom() & 0xFFFF) / 65535.0f;
}

/* How much loose sand a kind of ground has. */
static float
Storm_SandAmount(enum LandscapeType type)
{
	switch (type) {
		case LST_ENTIRELY_DUNE: return 1.0f;
		case LST_PARTIAL_DUNE:  return 0.9f;
		case LST_NORMAL_SAND:   return 0.8f;
		case LST_SPICE:
		case LST_THICK_SPICE:   return 0.6f;
		case LST_BLOOM_FIELD:   return 0.5f;
		case LST_PARTIAL_ROCK:  return 0.3f;
		default:                return 0.0f;
	}
}

static bool
Storm_IsHardGround(enum LandscapeType type)
{
	return type == LST_CONCRETE_SLAB || type == LST_STRUCTURE || type == LST_WALL || type == LST_DESTROYED_WALL;
}

static void
Storm_AddGrain(float x, float y, const unsigned char *rgb, bool nearHard)
{
	/* Look for a free grain from where the last one was found, not from the start every time. */
	static int s_next = 0;

	for (int n = 0; n < MAX_GRAINS; n++) {
		const int i = (s_next + n) % MAX_GRAINS;
		Grain *g = &s_grains[i];

		if (g->life > 0.0f && g->age < g->life)
			continue;

		s_next = (i + 1) % MAX_GRAINS;

		/* A constant speed, whatever the strength of the storm, so that nothing slows down or turns back. */
		const float speed = 70.0f + 110.0f * Storm_Random01();
		/* A little sideways spread, fixed for the grain. */
		const float side = (Storm_Random01() - 0.5f) * 8.0f;

		g->x = x;
		g->y = y;
		g->vx = s_windX * speed - s_windY * side;
		g->vy = s_windY * speed + s_windX * side;
		g->age = 0.0f;
		g->life = 0.6f + 1.1f * Storm_Random01() + (nearHard ? 0.4f : 0.0f);
		g->size = (uint8_t)(1 + (int)(speed / 110.0f));

		/* Loose grains catch the light a little. */
		for (int c = 0; c < 3; c++) {
			const int v = (int)(rgb[c] * 1.18f) + 8;
			g->rgb[c] = (uint8_t)((v > 255) ? 255 : v);
		}
		return;
	}
}

typedef struct Pick {
	int vx, vy;             /* In the viewport. */
	int wx, wy;             /* On the map. */
	bool nearHard;
} Pick;

static void
Storm_Grains(float k, float seconds, int w, int h, int ox, int oy)
{
	float dt = seconds - s_grainTime;
	s_grainTime = seconds;
	if (dt < 0.0f || dt > 0.25f)
		dt = 0.016f;

	/* Where the map's pixel 0,0 is in the viewport. */
	int ox0 = 0, oy0 = 0;
	const tile32 origin = { 0, 0 };
	Map_IsPositionInViewport(origin, &ox0, &oy0);

	/* Every few frames, choose random spots of sand and pick up what is on the screen there. */
	if ((s_grabFrame++ % GRAB_EVERY) == 0) {
		Pick picks[MAX_PICKS];
		int numPicks = 0;

		/* The sand that can be seen: the tiles in the viewport that are sand and
		 * not hidden, and how likely each is to lose sand.  Next to concrete or a
		 * building it is far more likely.
		 */
		typedef struct SandTile {
			int tx, ty;
			float weight;
			bool nearHard;
		} SandTile;

		static SandTile s_sandTiles[1600];
		int numTiles = 0;

		const int tx0 = (-ox0) / 16 - 1, tx1 = (w - ox0) / 16 + 1;
		const int ty0 = (-oy0) / 16 - 1, ty1 = (h - oy0) / 16 + 1;

		for (int ty = (ty0 < 0 ? 0 : ty0); ty <= ty1 && ty < MAP_SIZE_MAX; ty++) {
			for (int tx = (tx0 < 0 ? 0 : tx0); tx <= tx1 && tx < MAP_SIZE_MAX && numTiles < 1600; tx++) {
				const uint16 packed = Tile_PackXY(tx, ty);

				if (!Map_IsValidPosition(packed) || !Map_IsUnveiledToHouse(g_playerHouseID, packed)
						|| g_mapVisible[packed].fogOverlayBits == 0xF)
					continue;

				/* Not the pixels of units, buildings or explosions. */
				if (g_map[packed].hasUnit || g_map[packed].hasStructure || g_map[packed].hasExplosion)
					continue;

				const float sand = Storm_SandAmount(Map_GetLandscapeTypeVisible(packed));
				if (sand <= 0.0f)
					continue;

				bool nearHard = false;
				for (int dy = -1; dy <= 1 && !nearHard; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int nx = tx + dx, ny = ty + dy;

						if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= MAP_SIZE_MAX || ny >= MAP_SIZE_MAX)
							continue;

						if (Storm_IsHardGround(Map_GetLandscapeTypeVisible(Tile_PackXY(nx, ny)))) {
							nearHard = true;
							break;
						}
					}
				}

				SandTile *t = &s_sandTiles[numTiles++];
				t->tx = tx; t->ty = ty; t->nearHard = nearHard;
				t->weight = sand * (nearHard ? 0.6f : 0.35f);
			}
		}

		/* Choose spots among them, which are all on sand that is on the screen. */
		const float wanted = 300.0f * k * dt * GRAB_EVERY;
		int attempts = (int)wanted + ((Storm_Random01() < (wanted - (int)wanted)) ? 1 : 0);

		for (int a = 0; a < attempts * 2 && numPicks < MAX_PICKS - 3 && numTiles > 0; a++) {
			const SandTile *t = &s_sandTiles[Storm_NextRandom() % (uint32_t)numTiles];

			if (Storm_Random01() > t->weight)
				continue;

			const int wx = t->tx * 16 + (int)(Storm_Random01() * 15.99f);
			const int wy = t->ty * 16 + (int)(Storm_Random01() * 15.99f);

			/* Not right next to another one, so the dust is spread out rather than in clumps. */
			bool tooClose = false;
			for (int j = 0; j < numPicks && !tooClose; j++)
				tooClose = abs(picks[j].wx - wx) < MIN_PICK_SPACING && abs(picks[j].wy - wy) < MIN_PICK_SPACING;

			if (tooClose)
				continue;

			Pick *p = &picks[numPicks++];
			p->vx = wx + ox0; p->vy = wy + oy0; p->wx = wx; p->wy = wy; p->nearHard = t->nearHard;

			if (t->nearHard) {
				/* A little more of it along the edge. */
				Pick *q = &picks[numPicks++];

				q->vx = p->vx - (int)(s_windX * 6.0f);
				q->vy = p->vy - (int)(s_windY * 6.0f) + 1;
				q->wx = q->vx - ox0;
				q->wy = q->vy - oy0;
				q->nearHard = true;
			}
		}

		/* Look at the screen once, take the pixels, and let go of it before drawing. */
		if (numPicks > 0 && Prim_GrabBegin(ox, oy, w, h)) {
			unsigned char taken[MAX_PICKS][3];
			bool ok[MAX_PICKS];

			for (int i = 0; i < numPicks; i++)
				ok[i] = Prim_GrabPixel((float)(ox + picks[i].vx) + 0.5f, (float)(oy + picks[i].vy) + 0.5f, taken[i]);

			Prim_GrabEnd();


			for (int i = 0; i < numPicks; i++) {
				if (ok[i])
					Storm_AddGrain((float)picks[i].wx, (float)picks[i].wy, taken[i], picks[i].nearHard);
			}
		}
	}

	/* Move and draw them. */
	for (int i = 0; i < MAX_GRAINS; i++) {
		Grain *g = &s_grains[i];

		if (g->life <= 0.0f || g->age >= g->life)
			continue;

		g->age += dt;
		g->x += g->vx * dt;
		g->y += g->vy * dt;

		const float t = g->age / g->life;
		const float alpha = (1.0f - t * t * t) * Storm_Clamp01(g->age * 10.0f);

		Storm_Streak(ox, oy, w, h, g->x + ox0, g->y + oy0, g->size, 1, g->rgb, alpha);
	}
}

/* The colour of the storm: haze in bands over the viewport, and a darkening at the edges. */
static void
Storm_Render(float k, float seconds)
{
	static const unsigned char haze[] = { 196, 150, 86 };
	static const unsigned char dark[] = { 120, 86, 46 };

	const WidgetInfo *wi = &g_table_gameWidgetInfo[GAME_WIDGET_VIEWPORT];
	const int w = wi->width, h = wi->height;
	const int ox = g_widgetProperties[WINDOWID_VIEWPORT].xBase;
	const int oy = g_widgetProperties[WINDOWID_VIEWPORT].yBase;

	/* Haze, in bands of four pixels, each drifting in and out of strength so
	 * that the dust looks streaky rather than a flat wash.
	 */
	for (int band = 0; band * 4 < h; band++) {
		const float wave = 0.5f + 0.5f * sinf(seconds * (0.6f + Storm_Rand(band, 1)) + band * 0.9f);
		const float a = k * (0.30f + 0.24f * wave * Storm_Rand(band, 2));

		Storm_Rect(ox, oy, w, h, 0, band * 4, w, 4, haze, a);
	}

	/* A little darkening towards the edges, like a thick cloud closing in. */
	for (int i = 0; i < 6; i++) {
		const float a = k * 0.07f * (6 - i) / 6.0f;

		Storm_Rect(ox, oy, w, h, 0, i * 3, w, 3, dark, a);
		Storm_Rect(ox, oy, w, h, 0, h - (i + 1) * 3, w, 3, dark, a);
	}

	/* The sand picked up from the ground and carried off by the wind. */
	Storm_Grains(k, seconds, w, h, ox, oy);
}


void
Sandstorm_Draw(void)
{
	static int64_t s_testStart = 0;
	static bool s_testShot = false;
	const int64_t wall = Timer_GetTicks();

	if (g_debugTestSandstorm || g_debugSandstormNow) {
		/* A storm at full strength from the start.  The test takes a
		 * screenshot after a few seconds, and then quits; --sandstorm-now
		 * just stays on.
		 */
		if (s_testStart == 0) {
			s_testStart = wall;
			Storm_Start(g_timerGame);
			s_state = STORM_PEAK;
			s_phaseStart = g_timerGame;
			s_peakTicks = 1000 * TICKS_PER_SECOND;
			s_strength = 1.0f;
			s_scenarioStart = g_tickScenarioStart;
		}

		if (g_debugTestSandstorm && !s_testShot && wall - s_testStart > 3 * TICKS_PER_SECOND) {
			s_testShot = true;
			VideoA5_CaptureScreenshot();
		}

		if (g_debugTestSandstorm && s_testShot && wall - s_testStart > 5 * TICKS_PER_SECOND)
			exit(0);
	} else if (!g_sandstorms) {
		s_level = 0.0f;
		Audio_SetSandstormWind(0.0f);
		return;
	}

	const float k = Storm_Update(g_timerGame);

	/* Low key: at most about a third of the sound volume. */
	Audio_SetSandstormWind(k * 0.32f);

	if (k <= 0.0f)
		return;

	Storm_Render(k, wall / (float)TICKS_PER_SECOND);
}

/**
 * Game logic of a storm: while it blows strongly, the buildings are slowly worn
 * down by the sand, but never below half of their hitpoints.  Called from the
 * server logic, as the hitpoints are the server's to change.
 */
void
Sandstorm_Tick(void)
{
	static int64_t s_nextWear = 0;
	static int64_t s_wearScenario = -1;

	if (!g_debugTestSandstorm && !g_debugSandstormNow && !g_sandstorms) {
		s_level = 0.0f;
		return;
	}

	if (s_wearScenario != g_tickScenarioStart) {
		s_wearScenario = g_tickScenarioStart;
		s_nextWear = 0;
	}

	const float k = Storm_Update(g_timerGame);
	if (k < 0.25f || g_timerGame < s_nextWear)
		return;

	s_nextWear = g_timerGame + WEAR_SECONDS * TICKS_PER_SECOND;

	PoolFindStruct find;

	for (Structure *s = Structure_FindFirst(&find, HOUSE_INVALID, STRUCTURE_INVALID);
			s != NULL;
			s = Structure_FindNext(&find)) {
		if (s->o.type == STRUCTURE_SLAB_1x1 || s->o.type == STRUCTURE_SLAB_2x2 || s->o.type == STRUCTURE_WALL)
			continue;

		const StructureInfo *si = &g_table_structureInfo[s->o.type];
		const int floorHP = si->o.hitpoints / 2;

		if (s->o.hitpoints <= floorHP)
			continue;

		/* About 0.5% of the hitpoints each time at full strength, at least 1. */
		int wear = (int)(si->o.hitpoints * k / 200.0f);
		if (wear < 1) wear = 1;
		if (s->o.hitpoints - wear < floorHP) wear = s->o.hitpoints - floorHP;

		if (g_debugTestSandstorm) {
			FILE *log = fopen("sandstorm_test.log", "a");

			if (log != NULL) {
				fprintf(log, "t=%d structure %d: %d -> %d hp (max %d, floor %d)\n", (int)(g_timerGame / 60), (int)s->o.type,
						(int)s->o.hitpoints, (int)(s->o.hitpoints - wear), (int)si->o.hitpoints, floorHP);
				fclose(log);
			}
		}

		s->o.hitpoints -= wear;
	}
}

void
Sandstorm_GetWind(float *x, float *y)
{
	*x = s_windX;
	*y = s_windY;
}
