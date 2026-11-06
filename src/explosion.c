/** @file src/explosion.c Explosion routines. */

#include <assert.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "os/math.h"

#include "explosion.h"

#include "animation.h"
#include "binheap.h"
#include "enhancement.h"
#include "house.h"
#include "map.h"
#include "net/server.h"
#include "opendune.h"
#include "pool/pool.h"
#include "pool/pool_structure.h"
#include "pool/pool_unit.h"
#include "sandstorm.h"
#include "scenario.h"
#include "sprites.h"
#include "structure.h"
#include "tile.h"
#include "timer/timer.h"
#include "unit.h"
#include "video/prim.h"
#include "video/video_a5.h"
#include "gui/widget.h"
#include "tools/coord.h"
#include "tools/encoded_index.h"
#include "tools/random_general.h"
#include "tools/random_lcg.h"

static BinHeap s_explosions;

extern const ExplosionCommandStruct * const g_table_explosion[EXPLOSIONTYPE_MAX];

static void
Explosion_Update(const Explosion *e)
{
	Map_UpdateAround(24, e->position, NULL, 0);
}

/**
 * Handle damage to a tile, removing spice, removing concrete, stuff like that.
 * @param e The Explosion to handle damage on.
 * @param parameter Unused parameter.
 */
static void Explosion_Func_TileDamage(Explosion *e, uint16 parameter)
{
	static const int16 craterIconMapIndex[] = { -1, 2, 1 };

	uint16 packed = Tile_PackTile(e->position);
	uint16 type;
	Tile *t;
	int16 iconMapIndex;
	uint16 overlaySpriteID;
	uint16 *iconMap;
	VARIABLE_NOT_USED(parameter);

	type = Map_GetLandscapeType(packed);

	if (type == LST_STRUCTURE || type == LST_DESTROYED_WALL) return;

	t = &g_map[packed];

	if (type == LST_CONCRETE_SLAB) {
		t->groundSpriteID = g_mapSpriteID[packed];
	}

	if (g_table_landscapeInfo[type].craterType == 0) return;

	overlaySpriteID = t->overlaySpriteID;
	iconMapIndex = craterIconMapIndex[g_table_landscapeInfo[type].craterType];
	iconMap = &g_iconMap[g_iconMap[iconMapIndex]];

	if (iconMap[0] <= overlaySpriteID && overlaySpriteID <= iconMap[10]) {
		/* There already is a crater; make it bigger */
		overlaySpriteID -= iconMap[0];
		if (overlaySpriteID < 4) overlaySpriteID += 2;
	} else {
		/* Randomly pick 1 of the 2 possible craters */
		overlaySpriteID = Tools_Random_256() & 1;
	}

	/* Reduce spice if there is any */
	Map_ChangeSpiceAmount(packed, -1);

	/* Boom a bloom if there is one */
	if (t->groundSpriteID == g_bloomSpriteID) {
		Map_Bloom_ExplodeSpice(packed, FLAG_HOUSE_ALL);
		return;
	}

	/* Update the tile with the crater */
	t->overlaySpriteID = overlaySpriteID + iconMap[0];
}

/**
 * Play a voice for a Explosion.
 * @param e The Explosion to play the voice on.
 * @param voiceID The voice to play.
 */
static void Explosion_Func_PlayVoice(Explosion *e, uint16 voiceID)
{
	Server_Send_PlaySoundAtTile(FLAG_HOUSE_ALL, voiceID, e->position);
}

/**
 * Shake the screen.
 * @param e The Explosion.
 * @param parameter Unused parameter.
 */
static void Explosion_Func_ScreenShake(Explosion *e, uint16 parameter)
{
	VARIABLE_NOT_USED(parameter);

	Server_Send_ScreenShake(Tile_PackTile(e->position));
}

/**
 * Check if there is a bloom at the location, and make it explode if needed.
 * @param e The Explosion to perform the explosion on.
 * @param parameter Unused parameter.
 */
static void Explosion_Func_BloomExplosion(Explosion *e, uint16 parameter)
{
	uint16 packed = Tile_PackTile(e->position);
	VARIABLE_NOT_USED(parameter);

	if (g_map[packed].groundSpriteID != g_bloomSpriteID) return;

	Map_Bloom_ExplodeSpice(packed, FLAG_HOUSE_ALL);
}

/**
 * Set the animation of a Explosion.
 * @param e The Explosion to change.
 * @param animationMapID The animation map to use.
 */
static void Explosion_Func_SetAnimation(Explosion *e, uint16 animationMapID)
{
	uint16 packed = Tile_PackTile(e->position);

	if (Structure_Get_ByPackedTile(packed) != NULL) return;

	animationMapID += Tools_Random_256() & 0x1;
	animationMapID += g_table_landscapeInfo[Map_GetLandscapeType(packed)].isSand ? 0 : 2;

	assert(animationMapID < 16);
	Animation_Start(g_table_animation_map[animationMapID], e->position, 0, HOUSE_HARKONNEN, 3);
}

/**
 * Set position at the left of a row.
 * @param e The Explosion to change.
 * @param row Row number.
 */
static void Explosion_Func_MoveYPosition(Explosion *e, uint16 row)
{
	e->position.y += (int16)row;
}

/**
 * Stop performing an explosion.
 * @param e The Explosion to end.
 * @param parameter Unused parameter.
 */
static void Explosion_Func_Stop(Explosion *e, uint16 parameter)
{
	uint16 packed = Tile_PackTile(e->position);
	VARIABLE_NOT_USED(parameter);

	g_map[packed].hasExplosion = false;

	Explosion_Update(e);

	e->commands = NULL;
}

/**
 * Set timeout for next the activity of \a e.
 * @param e The Explosion to change.
 * @param value The new timeout value.
 */
static void Explosion_Func_SetTimeout(Explosion *e, uint16 value)
{
	e->timeOut = Timer_GetTicks() + value;
}

/**
 * Set timeout for next the activity of \a e to a random value up to \a value.
 * @param e The Explosion to change.
 * @param value The maximum amount of timeout.
 */
static void Explosion_Func_SetRandomTimeout(Explosion *e, uint16 value)
{
	e->timeOut = Timer_GetTicks() + Tools_RandomLCG_Range(0, value);
}

/**
 * Set the SpriteID of the Explosion.
 * @param e The Explosion to change.
 * @param spriteID The new SpriteID for the Explosion.
 */
static void Explosion_Func_SetSpriteID(Explosion *e, uint16 spriteID)
{
	e->spriteID = spriteID;

	Explosion_Update(e);
}

/**
 * Stop any Explosion at position \a packed.
 * @param packed A packed position where no activities should take place (any more).
 */
static void Explosion_StopAtPosition(uint16 packed)
{
	if (!g_map[packed].hasExplosion) return;

	for (int i = 1; i < s_explosions.num_elem; i++) {
		Explosion *e = (Explosion *)BinHeap_GetElem(&s_explosions, i);

		if (e->commands == NULL) continue;
		if (Tile_PackTile(e->position) != packed) continue;

		Explosion_Func_Stop(e, 0);
		break;
	}
}

/*--------------------------------------------------------------*/

/* Toxic cloud of the Death Hand.  For a few seconds, ground units in the
 * cloud take damage every second, and gas puffs are drawn over it.
 */

#define TOXIC_MAX_CLOUDS 8
#define TOXIC_SECONDS 10
#define TOXIC_INTERVAL_SECONDS 2
#define TOXIC_DAMAGE 10     /* Per unit, every TOXIC_INTERVAL_SECONDS. */

typedef struct ToxicCloud {
	bool active;
	tile32 position;
	uint16 radius;          /* 1/16 tile units, like the explosion radius. */
	uint8 houseID;          /* The house that fired it, which colours the gas. */
	int64_t end;
	int64_t nextDamage;
	int64_t nextPuff;
} ToxicCloud;

static ToxicCloud s_toxic[TOXIC_MAX_CLOUDS];
static FILE *s_testLog;

/* Test only: how far the Death Hand impacts land from the target. */
static tile32 s_testTarget;
static bool s_testTargetSet;
static double s_testErrorSum;
static int s_testImpacts;

static void
Explosion_TestLog(const char *fmt, ...)
{
	va_list args;

	if (s_testLog == NULL)
		return;

	va_start(args, fmt);
	vfprintf(s_testLog, fmt, args);
	va_end(args);
	fflush(s_testLog);
}

void
Explosion_ToxicCloud_Add(tile32 position, uint16 radius, uint8 houseID)
{
	ToxicCloud *cloud = NULL;

	for (int i = 0; i < TOXIC_MAX_CLOUDS; i++) {
		if (!s_toxic[i].active) {
			cloud = &s_toxic[i];
			break;
		}

		if (cloud == NULL || s_toxic[i].end < cloud->end)
			cloud = &s_toxic[i];
	}

	cloud->active = true;
	cloud->position = position;
	cloud->radius = radius;
	cloud->houseID = houseID;
	cloud->end = g_timerGame + Tools_AdjustToGameSpeed(60 * TOXIC_SECONDS, 1, 0xFFFF, true);
	cloud->nextDamage = g_timerGame + Tools_AdjustToGameSpeed(60 * TOXIC_INTERVAL_SECONDS, 1, 0xFFFF, true);
	cloud->nextPuff = g_timerGame;

	Explosion_TestLog("[toxic] cloud added: radius %u (1/16 tiles) at tile %u,%u, house %u, ends in %d ticks\n",
			radius, Tile_GetPackedX(Tile_PackTile(position)), Tile_GetPackedY(Tile_PackTile(position)),
			houseID, (int)(cloud->end - g_timerGame));

	if (s_testTargetSet) {
		const double error = Tile_GetDistance(position, s_testTarget) / 256.0;

		s_testErrorSum += error;
		s_testImpacts++;
		Explosion_TestLog("[test] impact %d landed %.2f tiles from the target\n", s_testImpacts, error);
	}
}

int
Explosion_ToxicCloud_Count(void)
{
	int count = 0;

	for (int i = 0; i < TOXIC_MAX_CLOUDS; i++) {
		if (s_toxic[i].active)
			count++;
	}

	return count;
}

static void
Explosion_ToxicCloud_Tick(void)
{
	for (int i = 0; i < TOXIC_MAX_CLOUDS; i++) {
		ToxicCloud *cloud = &s_toxic[i];

		if (!cloud->active)
			continue;

		if (g_timerGame >= cloud->end) {
			cloud->active = false;
			Explosion_TestLog("[toxic] cloud expired\n");
			continue;
		}

		/* Gas puffs: the deviator gas explosion is only a visual, the
		 * deviating itself is done elsewhere.  The gas takes the colour of
		 * the house that fired the missile.
		 */
		if (g_timerGame >= cloud->nextPuff) {
			cloud->nextPuff = g_timerGame + Tools_AdjustToGameSpeed(15, 1, 0xFFFF, true);

			for (int p = 0; p < 3; p++)
				Explosion_Start(EXPLOSION_DEVIATOR_GAS, Tile_MoveByRandom(cloud->position, cloud->radius, false), cloud->houseID);
		}

		if (g_timerGame >= cloud->nextDamage) {
			PoolFindStruct find;

			cloud->nextDamage += Tools_AdjustToGameSpeed(60 * TOXIC_INTERVAL_SECONDS, 1, 0xFFFF, true);

			for (Unit *u = Unit_FindFirst(&find, HOUSE_INVALID, UNIT_INVALID);
					u != NULL;
					u = Unit_FindNext(&find)) {
				const UnitInfo *ui = &g_table_unitInfo[u->o.type];

				if (u->o.flags.s.isNotOnMap || !ui->flags.isNormalUnit || u->o.type == UNIT_SANDWORM)
					continue;

				if (ui->movementType == MOVEMENT_WINGER)
					continue;

				if ((Tile_GetDistance(cloud->position, u->o.position) >> 4) >= cloud->radius)
					continue;

				Explosion_TestLog("[toxic] pulse hits unit %u type %u hp %u\n",
						u->o.index, u->o.type, u->o.hitpoints);

				/* Infantry can't survive the gas at all. */
				Unit_Damage(u, (ui->movementType == MOVEMENT_FOOT) ? u->o.hitpoints : TOXIC_DAMAGE, 0);
			}
		}
	}
}

/*--------------------------------------------------------------*/

/* Debug tests, run with a mission start such as
 *   --house atreides --mission 5 --test-deathhand
 *   --house atreides --mission 5 --test-deathhand-cy
 *
 * They fire a real Death Hand missile and log what happens to deathhand_test.log,
 * then quit.  The first puts tanks and infantry at various distances from the
 * target, the second targets the player's construction yard.
 */

#define TEST_UNITS 7
#define TEST_MISSILES 1

void
Explosion_DebugTestDeathHand(void)
{
	static int state = 0;
	static int64_t stateTick;
	static uint16 unitIndex[TEST_UNITS];
	static const enum UnitType unitType[TEST_UNITS] = {
		UNIT_TANK, UNIT_TANK, UNIT_TANK, UNIT_TANK, UNIT_TANK, UNIT_INFANTRY, UNIT_INFANTRY
	};
	static const int unitDistance[TEST_UNITS] = { 0, 1, 3, 5, 12, 0, 3 }; /* tiles from the target. */
	static const int unitRow[TEST_UNITS] = { 0, 0, 0, 0, 0, 2, 2 };       /* tiles south of the target. */
	static uint16 structureIndex = 0xFFFF;
	static int64_t nextReport;
	static int64_t impactTick;
	static bool screenshotTaken;
	const bool structureTest = (g_debugTestDeathHand == 2);

	if (state == 0) {
		s_testLog = fopen("deathhand_test.log", "w");
		stateTick = g_timerGame;
		state = 1;
		Explosion_TestLog("[test] started (%s)\n", structureTest ? "construction yard" : "units");
		return;
	}

	/* Wait a couple of seconds for the game to settle. */
	if (state == 1) {
		PoolFindStruct find;
		tile32 target;

		if (g_timerGame - stateTick < 120)
			return;

		if (structureTest) {
			const Structure *cy = Structure_FindFirst(&find, g_playerHouseID, STRUCTURE_CONSTRUCTION_YARD);

			if (cy == NULL) {
				Explosion_TestLog("[test] the player has no construction yard\n");
				state = 99;
				return;
			}

			structureIndex = cy->o.index;

			/* The yard is 2x2 tiles, its position is the top left one. */
			target = cy->o.position;
			target.x += 256;
			target.y += 256;

			Explosion_TestLog("[test] construction yard %u at tile %u,%u: %u hitpoints (max %u), targeting its centre\n",
					cy->o.index, Tile_GetPackedX(Tile_PackTile(cy->o.position)), Tile_GetPackedY(Tile_PackTile(cy->o.position)),
					cy->o.hitpoints, g_table_structureInfo[cy->o.type].o.hitpoints);
		} else {
			const Unit *anchor = Unit_FindFirst(&find, g_playerHouseID, UNIT_INVALID);
			const Structure *anchorS = Structure_FindFirst(&find, g_playerHouseID, STRUCTURE_INVALID);
			tile32 base;

			if (anchor != NULL) {
				base = anchor->o.position;
			} else if (anchorS != NULL) {
				base = anchorS->o.position;
			} else {
				Explosion_TestLog("[test] no player unit or structure to anchor on\n");
				state = 99;
				return;
			}

			const int bx = Tile_GetPackedX(Tile_PackTile(base)) + 8;
			const int by = Tile_GetPackedY(Tile_PackTile(base));

			for (int i = 0; i < TEST_UNITS; i++) {
				const uint16 packed = Tile_PackXY(bx + unitDistance[i], by + unitRow[i]);
				Unit *u = Unit_Create(UNIT_INDEX_INVALID, unitType[i], g_playerHouseID, Tile_Center(Tile_UnpackTile(packed)), 0);

				unitIndex[i] = (u != NULL) ? u->o.index : UNIT_INDEX_INVALID;
				Explosion_TestLog("[test] %s %d at %d tiles from target (tile %d,%d): %s (unit %u)\n",
						(unitType[i] == UNIT_TANK) ? "tank" : "infantry", i, unitDistance[i],
						bx + unitDistance[i], by + unitRow[i],
						(u != NULL) ? "created" : "FAILED to create", unitIndex[i]);
			}

			target = Tile_Center(Tile_UnpackTile(Tile_PackXY(bx, by)));
		}

		/* Fire the real missiles from 24 tiles away, a typical launch distance. */
		const int tx = Tile_GetPackedX(Tile_PackTile(target));
		const int ty = Tile_GetPackedY(Tile_PackTile(target));
		const tile32 start = Tile_Center(Tile_UnpackTile(Tile_PackXY(tx, (ty >= 24) ? ty - 24 : 0)));
		const uint16 encoded = Tools_Index_Encode(Tile_PackTile(target), IT_TILE);

		/* Several missiles at the same target, to measure the accuracy. */
		s_testTarget = target;
		s_testTargetSet = true;

		/* Show the impact, for the screenshot. */
		Map_CentreViewport(TILE_SIZE * tx + TILE_SIZE / 2, TILE_SIZE * ty + TILE_SIZE / 2);

		for (int m = 0; m < TEST_MISSILES; m++) {
			Unit *missile = Unit_CreateBullet(start, UNIT_MISSILE_HOUSE, g_playerHouseID, 500, encoded);
			Explosion_TestLog("[test] missile %d %s\n", m, (missile != NULL) ? "launched" : "FAILED to launch");
		}

		stateTick = g_timerGame;
		nextReport = g_timerGame;
		state = 2;
		return;
	}

	if (state == 99) {
		fclose(s_testLog);
		s_testLog = NULL;
		exit(1);
	}

	if (Explosion_ToxicCloud_Count() > 0 && impactTick == 0)
		impactTick = g_timerGame;

	/* A screenshot of the cloud, saved to the personal data directory. */
	if (impactTick != 0 && !screenshotTaken && g_timerGame - impactTick > 150) {
		screenshotTaken = true;
		VideoA5_CaptureScreenshot();
		Explosion_TestLog("[test] screenshot requested\n");
	}

	/* Report every second until the cloud has gone. */
	if (g_timerGame >= nextReport) {
		nextReport = g_timerGame + 60;

		Explosion_TestLog("[test] t=%d clouds=%d", (int)((g_timerGame - stateTick) / 60), Explosion_ToxicCloud_Count());

		if (structureTest) {
			const Structure *s = (structureIndex != 0xFFFF) ? Structure_Get_ByIndex(structureIndex) : NULL;

			if (s != NULL && s->o.flags.s.used && s->o.type == STRUCTURE_CONSTRUCTION_YARD) {
				Explosion_TestLog(" construction yard hp: %u\n", s->o.hitpoints);
			} else {
				Explosion_TestLog(" construction yard: destroyed\n");
			}
		} else {
			Explosion_TestLog(" hp:");
			for (int i = 0; i < TEST_UNITS; i++) {
				const Unit *u = (unitIndex[i] != UNIT_INDEX_INVALID) ? Unit_Get_ByIndex(unitIndex[i]) : NULL;

				if (u != NULL && u->o.flags.s.used && u->o.type == unitType[i]) {
					Explosion_TestLog(" %u", u->o.hitpoints);
				} else {
					Explosion_TestLog(" dead");
				}
			}
			Explosion_TestLog("\n");
		}
	}

	/* Done a few seconds after the cloud expired, or after a minute and a half. */
	if ((impactTick != 0 && Explosion_ToxicCloud_Count() == 0 && g_timerGame - impactTick > 60 * 8)
			|| g_timerGame - stateTick > 60 * 90) {
		Explosion_TestLog("[test] finished: %d impacts, mean error %.2f tiles\n",
				s_testImpacts, (s_testImpacts > 0) ? s_testErrorSum / s_testImpacts : -1.0);
		fclose(s_testLog);
		s_testLog = NULL;
		exit(0);
	}
}

void
Explosion_Init(void)
{
	BinHeap_Init(&s_explosions, sizeof(Explosion));
	memset(s_toxic, 0, sizeof(s_toxic));
}

void
Explosion_Uninit(void)
{
	BinHeap_Free(&s_explosions);
}

/**
 * Start a Explosion on a tile.
 * @param explosionType Type of Explosion.
 * @param position The position to use for init.
 * @param houseID House from which the explosion originates. Determines deviator gas color.
 */
void Explosion_Start(uint16 explosionType, tile32 position, uint8 houseID)
{
	if (explosionType > EXPLOSION_HIT_MEDIUM) return;

	uint16 packed = Tile_PackTile(position);
	Explosion_StopAtPosition(packed);

	Explosion *e = BinHeap_Push(&s_explosions, Timer_GetTicks());
	if (e != NULL) {
		e->commands = g_table_explosion[explosionType];
		e->current  = 0;
		e->spriteID = 0;
		e->position = position;
		e->houseID = houseID;

		g_map[packed].hasExplosion = true;

		/* Do not unveil for sandworm eat and spice bloom explosion types.
		 * In multiplayer, only reveal nearby explosions.
		 */
		if (enhancement_fog_of_war
				&& explosionType != EXPLOSION_SANDWORM_SWALLOW
				&& explosionType != EXPLOSION_SPICE_BLOOM_TREMOR) {
			const enum HouseFlag houses
				= (g_campaign_selected == CAMPAIGNID_MULTIPLAYER)
				? Map_FindHousesInRadius(position, 9)
				: FLAG_HOUSE_ALL;

			Tile_RefreshFogInRadius(houses, UNVEILCAUSE_EXPLOSION,
					position, 1, false);
		}
	}
}

/**
 * Timer tick for explosions.
 */
void Explosion_Tick(void)
{
	const int64_t curr_ticks = Timer_GetTicks();

	Explosion_ToxicCloud_Tick();

	Explosion *e = BinHeap_GetMin(&s_explosions);
	while ((e != NULL) && (e->timeOut <= curr_ticks)) {
		while (e->commands != NULL) {
			uint16 parameter = e->commands[e->current].parameter;
			uint16 command   = e->commands[e->current].command;

			e->current++;

			switch (command) {
				default:
				case EXPLOSION_STOP:               Explosion_Func_Stop(e, parameter); break;

				case EXPLOSION_SET_SPRITE:         Explosion_Func_SetSpriteID(e, parameter); break;
				case EXPLOSION_SET_TIMEOUT:        Explosion_Func_SetTimeout(e, parameter); break;
				case EXPLOSION_SET_RANDOM_TIMEOUT: Explosion_Func_SetRandomTimeout(e, parameter); break;
				case EXPLOSION_MOVE_Y_POSITION:    Explosion_Func_MoveYPosition(e, parameter); break;
				case EXPLOSION_TILE_DAMAGE:        Explosion_Func_TileDamage(e, parameter); break;
				case EXPLOSION_PLAY_VOICE:         Explosion_Func_PlayVoice(e, parameter); break;
				case EXPLOSION_SCREEN_SHAKE:       Explosion_Func_ScreenShake(e, parameter); break;
				case EXPLOSION_SET_ANIMATION:      Explosion_Func_SetAnimation(e, parameter); break;
				case EXPLOSION_BLOOM_EXPLOSION:    Explosion_Func_BloomExplosion(e, parameter); break;
			}

			if (e->timeOut > curr_ticks)
				break;
		}

		/* Push explosion with updated time before pop. */
		if (e->commands == NULL) {
			BinHeap_Pop(&s_explosions);
		} else {
			BinHeap_UpdateMin(&s_explosions);
		}

		e = BinHeap_GetMin(&s_explosions);
	}
}

/*--------------------------------------------------------------*/

/* Hardware effects (OpenGL only).  The explosion and smoke sprites are
 * replaced by procedural effects, built from the same chunky square pixels as
 * the sprites: a flash, a fireball of glowing particles, sparks, and smoke that
 * lingers after the fire.
 *
 * Clients only receive the sprite, position and house of an explosion, so the
 * effects run on a clock of their own.  An effect starts when a sprite of its
 * kind is first seen on a tile, and runs for its own duration, longer than the
 * original sprite animation.
 */

enum FxKind {
	FX_NONE,
	FX_SPARK,       /* Muzzle flashes. */
	FX_HIT,         /* A bullet hitting a unit. */
	FX_BLAST_S,
	FX_BLAST_M,
	FX_BLAST_L,
	FX_DUST,        /* Sand burst, spice bloom tremor. */
	FX_GAS,         /* Deviator gas, coloured by house. */
	FX_FLAME,       /* Burning wreck, loops while the explosion lasts. */
	FX_SMOKE,       /* Smoke plume, loops while the explosion lasts. */
	FX_PRE,         /* Fireball while the explosion still waits for its first sprite. */
	FX_MAX
};

static const struct {
	int duration;   /* In ticks.  For a loop, the time to fade out after the explosion ends. */
	bool loop;
} s_fx_info[FX_MAX] = {
	[FX_SPARK]   = { 16, false },
	[FX_HIT]     = { 10, false },
	[FX_BLAST_S] = { 20, false },
	[FX_BLAST_M] = { 50, false },
	[FX_BLAST_L] = { 70, false },
	[FX_DUST]    = { 44, false },
	[FX_GAS]     = { 90, false },
	[FX_FLAME]   = { 60, true },
	[FX_SMOKE]   = { 90, true },
	[FX_PRE]     = { 40, false },
};

#define FX_MAX_INSTANCES 96
#define FX_WAIT_TICKS 6         /* An explosion without a sprite this long is waiting. */
#define FX_RESTART_GAP 8       /* Not seen for this many ticks: a new explosion. */
#define FX_TWO_PI 6.2831853f

typedef struct FxInstance {
	bool used;
	uint8 kind;
	uint8 houseID;
	uint16 packed;
	tile32 position;
	int64_t start;
	int64_t lastSeen;
	uint32 seed;
} FxInstance;

static FxInstance s_fx[FX_MAX_INSTANCES];

static enum FxKind
Fx_Classify(int spriteID)
{
	if (spriteID & EXPLOSION_SPRITE_HIT)
		return ((spriteID & ~EXPLOSION_SPRITE_HIT) == 153 || (spriteID & ~EXPLOSION_SPRITE_HIT) == 154) ? FX_HIT : FX_NONE;

	switch (spriteID) {
		case 153: case 154: return FX_SPARK;
		case 183: case 184: return FX_BLAST_S;
		case 151: case 152:
		case 198: case 199: case 200: case 201: case 202:
		case 203: case 204: case 205: case 206: case 207: return FX_BLAST_M;
		case 188: case 189: case 190: case 191: case 192: return FX_BLAST_L;
		case 156: case 157: case 158: return FX_DUST;
		case 208: case 209: case 210: case 211: case 212: return FX_GAS;
		case 168: case 169: case 170: return FX_FLAME;
		case 180: case 181: case 182: return FX_SMOKE;
		default: break;
	}

	return FX_NONE;
}

static uint32
Fx_Hash(uint32 a, uint32 b)
{
	uint32 h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu;

	h ^= h >> 15; h *= 0x2C1B3C6Du;
	h ^= h >> 12; h *= 0x297A2D39u;
	h ^= h >> 15;
	return h;
}

/* A stable random number in [0, 1] for particle i. */
static float
Fx_Rand(uint32 seed, uint32 i, uint32 k)
{
	return (Fx_Hash(seed + k * 0x1000193u, i) & 0xFFFF) / 65535.0f;
}

static float
Fx_Clamp01(float v)
{
	return (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
}

/* A chunky pixel of the given size, centred on x,y around the explosion. */
static void
Fx_Pixel(int bx, int by, float x, float y, int size, const unsigned char *c, float alpha)
{
	const int a = (int)(255.0f * Fx_Clamp01(alpha));
	const int px = bx + (int)floorf(x) - size / 2;
	const int py = by + (int)floorf(y) - size / 2;

	if (a <= 0)
		return;

	if (size < 3) {
		Prim_FillRect_RGBA(px, py, px + size, py + size, c[0], c[1], c[2], a);
		return;
	}

	/* A disc, one row at a time so that the pixels do not overlap and
	 * blend twice.  Still whole pixels, so it stays pixel art.
	 */
	const float r = size * 0.5f;

	for (int row = 0; row < size; row++) {
		const float dy = row + 0.5f - r;
		const float half = sqrtf(r * r - dy * dy);
		const int inset = (int)floorf(r - half + 0.5f);

		if (inset * 2 >= size)
			continue;

		Prim_FillRect_RGBA(px + inset, py + row, px + size - inset, py + row + 1, c[0], c[1], c[2], a);
	}
}

/* Fire palette in a few hard steps, like the original 256 colour sprites. */
static const unsigned char *
Fx_FireColour(float heat)
{
	static const unsigned char white[] = { 255, 246, 190 };
	static const unsigned char yellow[] = { 255, 200, 70 };
	static const unsigned char amber[] = { 252, 158, 40 };
	static const unsigned char orange[] = { 236, 108, 22 };
	static const unsigned char red[] = { 190, 52, 16 };
	static const unsigned char ember[] = { 110, 40, 24 };

	if (heat > 0.82f) return white;
	if (heat > 0.64f) return yellow;
	if (heat > 0.48f) return amber;
	if (heat > 0.32f) return orange;
	if (heat > 0.16f) return red;
	return ember;
}

static const unsigned char *
Fx_SmokeColour(float shade)
{
	static const unsigned char black[] = { 30, 27, 26 };
	static const unsigned char dark[] = { 50, 46, 43 };
	static const unsigned char mid[] = { 80, 74, 70 };
	static const unsigned char light[] = { 122, 114, 106 };
	static const unsigned char pale[] = { 164, 156, 146 };

	if (shade > 0.80f) return pale;
	if (shade > 0.60f) return light;
	if (shade > 0.40f) return mid;
	if (shade > 0.20f) return dark;
	return black;
}

/* A fireball: flash, shock ring, glowing particles that cool into smoke, and sparks.
 * t goes from 0 to 1, radius is in pixels.
 */
static void
Fx_DrawBlast(int bx, int by, float t, float radius, uint32 seed, int num, bool smoke)
{
	static const unsigned char flash[] = { 255, 250, 210 };
	static const unsigned char ring[] = { 255, 220, 150 };
	static const unsigned char spark[] = { 255, 232, 150 };

	/* Smoke under the fire. */
	for (int i = 0; smoke && i < num / 2; i++) {
		const float st = Fx_Clamp01((t - 0.25f - 0.2f * Fx_Rand(seed, i, 10)) / 0.75f);
		const float angle = FX_TWO_PI * Fx_Rand(seed, i, 11);
		const float spread = radius * (0.2f + 0.6f * Fx_Rand(seed, i, 12));

		if (st <= 0.0f)
			continue;

		Fx_Pixel(bx, by,
				cosf(angle) * spread * (0.6f + 0.4f * st),
				sinf(angle) * spread * 0.6f - st * radius * 1.3f,
				2 + (int)(st * radius * 0.28f),
				Fx_SmokeColour(0.15f + 0.8f * st * Fx_Rand(seed, i, 13)),
				0.65f * (1.0f - st) * Fx_Clamp01(st * 8.0f));
	}

	Prim_SetAdditive(true);

	/* The fire. */
	for (int i = 0; i < num; i++) {
		const float delay = 0.12f * Fx_Rand(seed, i, 1);
		const float pt = Fx_Clamp01((t - delay) / 0.6f);
		const float angle = FX_TWO_PI * Fx_Rand(seed, i, 2);
		const float speed = 0.3f + 0.7f * Fx_Rand(seed, i, 3);
		const float heat = 1.0f - pt * 1.1f - 0.1f * Fx_Rand(seed, i, 4);
		const float r = radius * speed * (1.0f - (1.0f - pt) * (1.0f - pt));

		if (t <= delay || heat <= 0.12f)
			continue;

		Fx_Pixel(bx, by,
				cosf(angle) * r,
				sinf(angle) * r * 0.75f - pt * radius * 0.5f,
				1 + (int)((1.0f - pt) * radius * 0.24f),
				Fx_FireColour(heat),
				1.0f - pt * pt);
	}

	/* Sparks, thrown further than the fire and falling back. */
	for (int i = 0; i < num / 3; i++) {
		const float pt = Fx_Clamp01(t / 0.55f);
		const float angle = FX_TWO_PI * Fx_Rand(seed, i, 20);
		const float speed = 0.8f + 1.0f * Fx_Rand(seed, i, 21);

		if (t >= 0.55f)
			break;

		Fx_Pixel(bx, by,
				cosf(angle) * radius * speed * pt,
				sinf(angle) * radius * speed * pt * 0.7f + radius * 0.9f * pt * pt - radius * 0.3f * pt,
				1, spark, 1.0f - pt);
	}

	/* Flash and shock ring. */
	if (t < 0.22f) {
		const float f = t / 0.22f;
		const int s = (int)(radius * 0.8f * (1.0f - f)) + 1;
		const float a = 0.9f * (1.0f - f);

		Fx_Pixel(bx, by, 0, 0, 2 * s, flash, a);
		Fx_Pixel(bx, by, 0, 0, 2 * s + 4, flash, a * 0.4f);
		Fx_Pixel(bx, by, 0, 0, 1 + (int)(radius * 1.2f * f), flash, a * 0.5f);
	}

	if (radius >= 8.0f && t < 0.35f) {
		const float f = t / 0.35f;

		for (int i = 0; i < 20; i++) {
			const float angle = FX_TWO_PI * i / 20.0f;

			Fx_Pixel(bx, by,
					cosf(angle) * radius * 1.5f * f,
					sinf(angle) * radius * 1.5f * f * 0.75f,
					1, ring, 0.6f * (1.0f - f));
		}
	}

	Prim_SetAdditive(false);
}

/* The wind that blows the smoke, in pixels per tick: a light breeze, or the
 * sandstorm's wind while one blows.  storm is how strong the storm is, 0 to 1.
 */
static void
Fx_Wind(float *wx, float *wy, float *storm)
{
	float sx, sy;
	const float k = Sandstorm_GetStrength();

	Sandstorm_GetWind(&sx, &sy);
	*storm = k;
	*wx = 0.09f * (1.0f - k) + sx * 0.65f * k;
	*wy = sy * 0.65f * k;
}

/* A fire with a trail of smoke.  The smoke rises and is bent over by the wind,
 * dark near the fire and paler as it spreads; in a sandstorm it is blown along
 * flat behind the fire.  Flames lean with the wind, embers ride up the smoke, and
 * the ground glows under it.  big is the burning wreck, else a smaller fire under
 * a heavier column of smoke.
 */
static void
Fx_DrawFireEx(int bx, int by, int64_t ticks, float smokeEnv, float flameEnv, uint32 seed, bool big)
{
	const float now = ticks / 1.1f;    /* A little slower than the clock: the fire is calmer. */
	float wx, wy, storm;

	Fx_Wind(&wx, &wy, &storm);

	/* Smoke: each puff is born at the fire and lives out its life on the trail. */
	const int smokeNum = big ? 48 : 55;
	const float life = (big ? 150.0f : 190.0f) * (1.0f - 0.35f * storm);
	const float rise = (big ? 0.20f : 0.24f) * (1.0f - 0.8f * storm);

	for (int i = 0; i < smokeNum; i++) {
		const float phase = fmodf(now / life + (float)i / smokeNum + Fx_Rand(seed, i, 1) * 0.05f, 1.0f);
		const float a = phase * life;
		const float curl = sinf(phase * 6.0f + i * 1.9f) * (1.0f + phase * 5.0f) * (1.0f - 0.7f * storm);
		const float drift = a * (0.6f + 0.4f * phase);  /* Higher up, the wind takes it more. */

		Fx_Pixel(bx, by,
				wx * drift + curl,
				2.0f - rise * a + wy * drift + curl * 0.3f,
				3 + (int)(phase * (big ? 9.0f : 11.0f)),
				Fx_SmokeColour(0.05f + 0.85f * phase * phase),
				(big ? 0.85f : 0.92f) * smokeEnv * powf(1.0f - phase, 1.1f) * Fx_Clamp01(phase * 9.0f));
	}

	Prim_SetAdditive(true);

	/* A glow on the ground, pulsing with the flames. */
	{
		static const unsigned char glow[] = { 120, 46, 10 };
		const float pulse = 0.75f + 0.25f * sinf(now * 0.31f + seed);

		Fx_Pixel(bx, by, 0.0f, 3.0f, big ? 16 : 11, glow, 0.30f * flameEnv * pulse);
	}

	/* The flames, leaning with the wind. */
	const int flameNum = big ? 29 : 17;
	const float height = big ? 15.0f : 9.0f;
	const float spread = big ? 8.0f : 5.0f;

	for (int i = 0; i < flameNum; i++) {
		const float phase = fmodf(now / (big ? 34.0f : 28.0f) + (float)i / flameNum + Fx_Rand(seed, i, 2) * 0.1f, 1.0f);
		const float heat = 1.0f - phase * 1.15f;

		if (heat <= 0.12f)
			continue;

		Fx_Pixel(bx, by,
				(Fx_Rand(seed, i, 3) - 0.5f) * spread * (1.0f - phase * 0.8f)
					+ sinf(phase * 9.0f + i) * 1.3f + wx * phase * 40.0f,
				4.0f - phase * height + wy * phase * 20.0f,
				(big ? 4 : 3) - (int)(phase * (big ? 3.0f : 2.2f)),
				Fx_FireColour(heat), flameEnv * (1.0f - phase * 0.55f));
	}

	/* Embers, carried up and away on the smoke. */
	for (int i = 0; i < (big ? 10 : 6); i++) {
		static const unsigned char ember[] = { 255, 170, 60 };
		const float phase = fmodf(now / 95.0f + (float)i / 8.0f + Fx_Rand(seed, i, 7) * 0.5f, 1.0f);
		const float a = phase * 95.0f;
		const float flicker = (Fx_Hash(seed + i, (uint32)(now / 4)) & 3) ? 1.0f : 0.3f;

		Fx_Pixel(bx, by,
				wx * a * 1.2f + (Fx_Rand(seed, i, 8) - 0.5f) * 10.0f * phase,
				2.0f - a * 0.32f * (1.0f - 0.7f * storm) + wy * a * 1.2f,
				1, ember, flameEnv * flicker * (1.0f - phase));
	}

	Prim_SetAdditive(false);
}

static void
Fx_DrawFire(int bx, int by, int64_t now, float env, uint32 seed, bool big)
{
	Fx_DrawFireEx(bx, by, now, env, env, seed, big);
}

void
Explosion_DrawFire(int x, int y, float smoke, float flames, uint32 seed)
{
	if (VideoA5_HardwareEffectsActive())
		Fx_DrawFireEx(x, y, Timer_GetTicks(), Fx_Clamp01(smoke), Fx_Clamp01(flames), seed, false);
}

/* Billows of tinted gas that grow and drift. */
static void
Fx_DrawGas(int bx, int by, float t, uint8 houseID, uint32 seed)
{
	static const unsigned char tint[][3] = {
		{ 214, 70, 58 },    /* Harkonnen. */
		{ 70, 118, 226 },   /* Atreides. */
		{ 80, 206, 100 },   /* Ordos. */
		{ 214, 200, 70 },   /* Others. */
	};
	const unsigned char *base = tint[(houseID < 3) ? houseID : 3];
	const float env = Fx_Clamp01(t * 8.0f) * Fx_Clamp01((1.0f - t) * 3.0f);

	for (int i = 0; i < 29; i++) {
		const float angle = FX_TWO_PI * Fx_Rand(seed, i, 1);
		const float dist = (3.0f + 9.0f * Fx_Rand(seed, i, 2)) * (0.4f + 0.6f * Fx_Clamp01(t * 3.0f));
		const float shade = 0.65f + 0.35f * Fx_Rand(seed, i, 3);
		const unsigned char colour[3] = {
			(unsigned char)(base[0] * shade), (unsigned char)(base[1] * shade), (unsigned char)(base[2] * shade)
		};

		Fx_Pixel(bx, by,
				cosf(angle + t * 1.5f) * dist,
				sinf(angle + t * 1.5f) * dist * 0.7f - t * 5.0f,
				3 + (int)(Fx_Rand(seed, i, 4) * 3.0f + t * 3.0f),
				colour, 0.45f * env);
	}
}

/* Sand thrown up: a ring of dust that rises and settles. */
static void
Fx_DrawDust(int bx, int by, float t, float radius, int num, uint32 seed)
{
	static const unsigned char sand[] = { 206, 164, 96 };
	static const unsigned char pale[] = { 232, 200, 138 };

	for (int i = 0; i < num; i++) {
		const float pt = Fx_Clamp01((t - 0.1f * Fx_Rand(seed, i, 1)) / 0.9f);
		const float angle = FX_TWO_PI * Fx_Rand(seed, i, 2);
		const float r = radius * (0.3f + 0.7f * Fx_Rand(seed, i, 3)) * (1.0f - (1.0f - pt) * (1.0f - pt));

		Fx_Pixel(bx, by,
				cosf(angle) * r,
				sinf(angle) * r * 0.6f - sinf(pt * 3.1416f) * radius * 0.5f,
				2 + (int)((0.4f + pt) * 2.0f),
				(Fx_Rand(seed, i, 4) > 0.5f) ? sand : pale,
				0.75f * (1.0f - pt) * Fx_Clamp01(pt * 10.0f + 0.3f));
	}
}

static void
Fx_Draw(const FxInstance *fx, int64_t now)
{
	int x, y;

	if (!Map_IsUnveiledToHouse(g_playerHouseID, fx->packed))
		return;

	if (!Map_IsPositionInViewport(fx->position, &x, &y))
		return;

	x += g_widgetProperties[WINDOWID_VIEWPORT].xBase;
	y += g_widgetProperties[WINDOWID_VIEWPORT].yBase;

	const int duration = s_fx_info[fx->kind].duration;
	const int64_t age = now - fx->start;
	if (age < 0)
		return;

	const float t = Fx_Clamp01((float)age / duration);

	switch (fx->kind) {
		/* Muzzle flashes (SPARK) keep their size.  Bullet impacts (BLAST_S) are a
		 * puff without smoke.  Only deaths and structures get a fireball.
		 */
		case FX_SPARK:   Fx_DrawBlast(x, y, t, 4.0f, fx->seed, 8, false); break;
		case FX_HIT:     Fx_DrawBlast(x, y, t, 2.5f, fx->seed, 5, false); break;
		case FX_BLAST_S: Fx_DrawBlast(x, y, t, 5.0f, fx->seed, 11, false); break;
		case FX_BLAST_M: Fx_DrawBlast(x, y, t, 11.0f, fx->seed, 34, true); break;
		case FX_PRE:     Fx_DrawBlast(x, y, t, 9.0f, fx->seed, 26, true); break;
		case FX_BLAST_L: Fx_DrawBlast(x, y, t, 17.0f, fx->seed, 53, true); break;
		case FX_DUST:    Fx_DrawDust(x, y, t, 12.0f, 19, fx->seed); break;
		case FX_GAS:     Fx_DrawGas(x, y, t, fx->houseID, fx->seed); break;

		case FX_FLAME:
		case FX_SMOKE: {
			/* Fade in when it starts, and out after the explosion has ended. */
			const float fadeOut = Fx_Clamp01(1.0f - (float)(now - fx->lastSeen) / duration);
			const float env = Fx_Clamp01(age / 12.0f) * fadeOut;

			if (fx->kind == FX_FLAME)
				Fx_DrawFire(x, y, now, env, fx->seed, true);
			else
				Fx_DrawFire(x, y, now, env, fx->seed, false);
			break;
		}

		default: break;
	}
}

/* Note an explosion drawn this frame, starting its effect if it is new. */
static void
Fx_Track(const Explosion *e, enum FxKind kind, int64_t now, int delay)
{
	const uint16 packed = Tile_PackTile(e->position);
	FxInstance *fx = NULL;
	FxInstance *freeSlot = NULL;

	for (int i = 0; i < FX_MAX_INSTANCES; i++) {
		if (!s_fx[i].used) {
			if (freeSlot == NULL)
				freeSlot = &s_fx[i];
		} else if (s_fx[i].packed == packed && s_fx[i].kind == kind) {
			fx = &s_fx[i];
			break;
		}
	}

	if (fx == NULL) {
		fx = freeSlot;
		if (fx == NULL)
			return;

		fx->start = now + delay;
		fx->used = true;
	} else if (now - fx->lastSeen > FX_RESTART_GAP) {
		fx->start = now + delay;
	} else {
		fx->lastSeen = now;
		return;
	}

	fx->kind = kind;
	fx->packed = packed;
	fx->houseID = e->houseID;
	fx->position = e->position;
	fx->lastSeen = now;
	fx->seed = Fx_Hash(packed, now);
}

/* The first sprite has arrived soon after the explosion started, so it was no
 * long wait: drop the early fire that had not been drawn yet.
 */
static void
Fx_CancelWaiting(uint16 packed, int64_t now)
{
	for (int i = 0; i < FX_MAX_INSTANCES; i++) {
		FxInstance *fx = &s_fx[i];

		if (fx->used && fx->packed == packed && fx->start > now
				&& (fx->kind == FX_PRE || fx->kind == FX_FLAME))
			fx->used = false;
	}
}


/* --test-fire: fires and smoke plumes beside the construction yard, a screenshot
 * once the smoke has built up, then quit.  With --sandstorm-now, the smoke is in a storm.
 */
bool g_debugTestFire = false;

static void
Explosion_DebugTestFire(int64_t now)
{
	static int64_t start = -1;
	static bool shot = false;

	if (start < 0) {
		PoolFindStruct find;
		const Structure *s = Structure_FindFirst(&find, g_playerHouseID, STRUCTURE_CONSTRUCTION_YARD);

		if (s == NULL)
			return;

		start = now;
		Map_CentreViewport((s->o.position.x >> 4) + 16, (s->o.position.y >> 4) + 16);

		for (int i = 0; i < 4; i++) {
			tile32 t = s->o.position;

			t.x += (uint16)(256 * (2 * (i % 2) - 1) * 3);
			t.y += (uint16)(256 * (i / 2) * 3 + 256);
			Explosion_Start((i < 2) ? EXPLOSION_TANK_FLAMES : EXPLOSION_SMOKE_PLUME, Tile_Center(t), g_playerHouseID);
		}
		return;
	}

	if (!shot && now - start > 200) {
		shot = true;
		VideoA5_CaptureScreenshot();
	}

	if (shot && now - start > 260)
		exit(0);
}

static void
Explosion_DrawHardware(void)
{
	const int64_t now = Timer_GetTicks();

	if (g_debugTestFire)
		Explosion_DebugTestFire(now);

	for (int i = 1; i < s_explosions.num_elem; i++) {
		const Explosion *e = (const Explosion *)BinHeap_GetElem(&s_explosions, i);

		const uint16 packed = Tile_PackTile(e->position);
		if (!Map_IsUnveiledToHouse(g_playerHouseID, packed))
			continue;

		int x, y;
		if (!Map_IsPositionInViewport(e->position, &x, &y))
			continue;

		/* Waiting for the first sprite, as the structure explosions do for
		 * up to a second.  Start burning at once, rather than after the
		 * building is gone.
		 */
		if (e->spriteID == 0) {
			/* Not for the first few ticks, as every explosion starts like this. */
			Fx_Track(e, FX_PRE, now, FX_WAIT_TICKS);
			Fx_Track(e, FX_FLAME, now, FX_WAIT_TICKS);
			continue;
		}

		Fx_CancelWaiting(packed, now);

		const enum FxKind kind = Fx_Classify(e->spriteID);
		if (kind == FX_NONE) {
			Shape_DrawRemap(e->spriteID & ~EXPLOSION_SPRITE_HIT, e->houseID, x, y, 2, 0xC000);
			continue;
		}

		Fx_Track(e, kind, now, 0);
	}

	/* Draw the effects, including those whose explosion has already ended. */
	for (int i = 0; i < FX_MAX_INSTANCES; i++) {
		FxInstance *fx = &s_fx[i];

		if (!fx->used)
			continue;

		const int duration = s_fx_info[fx->kind].duration;
		const bool alive = s_fx_info[fx->kind].loop
				? (now - fx->lastSeen <= duration)
				: (now - fx->start <= duration);

		if (alive) {
			Fx_Draw(fx, now);
		} else if (now - fx->lastSeen > FX_RESTART_GAP) {
			fx->used = false;
		}
	}
}

void
Explosion_Draw(void)
{
	if (VideoA5_HardwareEffectsActive()) {
		Explosion_DrawHardware();
		return;
	}

	for (int i = 1; i < s_explosions.num_elem; i++) {
		const Explosion *e = (const Explosion *)BinHeap_GetElem(&s_explosions, i);

		if (e->spriteID == 0) continue;

		const uint16 packed = Tile_PackTile(e->position);
		if (!Map_IsUnveiledToHouse(g_playerHouseID, packed))
			continue;

		int x, y;
		if (!Map_IsPositionInViewport(e->position, &x, &y))
			continue;

		Shape_DrawRemap(e->spriteID & ~EXPLOSION_SPRITE_HIT, e->houseID, x, y, 2, 0xC000);
	}
}

uint8
Explosion_Get_NumActive(void)
{
	return min(s_explosions.num_elem, 0xFF);
}

void
Explosion_Set_NumActive(int num)
{
	if (s_explosions.max_elem < num) {
		BinHeap_Resize(&s_explosions, num);
	}

	s_explosions.num_elem = num;
}

Explosion *
Explosion_Get_ByIndex(int i)
{
	assert(1 <= i && i < s_explosions.num_elem);

	return (Explosion *)BinHeap_GetElem(&s_explosions, i);
}
