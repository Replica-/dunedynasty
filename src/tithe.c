/** @file src/tithe.c The Sardaukar tithe.
 *
 * Every five minutes a Sardaukar frigate comes in and lands on the refinery
 * nearest the middle of the map to collect 100 credits.  It waits for about two
 * minutes for the money to be there.  When it is, the frigate takes it and
 * leaves.  When it is not, the frigate leaves empty-handed, and after three of
 * those in a row the Sardaukar send reinforcements to wipe out the player:
 * three devastators and three sets of troopers, each brought in by carryall.
 *
 * The timer for the next pickup is shown in the top bar.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "tithe.h"

#include "daynight.h"

#include "gui/gui.h"
#include "house.h"
#include "map.h"
#include "net/net.h"
#include "object.h"
#include "opendune.h"
#include "pool/pool.h"
#include "pool/pool_house.h"
#include "pool/pool_structure.h"
#include "pool/pool_unit.h"
#include "scenario.h"
#include "structure.h"
#include "timer/timer.h"
#include "tools/coord.h"
#include "tools/encoded_index.h"
#include "tools/random_general.h"
#include "tools/random_lcg.h"
#include "unit.h"

bool g_tithe = true;
bool g_debugTestTithe = false;

#define TICKS_PER_SECOND 60

#define TITHE_CREDITS (Tithe_Credits())
#define TITHE_CREDITS_FIRST 50      /* The first mission. */
#define TITHE_CREDITS_MAX 300       /* The eighth: the last that has a tax. */

/* The tax grows with the campaign: 50 credits in the first mission, up to 300 in
 * the eighth, in steps of ten.  (The ninth has none: see Tithe_Enabled.)
 */
static int
Tithe_Credits(void)
{
	const int mission = (g_campaignID > 7) ? 7 : g_campaignID;     /* 0 to 7 */
	const int credits = TITHE_CREDITS_FIRST + (TITHE_CREDITS_MAX - TITHE_CREDITS_FIRST) * mission / 7;

	return (credits + 5) / 10 * 10;
}
#define TITHE_LAST_CAMPAIGN_ID 8   /* g_campaignID counts the missions won: 8 while playing the ninth. */
#define TITHE_MAX_MISSES 3
#define TITHE_HOUSE HOUSE_SARDAUKAR

/* A real tithe is every five minutes with two minutes of waiting.  The test is much quicker. */
/* One tithe a day, in the morning: the game starts in the morning, so the first is when one full day has gone.
 * With no day and night it is every five minutes.  The test is much quicker.
 */
#define INTERVAL_SECONDS (g_debugTestTithe ? 40 : (g_day_night ? DAYNIGHT_CYCLE_SECONDS : 300))
#define WAIT_SECONDS (g_debugTestTithe ? 15 : 120)
#define FIRST_SECONDS (g_debugTestTithe ? 10 : INTERVAL_SECONDS)

enum TitheState {
	TITHE_IDLE,         /* Waiting for the next pickup. */
	TITHE_INBOUND,      /* The frigate is on its way to the refinery. */
	TITHE_LANDED,       /* It is down, waiting for the money. */
	TITHE_LEAVING       /* It has what it came for, or has given up. */
};

#define NUM_PUNISHMENT 6

static enum TitheState s_state = TITHE_IDLE;
static int64_t s_scenarioStart = -1;
static int64_t s_nextArrival = 0;       /* Game time of the next pickup. */
static int64_t s_nextTry = 0;           /* When to try again to launch the frigate. */
static int64_t s_leaveAt = 0;           /* Game time when the frigate gives up waiting. */
static int64_t s_nextStep = 0;
static int64_t s_nextPending = 0;
static int64_t s_testStart = 0;
static int s_misses = 0;
static uint16 s_frigateIndex = UNIT_INDEX_INVALID;
static uint16 s_refineryIndex = STRUCTURE_INDEX_INVALID;
static enum UnitType s_pending[NUM_PUNISHMENT];
static int s_numPending = 0;
static int s_punishments = 0;
static int s_visits = 0;
static enum StructureType s_landType = STRUCTURE_REFINERY;   /* The test may land on something else when there is no refinery. */

enum TankPhase {
	TANK_NONE,         /* No tank this visit. */
	TANK_OUT,          /* Driving to the refinery. */
	TANK_WAIT,         /* At the refinery, waiting for the money. */
	TANK_BACK          /* Driving back to the frigate to get on. */
};

static tile32 s_landTarget;             /* Where the frigate sets down. */
static uint16 s_parkPacked = 0;         /* The tile under it, or 0 when it lands on the refinery itself. */
static uint16 s_pickupPacked = 0;       /* Where the tank is paid. */
static uint16 s_tankIndex = UNIT_INDEX_INVALID;
static enum TankPhase s_tankPhase = TANK_NONE;
static bool s_paid = false;

static void
Tithe_Log(const char *fmt, ...)
{
	if (!g_debugTestTithe)
		return;

	FILE *log = fopen("tithe_test.log", "a");
	if (log == NULL)
		return;

	va_list ap;

	fprintf(log, "t=%ds ", (int)((g_timerGame - s_testStart) / TICKS_PER_SECOND));
	va_start(ap, fmt);
	vfprintf(log, fmt, ap);
	va_end(ap);
	fputc('\n', log);
	fclose(log);
}

static bool
Tithe_Enabled(void)
{
	if (!g_tithe && !g_debugTestTithe)
		return false;

	/* The money and the frigate are the server's to take and make: single player only. */
	if (g_host_type != HOSTTYPE_NONE)
		return false;

	if (g_playerHouseID == TITHE_HOUSE || g_playerHouseID >= HOUSE_NEUTRAL)
		return false;

	if (House_IsHuman(TITHE_HOUSE) || House_AreAllied(TITHE_HOUSE, g_playerHouseID))
		return false;

	/* In the last mission the Emperor is at war with the player: no tax. */
	if (g_campaign_selected == CAMPAIGNID_DUNE_II && g_campaignID == TITHE_LAST_CAMPAIGN_ID)
		return false;

	return true;
}

static Unit *
Tithe_GetFrigate(void)
{
	if (s_frigateIndex == UNIT_INDEX_INVALID)
		return NULL;

	Unit *u = Unit_Get_ByIndex(s_frigateIndex);

	if (!u->o.flags.s.used || u->o.type != UNIT_FRIGATE || u->o.houseID != TITHE_HOUSE)
		return NULL;

	return u;
}

bool
Tithe_ControlsUnit(const Unit *u)
{
	return s_state != TITHE_IDLE && s_frigateIndex != UNIT_INDEX_INVALID
		&& u->o.index == s_frigateIndex
		&& u->o.type == UNIT_FRIGATE && u->o.houseID == TITHE_HOUSE;
}

/*--------------------------------------------------------------*/

static tile32
Tithe_StructureCentre(const Structure *s)
{
	const XYSize *size = &g_table_structure_layoutSize[g_table_structureInfo[s->o.type].layout];
	tile32 t = s->o.position;

	t.x += size->width * 128;
	t.y += size->height * 128;
	return t;
}

/* The player's refinery nearest the middle of the map, or NULL. */
static Structure *
Tithe_FindRefinery(void)
{
	const MapInfo *mapInfo = &g_mapInfos[g_scenario.mapScale];
	const int cx = mapInfo->minX + mapInfo->sizeX / 2;
	const int cy = mapInfo->minY + mapInfo->sizeY / 2;
	PoolFindStruct find;
	Structure *best = NULL;
	int bestScore = 0;

	for (Structure *s = Structure_FindFirst(&find, g_playerHouseID, s_landType);
			s != NULL;
			s = Structure_FindNext(&find)) {
		if (s->o.flags.s.isNotOnMap)
			continue;

		const uint16 packed = Tile_PackTile(s->o.position);
		const int dx = (int)Tile_GetPackedX(packed) - cx;
		const int dy = (int)Tile_GetPackedY(packed) - cy;
		const int score = dx * dx + dy * dy;

		if (best == NULL || score < bestScore) {
			best = s;
			bestScore = score;
		}
	}

	return best;
}

static House *
Tithe_GetHouse(void)
{
	House *h = House_Get_ByIndex(TITHE_HOUSE);

	/* The Sardaukar are not in every scenario: they join the game when they first come. */
	if (!h->flags.used) {
		h = House_Allocate(TITHE_HOUSE);

		if (h != NULL)
			h->unitCountMax = UNIT_MAX_PER_HOUSE_RAISED;
	}

	return h;
}

/*--------------------------------------------------------------*/

/*--------------------------------------------------------------*/

/* Ground a frigate can sit on and a tank can drive over, with nothing on it. */
static bool
Tithe_IsOpenGround(int x, int y)
{
	const MapInfo *mapInfo = &g_mapInfos[g_scenario.mapScale];

	if (x < mapInfo->minX + 1 || y < mapInfo->minY + 1
			|| x >= mapInfo->minX + mapInfo->sizeX - 1 || y >= mapInfo->minY + mapInfo->sizeY - 1)
		return false;

	const uint16 packed = Tile_PackXY(x, y);

	if (Object_GetByPackedTile(packed) != NULL)
		return false;

	switch (Map_GetLandscapeType(packed)) {
		case LST_NORMAL_SAND:
		case LST_PARTIAL_ROCK:
		case LST_ENTIRELY_DUNE:
		case LST_PARTIAL_DUNE:
		case LST_ENTIRELY_ROCK:
		case LST_MOSTLY_ROCK:
		case LST_SPICE:
		case LST_THICK_SPICE:
		case LST_CONCRETE_SLAB:
			return true;

		default:
			return false;
	}
}

/* Where the frigate sets down and where the tank goes to be paid, for the refinery it is
 * coming for.  The frigate parks as near the refinery as it can on an empty block of ground
 * (the ground it sits on and all round it free, so the tank can get out), and the tank
 * drives to the free tile touching the refinery that is nearest the frigate.
 */
static void
Tithe_PlanLanding(const Structure *refinery)
{
	const XYSize *size = &g_table_structure_layoutSize[g_table_structureInfo[refinery->o.type].layout];
	const uint16 corner = Tile_PackTile(refinery->o.position);
	const int rx0 = Tile_GetPackedX(corner), ry0 = Tile_GetPackedY(corner);
	const int rx1 = rx0 + size->width - 1, ry1 = ry0 + size->height - 1;
	int bestScore = 0;

	s_landTarget = Tithe_StructureCentre(refinery);
	s_parkPacked = 0;
	s_pickupPacked = 0;

	for (int y = ry0 - 12; y <= ry1 + 12; y++) {
		for (int x = rx0 - 12; x <= rx1 + 12; x++) {
			const int gapX = (x < rx0) ? rx0 - x : (x > rx1) ? x - rx1 : 0;
			const int gapY = (y < ry0) ? ry0 - y : (y > ry1) ? y - ry1 : 0;

			/* One free tile between the frigate's block and the refinery, at least. */
			if (gapX < 3 && gapY < 3)
				continue;

			bool block = true;

			for (int k = 0; k < 9 && block; k++)
				block = Tithe_IsOpenGround(x + k % 3 - 1, y + k / 3 - 1);

			if (!block)
				continue;

			const int score = (gapX > gapY ? gapX : gapY) * 100 + gapX + gapY;

			if (s_parkPacked == 0 || score < bestScore) {
				s_parkPacked = Tile_PackXY(x, y);
				bestScore = score;
			}
		}
	}

	if (s_parkPacked == 0)
		return;

	s_landTarget = Tile_Center(Tile_UnpackTile(s_parkPacked));

	const int px = Tile_GetPackedX(s_parkPacked), py = Tile_GetPackedY(s_parkPacked);
	int bestDistance = 0;

	for (int y = ry0 - 1; y <= ry1 + 1; y++) {
		for (int x = rx0 - 1; x <= rx1 + 1; x++) {
			if ((x >= rx0 && x <= rx1 && y >= ry0 && y <= ry1) || !Tithe_IsOpenGround(x, y))
				continue;

			const int distance = (x - px) * (x - px) + (y - py) * (y - py);

			if (s_pickupPacked == 0 || distance < bestDistance) {
				s_pickupPacked = Tile_PackXY(x, y);
				bestDistance = distance;
			}
		}
	}

	Tithe_Log("park tile %d,%d, pickup tile %d,%d", px, py,
			s_pickupPacked ? (int)Tile_GetPackedX(s_pickupPacked) : -1, s_pickupPacked ? (int)Tile_GetPackedY(s_pickupPacked) : -1);
}

static Unit *
Tithe_GetTank(void)
{
	if (s_tankIndex == UNIT_INDEX_INVALID)
		return NULL;

	Unit *u = Unit_Get_ByIndex(s_tankIndex);

	if (!u->o.flags.s.used || u->o.type != UNIT_TANK || u->o.houseID != TITHE_HOUSE)
		return NULL;

	return u;
}

/* A tank with no gun, flown in and driven by the Sardaukar alone. */
static void
Tithe_OrderTank(Unit *tank, uint16 packed)
{
	Unit_Server_SetAction(tank, ACTION_MOVE);
	tank->targetMove = Tools_Index_Encode(packed, IT_TILE);
	tank->targetAttack = 0;
	tank->fireDelay = 0xFFFF;   /* Never ready to fire. */
}

/* Sets the tank down next to the frigate and sends it to the refinery. */
static void
Tithe_UnloadTank(void)
{
	s_tankIndex = UNIT_INDEX_INVALID;
	s_tankPhase = TANK_NONE;

	if (s_pickupPacked == 0)
		return;

	const int px = Tile_GetPackedX(s_parkPacked), py = Tile_GetPackedY(s_parkPacked);
	const int tx = Tile_GetPackedX(s_pickupPacked), ty = Tile_GetPackedY(s_pickupPacked);
	int bestDistance = 0;
	int bx = -1, by = -1;

	for (int k = 0; k < 9; k++) {
		const int x = px + k % 3 - 1, y = py + k / 3 - 1;

		if (k == 4 || !Tithe_IsOpenGround(x, y))
			continue;

		const int distance = (x - tx) * (x - tx) + (y - ty) * (y - ty);

		if (bx < 0 || distance < bestDistance) {
			bx = x; by = y;
			bestDistance = distance;
		}
	}

	if (bx < 0)
		return;

	const tile32 spot = Tile_Center(Tile_UnpackTile(Tile_PackXY(bx, by)));
	Unit *tank = Unit_Create(UNIT_INDEX_INVALID, UNIT_TANK, TITHE_HOUSE, spot, Tile_GetDirection(spot, Tile_Center(Tile_UnpackTile(s_pickupPacked))));

	if (tank == NULL)
		return;

	tank->o.seenByHouses = 0xFF;
	tank->aiSquad = SQUADID_INVALID;
	Tithe_OrderTank(tank, s_pickupPacked);

	s_tankIndex = tank->o.index;
	s_tankPhase = TANK_OUT;
	Tithe_Log("tank out");
}

static void
Tithe_RemoveTank(void)
{
	Unit *tank = Tithe_GetTank();

	if (tank != NULL)
		Unit_Remove(tank);

	s_tankIndex = UNIT_INDEX_INVALID;
	s_tankPhase = TANK_NONE;
}

/* A free spot near the player's base for the reinforcements, or 0. */
static uint16
Tithe_FindDropTile(void)
{
	const MapInfo *mapInfo = &g_mapInfos[g_scenario.mapScale];
	PoolFindStruct find;
	const Structure *anchor = Structure_FindFirst(&find, g_playerHouseID, STRUCTURE_CONSTRUCTION_YARD);

	if (anchor == NULL) {
		for (const Structure *s = Structure_FindFirst(&find, g_playerHouseID, STRUCTURE_INVALID);
				s != NULL;
				s = Structure_FindNext(&find)) {
			if (!Structure_SharesPoolElement(s->o.type) && !s->o.flags.s.isNotOnMap) {
				anchor = s;
				break;
			}
		}
	}

	if (anchor == NULL)
		return 0;

	const uint16 anchorPacked = Tile_PackTile(anchor->o.position);
	const int ax = Tile_GetPackedX(anchorPacked);
	const int ay = Tile_GetPackedY(anchorPacked);

	for (int attempt = 0; attempt < 60; attempt++) {
		const int radius = 3 + attempt / 10;
		const int x = ax + (int)Tools_RandomLCG_Range(0, 2 * radius) - radius;
		const int y = ay + (int)Tools_RandomLCG_Range(0, 2 * radius) - radius;

		if (x < mapInfo->minX + 1 || y < mapInfo->minY + 1
				|| x >= mapInfo->minX + mapInfo->sizeX - 1 || y >= mapInfo->minY + mapInfo->sizeY - 1)
			continue;

		const uint16 packed = Tile_PackXY(x, y);

		if (Object_GetByPackedTile(packed) != NULL)
			continue;

		switch (Map_GetLandscapeType(packed)) {
			case LST_NORMAL_SAND:
			case LST_PARTIAL_ROCK:
			case LST_ENTIRELY_DUNE:
			case LST_PARTIAL_DUNE:
			case LST_ENTIRELY_ROCK:
			case LST_MOSTLY_ROCK:
			case LST_SPICE:
			case LST_THICK_SPICE:
			case LST_CONCRETE_SLAB:
				return packed;

			default:
				break;
		}
	}

	return 0;
}

/* Three devastators and three sets of troopers, each in a carryall. */
static void
Tithe_Punish(void)
{
	s_punishments++;
	Tithe_GetHouse();

	for (int i = 0; i < 3; i++) {
		s_pending[i] = UNIT_DEVASTATOR;
		s_pending[3 + i] = UNIT_TROOPERS;
	}
	s_numPending = NUM_PUNISHMENT;
	s_nextPending = 0;

	GUI_DisplayText("The Sardaukar are sending reinforcements!", 3);
	Tithe_Log("punishment: %d units queued", s_numPending);
}

/* One delivery a second, as the carryalls and the drop spots are not always there. */
static void
Tithe_SendPending(void)
{
	if (s_numPending == 0 || g_timerGame < s_nextPending)
		return;

	s_nextPending = g_timerGame + TICKS_PER_SECOND;

	const uint16 packed = Tithe_FindDropTile();
	if (packed == 0)
		return;

	Unit *u = Unit_CreateWrapper(TITHE_HOUSE, s_pending[s_numPending - 1],
			Tools_Index_Encode(packed, IT_TILE));

	if (u == NULL)
		return;

	/* They are after the player, wherever they land. */
	u->o.seenByHouses = 0xFF;

	Tithe_Log("sent unit type %d to tile %d (%d left)", (int)u->o.type, (int)packed, s_numPending - 1);
	s_numPending--;
}

/*--------------------------------------------------------------*/

static void
Tithe_Finish(void)
{
	/* A tank that is left behind is of no use to anyone. */
	Tithe_RemoveTank();

	s_state = TITHE_IDLE;
	s_frigateIndex = UNIT_INDEX_INVALID;
	s_refineryIndex = STRUCTURE_INDEX_INVALID;
}

static void
Tithe_Reset(void)
{
	Unit *u = Tithe_GetFrigate();

	/* The frigate belongs to the old game. */
	if (u != NULL)
		Unit_Remove(u);

	Tithe_Finish();
	s_scenarioStart = g_tickScenarioStart;
	s_nextArrival = (g_debugTestTithe ? g_timerGame : g_tickScenarioStart) + (int64_t)FIRST_SECONDS * TICKS_PER_SECOND;
	s_nextTry = 0;
	s_nextStep = 0;
	s_misses = 0;
	s_numPending = 0;
	s_punishments = 0;
	s_testStart = g_timerGame;
}

static bool
Tithe_Launch(Structure *refinery)
{
	const uint16 edge = Map_Server_FindLocationTile(Tools_Random_256() & 3, TITHE_HOUSE);
	Tithe_PlanLanding(refinery);
	const tile32 target = s_landTarget;

	if (edge == 0 || Tithe_GetHouse() == NULL)
		return false;

	const tile32 from = Tile_UnpackTile(edge);
	/* The frigate is a flyer, and flyers use the same slots as the carryalls.  The
	 * slot the starport frigate has is only for that one, so take one of the others.
	 */
	uint16 index = 0;

	for (uint16 i = 10; i >= 1; i--) {
		if (!Unit_Get_ByIndex(i)->o.flags.s.used) {
			index = i;
			break;
		}
	}

	if (index == 0)
		return false;

	Unit *u = Unit_Create(index, UNIT_FRIGATE, TITHE_HOUSE, from, Tile_GetDirection(from, target));
	if (u == NULL)
		return false;

	u->o.flags.s.byScenario = true;
	u->o.seenByHouses = 0xFF;
	u->currentDestination = target;
	Unit_SetSpeed(u, 255);

	s_frigateIndex = u->o.index;
	s_refineryIndex = refinery->o.index;
	return true;
}

static void
Tithe_Depart(Unit *u)
{
	const MapInfo *mapInfo = &g_mapInfos[g_scenario.mapScale];
	tile32 centre;

	/* Away from the middle, the shortest way off the map. */
	centre.x = (mapInfo->minX + mapInfo->sizeX / 2) * 256;
	centre.y = (mapInfo->minY + mapInfo->sizeY / 2) * 256;

	const uint8 away = Tile_GetDirection(centre, u->o.position);

	s_state = TITHE_LEAVING;
	u->targetMove = 0;
	u->currentDestination = Tile_MoveByDirectionUnbounded(u->o.position, away, 64 * 256);
	Unit_SetOrientation(u, away, false, 0);
	Unit_SetSpeed(u, 255);
}

static bool
Tithe_NearEdge(const Unit *u)
{
	const MapInfo *mapInfo = &g_mapInfos[g_scenario.mapScale];
	const uint16 packed = Tile_PackTile(u->o.position);
	const int x = Tile_GetPackedX(packed), y = Tile_GetPackedY(packed);

	return x <= mapInfo->minX + 1 || y <= mapInfo->minY + 1
		|| x >= mapInfo->minX + mapInfo->sizeX - 2 || y >= mapInfo->minY + mapInfo->sizeY - 2;
}

static void
Tithe_Step(void)
{
	Unit *u = Tithe_GetFrigate();

	if (u == NULL) {
		/* Shot down on the way in or while waiting: nothing was collected, and nothing is held against the player.
		 * One that was leaving has simply flown off the map.
		 */
		if (s_state != TITHE_LEAVING)
			GUI_DisplayText("The Sardaukar frigate was destroyed.", 2);
		Tithe_Log(s_state == TITHE_LEAVING ? "frigate gone" : "frigate lost");
		Tithe_Finish();
		return;
	}

	House *player = House_Get_ByIndex(g_playerHouseID);

	switch (s_state) {
		case TITHE_INBOUND: {
			Structure *s = (s_refineryIndex == STRUCTURE_INDEX_INVALID) ? NULL : Structure_Get_ByIndex(s_refineryIndex);

			if (s == NULL || !s->o.flags.s.used || s->o.type != s_landType || s->o.houseID != g_playerHouseID) {
				/* The refinery is gone, so find another, or go home. */
				s = Tithe_FindRefinery();

				if (s == NULL) {
					Tithe_Log("no refinery left, leaving");
					Tithe_Depart(u);
					return;
				}

				s_refineryIndex = s->o.index;
				Tithe_PlanLanding(s);
			}

			const tile32 target = s_landTarget;

			u->currentDestination = target;
			/* A straight line to the refinery, slowing down for the landing. */
			Unit_SetOrientation(u, Tile_GetDirection(u->o.position, target), true, 0);
			if (u->movingSpeed != ((Tile_GetDistance(u->o.position, target) < 1024) ? 100 : 255))
				Unit_SetSpeed(u, (Tile_GetDistance(u->o.position, target) < 1024) ? 100 : 255);

			if (g_debugTestTithe && g_timerGame % 300 == 0)
				Tithe_Log("inbound pos=%d,%d dest=%d,%d dist=%d rem=%d ms=%d speed=%d spt=%d orient=%d/%d", (int)u->o.position.x, (int)u->o.position.y, (int)target.x, (int)target.y, (int)Tile_GetDistance(u->o.position, target), (int)u->speedRemainder, (int)u->movingSpeed, (int)u->speed, (int)u->speedPerTick, (int)u->orientation[0].current, (int)u->orientation[0].target);

			if (Tile_GetDistance(u->o.position, target) < 64) {
				s_state = TITHE_LANDED;
				s_leaveAt = g_timerGame + (int64_t)WAIT_SECONDS * TICKS_PER_SECOND;
				u->o.position = target;
				u->currentDestination.x = 0;
				u->currentDestination.y = 0;
				Unit_SetSpeed(u, 0);
				s_paid = false;

				Tithe_UnloadTank();

				GUI_DisplayText("A Sardaukar frigate has landed. It wants %d credits.", 3, TITHE_CREDITS);
				Tithe_Log("landed, credits=%d", (int)player->credits);
			}
			break;
		}

		case TITHE_LANDED: {
			Unit *tank = Tithe_GetTank();

			if (g_debugTestTithe && s_visits > 1 && s_punishments == 0)
				player->credits = 0;

			/* With no tank, the frigate takes the money from where it sits. */
			if (s_tankPhase == TANK_NONE && s_tankIndex == UNIT_INDEX_INVALID && !s_paid && s_pickupPacked == 0) {
				if (player->credits >= TITHE_CREDITS) {
					player->credits -= TITHE_CREDITS;
					s_misses = 0;
					s_paid = true;
					GUI_DisplayText("The Sardaukar have taken their %d credits.", 2, TITHE_CREDITS);
					Tithe_Log("paid, credits=%d", (int)player->credits);
					Tithe_Depart(u);
				} else if (g_timerGame >= s_leaveAt) {
					s_misses++;
					if (s_misses >= TITHE_MAX_MISSES) {
						s_misses = 0;
						Tithe_Punish();
					} else {
						GUI_DisplayText("No spice tax! The Sardaukar are angry (%d of %d).", 3, s_misses, TITHE_MAX_MISSES);
					}
					Tithe_Depart(u);
				}
				break;
			}

			if (s_tankPhase != TANK_NONE && tank == NULL) {
				/* Destroyed on its errand: the frigate goes without it, and nothing is held against the player. */
				GUI_DisplayText("The Sardaukar tax collector was destroyed.", 2);
				Tithe_Log("tank lost");
				s_tankPhase = TANK_NONE;
				s_tankIndex = UNIT_INDEX_INVALID;
				Tithe_Depart(u);
				break;
			}

			if (s_tankPhase == TANK_NONE) {
				/* The tank could not be put down: give up on this visit. */
				Tithe_Log("no tank, leaving");
				Tithe_Depart(u);
				break;
			}

			/* The tank has no gun, and the AI must not give it one's worth of ideas. */
			tank->fireDelay = 0xFFFF;
			tank->targetAttack = 0;

			const tile32 pickup = Tile_Center(Tile_UnpackTile(s_pickupPacked));
			const tile32 park = Tile_Center(Tile_UnpackTile(s_parkPacked));

			switch (s_tankPhase) {
				case TANK_OUT:
					if (Tile_GetDistance(tank->o.position, pickup) < 200) {
						s_tankPhase = TANK_WAIT;
						Tithe_Log("tank at the refinery, credits=%d", (int)player->credits);
					} else if (tank->targetMove == 0) {
						Tithe_OrderTank(tank, s_pickupPacked);   /* Stopped short: try again. */
					}
					break;

				case TANK_WAIT:
					if (player->credits >= TITHE_CREDITS) {
						player->credits -= TITHE_CREDITS;
						s_misses = 0;
						s_paid = true;

						GUI_DisplayText("The Sardaukar have taken their %d credits.", 2, TITHE_CREDITS);
						Tithe_Log("paid, credits=%d", (int)player->credits);
						s_tankPhase = TANK_BACK;
						Tithe_OrderTank(tank, s_parkPacked);
					} else if (g_timerGame >= s_leaveAt) {
						s_misses++;

						if (s_misses >= TITHE_MAX_MISSES) {
							s_misses = 0;
							Tithe_Log("missed %d in a row", TITHE_MAX_MISSES);
							Tithe_Punish();
						} else {
							GUI_DisplayText("No spice tax! The Sardaukar are angry (%d of %d).", 3, s_misses, TITHE_MAX_MISSES);
							Tithe_Log("missed %d of %d", s_misses, TITHE_MAX_MISSES);
						}

						s_tankPhase = TANK_BACK;
						Tithe_OrderTank(tank, s_parkPacked);
					}
					break;

				case TANK_BACK:
					if (Tile_GetDistance(tank->o.position, park) < 300) {
						/* On board. */
						Tithe_Log("tank aboard");
						Tithe_RemoveTank();
						Tithe_Depart(u);
					} else if (tank->targetMove == 0) {
						Tithe_OrderTank(tank, s_parkPacked);
					}
					break;

				case TANK_NONE:
					break;
			}

			/* A tank that cannot get back is left, so the frigate does not wait for ever. */
			if (g_timerGame > s_leaveAt + 45 * TICKS_PER_SECOND && s_state == TITHE_LANDED) {
				Tithe_Log("tank stuck, leaving without it");
				Tithe_RemoveTank();
				Tithe_Depart(u);
			}
			break;
		}

		case TITHE_LEAVING:
			if (Tithe_NearEdge(u) || !Tile_IsValid(u->o.position)) {
				Tithe_Log("frigate gone");
				Unit_Remove(u);
				Tithe_Finish();
			}
			break;

		case TITHE_IDLE:
			break;
	}
}

void
Tithe_Tick(void)
{
	if (g_debugTestTithe && s_testStart != 0 && g_timerGame - s_testStart > 60 * 230) {
		Tithe_Log("test finished");
		exit(0);
	}

	if (g_debugTestTithe && s_punishments > 0 && g_timerGame % 600 == 0) {
		PoolFindStruct find;

		for (const Unit *u = Unit_FindFirst(&find, TITHE_HOUSE, UNIT_INVALID);
				u != NULL;
				u = Unit_FindNext(&find)) {
			if (u->o.type == UNIT_FRIGATE)
				continue;

			Tithe_Log("sardaukar type=%d action=%d pos=%d,%d notOnMap=%d hp=%d", (int)u->o.type, (int)u->actionID,
					(int)Tile_GetPackedX(Tile_PackTile(u->o.position)), (int)Tile_GetPackedY(Tile_PackTile(u->o.position)),
					(int)u->o.flags.s.isNotOnMap, (int)u->o.hitpoints);
		}
	}

	if (!Tithe_Enabled()) {
		if (g_debugTestTithe && g_timerGame % 600 == 0)
			Tithe_Log("disabled: host=%d player=%d human=%d allied=%d", (int)g_host_type, (int)g_playerHouseID,
					(int)House_IsHuman(TITHE_HOUSE), (int)House_AreAllied(TITHE_HOUSE, g_playerHouseID));
		return;
	}

	if (s_scenarioStart != g_tickScenarioStart)
		Tithe_Reset();

	Tithe_SendPending();

	if (s_state == TITHE_IDLE) {
		if (g_timerGame < s_nextArrival || g_timerGame < s_nextTry)
			return;

		Structure *refinery = Tithe_FindRefinery();

		if (refinery == NULL && g_debugTestTithe) {
			s_landType = STRUCTURE_CONSTRUCTION_YARD;
			refinery = Tithe_FindRefinery();
		}

		if (refinery == NULL) {
			/* Nothing to land on. Try the next time. */
			Tithe_Log("no refinery");
			s_nextArrival += (int64_t)INTERVAL_SECONDS * TICKS_PER_SECOND;
			return;
		}

		if (!Tithe_Launch(refinery)) {
			Tithe_Log("launch failed");
			s_nextTry = g_timerGame + 5 * TICKS_PER_SECOND;
			return;
		}

		s_visits++;
		s_state = TITHE_INBOUND;
		s_nextArrival += (int64_t)INTERVAL_SECONDS * TICKS_PER_SECOND;
		s_nextStep = 0;

		GUI_DisplayText("A Sardaukar frigate is coming for the spice tax.", 2);
		Tithe_Log("launched, credits=%d", (int)House_Get_ByIndex(g_playerHouseID)->credits);
		return;
	}

	if (g_timerGame < s_nextStep)
		return;

	/* Landing needs a close watch, as the frigate moves several steps in 15 ticks. */
	s_nextStep = g_timerGame + (s_state == TITHE_INBOUND ? 1 : 15);
	Tithe_Step();
}

bool
Tithe_GetStatusText(char *buf, size_t size, bool *alert)
{
	*alert = false;

	if (!Tithe_Enabled() || s_scenarioStart != g_tickScenarioStart)
		return false;

	const int missNote = s_misses;

	switch (s_state) {
		case TITHE_INBOUND:
			snprintf(buf, size, "Spice Tax: %d due", TITHE_CREDITS);
			*alert = true;
			return true;

		case TITHE_LANDED: {
			const int left = (int)((s_leaveAt - g_timerGame) / TICKS_PER_SECOND);

			snprintf(buf, size, "Spice Tax: %d, %d:%02d", TITHE_CREDITS, (left > 0 ? left : 0) / 60, (left > 0 ? left : 0) % 60);
			*alert = true;
			return true;
		}

		case TITHE_LEAVING:
		case TITHE_IDLE: {
			int left = (int)((s_nextArrival - g_timerGame) / TICKS_PER_SECOND);

			if (left < 0)
				left = 0;

			if (missNote > 0) {
				snprintf(buf, size, "Spice Tax: %d, %d:%02d x%d", TITHE_CREDITS, left / 60, left % 60, missNote);
				*alert = true;
			} else {
				snprintf(buf, size, "Spice Tax: %d, %d:%02d", TITHE_CREDITS, left / 60, left % 60);
			}
			return true;
		}
	}

	return false;
}

bool
Tithe_IsCollectorTank(const Unit *u)
{
	return s_tankIndex != UNIT_INDEX_INVALID && u->o.index == s_tankIndex
		&& u->o.type == UNIT_TANK && u->o.houseID == TITHE_HOUSE;
}
