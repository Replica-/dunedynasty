/** @file src/daynight.c Day and night, with shadows and headlights.
 *
 * One full day and night takes DAYNIGHT_CYCLE_SECONDS of game time.  By day the
 * units and buildings cast shadows that swing round with the sun and grow long
 * at dawn and dusk.  At night a dark blue layer covers the viewport and the
 * lights make holes in it: headlights in front of the vehicles, glows round
 * the buildings and the infantry, and flashes from explosions.  Night also
 * turns fog of war on, and in a sandstorm the units then see only what is
 * right next to them.
 *
 * Everything is drawn with the same chunky pixels as the rest of the game.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "daynight.h"

#include "enhancement.h"
#include "explosion.h"
#include "gui/widget.h"
#include "house.h"
#include "map.h"
#include "pool/pool.h"
#include "pool/pool_house.h"
#include "pool/pool_structure.h"
#include "pool/pool_unit.h"
#include "sandstorm.h"
#include "structure.h"
#include "table/widgetinfo.h"
#include "timer/timer.h"
#include "tools/coord.h"
#include "unit.h"
#include "video/prim.h"
#include "video/video_a5.h"

bool g_day_night = true;
float g_debugTimeOfDay = -1.0f;
bool g_debugTestDayNight = false;

#define TICKS_PER_SECOND 60
#define PI_F 3.14159265f

/* Where the phases are in the cycle, 0 to 1.  The game starts in the morning. */
#define PHASE_START 0.15f
#define PHASE_DAWN_BEGIN 0.94f      /* Dawn runs from here, through 1 = 0, to PHASE_DAY_BEGIN. */
#define PHASE_DAY_BEGIN 0.06f
#define PHASE_DUSK_BEGIN 0.46f
#define PHASE_NIGHT_BEGIN 0.54f

static float
DN_Smooth(float t)
{
	t = (t < 0.0f) ? 0.0f : (t > 1.0f) ? 1.0f : t;
	return t * t * (3.0f - 2.0f * t);
}

static float
DN_Clamp01(float v)
{
	return (v < 0.0f) ? 0.0f : (v > 1.0f) ? 1.0f : v;
}

bool
DayNight_Active(void)
{
	return g_day_night;
}

float
DayNight_Phase(void)
{
	if (g_debugTimeOfDay >= 0.0f)
		return g_debugTimeOfDay;

	double t = (double)(g_timerGame - g_tickScenarioStart) / ((double)TICKS_PER_SECOND * DAYNIGHT_CYCLE_SECONDS);
	if (t < 0.0)
		t = 0.0;

	return (float)fmod(PHASE_START + t, 1.0);
}

static float
DN_DaylightAt(float p)
{
	if (p >= PHASE_DAWN_BEGIN || p < PHASE_DAY_BEGIN) {
		const float r = (p >= PHASE_DAWN_BEGIN) ? p - PHASE_DAWN_BEGIN : p + (1.0f - PHASE_DAWN_BEGIN);

		return DN_Smooth(r / ((1.0f - PHASE_DAWN_BEGIN) + PHASE_DAY_BEGIN));
	}

	if (p <= PHASE_DUSK_BEGIN)
		return 1.0f;

	if (p < PHASE_NIGHT_BEGIN)
		return 1.0f - DN_Smooth((p - PHASE_DUSK_BEGIN) / (PHASE_NIGHT_BEGIN - PHASE_DUSK_BEGIN));

	return 0.0f;
}

float
DayNight_Daylight(void)
{
	if (!g_day_night)
		return 1.0f;

	return DN_DaylightAt(DayNight_Phase());
}

bool
DayNight_IsNight(void)
{
	return g_day_night && DayNight_Daylight() < 0.5f;
}

const char *
DayNight_Name(void)
{
	const float d = DayNight_Daylight();
	const float p = DayNight_Phase();

	if (d >= 0.97f) return "Day";
	if (d <= 0.03f) return "Night";
	return (p > 0.3f && p < PHASE_DAWN_BEGIN) ? "Dusk" : "Dawn";
}

int
DayNight_VisionLimit(void)
{
	if (!DayNight_IsNight())
		return 0;

	return (Sandstorm_GetStrength() >= 0.25f) ? 2 : 0;
}

/*--------------------------------------------------------------*/

/* Night turns fog of war on, and day gives the player's own setting back. */

static bool s_fogSaved = false;
static bool s_fogOriginal = false;

void
DayNight_RestoreFog(void)
{
	if (!s_fogSaved)
		return;

	enhancement_fog_of_war = s_fogOriginal;
	s_fogSaved = false;
}

void
DayNight_Tick(void)
{
	if (DayNight_IsNight()) {
		if (!s_fogSaved) {
			s_fogOriginal = enhancement_fog_of_war;
			s_fogSaved = true;
		}

		enhancement_fog_of_war = true;
	} else {
		DayNight_RestoreFog();
	}
}

/*--------------------------------------------------------------*/

/* The direction shadows fall, as a unit-ish vector, and how long they are. */
static void
DN_Sun(float *sx, float *sy, float *length)
{
	float p = DayNight_Phase();

	if (p >= PHASE_DAWN_BEGIN)
		p -= 1.0f;

	/* 0 at sunrise, 1 at sunset. */
	const float u = DN_Clamp01((p + (1.0f - PHASE_DAWN_BEGIN)) / (PHASE_NIGHT_BEGIN + (1.0f - PHASE_DAWN_BEGIN)));

	*sx = -cosf(PI_F * u);                          /* Left in the morning, right in the evening. */
	*sy = 0.55f;
	*length = 0.8f + 1.9f * (1.0f - sinf(PI_F * u)); /* Shortest at noon, long at the ends of the day. */
}

static void
DN_Viewport(int *w, int *h, int *ox, int *oy)
{
	const WidgetInfo *wi = &g_table_gameWidgetInfo[GAME_WIDGET_VIEWPORT];

	*w = wi->width;
	*h = wi->height;
	*ox = g_widgetProperties[WINDOWID_VIEWPORT].xBase;
	*oy = g_widgetProperties[WINDOWID_VIEWPORT].yBase;
}

static bool
DN_IsHidden(tile32 position)
{
	return g_mapVisible[Tile_PackTile(position)].fogOverlayBits == 0xF;
}

static bool
DN_IsSolidStructure(enum StructureType type)
{
	return type != STRUCTURE_SLAB_1x1 && type != STRUCTURE_SLAB_2x2 && type != STRUCTURE_WALL;
}

/* A pixel-art ellipse, a row at a time. */
static void
DN_Ellipse(float cx, float cy, float rx, float ry, unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
	const int rows = (int)(ry * 2.0f + 0.5f);

	for (int row = 0; row < rows; row++) {
		const float dy = (row + 0.5f - ry) / ry;
		const float half = rx * sqrtf(fmaxf(0.0f, 1.0f - dy * dy));

		if (half < 0.5f)
			continue;

		Prim_FillRect_RGBA(floorf(cx - half), floorf(cy - ry) + row, floorf(cx + half), floorf(cy - ry) + row + 1, r, g, b, a);
	}
}

void
DayNight_DrawShadows(void)
{
	if (!g_day_night)
		return;

	const float d = DayNight_Daylight();
	if (d < 0.08f)
		return;

	int w, h, ox, oy;
	DN_Viewport(&w, &h, &ox, &oy);

	float sx, sy, length;
	DN_Sun(&sx, &sy, &length);

	const unsigned char alpha = (unsigned char)(62.0f * d);   /* Faint: about half as dark as before. */

	if (!Prim_LayerBegin(PRIM_LAYER_SHADOW, w, h, 0, 0, 0, 0))
		return;

	/* The last shape wins, so shadows that overlap do not get darker. */
	Prim_SetBlend(PRIM_BLEND_REPLACE);

	PoolFindStruct find;

	/* Buildings: their footprint swept along the shadow. */
	for (const Structure *s = Structure_FindFirst(&find, HOUSE_INVALID, STRUCTURE_INVALID);
			s != NULL;
			s = Structure_FindNext(&find)) {
		if (s->o.flags.s.isNotOnMap || !DN_IsSolidStructure(s->o.type) || DN_IsHidden(s->o.position))
			continue;

		int x, y;
		if (!Map_IsPositionInViewport(s->o.position, &x, &y))
			continue;

		const XYSize *size = &g_table_structure_layoutSize[g_table_structureInfo[s->o.type].layout];
		const int tw = size->width, th = size->height;
		const float height = 2.0f + 1.5f * (float)((tw > th) ? tw : th);   /* Low, just a short shadow beside the building. */
		const float dx = sx * length * height, dy = sy * length * height;
		const int steps = (int)ceilf(fmaxf(fabsf(dx), fabsf(dy)));

		for (int i = 1; i <= steps; i++) {
			const float ox2 = dx * i / steps, oy2 = dy * i / steps;

			Prim_FillRect_RGBA(floorf(x + ox2), floorf(y + oy2), floorf(x + ox2) + tw * 16, floorf(y + oy2) + th * 16,
					0, 0, 0, alpha);
		}
	}

	/* ... but not on the buildings themselves. */
	for (const Structure *s = Structure_FindFirst(&find, HOUSE_INVALID, STRUCTURE_INVALID);
			s != NULL;
			s = Structure_FindNext(&find)) {
		if (s->o.flags.s.isNotOnMap || !DN_IsSolidStructure(s->o.type) || DN_IsHidden(s->o.position))
			continue;

		int x, y;
		if (!Map_IsPositionInViewport(s->o.position, &x, &y))
			continue;

		const XYSize *size = &g_table_structure_layoutSize[g_table_structureInfo[s->o.type].layout];

		Prim_FillRect_RGBA(x, y, x + size->width * 16, y + size->height * 16, 0, 0, 0, 0);
	}

	/* Units: a squashed shadow smeared along the sun's direction. */
	for (const Unit *u = Unit_FindFirst(&find, HOUSE_INVALID, UNIT_INVALID);
			u != NULL;
			u = Unit_FindNext(&find)) {
		const UnitInfo *ui = &g_table_unitInfo[u->o.type];

		if (u->o.flags.s.isNotOnMap || u->o.type == UNIT_SANDWORM || ui->movementType == MOVEMENT_WINGER)
			continue;

		if (DN_IsHidden(u->o.position))
			continue;

		int x, y;
		if (!Map_IsPositionInViewport(u->o.position, &x, &y))
			continue;

		const bool foot = (ui->movementType == MOVEMENT_FOOT);
		const float rx = foot ? 3.0f : 7.0f;
		const float ry = foot ? 2.0f : 4.0f;
		const float reach = (foot ? 5.0f : 8.0f) * length;

		for (int i = 0; i <= 3; i++) {
			const float t = (float)i / 3.0f;

			DN_Ellipse(x + sx * reach * t, y + 2.0f + sy * reach * t, rx, ry, 0, 0, 0, alpha);
		}
	}

	Prim_LayerEnd();
	Prim_LayerDraw(PRIM_LAYER_SHADOW, ox, oy);
}

/*--------------------------------------------------------------*/

enum LightKind {
	LIGHT_HEADLIGHT,
	LIGHT_FOOT,
	LIGHT_FLYER,
	LIGHT_BUILDING,
	LIGHT_EXPLOSION
};

typedef struct Light {
	int x, y;
	float dx, dy;   /* Facing, for the headlights. */
	int size;       /* Buildings: the larger side, in tiles. */
	enum LightKind kind;
} Light;

#define MAX_LIGHTS 400

static Light s_lights[MAX_LIGHTS];
static int s_numLights;

static void
DN_AddLight(int x, int y, float dx, float dy, int size, enum LightKind kind)
{
	if (s_numLights >= MAX_LIGHTS)
		return;

	Light *l = &s_lights[s_numLights++];

	l->x = x; l->y = y; l->dx = dx; l->dy = dy; l->size = size; l->kind = kind;
}

static void
DN_GatherLights(void)
{
	PoolFindStruct find;

	s_numLights = 0;

	for (const Unit *u = Unit_FindFirst(&find, HOUSE_INVALID, UNIT_INVALID);
			u != NULL;
			u = Unit_FindNext(&find)) {
		const UnitInfo *ui = &g_table_unitInfo[u->o.type];

		if (u->o.flags.s.isNotOnMap || u->o.type == UNIT_SANDWORM || DN_IsHidden(u->o.position))
			continue;

		int x, y;
		if (!Map_IsPositionInViewport(u->o.position, &x, &y))
			continue;

		if (ui->movementType == MOVEMENT_FOOT) {
			/* A flashlight, pointing the way the soldier faces. */
			const float a = (float)(uint8)u->orientation[0].current * 2.0f * PI_F / 256.0f;

			DN_AddLight(x, y, sinf(a), -cosf(a), 0, LIGHT_FOOT);
		} else if (ui->movementType == MOVEMENT_WINGER) {
			continue;   /* Flyers carry no light. */
		} else {
			/* Facing: 0 is up, and it turns clockwise.  Vehicles with a turret shine where it points. */
			const float a = (float)(uint8)u->orientation[ui->o.flags.hasTurret ? 1 : 0].current * 2.0f * PI_F / 256.0f;

			DN_AddLight(x, y, sinf(a), -cosf(a), 0, LIGHT_HEADLIGHT);
		}
	}

	for (const Structure *s = Structure_FindFirst(&find, HOUSE_INVALID, STRUCTURE_INVALID);
			s != NULL;
			s = Structure_FindNext(&find)) {
		if (s->o.flags.s.isNotOnMap || !DN_IsSolidStructure(s->o.type) || DN_IsHidden(s->o.position))
			continue;

		/* Without enough power the lights go out. */
		const House *house = House_Get_ByIndex(s->o.houseID);

		if (house->powerProduction < house->powerUsage)
			continue;

		int x, y;
		if (!Map_IsPositionInViewport(s->o.position, &x, &y))
			continue;

		const XYSize *size = &g_table_structure_layoutSize[g_table_structureInfo[s->o.type].layout];

		DN_AddLight(x + size->width * 8, y + size->height * 8, 0.0f, 0.0f,
				(size->width > size->height) ? size->width : size->height, LIGHT_BUILDING);
	}

	for (int i = 1; i < Explosion_Get_NumActive(); i++) {
		const Explosion *e = Explosion_Get_ByIndex(i);
		int x, y;

		if (e->spriteID == 0 || DN_IsHidden(e->position) || !Map_IsPositionInViewport(e->position, &x, &y))
			continue;

		DN_AddLight(x, y, 0.0f, 0.0f, 0, LIGHT_EXPLOSION);
	}
}

/* A soft light: many discs, each a little smaller than the last, that each take a
 * small share of the darkness away.  At the middle they add up to the full amount,
 * and towards the edge there are fewer, so it fades out evenly with no visible ring.
 * Bigger lights get more rings, to keep each step as faint as in the small ones.
 */
static void
DN_SoftLight(int cx, int cy, float radius, float amount, float darkness)
{
	int rings = (int)(radius / 4.0f);
	rings = (rings < 6) ? 6 : (rings > 14) ? 14 : rings;

	const float each = darkness * amount / rings;

	for (int i = 0; i < rings; i++) {
		const float r = radius * (1.0f - (float)i / rings);

		DN_Ellipse((float)cx, (float)cy, r, r, 0, 0, 0, (unsigned char)(each + 0.5f));
	}
}

/* Units see three times as far as they used to, and the light is much stronger. */
#define UNIT_LIGHT_RANGE 3.0f

/* The lights make holes in the darkness.  Where they overlap, they simply blend. */
static void
DN_PaintLight(const Light *l, float darkness)
{
	const int cx = l->x, cy = l->y;
	const float R = UNIT_LIGHT_RANGE;

	switch (l->kind) {
		case LIGHT_HEADLIGHT:
			/* Only the beam in front, widening and fading, with no circle round the vehicle. */

			for (int k = 0; k < 6; k++) {
				const float dist = (10.0f + 6.5f * k) * R;

				DN_SoftLight(cx + (int)floorf(l->dx * dist + 0.5f), cy + (int)floorf(l->dy * dist + 0.5f),
						(5.0f + 2.6f * k) * R, 0.50f, darkness);
			}
			break;

		case LIGHT_FOOT:
			/* Only the flashlight beam, no circle round the soldier. */

			for (int k = 0; k < 4; k++) {
				const float dist = (8.0f + 5.0f * k) * R;

				DN_SoftLight(cx + (int)floorf(l->dx * dist + 0.5f), cy + (int)floorf(l->dy * dist + 0.5f),
						(3.5f + 1.6f * k) * R, 0.55f, darkness);
			}
			break;

		case LIGHT_FLYER:
			/* No light of their own. */
			break;

		case LIGHT_BUILDING:
			/* Large: a building lights up a wide area, three times what it did. */
			DN_SoftLight(cx, cy, (30.0f + 14.0f * l->size) * 3.0f, 0.90f, darkness);
			DN_SoftLight(cx, cy, (14.0f + 7.0f * l->size) * 3.0f, 0.60f, darkness);
			break;

		case LIGHT_EXPLOSION:
			DN_SoftLight(cx, cy, 28.0f, 0.75f, darkness);
			DN_SoftLight(cx, cy, 14.0f, 0.55f, darkness);
			break;
	}
}

void
DayNight_DrawDarkness(void)
{
	static int64_t s_testStart = 0;
	static bool s_testShot = false;

	if (g_debugTestDayNight) {
		const int64_t wall = Timer_GetTicks();

		if (s_testStart == 0)
			s_testStart = wall;

		if (!s_testShot && wall - s_testStart > 3 * TICKS_PER_SECOND) {
			s_testShot = true;
			VideoA5_CaptureScreenshot();
		}

		if (s_testShot && wall - s_testStart > 5 * TICKS_PER_SECOND)
			exit(0);
	}

	if (!g_day_night)
		return;

	const float n = 1.0f - DayNight_Daylight();
	if (n < 0.02f)
		return;

	int w, h, ox, oy;
	DN_Viewport(&w, &h, &ox, &oy);

	/* The ambient colour: a warm brown in the twilight, a deep blue at night. */
	const float k = DN_Clamp01(powf(n, 0.8f));
	const unsigned char r = (unsigned char)(66.0f + (6.0f - 66.0f) * k);
	const unsigned char g = (unsigned char)(30.0f + (12.0f - 30.0f) * k);
	const unsigned char b = (unsigned char)(24.0f + (44.0f - 24.0f) * k);
	const int darkness = (int)(255.0f * 0.76f * powf(n, 1.1f));

	DN_GatherLights();

	/* Ground that is hidden is black, which would show up as black patches where
	 * the lights are.  At night it is the colour of the dark instead.
	 */
	for (int ty = 0; ty < MAP_SIZE_MAX; ty++) {
		for (int tx = 0; tx < MAP_SIZE_MAX; tx++) {
			const tile32 corner = { (uint16)(tx * 256), (uint16)(ty * 256) };
			int x, y;

			if (!Map_IsPositionInViewport(corner, &x, &y))
				continue;

			if (g_mapVisible[Tile_PackXY(tx, ty)].fogOverlayBits != 0xF)
				continue;

			Prim_FillRect_RGBA(ox + x, oy + y, ox + x + 16, oy + y + 16,
					6, 10, 34, (unsigned char)(255.0f * DN_Clamp01(n * 1.3f)));
		}
	}

	if (!Prim_LayerBegin(PRIM_LAYER_DARK, w, h, r, g, b, (unsigned char)darkness))
		return;

	/* Each light takes some of the darkness away.  Where they overlap, they blend into one. */
	Prim_SetBlend(PRIM_BLEND_PUNCH);
	for (int i = 0; i < s_numLights; i++)
		DN_PaintLight(&s_lights[i], (float)darkness);

	Prim_LayerEnd();
	Prim_LayerDraw(PRIM_LAYER_DARK, ox, oy);
}

/*--------------------------------------------------------------*/

#define BAR_W 32

/* A sun or moon, and a bar for the whole day with a marker where we are now.
 * x, y is its top left corner in the top bar.
 */
void
DayNight_DrawIndicator(int x, int y)
{
	const float d = DayNight_Daylight();
	const float p = DayNight_Phase();

	if (d >= 0.5f) {
		/* Sun. */
		DN_Ellipse(x + 4.5f, y + 4.5f, 3.5f, 3.5f, 255, 214, 70, 255);
		Prim_FillRect_RGBA(x + 4, y, x + 5, y + 1, 255, 170, 40, 255);
		Prim_FillRect_RGBA(x + 4, y + 8, x + 5, y + 9, 255, 170, 40, 255);
		Prim_FillRect_RGBA(x, y + 4, x + 1, y + 5, 255, 170, 40, 255);
		Prim_FillRect_RGBA(x + 8, y + 4, x + 9, y + 5, 255, 170, 40, 255);
	} else {
		/* Moon, with a few craters. */
		DN_Ellipse(x + 4.5f, y + 4.5f, 4.0f, 4.0f, 196, 206, 245, 255);
		Prim_FillRect_RGBA(x + 3, y + 3, x + 4, y + 4, 140, 150, 200, 255);
		Prim_FillRect_RGBA(x + 5, y + 5, x + 7, y + 6, 140, 150, 200, 255);
	}

	/* The day: night, dawn, day, dusk, night, starting at the beginning of dawn. */
	const int bx = x + 12, by = y + 3;

	Prim_FillRect_RGBA(bx - 1, by - 1, bx + BAR_W + 1, by + 5, 0, 0, 0, 255);

	for (int i = 0; i < BAR_W; i++) {
		const float pi = fmodf(PHASE_DAWN_BEGIN + (float)i / BAR_W, 1.0f);
		const float di = DN_DaylightAt(pi);
		unsigned char cr, cg, cb;

		if (di >= 0.97f) { cr = 250; cg = 206; cb = 84; }
		else if (di <= 0.03f) { cr = 22; cg = 28; cb = 84; }
		else { cr = 226; cg = 112; cb = 58; }

		Prim_FillRect_RGBA(bx + i, by, bx + i + 1, by + 4, cr, cg, cb, 255);
	}

	/* Where we are. */
	float m = fmodf(p - PHASE_DAWN_BEGIN + 1.0f, 1.0f);
	const int mx = bx + (int)(m * BAR_W);

	Prim_FillRect_RGBA(mx, by - 2, mx + 2, by + 6, 0, 0, 0, 255);
	Prim_FillRect_RGBA(mx, by - 1, mx + 1, by + 5, 255, 255, 255, 255);
}
