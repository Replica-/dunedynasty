/** @file src/fog.c Fog of war drawn by the graphics card.
 *
 * Instead of a sprite for every tile edge, the fog is a small picture with one
 * pixel for each tile, stretched over the viewport with smooth filtering.  So
 * its edges are soft, and it can have any strength.
 *
 * Each tile holds how thick its fog is.  Ground that is in view clears quickly.
 * Ground that has gone out of view fogs over slowly, and the fog creeps in the
 * more slowly the darker it is, so at night the dark closes in behind a unit
 * rather than switching on.  Ground that has never been seen is black.
 */

#include <math.h>
#include <stdio.h>

#include "fog.h"

#include "daynight.h"
#include "gui/gui.h"
#include "sprites.h"
#include "enhancement.h"
#include "gui/widget.h"
#include "map.h"
#include "scenario.h"
#include "table/widgetinfo.h"
#include "timer/timer.h"
#include "tools/coord.h"
#include "video/prim.h"
#include "video/video_a5.h"

#define TICKS_PER_SECOND 60

#define FOG_DAY 0.55f            /* How dark ground out of view is by day. */
#define FOG_NIGHT 0.75f          /* ... and at night: lighter than the dark, so explored ground still shows. */
#define CLEAR_SECONDS 0.3f       /* How long ground takes to clear when it comes into view. */
#define CREEP_DAY_SECONDS 1.5f   /* How long it takes to fog over, by day. */
#define CREEP_NIGHT_SECONDS 12.0f

static float s_density[MAP_SIZE_MAX * MAP_SIZE_MAX];    /* 0 clear, up to 1 black. */
static bool s_start = true;
#define ALL_FOG 1.0f
static int64_t s_lastTick = 0;

bool
Fog_Active(void)
{
	return VideoA5_HardwareEffectsActive();
}

void
Fog_Reset(void)
{
	s_start = true;

	/* The edges of the map are never updated, and are black. */
	for (int i = 0; i < MAP_SIZE_MAX * MAP_SIZE_MAX; i++)
		s_density[i] = ALL_FOG;
}

static float
Fog_Night(void)
{
	return 1.0f - DayNight_Daylight();
}

static float
Fog_Strength(float night)
{
	return FOG_DAY + (FOG_NIGHT - FOG_DAY) * night;
}

void
Fog_Update(void)
{
	const int64_t now = Timer_GetTicks();
	float dt = (float)(now - s_lastTick) / TICKS_PER_SECOND;

	s_lastTick = now;

	/* The first time, or after the game has been away: no creeping, just as it is. */
	const bool jump = s_start || dt < 0.0f || dt > 1.0f;
	s_start = false;

	const float night = Fog_Night();
	const float strength = Fog_Strength(night);
	const float creep = CREEP_DAY_SECONDS + (CREEP_NIGHT_SECONDS - CREEP_DAY_SECONDS) * night;
	const float slow = strength * dt / creep;
	const float fast = strength * dt / CLEAR_SECONDS;

	for (uint16 packed = 65; packed < MAP_SIZE_MAX * MAP_SIZE_MAX - 65; packed++) {
		FogOfWarTile *f = &g_mapVisible[packed];
		float *d = &s_density[packed];

		if (!Map_IsUnveiledToHouse(g_playerHouseID, packed)) {
			*d = 1.0f;
			f->fogOverlayBits = 0xF;
			continue;
		}

		const Tile *t = &g_map[packed];
		const bool inView = !enhancement_fog_of_war || (f->timeout[g_playerHouseID] > g_timerGame);
		const float target = inView ? 0.0f : strength;

		if (inView) {
			/* What is in view is what the map shows now. */
			f->groundSpriteID = t->groundSpriteID;
			f->overlaySpriteID = t->overlaySpriteID;
			f->houseID = t->houseID;
			f->hasStructure = t->hasStructure;
		}

		if (jump || *d >= 1.0f) {
			*d = target;
		} else if (*d < target) {
			*d = fminf(target, *d + slow);
		} else if (*d > target) {
			*d = fmaxf(target, *d - (inView ? fast : slow));
		}

		/* The game hides what is under the fog once it is mostly in. */
		f->fogOverlayBits = (enhancement_fog_of_war && *d >= 0.5f * strength) ? 0xF : 0;
	}
}

void
Fog_Draw(void)
{
	if (!Fog_Active())
		return;

	const WidgetInfo *wi = &g_table_gameWidgetInfo[GAME_WIDGET_VIEWPORT];
	const int w = wi->width, h = wi->height;
	const int ox = g_widgetProperties[WINDOWID_VIEWPORT].xBase;
	const int oy = g_widgetProperties[WINDOWID_VIEWPORT].yBase;
	const int x0 = Tile_GetPackedX(g_viewportPosition);
	const int y0 = Tile_GetPackedY(g_viewportPosition);

	/* The tiles in view, from the one that is partly scrolled off to the one at the far edge. */
	const int cols = (w + g_viewport_scrollOffsetX + TILE_SIZE - 1) / TILE_SIZE;
	const int rows = (h + g_viewport_scrollOffsetY + TILE_SIZE - 1) / TILE_SIZE;
	static float corner[(160 + 1) * (100 + 1)];

	if (cols > 160 || rows > 100)
		return;

	/* Ground never seen is black.  So that the outline of the ground that has been seen is not
	 * blocky, the black is spread out first: each tile takes a share of its neighbours' (twice
	 * over, for a wide, round falloff), and the fog starts out solid at the edge of the seen ground
	 * and fades away over the next tile or two.
	 */
	enum { MARGIN = 3, GW = 160 + 2 * MARGIN + 2, GH = 100 + 2 * MARGIN + 2 };
	static float unseen[2][GW * GH];
	const int gw = cols + 2 * MARGIN + 2, gh = rows + 2 * MARGIN + 2;

	for (int j = 0; j < gh; j++) {
		for (int i = 0; i < gw; i++) {
			const int tx = x0 + i - 1 - MARGIN, ty = y0 + j - 1 - MARGIN;
			const bool out = (tx < 0 || ty < 0 || tx >= MAP_SIZE_MAX || ty >= MAP_SIZE_MAX);

			unseen[0][j * gw + i] = (out || s_density[Tile_PackXY(tx, ty)] >= 1.0f) ? 1.0f : 0.0f;
		}
	}

	/* Seen ground that has black on two or more sides is a notch or a narrow strip, which would
	 * otherwise be left as a black block with lit ground right up to it.  Give it a share of black.
	 */
	{
		static float seenCopy[GW * GH];

		for (int k = 0; k < gw * gh; k++)
			seenCopy[k] = unseen[0][k];

		for (int j = 1; j < gh - 1; j++) {
			for (int i = 1; i < gw - 1; i++) {
				if (seenCopy[j * gw + i] >= 1.0f)
					continue;

				const int n4 = (int)(seenCopy[j * gw + i - 1] + seenCopy[j * gw + i + 1]
						+ seenCopy[(j - 1) * gw + i] + seenCopy[(j + 1) * gw + i] + 0.5f);

				if (n4 >= 2)
					unseen[0][j * gw + i] = fminf(1.0f, 0.28f * n4);
			}
		}
	}

	for (int pass = 0; pass < 2; pass++) {
		const float *src = unseen[pass];
		float *dst = unseen[1 - pass];

		for (int j = 0; j < gh; j++) {
			for (int i = 0; i < gw; i++) {
				float sum = 0.0f;

				for (int dj = -1; dj <= 1; dj++) {
					for (int di = -1; di <= 1; di++) {
						const int ii = (i + di < 0) ? 0 : (i + di >= gw) ? gw - 1 : i + di;
						const int jj = (j + dj < 0) ? 0 : (j + dj >= gh) ? gh - 1 : j + dj;

						sum += src[jj * gw + ii];
					}
				}

				dst[j * gw + i] = sum / 9.0f;
			}
		}
	}

	/* After two passes the result is back in unseen[0]. */
	for (int j = 0; j <= rows; j++) {
		for (int i = 0; i <= cols; i++) {
			float fogged = 0.0f, black = 0.0f;
			int hidden = 0;

			for (int k = 0; k < 4; k++) {
				const int tx = x0 + i - 1 + (k & 1), ty = y0 + j - 1 + (k >> 1);
				const float d = (tx >= 0 && ty >= 0 && tx < MAP_SIZE_MAX && ty < MAP_SIZE_MAX)
					? s_density[Tile_PackXY(tx, ty)] : 1.0f;

				if (d >= 1.0f)
					hidden++;
				else
					fogged += d * 0.25f;

				black += unseen[0][(j + k / 2 + MARGIN) * gw + (i + (k & 1) + MARGIN)] * 0.25f;
			}

			float edge = fminf(1.0f, black * 1.6f);

			edge = edge * edge * (3.0f - 2.0f * edge);
			/* The ground that has not been seen is drawn under the fog, so inside it the fog is solid. */
			corner[j * (cols + 1) + i] = (hidden == 4) ? 1.0f : fmaxf(fogged, edge);
		}
	}

	/* At night the fog is the colour of the dark. */
	const float night = Fog_Night();

	Prim_AlphaGrid(cols, rows, corner, (float)(ox - g_viewport_scrollOffsetX), (float)(oy - g_viewport_scrollOffsetY), (float)TILE_SIZE,
			(unsigned char)(6.0f * night), (unsigned char)(10.0f * night), (unsigned char)(34.0f * night),
			(float)ox, (float)oy, (float)(ox + w), (float)(oy + h));
}

void
Fog_DrawBackground(void)
{
	if (!Fog_Active())
		return;

	const WidgetInfo *wi = &g_table_gameWidgetInfo[GAME_WIDGET_VIEWPORT];
	const int ox = g_widgetProperties[WINDOWID_VIEWPORT].xBase;
	const int oy = g_widgetProperties[WINDOWID_VIEWPORT].yBase;
	const float night = Fog_Night();

	/* Ground that has not been seen is not drawn at all, so this is what shows there.  It is the
	 * colour of the fog over it, or the fog would be a different colour from the black where a
	 * light cuts through the dark, and show as blocks.
	 */
	Prim_FillRect_RGBA((float)ox, (float)oy, (float)(ox + wi->width), (float)(oy + wi->height),
			(unsigned char)(6.0f * night), (unsigned char)(10.0f * night), (unsigned char)(34.0f * night), 255);
}
