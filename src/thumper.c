/** @file src/thumper.c Thumpers: infantry plant one in the sand to call the worms.
 *
 * A thumper beats on the sand.  Any sandworm near enough leaves what it was
 * after and goes for it instead, and swallows it.  It is not a meal: the worm is
 * no nearer to being full.  One costs 50 credits, and there can be five on the
 * map at once, whoever planted them.  It beats for three minutes,
 * if no worm comes first.
 */

#include <allegro5/allegro.h>
#include <allegro5/allegro_primitives.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "thumper.h"

#include "audio/audio.h"
#include "gui/gui.h"
#include "gui/widget.h"
#include "house.h"
#include "map.h"
#include "net/server.h"
#include "opendune.h"
#include "pool/pool.h"
#include "pool/pool_house.h"
#include "pool/pool_unit.h"
#include "table/widgetinfo.h"
#include "scenario.h"
#include "tile.h"
#include "timer/timer.h"
#include "tools/coord.h"
#include "tools/encoded_index.h"
#include "unit.h"
#include "video/prim.h"
#include "video/video_a5.h"

#define MAX_THUMPERS 5               /* On the map at once, all houses together. */
#define THUMPER_COST 50              /* Credits. */
#define THUMPER_TICKS (180 * 60)     /* Three minutes. */
#define THUMPER_RANGE 24            /* Tiles: the worms that hear it. */
#define BEAT_TICKS 64               /* One blow a second or so... */
#define SLAM_AT 54                  /* ... landing this far into the beat. */
#define WAVES 3                     /* A wave lives this many beats. */

typedef struct Thumper {
	bool used;
	uint16 packed;
	uint8 houseID;
	int64_t born;
} Thumper;

static Thumper s_thumpers[MAX_THUMPERS];
static int64_t s_scenarioStart = -1;
static int64_t s_lastBlow[MAX_THUMPERS] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };

bool g_debugTestThumper = false;

static void
Thumper_Log(const char *fmt, ...)
{
	FILE *log = fopen("thumper_test.log", "a");
	va_list ap;

	if (log == NULL)
		return;

	fprintf(log, "t=%5.1f ", (g_timerGame - g_tickScenarioStart) / 60.0);
	va_start(ap, fmt);
	vfprintf(log, fmt, ap);
	va_end(ap);
	fputc('\n', log);
	fclose(log);
}

static void
Thumper_Check(void)
{
	if (s_scenarioStart != g_tickScenarioStart) {
		s_scenarioStart = g_tickScenarioStart;
		for (int i = 0; i < MAX_THUMPERS; i++)
			s_thumpers[i].used = false;
	}

	for (int i = 0; i < MAX_THUMPERS; i++) {
		if (s_thumpers[i].used && g_timerGame - s_thumpers[i].born > THUMPER_TICKS)
			s_thumpers[i].used = false;
	}
}

static void
Thumper_Message(uint8 houseID, const char *text)
{
	if (houseID == g_playerHouseID)
		GUI_DisplayText(text, 2);
}

bool
Thumper_IsDropper(const Unit *u)
{
	if (u == NULL)
		return false;

	/* Only the Atreides and the Fremen, who know the desert, use thumpers. */
	const uint8 houseID = Unit_GetHouseID(u);

	if (houseID != HOUSE_ATREIDES && houseID != HOUSE_FREMEN)
		return false;

	return u->o.type == UNIT_INFANTRY || u->o.type == UNIT_SOLDIER
		|| u->o.type == UNIT_TROOPERS || u->o.type == UNIT_TROOPER;
}

bool
Thumper_Drop(Unit *u)
{
	if (!Thumper_IsDropper(u) || u->o.flags.s.isNotOnMap)
		return false;

	Thumper_Check();

	const uint8 houseID = Unit_GetHouseID(u);
	House *h = House_Get_ByIndex(houseID);
	const uint16 packed = Tile_PackTile(u->o.position);
	Thumper *slot = NULL;

	if (!g_table_landscapeInfo[Map_GetLandscapeType(packed)].isSand) {
		Thumper_Message(houseID, "A thumper has to go on the sand.");
		return false;
	}

	for (int i = 0; i < MAX_THUMPERS; i++) {
		if (!s_thumpers[i].used) {
			if (slot == NULL)
				slot = &s_thumpers[i];
			continue;
		}

		if (s_thumpers[i].packed == packed) {
			Thumper_Message(houseID, "There is a thumper here already.");
			return false;
		}
	}

	if (slot == NULL) {
		Thumper_Message(houseID, "There are five thumpers on the map already.");
		return false;
	}

	if (h == NULL || h->credits < THUMPER_COST) {
		Thumper_Message(houseID, "Not enough credits for a thumper (50).");
		return false;
	}

	h->credits -= THUMPER_COST;

	slot->used = true;
	slot->packed = packed;
	slot->houseID = houseID;
	slot->born = g_timerGame;

	Thumper_Message(houseID, "Thumper planted.  The worms will come.");
	if (g_debugTestThumper)
		Thumper_Log("thumper planted at %d,%d by unit %d; credits now %d", Tile_GetPackedX(packed), Tile_GetPackedY(packed), u->o.index, h->credits);
	return true;
}

uint16
Thumper_WormTarget(const Unit *worm)
{
	Thumper_Check();

	const Thumper *best = NULL;
	uint16 bestDistance = THUMPER_RANGE * 256;

	for (int i = 0; i < MAX_THUMPERS; i++) {
		const Thumper *t = &s_thumpers[i];

		if (!t->used || !Unit_Sandworm_CanReach(worm, t->packed))
			continue;

		const uint16 d = Tile_GetDistance(worm->o.position, Tile_Center(Tile_UnpackTile(t->packed)));

		if (d <= bestDistance) {
			bestDistance = d;
			best = t;
		}
	}

	return (best != NULL) ? Tools_Index_Encode(best->packed, IT_TILE) : 0;
}

bool
Thumper_Eat(uint16 packed)
{
	for (int i = 0; i < MAX_THUMPERS; i++) {
		if (s_thumpers[i].used && s_thumpers[i].packed == packed) {
			s_thumpers[i].used = false;
			if (g_debugTestThumper)
				Thumper_Log("thumper at %d,%d swallowed", Tile_GetPackedX(packed), Tile_GetPackedY(packed));
			return true;
		}
	}

	return false;
}

/* A post in the sand with a weight that rises and slams down, and the sand
 * shaking in rings around it.
 */
void
Thumper_Draw(void)
{
	Thumper_Check();

	const int64_t now = Timer_GetTicks();

	/* --test-thumper: a look at it, close up, once it has been beating a while. */
	if (g_debugTestThumper) {
		static int64_t first = -1;
		static bool shot = false;

		for (int i = 0; i < MAX_THUMPERS && !shot; i++) {
			if (!s_thumpers[i].used)
				continue;
			if (first < 0) {
				first = now;
				Map_CentreViewport(Tile_GetPackedX(s_thumpers[i].packed) * 16 + 8, Tile_GetPackedY(s_thumpers[i].packed) * 16 + 8);
			}
			if (now - first > 150) {
				shot = true;
				VideoA5_CaptureScreenshot();
			}
		}
	}

	for (int i = 0; i < MAX_THUMPERS; i++) {
		const Thumper *t = &s_thumpers[i];

		if (!t->used || !Map_IsUnveiledToHouse(g_playerHouseID, t->packed))
			continue;

		int x, y;

		if (!Map_IsPositionInViewport(Tile_Center(Tile_UnpackTile(t->packed)), &x, &y))
			continue;

		x += g_widgetProperties[WINDOWID_VIEWPORT].xBase;
		y += g_widgetProperties[WINDOWID_VIEWPORT].yBase;

		/* A beat every BEAT_TICKS: the weight is lifted slowly, then drops, and
		 * lands SLAM_AT ticks into the beat.
		 */
		const int64_t clock = now + i * 13;
		const int beat = (int)(clock % BEAT_TICKS);
		const float lift = (beat < SLAM_AT - 3) ? beat / (float)(SLAM_AT - 3) : 1.0f - (beat - (SLAM_AT - 3)) / 3.0f;
		const int top = (int)(4.0f * fmaxf(0.0f, lift));
		const bool hit = beat >= SLAM_AT && beat < SLAM_AT + 4;

		/* Each blow thuds, softly, and quieter the further it is from the middle of the view. */
		{
			const int64_t blow = (clock - SLAM_AT) / BEAT_TICKS;

			if (blow != s_lastBlow[i]) {
				const WidgetInfo *wi = &g_table_gameWidgetInfo[GAME_WIDGET_VIEWPORT];
				const float off = (x - g_widgetProperties[WINDOWID_VIEWPORT].xBase - wi->width / 2.0f) / (wi->width / 2.0f);

				if (s_lastBlow[i] >= 0 && beat >= SLAM_AT && beat < SLAM_AT + 6)
					Audio_PlayThump(0.22f * (1.0f - 0.5f * fminf(fabsf(off), 1.0f)), fmaxf(-1.0f, fminf(1.0f, off * 0.8f)));
				s_lastBlow[i] = blow;
			}
		}

		/* Waves through the sand, one from each blow, as it lands: a light crest with
		 * its shadow just behind, rolling outwards and dying away over a few beats.
		 */
		for (int ring = 0; ring < WAVES; ring++) {
			const float since = (float)((clock - SLAM_AT + BEAT_TICKS * 8) % BEAT_TICKS) + ring * BEAT_TICKS;
			const float phase = since / (BEAT_TICKS * WAVES);
			const float r = 2.0f + 15.0f * phase;
			const float fade = (1.0f - phase) * (1.0f - phase);

			al_draw_ellipse(x + 0.5f, y + 4.0f, r + 1.0f, (r + 1.0f) * 0.55f,
					al_map_rgba(70, 42, 18, (unsigned char)(70.0f * fade)), 1.0f);
			al_draw_ellipse(x + 0.5f, y + 3.5f, r, r * 0.55f,
					al_map_rgba(240, 212, 156, (unsigned char)(130.0f * fade)), 1.0f);
		}

		/* Shadow, legs, post, weight and the house's colour band. */
		const unsigned char *house = (t->houseID == HOUSE_HARKONNEN) ? (const unsigned char *)"\xC8\x3C\x30"
			: (t->houseID == HOUSE_ORDOS) ? (const unsigned char *)"\x40\xB0\x50" : (const unsigned char *)"\x48\x70\xD8";

		Prim_FillRect_RGBA(x - 3, y + 3, x + 4, y + 5, 40, 26, 12, 120);
		Prim_FillRect_RGBA(x - 3, y + 1, x - 2, y + 4, 70, 66, 62, 255);
		Prim_FillRect_RGBA(x + 2, y + 1, x + 3, y + 4, 70, 66, 62, 255);
		Prim_FillRect_RGBA(x, y - 9, x + 1, y + 4, 120, 116, 110, 255);
		Prim_FillRect_RGBA(x - 1, y - 2 - top, x + 2, y + 1 - top, 92, 88, 84, 255);
		Prim_FillRect_RGBA(x - 1, y - 9, x + 2, y - 8, house[0], house[1], house[2], 255);

		if (hit)
			Prim_FillRect_RGBA(x - 3, y + 1, x + 4, y + 2, 236, 206, 150, 200);
	}
}

/*--------------------------------------------------------------*/

/* --test-thumper: a worm on the sand, a soldier eight tiles off who plants a
 * thumper.  The worm should come and swallow it, and still be as hungry.
 */
void
Thumper_DebugTest(void)
{
	static int stage = 0;
	static int64_t start;
	static uint16 wormIndex = UNIT_INDEX_INVALID;
	static int wormAmount;
	static int creditsBefore;

	if (stage == 0) {
		const MapInfo *mapInfo = &g_mapInfos[g_scenario.mapScale];

		for (int y = mapInfo->minY + 2; y < mapInfo->minY + mapInfo->sizeY - 2 && stage == 0; y++) {
			for (int x = mapInfo->minX + 2; x < mapInfo->minX + mapInfo->sizeX - 12 && stage == 0; x++) {
				bool ok = true;

				for (int k = 0; k <= 9 && ok; k++) {
					const uint16 packed = Tile_PackXY(x + k, y);

					if (g_map[packed].hasStructure || Unit_Get_ByPackedTile(packed) != NULL
							|| Map_GetLandscapeType(packed) != LST_NORMAL_SAND)
						ok = false;
				}

				if (!ok)
					continue;

				Unit *soldier = Unit_Create(UNIT_INDEX_INVALID, UNIT_SOLDIER, g_playerHouseID, Tile_Center(Tile_UnpackTile(Tile_PackXY(x, y))), 0);
				Unit *worm = Unit_Create(UNIT_INDEX_INVALID, UNIT_SANDWORM, HOUSE_FREMEN, Tile_Center(Tile_UnpackTile(Tile_PackXY(x + 9, y))), 0);

				if (soldier == NULL || worm == NULL) {
					Thumper_Log("FAIL: could not make the soldier or the worm");
					exit(1);
				}

				worm->amount = 2;
				worm->o.seenByHouses = 0xFF;
				Unit_Server_SetAction(worm, ACTION_HUNT);
				wormIndex = worm->o.index;
				wormAmount = worm->amount;

				House *h = House_Get_ByIndex(g_playerHouseID);

				if (h->credits < 500)
					h->credits = 500;
				creditsBefore = h->credits;

				/* Off the sand it is refused; on it, planted. */
				if (!Thumper_Drop(soldier)) {
					Thumper_Log("FAIL: the thumper was not planted");
					exit(1);
				}

				if (h->credits != creditsBefore - THUMPER_COST) {
					Thumper_Log("FAIL: credits %d -> %d, not %d less", creditsBefore, h->credits, THUMPER_COST);
					exit(1);
				}

				Tile_RemoveFogInRadius(1 << g_playerHouseID, UNVEILCAUSE_EXPLOSION, soldier->o.position, 12);
				Thumper_Log("soldier at %d,%d, worm at %d,%d (amount %d)", x, y, x + 9, y, wormAmount);
				start = g_timerGame;
				stage = 1;
			}
		}

		if (stage == 0) {
			Thumper_Log("FAIL: no sand to test on");
			exit(1);
		}
		return;
	}

	const House *h = House_Get_ByIndex(g_playerHouseID);
	const Unit *worm = Unit_Get_ByIndex(wormIndex);
	bool beating = false;

	for (int i = 0; i < MAX_THUMPERS; i++)
		beating |= s_thumpers[i].used;

	if ((g_timerGame - start) % 120 == 0 && worm != NULL && worm->o.flags.s.used)
		Thumper_Log("worm at %d,%d amount %d target %04x; thumper %s",
				Tile_GetPosX(worm->o.position), Tile_GetPosY(worm->o.position), worm->amount, worm->targetAttack,
				beating ? "beating" : "gone");

	if (!beating) {
		const bool ok = worm != NULL && worm->o.flags.s.used && worm->o.type == UNIT_SANDWORM
			&& worm->amount == wormAmount;

		Thumper_Log("%s: swallowed after %.1f s; worm amount %d (was %d); credits %d -> %d",
				ok ? "PASS" : "FAIL", (g_timerGame - start) / 60.0, worm ? worm->amount : -1, wormAmount, creditsBefore, h->credits);
		exit(ok ? 0 : 1);
	}

	if (g_timerGame - start > 60 * 60) {
		Thumper_Log("FAIL: no worm came in a minute");
		exit(1);
	}
}
