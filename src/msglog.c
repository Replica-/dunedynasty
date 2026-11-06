/** @file src/msglog.c A floating message log.
 *
 * Instead of the status bar, the last few messages float over the top left of the
 * viewport on a dark, see-through panel.  Each has the game time it came at.  They
 * fade out after a while, and when the list is full the oldest is already half gone.
 */

#include <stdio.h>
#include <string.h>

#include "msglog.h"

#include "common_a5.h"
#include "gfx.h"
#include "gui/font.h"
#include "gui/gui.h"
#include "timer/timer.h"
#include "video/prim.h"

#define MAX_ENTRIES 5
#define STORED 16
#define LIFE_TICKS (16 * 60)
#define FADE_TICKS (4 * 60)         /* The last of its life, it fades. */
#define SAME_TICKS (3 * 60)         /* The same message again within this is not added. */
#define TEXT_STYLE 0x22
#define LINE_H 10

bool g_floating_log = true;

typedef struct Entry {
	int64_t born;                   /* Timer_GetTicks(). */
	int seconds;                    /* Game time. */
	char text[96];
} Entry;

static Entry s_entries[STORED];
static int s_count;                 /* The newest are at the end. */
static int64_t s_scenarioStart = -1;
static bool s_muted;

float
MsgLog_MenubarHeight(void)
{
	return g_floating_log ? 17.0f : 40.0f;
}

void
MsgLog_Mute(bool mute)
{
	s_muted = mute;
}

static void
MsgLog_CheckScenario(void)
{
	if (s_scenarioStart != g_tickScenarioStart) {
		s_scenarioStart = g_tickScenarioStart;
		s_count = 0;
	}
}

void
MsgLog_Add(const char *text)
{
	if (s_muted || text == NULL || text[0] == '\0')
		return;

	MsgLog_CheckScenario();

	const int64_t now = Timer_GetTicks();

	if (s_count > 0 && strcmp(s_entries[s_count - 1].text, text) == 0
			&& now - s_entries[s_count - 1].born < SAME_TICKS)
		return;

	if (s_count == STORED) {
		memmove(&s_entries[0], &s_entries[1], sizeof(s_entries[0]) * (STORED - 1));
		s_count--;
	}

	Entry *e = &s_entries[s_count++];

	e->born = now;
	e->seconds = (g_timerGame > g_tickScenarioStart) ? (int)((g_timerGame - g_tickScenarioStart) / 60) : 0;
	snprintf(e->text, sizeof(e->text), "%s", text);
}

void
MsgLog_Draw(void)
{
	if (!g_floating_log)
		return;

	MsgLog_CheckScenario();

	const int64_t now = Timer_GetTicks();
	const Entry *shown[MAX_ENTRIES];
	float alpha[MAX_ENTRIES];
	int n = 0;

	/* The newest few that are still alive, oldest first. */
	int first = s_count;

	while (first > 0 && s_count - first < MAX_ENTRIES && now - s_entries[first - 1].born < LIFE_TICKS)
		first--;

	for (int i = first; i < s_count; i++) {
		const int64_t left = LIFE_TICKS - (now - s_entries[i].born);

		shown[n] = &s_entries[i];
		alpha[n] = (left < FADE_TICKS) ? (float)left / FADE_TICKS : 1.0f;
		n++;
	}

	if (n == 0)
		return;

	/* A full list is losing its oldest message: it is already fading. */
	if (n == MAX_ENTRIES && alpha[0] > 0.4f)
		alpha[0] = 0.4f;

	const enum ScreenDivID old_div = A5_SaveTransform();
	const ScreenDiv *hud = &g_screenDiv[SCREENDIV_HUD];
	const ScreenDiv *viewport = &g_screenDiv[SCREENDIV_VIEWPORT];

	A5_UseTransform(SCREENDIV_HUD);

	const int x = 6;
	const int y = (int)(viewport->y / hud->scaley) + 5;
	char line[MAX_ENTRIES][16];
	int width = 0;
	float peak = 0.0f;

	GUI_DrawText_Wrapper(NULL, 0, 0, 15, 0, TEXT_STYLE);

	for (int i = 0; i < n; i++) {
		snprintf(line[i], sizeof(line[i]), "%d:%02d ", shown[i]->seconds / 60, shown[i]->seconds % 60);

		const int w = Font_GetStringWidth(line[i]) + Font_GetStringWidth(shown[i]->text);

		if (w > width)
			width = w;
		if (alpha[i] > peak)
			peak = alpha[i];
	}

	/* The dark panel behind. */
	Prim_FillRect_RGBA(x - 4, y - 3, x + width + 4, y + n * LINE_H + 2, 0, 0, 0, (unsigned char)(130.0f * peak));

	for (int i = 0; i < n; i++) {
		const unsigned char a = (unsigned char)(255.0f * alpha[i]);
		const int ly = y + i * LINE_H;

		GUI_DrawText_Wrapper(NULL, 0, 0, 29, 0, TEXT_STYLE);
		GUI_DrawTextAlpha(line[i], x, ly, a);

		GUI_DrawText_Wrapper(NULL, 0, 0, 15, 0, TEXT_STYLE);
		GUI_DrawTextAlpha(shown[i]->text, x + Font_GetStringWidth(line[i]), ly, a);
	}

	A5_UseTransform(old_div);
}
