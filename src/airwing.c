/** @file src/airwing.c The player's ornithopters and idle harvesters.
 *
 * Ornithopters fly fast and are hard to click.  A row of icons in the bottom left
 * of the viewport has one for each of the player's ornithopters, and one for each
 * harvester standing idle.  Clicking an icon selects the unit and centres the view
 * on it; shift-click adds it to the selection.
 *
 * Ornithopters have two orders.  Hunt is what the game's own script does: fly to
 * the best target anywhere.  Patrol keeps one slowly circling where it was told,
 * and it only goes for enemies that come into its area.  Its area is drawn when it
 * is selected.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "os/math.h"

#include "airwing.h"

#include "audio/audio_a5.h"

#include "common_a5.h"
#include "gfx.h"
#include "gui/font.h"
#include "gui/gui.h"
#include "gui/widget.h"
#include "house.h"
#include "map.h"
#include "net/net.h"
#include "net/client.h"
#include "opendune.h"
#include "pool/pool.h"
#include "pool/pool_house.h"
#include "pool/pool_structure.h"
#include "pool/pool_unit.h"
#include "shape.h"
#include "sprites.h"
#include "structure.h"
#include "table/widgetinfo.h"
#include "timer/timer.h"
#include "tools/coord.h"
#include "tools/encoded_index.h"
#include "unit.h"
#include "video/prim.h"
#include "video/video_a5.h"

#define PATROL_RADIUS 8             /* Tiles: it goes for enemies this close to its post. */
#define PATROL_CIRCLE 5             /* Tiles: the circle it flies while there is nothing to do. */
#define PATROL_POINTS 12            /* Waypoints on that circle. */
#define PATROL_SPEED 90             /* Of 255: slow, so it is not a distraction. */
#define MAX_AIR 16                  /* Air units have the low unit indices. */

#define MAX_ICONS 12
#define LABEL_H 8               /* Under the health bar: what it is doing. */
#define ICON_W 36
#define ICON_H 27

int  g_debugSpawnOrnithopters = 0;
bool g_debugTestAirwing = false;
bool g_debugTestAirDefence = false;

static int s_testFuses, s_testLaunches;   /* For --test-air-defence. */
static bool s_testEngaged;

typedef struct Patrol {
	bool active;
	tile32 centre;
	int point;                      /* The waypoint it is flying to. */
	uint16 attack;                  /* An attack order: what the player told it to attack. */
} Patrol;

static Patrol s_patrol[MAX_AIR];
static int64_t s_scenarioStart = -1;

/* Where the icons were last drawn, in screen pixels, so clicks can find them. */
static struct {
	float x1, y1, x2, y2;
	uint16 index;
} s_icons[MAX_ICONS];
static int s_iconCount;

/*--------------------------------------------------------------*/

static void
Airwing_Log(const char *fmt, ...)
{
	FILE *log = fopen("ornithopter_test.log", "a");
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

static bool
Airwing_IsPlayerOrnithopter(const Unit *u)
{
	return u != NULL && u->o.type == UNIT_ORNITHOPTER && u->o.index < MAX_AIR
		&& House_IsHuman(Unit_GetHouseID(u));
}

static const Patrol *
Airwing_GetPatrol(const Unit *u)
{
	if (!Airwing_IsPlayerOrnithopter(u) || u->actionID != ACTION_AREA_GUARD)
		return NULL;

	const Patrol *p = &s_patrol[u->o.index];

	return p->active ? p : NULL;
}

static tile32
Airwing_Waypoint(const Patrol *p, int point)
{
	const float a = point * 6.2831853f / PATROL_POINTS;
	tile32 t = p->centre;
	const int x = (int)t.x + (int)(cosf(a) * PATROL_CIRCLE * 256);
	const int y = (int)t.y + (int)(sinf(a) * PATROL_CIRCLE * 256);

	t.x = (uint16)clamp(128, x, 64 * 256 - 128);
	t.y = (uint16)clamp(128, y, 64 * 256 - 128);
	return t;
}

/* The nearest enemy the player can see inside the patrol area. */
static uint16
Airwing_FindPatrolTarget(const Unit *unit, const Patrol *p)
{
	const uint8 houseID = Unit_GetHouseID(unit);
	const uint16 reach = PATROL_RADIUS * 256;
	PoolFindStruct find;
	uint16 best = 0;
	uint16 bestDistance = 0xFFFF;

	for (const Unit *u = Unit_FindFirst(&find, HOUSE_INVALID, UNIT_INVALID); u != NULL; u = Unit_FindNext(&find)) {
		const UnitInfo *ui = &g_table_unitInfo[u->o.type];

		if (u->o.flags.s.isNotOnMap || !ui->flags.isNormalUnit || u->o.type == UNIT_SANDWORM)
			continue;
		if (ui->movementType == MOVEMENT_WINGER)
			continue;   /* Its rockets cannot hit things in the air. */
		if (House_AreAllied(Unit_GetHouseID(u), houseID))
			continue;
		if ((u->o.seenByHouses & (1 << houseID)) == 0)
			continue;
		if (Tile_GetDistance(p->centre, u->o.position) > reach)
			continue;

		const uint16 d = Tile_GetDistance(unit->o.position, u->o.position);

		if (d < bestDistance) {
			bestDistance = d;
			best = Tools_Index_Encode(u->o.index, IT_UNIT);
		}
	}

	for (const Structure *s = Structure_FindFirst(&find, HOUSE_INVALID, STRUCTURE_INVALID); s != NULL; s = Structure_FindNext(&find)) {
		if (s->o.type == STRUCTURE_SLAB_1x1 || s->o.type == STRUCTURE_SLAB_2x2 || s->o.type == STRUCTURE_WALL)
			continue;
		if (House_AreAllied(s->o.houseID, houseID))
			continue;
		if ((s->o.seenByHouses & (1 << houseID)) == 0)
			continue;
		if (Tile_GetDistance(p->centre, s->o.position) > reach)
			continue;

		/* Units first: a structure only when it is a good deal nearer. */
		const uint16 d = Tile_GetDistance(unit->o.position, s->o.position) + 4 * 256;

		if (d < bestDistance) {
			bestDistance = d;
			best = Tools_Index_Encode(s->o.index, IT_STRUCTURE);
		}
	}

	return best;
}

bool
Airwing_OverrideTarget(const Unit *unit, uint16 *encoded)
{
	/* On an attack order it goes for that, and nothing else. */
	if (Airwing_IsPlayerOrnithopter(unit) && (unit->actionID == ACTION_ATTACK || unit->nextActionID == ACTION_ATTACK) && s_patrol[unit->o.index].attack != 0) {
		*encoded = s_patrol[unit->o.index].attack;
		return true;
	}

	const Patrol *p = Airwing_GetPatrol(unit);

	if (p == NULL)
		return false;

	*encoded = Airwing_FindPatrolTarget(unit, p);
	return true;
}

bool
Airwing_LimitSpeed(const Unit *unit, uint16 *speed)
{
	const Patrol *p = Airwing_GetPatrol(unit);

	/* Slow while it circles; at full speed when it goes for something. */
	if (p == NULL || unit->targetAttack != 0)
		return false;

	if (*speed > PATROL_SPEED)
		*speed = PATROL_SPEED;
	return true;
}

/* Keeps a patrolling ornithopter at its post. */
static void
Airwing_PatrolTick(Unit *u)
{
	Patrol *p = &s_patrol[u->o.index];

	/* An attack order: after its target until it is gone, then on patrol where it is. */
	if (u->actionID == ACTION_ATTACK || u->nextActionID == ACTION_ATTACK) {
		const Unit *target = (Tools_Index_GetType(p->attack) == IT_UNIT) ? Tools_Index_GetUnit(p->attack) : NULL;

		if (p->attack == 0 || !Tools_Index_IsValid(p->attack) || Tools_Index_GetType(p->attack) == IT_TILE
				|| (target != NULL && House_AreAllied(Unit_GetHouseID(target), Unit_GetHouseID(u)))) {
			p->attack = 0;
			p->active = false;
			Unit_Server_SetAction(u, ACTION_AREA_GUARD);
			return;
		}

		if (u->targetAttack != p->attack)
			Unit_SetTarget(u, p->attack);
		return;
	}

	p->attack = 0;

	if (u->actionID != ACTION_AREA_GUARD) {
		/* A patrol order waits until the move it is in the middle of is done. */
		if (u->nextActionID != ACTION_AREA_GUARD)
			p->active = false;
		return;
	}

	if (!p->active) {
		/* A new order: its post is where it is now. */
		p->active = true;
		p->centre = u->o.position;
		p->point = 0;
		u->targetAttack = 0;
		u->targetMove = 0;
	}

	/* A target that has left the area is let go. */
	if (u->targetAttack != 0) {
		if (!Tools_Index_IsValid(u->targetAttack)
				|| Tile_GetDistance(p->centre, Tools_Index_GetTile(u->targetAttack)) > (PATROL_RADIUS + 2) * 256) {
			u->targetAttack = 0;
			u->targetMove = 0;
		} else {
			return;
		}
	}

	/* Nothing to shoot: fly the circle.  It aims a little ahead of where it is on
	 * the circle, so it goes round smoothly; from far away that point is on the near
	 * side, and it comes back first.
	 */
	const float dx = (float)u->o.position.x - p->centre.x, dy = (float)u->o.position.y - p->centre.y;
	const float here = atan2f(dy, dx);
	const int ahead = (int)floorf((here / 6.2831853f) * PATROL_POINTS + 0.5f) + 2;

	p->point = ((ahead % PATROL_POINTS) + PATROL_POINTS) % PATROL_POINTS;

	const tile32 next = Airwing_Waypoint(p, p->point);

	u->targetMove = 0;
	Unit_SetOrientation(u, Tile_GetDirection(u->o.position, next), false, 0);

	if (u->movingSpeed != PATROL_SPEED)
		Unit_SetSpeed(u, PATROL_SPEED);
}

/*--------------------------------------------------------------*/

static void
Airwing_Spawn(int count)
{
	PoolFindStruct find;
	const Structure *s = Structure_FindFirst(&find, g_playerHouseID, STRUCTURE_CONSTRUCTION_YARD);

	if (s == NULL)
		s = Structure_FindFirst(&find, g_playerHouseID, STRUCTURE_INVALID);
	if (s == NULL)
		return;

	for (int i = 0; i < count; i++) {
		tile32 pos = s->o.position;

		pos.x += 256 * (i % 3) - 256;
		pos.y += 256 * 3 + 256 * (i / 3);

		Unit *u = Unit_Create(UNIT_INDEX_INVALID, UNIT_ORNITHOPTER, g_playerHouseID, pos, 0);

		if (g_debugTestAirwing)
			Airwing_Log("spawn %d: %s", i, u ? "ok" : "failed");
	}
}

/* --test-ornithopters: one on patrol, the others hunt.  Logs where they are. */
static void
Airwing_TestTick(void)
{
	static int64_t next;
	static int s_testFailures = 0;
	static bool s_testClicked = false;
	static bool s_testMoved = false, s_testArrived = false;
	static int s_testAttacker = -1, s_testTargetIndex = -1, s_testTargetHp = 0;
	static bool s_testAttackDone = false, s_testAttacked = false;
	static tile32 s_testTarget;
	PoolFindStruct find;

	if (g_timerGame < next)
		return;
	next = g_timerGame + 120;

	int n = 0;

	for (Unit *u = Unit_FindFirst(&find, g_playerHouseID, UNIT_ORNITHOPTER); u != NULL; u = Unit_FindNext(&find), n++) {
		static int patroller = -1;

		/* They start on patrol.  The first stays on it; the others are sent to hunt. */
		if (patroller < 0)
			patroller = u->o.index;
		if (g_timerGame - g_tickScenarioStart < 200 && u->o.index != patroller && u->actionID != ACTION_HUNT) {
			Airwing_Log("orni idx=%d started with action=%d, now sent to hunt", u->o.index, u->actionID);
			Unit_Server_SetAction(u, ACTION_HUNT);
		}

		/* The patroller in view and selected (for the screenshot), and found by a click on it. */
		if (u->o.index == patroller && g_timerGame - g_tickScenarioStart >= 600 && g_timerGame - g_tickScenarioStart < 720) {
			Map_CentreViewport(u->o.position.x >> 4, u->o.position.y >> 4);

			int ux, uy;

			if (Map_IsPositionInViewport(u->o.position, &ux, &uy)) {
				const Unit *hit = Airwing_UnitAtViewport(ux + 3, uy - 2);

				Airwing_Log("click at its sprite finds idx=%d (want %d)", hit ? hit->o.index : -1, u->o.index);
				if (hit == u)
					s_testClicked = true;
				else
					s_testFailures++;
				if (hit != NULL && !Unit_IsSelected(hit)) {
					Map_SetSelection(0xFFFF);
					Unit_UnselectAll();
					Unit_Select(u);
				}
			}
		}

		const Patrol *p = &s_patrol[u->o.index < MAX_AIR ? u->o.index : 0];
		const int fromPost = p->active ? Tile_GetDistance(p->centre, u->o.position) / 256 : -1;

		Airwing_Log("orni idx=%d action=%d pos=%d,%d from_post=%d attack=%04x move=%04x speed=%d hp=%d",
				u->o.index, u->actionID, u->o.position.x / 256, u->o.position.y / 256, fromPost,
				u->targetAttack, u->targetMove, u->movingSpeed, u->o.hitpoints);

		/* With nothing to shoot, the patroller stays by its post, slowly. */
		const int64_t age = g_timerGame - g_tickScenarioStart;

		/* At 30 s a hunter is told, as the player would, to attack an enemy building. */
		if (u->o.index != patroller && age >= 1800 && s_testAttacker < 0) {
			PoolFindStruct findS;

			for (const Structure *s = Structure_FindFirst(&findS, HOUSE_INVALID, STRUCTURE_INVALID); s != NULL; s = Structure_FindNext(&findS)) {
				if (House_AreAllied(s->o.houseID, g_playerHouseID) || s->o.type == STRUCTURE_SLAB_1x1 || s->o.type == STRUCTURE_SLAB_2x2 || s->o.type == STRUCTURE_WALL)
					continue;

				s_testAttacker = u->o.index;
				s_testTargetIndex = s->o.index;
				s_testTargetHp = s->o.hitpoints;
				Airwing_Log("idx=%d ordered to attack structure idx=%d type=%d hp=%d", u->o.index, s->o.index, s->o.type, s->o.hitpoints);
				Client_Send_IssueUnitAction(ACTION_ATTACK, Tools_Index_Encode(s->o.index, IT_STRUCTURE), &u->o);
				break;
			}
		}

		/* It keeps to that target, and hurts it. */
		if (u->o.index == s_testAttacker && age >= 1900 && !s_testAttackDone) {
			const Structure *s = Structure_Get_ByIndex(s_testTargetIndex);
			const bool alive = s != NULL && s->o.flags.s.used && !House_AreAllied(s->o.houseID, g_playerHouseID);
			const uint16 want = Tools_Index_Encode(s_testTargetIndex, IT_STRUCTURE);

			if (u->actionID == ACTION_DIE) {
				/* Being shot down: judged by the damage it did, below. */
			} else if (alive && (u->actionID != ACTION_ATTACK || u->targetAttack != want)) {
				Airwing_Log("FAIL: the attacker left its target (action=%d attack=%04x want %04x)", u->actionID, u->targetAttack, want);
				s_testFailures++;
				s_testAttackDone = true;
			} else if (!alive || age >= 3000) {
				const int hp = alive ? s->o.hitpoints : 0;

				Airwing_Log("attack: target hp %d -> %d, attacker action=%d", s_testTargetHp, hp, u->actionID);
				if (hp >= s_testTargetHp || (!alive && u->actionID != ACTION_AREA_GUARD && u->nextActionID != ACTION_AREA_GUARD)) {
					Airwing_Log("FAIL: the attack did no damage, or it did not go back to patrol");
					s_testFailures++;
				} else {
					s_testAttacked = true;
				}
				s_testAttackDone = true;
			}
		}

		/* At 20 s it is sent, as the player would, to patrol somewhere else. */
		if (u->o.index == patroller && age >= 1200 && !s_testMoved) {
			tile32 there = u->o.position;

			there.x = (uint16)((there.x / 256 < 32) ? there.x + 12 * 256 : there.x - 12 * 256);
			s_testMoved = true;
			s_testTarget = Tile_Center(there);
			Airwing_Log("ordered to patrol at %d,%d", s_testTarget.x / 256, s_testTarget.y / 256);
			Client_Send_IssueUnitAction(ACTION_AREA_GUARD, Tools_Index_Encode(Tile_PackTile(there), IT_TILE), &u->o);
		}

		/* Ten seconds later it should be circling there. */
		if (u->o.index == patroller && s_testMoved && age >= 1800 && !s_testArrived) {
			const int fromTarget = Tile_GetDistance(s_testTarget, u->o.position) / 256;

			Airwing_Log("after the order: post %d,%d, %d tiles from the place", p->centre.x / 256, p->centre.y / 256, fromTarget);
			if (!p->active || Tile_GetDistance(p->centre, s_testTarget) > 256 || fromTarget > PATROL_CIRCLE + 2) {
				Airwing_Log("FAIL: it is not patrolling the place it was sent to");
				s_testFailures++;
			}
			s_testArrived = true;
		}

		if (u->o.index == patroller && u->targetAttack == 0 && age > 600 && (age < 1200 || age > 1800)) {
			if (u->actionID != ACTION_AREA_GUARD || fromPost > PATROL_CIRCLE + 2 || u->movingSpeed > PATROL_SPEED) {
				Airwing_Log("FAIL: the patroller wandered (action=%d from_post=%d speed=%d)", u->actionID, fromPost, u->movingSpeed);
				s_testFailures++;
			}
		}
	}

	/* Shot down before it was done: judge the attack by the damage it did. */
	if (s_testAttacker >= 0 && !s_testAttackDone) {
		const Unit *a = Unit_Get_ByIndex(s_testAttacker);

		if (a == NULL || !a->o.flags.s.used || a->o.type != UNIT_ORNITHOPTER || a->actionID == ACTION_DIE) {
			const Structure *s = Structure_Get_ByIndex(s_testTargetIndex);
			const int hp = (s != NULL && s->o.flags.s.used) ? s->o.hitpoints : 0;

			Airwing_Log("attack: the attacker was shot down; target hp %d -> %d", s_testTargetHp, hp);
			s_testAttacked = hp < s_testTargetHp;
			if (!s_testAttacked)
				s_testFailures++;
			s_testAttackDone = true;
		}
	}

	if (g_timerGame - g_tickScenarioStart > 60 * 90) {
		Airwing_Log("finished: %d failures, click test %s, attack test %s", s_testFailures, s_testClicked ? "passed" : "not passed", s_testAttacked ? "passed" : "not passed");
		exit(s_testFailures == 0 && s_testClicked && s_testArrived && s_testAttacked ? 0 : 1);
	}
}

static void Airwing_UpdateSight(void);
static void Airwing_TrackAir(void);
static void Airwing_AirDefence(void);
static void Airwing_Fuses(void);
static void Airwing_AirDefenceTest(void);

void
Airwing_Tick(void)
{
	if (s_scenarioStart != g_tickScenarioStart) {
		s_scenarioStart = g_tickScenarioStart;

		for (int i = 0; i < MAX_AIR; i++)
			s_patrol[i].active = false;

		const int count = g_debugTestAirwing ? 3 : g_debugTestAirDefence ? 2 : g_debugSpawnOrnithopters;

		if (count > 0)
			Airwing_Spawn(count);
	}

	Airwing_UpdateSight();
	Airwing_TrackAir();
	Airwing_AirDefence();
	Airwing_Fuses();

	PoolFindStruct find;

	for (Unit *u = Unit_FindFirst(&find, HOUSE_INVALID, UNIT_ORNITHOPTER); u != NULL; u = Unit_FindNext(&find)) {
		if (Airwing_IsPlayerOrnithopter(u))
			Airwing_PatrolTick(u);
	}

	if (g_debugTestAirwing)
		Airwing_TestTick();

	if (g_debugTestAirDefence)
		Airwing_AirDefenceTest();
}

/*--------------------------------------------------------------*/

static bool
Airwing_IsIdleHarvester(const Unit *u)
{
	return u->o.type == UNIT_HARVESTER && !u->o.flags.s.isNotOnMap
		&& (u->actionID == ACTION_STOP || u->actionID == ACTION_GUARD);
}

/* Screen position of a map position, in the viewport's own units. */
static void
Airwing_ToViewport(tile32 pos, float *x, float *y)
{
	*x = (float)(pos.x >> 4) - TILE_SIZE * Tile_GetPackedX(g_viewportPosition) - g_viewport_scrollOffsetX;
	*y = (float)(pos.y >> 4) - TILE_SIZE * Tile_GetPackedY(g_viewportPosition) - g_viewport_scrollOffsetY;
}

void
Airwing_DrawPatrolAreas(void)
{
	int iter;

	for (const Unit *u = Unit_FirstSelected(&iter); u != NULL; u = Unit_NextSelected(&iter)) {
		const Patrol *p = Airwing_GetPatrol(u);

		if (p == NULL)
			continue;

		float cx, cy;
		const float r = PATROL_RADIUS * TILE_SIZE;

		Airwing_ToViewport(p->centre, &cx, &cy);

		/* The area, faintly, with a brighter edge and a mark at its post. */
		Prim_FillCircle_RGBA(cx, cy, r, 120, 200, 255, 22);
		Prim_Circle_RGBA(cx, cy, r, 150, 220, 255, 150, 1.0f);
		Prim_Circle_RGBA(cx, cy, 3.0f, 150, 220, 255, 200, 1.0f);
	}
}

void
Airwing_DrawIcons(void)
{
	PoolFindStruct find;
	const Unit *units[MAX_ICONS];
	int n = 0;

	s_iconCount = 0;

	if (g_playerHouseID >= HOUSE_MAX)
		return;

	for (const Unit *u = Unit_FindFirst(&find, g_playerHouseID, UNIT_ORNITHOPTER); u != NULL && n < MAX_ICONS; u = Unit_FindNext(&find)) {
		if (!u->o.flags.s.isNotOnMap)
			units[n++] = u;
	}

	for (const Unit *u = Unit_FindFirst(&find, g_playerHouseID, UNIT_HARVESTER); u != NULL && n < MAX_ICONS; u = Unit_FindNext(&find)) {
		if (Airwing_IsIdleHarvester(u))
			units[n++] = u;
	}

	if (g_debugTestAirwing && g_timerGame - g_tickScenarioStart > 660) {
		static bool shot;

		if (!shot) {
			shot = true;
			VideoA5_CaptureScreenshot();
		}
	}

	if (n == 0)
		return;

	const enum ScreenDivID old_div = A5_SaveTransform();
	const ScreenDiv *hud = &g_screenDiv[SCREENDIV_HUD];

	A5_UseTransform(SCREENDIV_HUD);

	const int y = hud->height - ICON_H - LABEL_H - 8;
	int x = 6;

	GUI_DrawText_Wrapper(NULL, 0, 0, 15, 0, 0x21);

	for (int i = 0; i < n; i++, x += ICON_W + 4) {
		const Unit *u = units[i];
		const UnitInfo *ui = &g_table_unitInfo[u->o.type];
		const bool selected = Unit_IsSelected(u);

		Prim_FillRect_RGBA(x - 1, y - 1, x + ICON_W + 1, y + ICON_H + 4 + LABEL_H, 0, 0, 0, 150);
		Shape_DrawScale(ui->o.spriteID, x, y, ICON_W, ICON_H, 0, 0);

		if (selected)
			Prim_Rect_RGBA(x - 1, y - 1, x + ICON_W + 1, y + ICON_H + 4 + LABEL_H, 255, 255, 255, 230, 1.0f);

		/* Health. */
		const float hp = (float)u->o.hitpoints / ui->o.hitpoints;
		const unsigned char r = (hp < 0.5f) ? 230 : 60, g = (hp < 0.25f) ? 40 : 210;

		Prim_FillRect_RGBA(x, y + ICON_H + 1, x + ICON_W * fminf(1.0f, hp), y + ICON_H + 3, r, g, 40, 230);

		/* What it is doing. */
		const char *tag = (u->o.type == UNIT_HARVESTER) ? "IDLE"
			: (u->actionID == ACTION_AREA_GUARD) ? "PATROL" : "HUNT";

		GUI_DrawText_Wrapper(NULL, 0, 0, (u->o.type == UNIT_HARVESTER) ? 6 : 15, 0, 0x21);
		GUI_DrawTextAlpha(tag, x + (ICON_W - Font_GetStringWidth(tag)) / 2, y + ICON_H + 4, 230);

		s_icons[s_iconCount].x1 = hud->x + (x - 1) * hud->scalex;
		s_icons[s_iconCount].y1 = hud->y + (y - 1) * hud->scaley;
		s_icons[s_iconCount].x2 = hud->x + (x + ICON_W + 1) * hud->scalex;
		s_icons[s_iconCount].y2 = hud->y + (y + ICON_H + 4 + LABEL_H) * hud->scaley;
		s_icons[s_iconCount].index = u->o.index;
		s_iconCount++;
	}

	A5_UseTransform(old_div);
}

bool
Airwing_Click(int mouseX, int mouseY, bool add)
{
	for (int i = 0; i < s_iconCount; i++) {
		if (mouseX < s_icons[i].x1 || mouseX >= s_icons[i].x2 || mouseY < s_icons[i].y1 || mouseY >= s_icons[i].y2)
			continue;

		Unit *u = Unit_Get_ByIndex(s_icons[i].index);

		if (u == NULL || !u->o.flags.s.used || u->o.flags.s.isNotOnMap || Unit_GetHouseID(u) != g_playerHouseID)
			return true;

		if (add) {
			if (Unit_IsSelected(u))
				Unit_Unselect(u);
			else
				Unit_Select(u);
		} else {
			Map_SetSelection(0xFFFF);
			Unit_UnselectAll();
			Unit_Select(u);
			Map_CentreViewport(u->o.position.x >> 4, u->o.position.y >> 4);
		}
		return true;
	}

	return false;
}

void
Airwing_OrderGiven(const Unit *u)
{
	if (u != NULL && u->o.index < MAX_AIR)
		s_patrol[u->o.index].active = false;
}

Unit *
Airwing_UnitAtViewport(int x, int y)
{
	PoolFindStruct find;
	Unit *best = NULL;
	int bestDistance = 12 * 12;     /* Pixels: about the size of its sprite. */

	for (Unit *u = Unit_FindFirst(&find, g_playerHouseID, UNIT_ORNITHOPTER); u != NULL; u = Unit_FindNext(&find)) {
		int ux, uy;

		if (u->o.flags.s.isNotOnMap || !Map_IsPositionInViewport(u->o.position, &ux, &uy))
			continue;

		const int d = (ux - x) * (ux - x) + (uy - y) * (uy - y);

		if (d < bestDistance) {
			bestDistance = d;
			best = u;
		}
	}

	return best;
}

bool
Airwing_ActionNeedsTarget(const Unit *u, int action)
{
	return action == ACTION_AREA_GUARD && Airwing_IsPlayerOrnithopter(u);
}

void
Airwing_SetPatrol(Unit *u, uint16_t encoded)
{
	if (!Airwing_IsPlayerOrnithopter(u) || !Tools_Index_IsValid(encoded))
		return;

	Patrol *p = &s_patrol[u->o.index];

	p->active = true;
	p->centre = Tile_Center(Tools_Index_GetTile(encoded));
	p->point = 0;
	u->targetAttack = 0;
	u->targetMove = 0;
}

/*--------------------------------------------------------------*/
/* Air defence.
 *
 * Missiles fired at something in the air aim where it is going to be, not where it
 * is: every aircraft's velocity is tracked, and so is how fast each kind of missile
 * flies.  At launch and while flying the missile steers for the point where the
 * two meet, and it goes off as soon as it is close to its target.
 */

#define FUSE_DISTANCE 112           /* Tile units (256 to a tile): close enough to go off. */
#define MAX_LEAD_TICKS 180

typedef struct Track {
	bool valid;
	tile32 pos;
	int64_t time;
	float vx, vy;                   /* Tile units per game tick. */
} Track;

static Track s_track[UNIT_INDEX_MAX];
static float s_bulletSpeed[UNIT_MAX];  /* Learnt from missiles in flight, tile units per game tick. */

static bool
Airwing_IsAir(const Unit *u)
{
	return u != NULL && g_table_unitInfo[u->o.type].movementType == MOVEMENT_WINGER;
}

static void
Airwing_TrackAir(void)
{
	PoolFindStruct find;

	for (Unit *u = Unit_FindFirst(&find, HOUSE_INVALID, UNIT_INVALID); u != NULL; u = Unit_FindNext(&find)) {
		if (!Airwing_IsAir(u) || u->o.index >= UNIT_INDEX_MAX)
			continue;

		Track *t = &s_track[u->o.index];
		const int64_t dt = g_timerGame - t->time;

		if (!t->valid || dt <= 0 || dt > 30 || Tile_GetDistance(t->pos, u->o.position) > 1024) {
			/* New, or it was somewhere else: start again. */
			t->valid = true;
			t->vx = t->vy = 0.0f;
		} else {
			const float vx = ((float)u->o.position.x - t->pos.x) / dt;
			const float vy = ((float)u->o.position.y - t->pos.y) / dt;

			/* Smoothed: the speed comes in steps. */
			t->vx = 0.6f * t->vx + 0.4f * vx;
			t->vy = 0.6f * t->vy + 0.4f * vy;

			if (g_table_unitInfo[u->o.type].flags.isBullet) {
				const float speed = sqrtf(vx * vx + vy * vy);

				if (speed > 1.0f)
					s_bulletSpeed[u->o.type] = (s_bulletSpeed[u->o.type] == 0.0f) ? speed : 0.9f * s_bulletSpeed[u->o.type] + 0.1f * speed;
			}
		}

		t->pos = u->o.position;
		t->time = g_timerGame;
	}
}

/* Aircraft are not put on the map, so nobody ever "sees" one, and so nothing ever
 * shoots at one.  An aircraft is seen by every house with a unit or a building
 * near it.  The player's own sight of it is left as the game keeps it.
 */
#define SIGHT_TILES 7

static void
Airwing_UpdateSight(void)
{
	static int64_t next;
	PoolFindStruct find, find2;

	if (g_timerGame < next)
		return;
	next = g_timerGame + 10;

	for (Unit *air = Unit_FindFirst(&find, HOUSE_INVALID, UNIT_INVALID); air != NULL; air = Unit_FindNext(&find)) {
		if (!Airwing_IsAir(air) || g_table_unitInfo[air->o.type].flags.isBullet || air->o.flags.s.isNotOnMap)
			continue;
		if (air->o.type == UNIT_FRIGATE)
			continue;

		uint8 seen = (uint8)((1 << Unit_GetHouseID(air)) | (air->o.seenByHouses & (1 << g_playerHouseID)));

		for (const Unit *u = Unit_FindFirst(&find2, HOUSE_INVALID, UNIT_INVALID); u != NULL; u = Unit_FindNext(&find2)) {
			const UnitInfo *ui = &g_table_unitInfo[u->o.type];

			if (u->o.flags.s.isNotOnMap || !ui->flags.isNormalUnit || ui->movementType == MOVEMENT_WINGER)
				continue;
			if (Tile_GetDistance(u->o.position, air->o.position) < SIGHT_TILES * 256)
				seen |= 1 << Unit_GetHouseID(u);
		}

		for (const Structure *s = Structure_FindFirst(&find2, HOUSE_INVALID, STRUCTURE_INVALID); s != NULL; s = Structure_FindNext(&find2)) {
			if (Tile_GetDistance(s->o.position, air->o.position) < SIGHT_TILES * 256)
				seen |= 1 << s->o.houseID;
		}

		air->o.seenByHouses = seen;
	}
}

/* Units that can shoot at aircraft (troopers) turn to one that comes in range,
 * whatever they were shooting at on the ground.  The player's explicit attack
 * orders are left alone.
 */
static void
Airwing_AirDefence(void)
{
	static int64_t next;
	PoolFindStruct find, find2;

	if (g_timerGame < next)
		return;
	next = g_timerGame + 15;

	for (Unit *u = Unit_FindFirst(&find, HOUSE_INVALID, UNIT_INVALID); u != NULL; u = Unit_FindNext(&find)) {
		const UnitInfo *ui = &g_table_unitInfo[u->o.type];

		if (!ui->o.flags.targetAir || u->o.flags.s.isNotOnMap || !ui->flags.isNormalUnit)
			continue;
		if (House_IsHuman(Unit_GetHouseID(u)) && u->actionID == ACTION_ATTACK)
			continue;
		if (Airwing_IsAir(Tools_Index_GetUnit(u->targetAttack)))
			continue;   /* Already on one. */

		Unit *best = NULL;
		uint16 bestDistance = (uint16)(ui->fireDistance * 256);

		for (Unit *air = Unit_FindFirst(&find2, HOUSE_INVALID, UNIT_INVALID); air != NULL; air = Unit_FindNext(&find2)) {
			if (!Airwing_IsAir(air) || g_table_unitInfo[air->o.type].flags.isBullet || !g_table_unitInfo[air->o.type].flags.isNormalUnit)
				continue;
			if (House_AreAllied(Unit_GetHouseID(air), Unit_GetHouseID(u)))
				continue;
			if (Unit_GetTargetUnitPriority(u, air) == 0)
				continue;

			const uint16 d = Tile_GetDistance(u->o.position, air->o.position);

			if (d <= bestDistance) {
				bestDistance = d;
				best = air;
			}
		}

		if (best != NULL)
			Unit_SetTarget(u, Tools_Index_Encode(best->o.index, IT_UNIT));
	}
}

static float
Airwing_BulletSpeed(const Unit *bullet)
{
	const float learnt = s_bulletSpeed[bullet->o.type];

	if (learnt > 0.0f)
		return learnt;

	/* Before any has been seen: a guess from its table speed. */
	return fmaxf(8.0f, g_table_unitInfo[bullet->o.type].movingSpeedFactor / 6.0f);
}

/* Where a missile from here should aim to meet the target. */
static tile32
Airwing_Intercept(tile32 from, const Unit *target, float speed)
{
	if (target == NULL || target->o.index >= UNIT_INDEX_MAX)
		return from;

	const Track *t = &s_track[target->o.index];
	tile32 aim = target->o.position;

	if (!t->valid || speed <= 0.0f)
		return aim;

	/* A few rounds: the time to get there, where it will be by then. */
	for (int i = 0; i < 3; i++) {
		float ticks = Tile_GetDistance(from, aim) / speed;

		if (ticks > MAX_LEAD_TICKS)
			ticks = MAX_LEAD_TICKS;

		const int x = (int)target->o.position.x + (int)(t->vx * ticks);
		const int y = (int)target->o.position.y + (int)(t->vy * ticks);

		aim.x = (uint16)clamp(0, x, 64 * 256 - 1);
		aim.y = (uint16)clamp(0, y, 64 * 256 - 1);
	}

	return aim;
}

static Unit *
Airwing_AirTarget(const Unit *bullet)
{
	if (Tools_Index_GetType(bullet->targetAttack) != IT_UNIT)
		return NULL;

	Unit *target = Tools_Index_GetUnit(bullet->targetAttack);

	return (Airwing_IsAir(target) && !g_table_unitInfo[target->o.type].flags.isBullet) ? target : NULL;
}

void
Airwing_AimBullet(Unit *bullet, bool launch)
{
	Unit *target = Airwing_AirTarget(bullet);

	if (target == NULL)
		return;

	const tile32 aim = Airwing_Intercept(bullet->o.position, target, Airwing_BulletSpeed(bullet));
	const int8 orientation = Tile_GetDirection(bullet->o.position, aim);

	bullet->currentDestination = aim;
	bullet->distanceToDestination = 0x7FFF;     /* The aim moves: do not go off because it got further away. */

	if (launch) {
		s_testLaunches++;
		bullet->orientation[0].current = orientation;
		bullet->orientation[0].target = orientation;
		bullet->orientation[0].speed = 0;
	} else {
		Unit_SetOrientation(bullet, orientation, false, 0);
	}
}

/* Missiles close to the aircraft they are after go off. */
static void
Airwing_Fuses(void)
{
	PoolFindStruct find;
	Unit *fuse[32];
	int n = 0;

	for (Unit *u = Unit_FindFirst(&find, HOUSE_INVALID, UNIT_INVALID); u != NULL && n < 32; u = Unit_FindNext(&find)) {
		const UnitInfo *ui = &g_table_unitInfo[u->o.type];

		if (!ui->flags.isBullet || u->o.type == UNIT_MISSILE_HOUSE || u->o.type == UNIT_BULLET || u->o.type == UNIT_SONIC_BLAST)
			continue;

		const Unit *target = Airwing_AirTarget(u);

		if (target != NULL && Tile_GetDistance(u->o.position, target->o.position) < FUSE_DISTANCE)
			fuse[n++] = u;
	}

	for (int i = 0; i < n; i++) {
		Unit *u = fuse[i];
		const Unit *target = Airwing_AirTarget(u);
		const UnitInfo *ui = &g_table_unitInfo[u->o.type];

		if (target == NULL || ui->explosionType == 0xFFFF)
			continue;

		if (g_debugTestAirDefence)
			Airwing_Log("fuse: missile type=%d went off by its target idx=%d", u->o.type, target->o.index);
		s_testFuses++;

		Map_MakeExplosion((ui->explosionType + u->o.hitpoints / 20) & 3, target->o.position, u->o.hitpoints, u->originEncoded);
		Unit_Remove(u);
	}
}

/* --test-air-defence: two of the player's ornithopters patrol over four enemy
 * troopers.  Logs the missiles, the near misses that went off and the damage, and
 * fails when the troopers hardly fire or their missiles hardly ever get close.
 */
static void
Airwing_AirDefenceTest(void)
{
	static int stage;
	static int64_t next;
	static tile32 post;
	const int64_t age = g_timerGame - g_tickScenarioStart;

	if (stage == 0) {
		PoolFindStruct find;
		const Unit *first = Unit_FindFirst(&find, g_playerHouseID, UNIT_ORNITHOPTER);

		if (first == NULL)
			return;

		/* Somewhere open, away from the player's base. */
		enum HouseType enemy = HOUSE_INVALID;

		for (const Structure *s = Structure_FindFirst(&find, HOUSE_INVALID, STRUCTURE_INVALID); s != NULL; s = Structure_FindNext(&find)) {
			if (!House_AreAllied(s->o.houseID, g_playerHouseID)) {
				enemy = s->o.houseID;
				break;
			}
		}

		if (enemy == HOUSE_INVALID)
			enemy = HOUSE_HARKONNEN;

		post = first->o.position;
		post.x = (uint16)((post.x / 256 < 32) ? post.x + 8 * 256 : post.x - 8 * 256);

		for (int i = 0; i < 4; i++) {
			tile32 t = post;

			t.x += 256 * (i % 2) * 2 - 256;
			t.y += 256 * (i / 2) * 2 - 256;

			Unit *trooper = Unit_Create(UNIT_INDEX_INVALID, UNIT_TROOPERS, enemy, Tile_Center(t), 0);

			if (trooper != NULL) {
				trooper->o.seenByHouses = 0xFF;
				Unit_Server_SetAction(trooper, ACTION_GUARD);
			}
			Airwing_Log("trooper %d for house %d: %s", i, (int)enemy, trooper ? "ok" : "failed");
		}

		for (Unit *u = Unit_FindFirst(&find, g_playerHouseID, UNIT_ORNITHOPTER); u != NULL; u = Unit_FindNext(&find)) {
			Unit_Server_SetAction(u, ACTION_AREA_GUARD);
			Airwing_SetPatrol(u, Tools_Index_Encode(Tile_PackTile(post), IT_TILE));
		}

		stage = 1;
		return;
	}

	if (g_timerGame < next)
		return;
	next = g_timerGame + 120;

	PoolFindStruct find;
	int ornis = 0, hp = 0, troopers = 0;

	for (const Unit *u = Unit_FindFirst(&find, g_playerHouseID, UNIT_ORNITHOPTER); u != NULL; u = Unit_FindNext(&find)) {
		ornis++;
		hp += u->o.hitpoints;
	}

	for (const Unit *u = Unit_FindFirst(&find, HOUSE_INVALID, UNIT_TROOPERS); u != NULL; u = Unit_FindNext(&find)) {
		if (Unit_GetHouseID(u) != g_playerHouseID)
			troopers++;
	}

	Airwing_Log("ornithopters=%d hp=%d enemy troopers=%d missiles=%d fuses=%d", ornis, hp, troopers, s_testLaunches, s_testFuses);

	/* Units in this fight count as engaged: they say "Under attack!" when selected. */
	for (const Unit *u = Unit_FindFirst(&find, g_playerHouseID, UNIT_ORNITHOPTER); u != NULL; u = Unit_FindNext(&find)) {
		if (Unit_IsEngaged(u))
			s_testEngaged = true;
	}

	if (age > 60 * 60 || ornis == 0 || troopers == 0) {
		/* Every radio voice has its lines for that, and can say one. */
		int busyVoices = 0;

		for (int v = 0; v < AudioA5_RadioVoiceCount(); v++) {
			if (AudioA5_PlayRadio(v, RADIO_BUSY, 0.0f))
				busyVoices++;
		}

		Airwing_Log("engaged seen: %s; radio voices with busy lines: %d of %d",
				s_testEngaged ? "yes" : "no", busyVoices, AudioA5_RadioVoiceCount());
		if (!s_testEngaged || busyVoices != AudioA5_RadioVoiceCount())
			exit(1);

		Airwing_Log("finished: %d missiles at the ornithopters, %d went off next to them", s_testLaunches, s_testFuses);
		exit((s_testLaunches >= 3 && s_testFuses >= 1) ? 0 : 1);   /* They fire, and their rockets get close (how many is down to chance). */
	}
}

void
Airwing_SetAttack(Unit *u, uint16_t encoded)
{
	if (!Airwing_IsPlayerOrnithopter(u))
		return;

	Patrol *p = &s_patrol[u->o.index];

	/* Told to attack bare ground: patrol there instead. */
	if (Tools_Index_GetType(encoded) == IT_TILE) {
		Unit_Server_SetAction(u, ACTION_AREA_GUARD);
		Airwing_SetPatrol(u, encoded);
		return;
	}

	p->attack = encoded;
	p->active = false;
	Unit_SetTarget(u, encoded);
}

bool
Airwing_ActsAtOnce(const Unit *u)
{
	return Airwing_IsPlayerOrnithopter(u);
}
