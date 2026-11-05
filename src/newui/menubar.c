/* menubar.c */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "enum_string.h"
#include "../os/common.h"
#include "../os/math.h"
#include "../os/sleep.h"

#include "menubar.h"

#include "mentat.h"
#include "savemenu.h"
#include "scrollbar.h"
#include "slider.h"
#include "viewport.h"
#include "../audio/audio.h"
#include "../daynight.h"
#include "../sandstorm.h"
#include "../common_a5.h"
#include "../config.h"
#include "../enhancement.h"
#include "../gfx.h"
#include "../gui/font.h"
#include "../gui/gui.h"
#include "../gui/mentat.h"
#include "../gui/widget.h"
#include "../input/input.h"
#include "../input/mouse.h"
#include "../net/net.h"
#include "../opendune.h"
#include "../pool/pool.h"
#include "../pool/pool_structure.h"
#include "../pool/pool_unit.h"
#include "../scenario.h"
#include "../sprites.h"
#include "../string.h"
#include "../table/widgetinfo.h"
#include "../timer/timer.h"
#include "../tools/random_xorshift.h"
#include "../video/video.h"
#include "../video/video_a5.h"
#include "../wsa.h"
#include "../mods/multiplayer.h"

static enum {
	RADAR_ANIMATION_NONE,
	RADAR_ANIMATION_ACTIVATE,
	RADAR_ANIMATION_DEACTIVATE
} radar_animation_state;

static int64_t radar_animation_timer;
static char s_modal_message_buf[768];
static enum ShapeID s_modal_message_shapeID;
static int s_save_entry;

void
MenuBar_DrawCredits(int credits_new, int credits_old, int offset, int x0)
{
	const int digit_w = 10;
	const int y = g_widgetProperties[WINDOWID_CREDITS].yBase;

	char char_old[7];
	char char_new[7];

	snprintf(char_old, sizeof(char_old), "%6d", credits_old);
	snprintf(char_new, sizeof(char_new), "%6d", credits_new);

	for (int i = 0; i < 6; i++) {
		const enum ShapeID shape_old = SHAPE_CREDITS_NUMBER_0 + char_old[i] - '0';
		const enum ShapeID shape_new = SHAPE_CREDITS_NUMBER_0 + char_new[i] - '0';
		const int x = x0 - digit_w * (6 - i);

		if (char_old[i] != char_new[i]) {
			if (char_old[i] != ' ')
				Shape_Draw(shape_old, x, y + offset, 0, 0);

			if (char_new[i] != ' ')
				Shape_Draw(shape_new, x, y + 8 + offset, 0, 0);
		} else {
			if (char_new[i] != ' ')
				Shape_Draw(shape_new, x, y + 1, 0, 0);
		}
	}
}

void
MenuBar_DrawStatusBar(const char *line1, const char *line2,
		bool scrollInProgress, int x, int y, int offset)
{
	const enum ScreenDivID divID = A5_SaveTransform();
	const ScreenDiv *div = &g_screenDiv[divID];
	const int h = g_widgetProperties[WINDOWID_STATUSBAR].height;

	Video_SetClippingArea(0, div->scaley * y + div->y, TRUE_DISPLAY_WIDTH, div->scaley * h);

	if (scrollInProgress) {
		GUI_DrawText_Wrapper(line2, x, y - offset + 2, 12, 0, 0x012);
		GUI_DrawText_Wrapper(line1, x, y - offset + 13, 12, 0, 0x012);
	} else {
		GUI_DrawText_Wrapper(line1, x, y - offset + 2, 12, 0, 0x012);
	}

	Video_SetClippingArea(0, 0, TRUE_DISPLAY_WIDTH, TRUE_DISPLAY_HEIGHT);
}

static bool
MenuBar_DrawRadarAnimation(void)
{
	if (radar_animation_state == RADAR_ANIMATION_NONE)
		return false;

	const int64_t curr_ticks = Timer_GetTicks();

	if (curr_ticks - radar_animation_timer >= RADAR_ANIMATION_FRAME_COUNT * RADAR_ANIMATION_DELAY) {
		radar_animation_state = RADAR_ANIMATION_NONE;
		return false;
	}

	const int x = g_widgetProperties[WINDOWID_MINIMAP].xBase;
	const int y = g_widgetProperties[WINDOWID_MINIMAP].yBase;

	int frame = (curr_ticks - radar_animation_timer) / RADAR_ANIMATION_DELAY;
	frame = clamp(0, frame, RADAR_ANIMATION_FRAME_COUNT);

	if (radar_animation_state == RADAR_ANIMATION_ACTIVATE)
		frame = RADAR_ANIMATION_FRAME_COUNT - frame - 1;

	Video_DrawWSAStatic(frame, x, y);
	return true;
}

/* Harvester and sandworm counters, next to the credits.  The harvester
 * count turns red above 3, the most a refinery set-up needs.
 */
static void
MenuBar_DrawUnitCounters(const ScreenDiv *menubar)
{
	const int max_harvesters = 3;
	PoolFindStruct find;
	int harvesters = 0;
	int sandworms = 0;

	for (const Unit *u = Unit_FindFirst(&find, g_playerHouseID, UNIT_HARVESTER);
			u != NULL;
			u = Unit_FindNext(&find)) {
		harvesters++;
	}

	for (const Unit *u = Unit_FindFirst(&find, HOUSE_INVALID, UNIT_SANDWORM);
			u != NULL;
			u = Unit_FindNext(&find)) {
		sandworms++;
	}

	/* Worms are a threat once they go for the harvesters. */
	const char *threat = "None";
	uint8 threat_fg = 29;

	if (sandworms > 0) {
		if (Unit_SandwormsHuntHarvesters(NULL)) {
			threat = "High";
			threat_fg = 6;
		} else {
			threat = "Low";
		}
	}

	const int x = menubar->width - 142;
	const uint8 harvester_fg = (harvesters > max_harvesters) ? 6 : 29;

	GUI_DrawText_Wrapper("Harvesters: %d", x, 5, harvester_fg, 0, 0x211, harvesters);
	GUI_DrawText_Wrapper("Worm Threat: %s", x - 100, 5, threat_fg, 0, 0x211, threat);

	/* The time of day: a sun or moon, a bar for the whole day with a marker, and its name. */
	if (DayNight_Active()) {
		const int ix = x - 100 - 180;

		DayNight_DrawIndicator(ix, 4);
		/* Text is positioned on a different scale from the pixels of the indicator. */
		GUI_DrawText_Wrapper("%s", x - 208, 5, 29, 0, 0x211, DayNight_Name());
	}
}

void
MenuBar_Draw(enum HouseType houseID)
{
	const ScreenDiv *menubar = &g_screenDiv[SCREENDIV_MENUBAR];
	const ScreenDiv *sidebar = &g_screenDiv[SCREENDIV_SIDEBAR];
	const enum ScreenDivID prev_transform = A5_SaveTransform();
	const WidgetProperties *statusbar = &g_widgetProperties[WINDOWID_STATUSBAR];

	Widget *w;

	/* MenuBar. */
	A5_UseTransform(SCREENDIV_MENUBAR);

	for (int x = menubar->width - 136; x + 320 - 1 >= 184; x -= 320) {
		Video_DrawCPSSpecial(CPS_MENUBAR_MIDDLE, houseID, x, 0);
	}

	for (int x = menubar->width - 8 - 425; x + 425 - 1 >= 8; x -= 425) {
		Video_DrawCPSSpecial(CPS_STATUSBAR_MIDDLE, houseID, x, 17);
	}

	Video_DrawCPSSpecial(CPS_MENUBAR_LEFT, houseID, 0, 0);
	Video_DrawCPSSpecial(CPS_MENUBAR_RIGHT, houseID, menubar->width - 136, 0);
	Video_DrawCPSSpecial(CPS_STATUSBAR_LEFT, houseID, 0, 17);
	Video_DrawCPSSpecial(CPS_STATUSBAR_RIGHT, houseID, menubar->width - 8, 17);
	Shape_DrawRemap(SHAPE_CREDITS_LABEL, houseID, menubar->width - 128, 0, 0, 0);

	/* Mentat. */
	w = GUI_Widget_Get_ByIndex(g_widgetLinkedListHead, 1);
	GUI_Widget_Draw(w);

	/* Options. */
	w = GUI_Widget_Get_ByIndex(g_widgetLinkedListHead, 2);
	GUI_Widget_Draw(w);

	Video_SetClippingArea(0, menubar->scaley * 4, TRUE_DISPLAY_WIDTH, menubar->scaley * 9);
	GUI_DrawCredits(g_playerHouse->credits, (g_playerCredits == 0xFFFF) ? 2 : 1, ceil(TRUE_DISPLAY_WIDTH / menubar->scalex));
	MenuBar_DrawUnitCounters(menubar);
	GUI_DrawStatusBarText(statusbar->xBase, statusbar->yBase);

	Video_SetClippingArea(0, 0, TRUE_DISPLAY_WIDTH, TRUE_DISPLAY_HEIGHT);

	/* SideBar. */
	A5_UseTransform(SCREENDIV_SIDEBAR);

	for (int y = sidebar->height - 85 - 52; y + 52 - 1 >= 17; y -= 52) {
		Video_DrawCPSSpecial(CPS_SIDEBAR_MIDDLE, houseID, 0, y);
	}

	Video_DrawCPSSpecial(CPS_SIDEBAR_TOP, houseID, 0, 0);
	Video_DrawCPSSpecial(CPS_SIDEBAR_BOTTOM, houseID, 0, sidebar->height - 85);
	Prim_FillRect_i(16, sidebar->height - 64, 80, sidebar->height, 0);

	if (!MenuBar_DrawRadarAnimation()) {
		GUI_Widget_Viewport_RedrawMap();
	}

	A5_UseTransform(prev_transform);
}

void
MenuBar_StartRadarAnimation(bool activate)
{
	if (enhancement_nonblocking_radar_animation
			|| (g_host_type != HOSTTYPE_NONE)) {
		radar_animation_state = activate ? RADAR_ANIMATION_ACTIVATE : RADAR_ANIMATION_DEACTIVATE;
		radar_animation_timer = Timer_GetTicks();
	} else {
		Timer_SetTimer(TIMER_GAME, false);

		for (int frame = 0; frame < RADAR_ANIMATION_FRAME_COUNT; frame++) {
			const int x = g_widgetProperties[WINDOWID_MINIMAP].xBase;
			const int y = g_widgetProperties[WINDOWID_MINIMAP].yBase;

			GUI_DrawInterfaceAndRadar();
			Video_DrawWSAStatic(activate ? RADAR_ANIMATION_FRAME_COUNT - frame - 1 : frame, x, y);
			Video_Tick();
			Timer_Sleep(RADAR_ANIMATION_DELAY);
		}

		Timer_SetTimer(TIMER_GAME, true);
	}
}

/*--------------------------------------------------------------*/

static void
MenuBar_PrepareModalMessage(enum ShapeID shapeID)
{
	WidgetProperties *w = &g_widgetProperties[WINDOWID_MODAL_MESSAGE];

	s_modal_message_shapeID = shapeID;

	GUI_DrawText_Wrapper(NULL, 0, 0, 0, 0, 0x22);

	const int padding = (shapeID == SHAPE_INVALID) ? 2*8 : 7*8;
	const int lines
		= GUI_SplitText(s_modal_message_buf, w->width - padding - 6, '\r');

	w->height = g_fontCurrent->height * max(lines, 3) + 18;

	/* Stop panning mode and show the regular cursor. */
	Viewport_Init();
	Video_ShowCursor();
	Input_History_Clear();
}

static bool
MenuBar_TickModalMessage(void)
{
	const bool narrator_speaking = Audio_Poll();

	if (Input_IsInputAvailable()) {
		const int key = Input_GetNextKey();

		if (narrator_speaking)
			return false;

		if (key == SCANCODE_MOUSE_LMB
		 || key == SCANCODE_MOUSE_RMB
		 || key == SCANCODE_ESCAPE
		 || key == SCANCODE_SPACE) {
			return true;
		}
	}

	return false;
}

static void
MenuBar_DrawModalMessage(void)
{
	const ScreenDiv *viewport = &g_screenDiv[SCREENDIV_VIEWPORT];
	const ScreenDiv *div = &g_screenDiv[SCREENDIV_MENU];

	WidgetProperties *w = &g_widgetProperties[WINDOWID_MODAL_MESSAGE];
	const int old_x = w->xBase;
	w->xBase = ((viewport->scalex * viewport->width) / div->scalex - w->width) / 2;

	GUI_Widget_DrawBorder(WINDOWID_MODAL_MESSAGE, 1, 1);
	GUI_DrawText_Wrapper(NULL, 0, 0, 0, 0, 0x22);

	if (s_modal_message_shapeID != SHAPE_INVALID) {
		Shape_Draw(s_modal_message_shapeID, 7, 8, WINDOWID_MODAL_MESSAGE, 0x4000);
		GUI_DrawText(s_modal_message_buf, w->xBase + 5*8, w->yBase + 8,
				w->fgColourBlink, 0);
	} else {
		GUI_DrawText(s_modal_message_buf, w->xBase + 1*8, w->yBase + 8,
				w->fgColourBlink, 0);
	}

	w->xBase = old_x;
}

void
MenuBar_DrawInGameOverlay(void)
{
	const enum ScreenDivID divID = A5_SaveTransform();

	Video_ShadeScreen(128);

	A5_UseTransform(SCREENDIV_MENU);

	MenuBar_DrawModalMessage();

	A5_UseTransform(divID);
}

/*--------------------------------------------------------------*/

void
GUI_DisplayHint(enum HouseType houseID,
		enum StringID stringID, enum ShapeID shapeID)
{
	if (g_debugGame
			|| stringID == STR_NULL
			|| !g_gameConfig.hints
			|| houseID != g_playerHouseID
			|| g_selectionType == SELECTIONTYPE_MENTAT
			|| g_host_type != HOSTTYPE_NONE)
		return;

	const int hint
		= stringID
		- STR_HINT_YOU_MUST_BUILD_A_WINDTRAP_TO_PROVIDE_POWER_TO_YOUR_BASE_WITHOUT_POWER_YOUR_STRUCTURES_WILL_DECAY;
	assert(hint < 64);

	const uint32 mask = (1 << (hint % 32));
	uint32 *hintsShown = (hint < 32) ? &g_hintsShown1 : &g_hintsShown2;

	if (!(*hintsShown & mask)) {
		*hintsShown |= mask;

		g_gameOverlay = GAMEOVERLAY_HINT;

		strncpy(s_modal_message_buf, String_Get_ByIndex(stringID),
				sizeof(s_modal_message_buf));
		s_modal_message_buf[sizeof(s_modal_message_buf) - 1] = '\0';

		MenuBar_PrepareModalMessage(shapeID);
	}
}

void
MenuBar_TickHintOverlay(void)
{
	if (MenuBar_TickModalMessage())
		g_gameOverlay = GAMEOVERLAY_NONE;
}

/*--------------------------------------------------------------*/

void
MenuBar_DisplayWinLose(bool win)
{
	enum VoiceID voiceID;
	enum StringID stringID;

	Audio_PlayMusic(MUSIC_STOP);
	Audio_PlayVoice(VOICE_STOP);
	Video_SetCursor(SHAPE_CURSOR_NORMAL);

	if (win) {
		g_gameOverlay = GAMEOVERLAY_WIN;
		stringID = STR_YOU_HAVE_SUCCESSFULLY_COMPLETED_YOUR_MISSION;
		voiceID = VOICE_YOUR_MISSION_IS_COMPLETE;
	} else {
		g_gameOverlay = GAMEOVERLAY_LOSE;
		stringID = STR_YOU_HAVE_FAILED_YOUR_MISSION;
		voiceID = VOICE_YOU_HAVE_FAILED_YOUR_MISSION;
	}

	Video_UngrabCursor();

	Audio_PlayVoice(voiceID);

	if (!win && g_campaign_selected == CAMPAIGNID_MULTIPLAYER && Net_HasServerRole() && Multiplayer_IsAnyHouseLeftPlaying()) {
		snprintf(
		 s_modal_message_buf,
		 sizeof(s_modal_message_buf), "%s %s",
		 String_Get_ByIndex(stringID),
		 "As host you will become observer until the other players finish the game."
		);
	}
	else {
		snprintf(s_modal_message_buf, sizeof(s_modal_message_buf), "%s",
				String_Get_ByIndex(stringID));
	}
	MenuBar_PrepareModalMessage(SHAPE_INVALID);
}

void
MenuBar_TickWinLoseOverlay(void)
{
	if (MenuBar_TickModalMessage()) {
		bool finishMap = true;

		// Multiplayer host can only end map, if all houses have either won or lost.
		if (g_campaign_selected == CAMPAIGNID_MULTIPLAYER && Net_HasServerRole() && Multiplayer_IsAnyHouseLeftPlaying()) {
			finishMap = false;
		}		

		if (finishMap) {
			g_gameMode = (g_gameOverlay == GAMEOVERLAY_WIN) ? GM_WIN : GM_LOSE;
			g_gameOverlay = GAMEOVERLAY_NONE;
			GUI_ChangeSelectionType(SELECTIONTYPE_MENTAT);
		}
		else  {
			g_gameOverlay = GAMEOVERLAY_NONE;
		}
	}
}

/*--------------------------------------------------------------*/

bool
MenuBar_ClickMentat(Widget *w)
{
	MentatState *mentat = &g_mentat_state;
	VARIABLE_NOT_USED(w);

	if (g_host_type != HOSTTYPE_NONE || g_gameOverlay != GAMEOVERLAY_NONE)
		return false;

	g_gameOverlay = GAMEOVERLAY_MENTAT;
	Sprites_InitMentat(g_table_houseInfo[g_playerHouseID].mentat);
	mentat->state = MENTAT_SHOW_CONTENTS;
	mentat->wsa = NULL;
	Video_SetCursor(SHAPE_CURSOR_NORMAL);
	g_musicInBattle = 0;
	Audio_PlayVoice(VOICE_STOP);
	Audio_PlayMusic(g_table_houseInfo[g_playerHouseID].musicBriefing);
	Timer_SetTimer(TIMER_GAME, false);
	Video_UngrabCursor();

	g_widgetLinkedListTail = NULL;
	g_widgetMentatFirst = GUI_Widget_Allocate(1, SCANCODE_ESCAPE, 200, 168, SHAPE_EXIT, 5);
	GUI_Mentat_Create_HelpScreen_Widgets();

	Widget *scrollbar = GUI_Widget_Get_ByIndex(g_widgetMentatTail, 15);
	const bool skip_advice = (g_campaign_selected == CAMPAIGNID_SKIRMISH);
	Mentat_LoadHelpSubjects(scrollbar, true, SEARCHDIR_CAMPAIGN_DIR, g_playerHouseID, g_campaignID, skip_advice);

	/* The mentat screen sits at the left of a widescreen window. */
	A5_SetMenuAlignLeft(true);

	Mouse_TransformToDiv(SCREENDIV_MENU, &g_mouseX, &g_mouseY);
	return true;
}

void
MenuBar_TickMentatOverlay(void)
{
	if (MentatHelp_Tick(&g_mentat_state)) {
		free(g_widgetMentatFirst);
		g_widgetMentatFirst = NULL;

		free(g_widgetMentatTail);
		g_widgetMentatTail = NULL;

		GUI_Widget_Free_WithScrollbar(g_widgetMentatScrollbar);
		g_widgetMentatScrollbar = NULL;

		free(g_widgetMentatScrollUp);
		g_widgetMentatScrollUp = NULL;

		free(g_widgetMentatScrollDown);
		g_widgetMentatScrollDown = NULL;

		A5_SetMenuAlignLeft(false);

		g_gameOverlay = GAMEOVERLAY_NONE;
		Timer_SetTimer(TIMER_GAME, true);
		Video_GrabCursor();
		Audio_PlayEffect(EFFECT_FADE_OUT);

		/* XXX: fix this rubbish. */
		Sprites_UnloadTiles();
		Sprites_LoadTiles();

		Audio_PlayMusic(MUSIC_RANDOM_IDLE);

		Mouse_TransformFromDiv(SCREENDIV_MENU, &g_mouseX, &g_mouseY);
	}
}

void
MenuBar_DrawMentatOverlay(void)
{
	const enum MentatID mentatID = g_table_houseInfo[g_playerHouseID].mentat;

	A5_UseTransform(SCREENDIV_MENU);

	MentatHelp_Draw(mentatID, &g_mentat_state);
}

/*--------------------------------------------------------------*/

static void
MenuBar_MakeDisabled(int widgetID)
{
	Widget *w = GUI_Widget_Get_ByIndex(g_widgetLinkedListTail, widgetID);

	GUI_Widget_MakeInvisible(w);
	w->flags.greyWhenInvisible = true;
}

bool
MenuBar_ClickOptions(Widget *w)
{
	VARIABLE_NOT_USED(w);

	if (g_gameOverlay != GAMEOVERLAY_NONE)
		return false;

	g_gameOverlay = GAMEOVERLAY_OPTIONS;
	Video_SetCursor(SHAPE_CURSOR_NORMAL);
	Timer_SetTimer(TIMER_GAME, false);
	Video_UngrabCursor();
	GUI_Window_Create(&g_optionsWindowDesc);

	if (g_host_type != HOSTTYPE_NONE) {
		MenuBar_MakeDisabled(30); /* Load a game. */
		MenuBar_MakeDisabled(31); /* Save this game. */
		MenuBar_MakeDisabled(33); /* Restart scenario. */
		MenuBar_MakeDisabled(34); /* Pick another house. */
	}

	// Disable the "Restart scenario" for skirmish games,
	// since it does not work for loaded games.
	if (g_campaign_selected == CAMPAIGNID_SKIRMISH) {
		MenuBar_MakeDisabled(33); /* Restart scenario. */
	}

	Mouse_TransformToDiv(SCREENDIV_MENU, &g_mouseX, &g_mouseY);
	return true;
}

static void
MenuBar_DrawGameControlLabel(Widget *w)
{
	const WidgetProperties *wi = &g_widgetProperties[w->parentID];

	if (g_gameConfig.language == LANGUAGE_FRENCH) {
		GUI_DrawText_Wrapper(w->data, wi->xBase + 40 - 24, w->offsetY + wi->yBase + 3, 232, 0, 0x22);
	} else {
		GUI_DrawText_Wrapper(w->data, w->offsetX + wi->xBase - 10, w->offsetY + wi->yBase + 3, 232, 0, 0x222);
	}
}

static bool
MenuBar_ClickRadioButton(Widget *radio)
{
	const int visible_widgets[6][7 + 5*3 + 4]= {
		{ 26, 90, 91, 92, 93, 94, 95,
		  20, 30, 100, 21, 31, 101, 22, 32, 102, 23, 33, 103, 24, 34, 104,
		  -1
		},
		{ 26, 90, 91, 92, 93, 94, 95,
		  40, 50, 110, 41, 51, 111, 42, 52, 112, 43, 53, 113, 44, 54, 114,
		  -1
		},
		{ 26, 90, 91, 92, 93, 94, 95,
		  60, 70, 120, 61, 71, 121, 62, 72, 122, 63, 73, 123, 64, 74, 124,
		  -1
		},
		{ 26, 90, 91, 92, 93, 94, 95,
		  45, 55, 115, 46, 56, 116, 47, 57, 117, 48, 58, 49, 59,
		  -1
		},
		{ 26, 90, 91, 92, 93, 94, 95,
		  25, 35, 27, 28, 29, 19, 130, 131, 132, 133,
		  -1
		},
		{ 26, 90, 91, 92, 93, 94, 95,
		  66, 76, 67, 77,
		  -1
		},
	};

	const int page = radio->index - 90;
	Widget *w;

	w = g_widgetLinkedListTail;
	while (w != NULL) {
		bool visible = false;

		for (unsigned int i = 0; visible_widgets[page][i] >= 0; i++) {
			if (w->index == visible_widgets[page][i]) {
				visible = true;
				break;
			}
		}

		if (visible) {
			GUI_Widget_MakeVisible(w);
		} else {
			GUI_Widget_MakeInvisible(w);
		}

		if (w->clickProc == MenuBar_ClickRadioButton) {
			w->drawParameterNormal.sprite = SHAPE_RADIO_BUTTON_OFF;
			w->state.selected = 0;
		}

		w = GUI_Widget_GetNext(w);
	}

	/* Bit of a hack to make the radio button retain the ON sprite
	 * after we click something else.
	 */
	bool isCheckBox = (radio->drawParameterNormal.sprite == SHAPE_CHECKBOX_OFF)
			|| (radio->drawParameterNormal.sprite == SHAPE_CHECKBOX_ON);
	radio->drawParameterNormal.sprite = (isCheckBox)? SHAPE_CHECKBOX_ON: SHAPE_RADIO_BUTTON_ON;
	radio->state.selected = 1;

	return true;
}

static bool
MenuBar_ClickMusicVolumeSlider(Widget *w)
{
	if (Slider_Click(w)) {
		const SliderData *data = w->data;

		music_volume = (float)data->curr / (data->max - data->min);
		Audio_AdjustMusicVolume(0.0f, false);
	}

	return true;
}

static bool
MenuBar_ClickSoundVolumeSlider(Widget *w)
{
	if (Slider_Click(w) || w->state.buttonState & 0x01) {
		const enum SampleID sampleID = Random_Xorshift_Range(SAMPLE_AFFIRMATIVE, SAMPLE_MOVING_OUT);
		const SliderData *data = w->data;

		sound_volume = (float)data->curr / (data->max - data->min);
		Audio_PlaySample(sampleID, 255, 0.0f);
	}

	return true;
}

static bool
MenuBar_ClickScrollSpeedSlider(Widget *w)
{
	if (Slider_Click(w)) {
		const SliderData *data = w->data;

		g_gameConfig.scrollSpeed = (data->curr == data->min) ? 2 : (4 * data->curr);
	}

	return true;
}

static bool
MenuBar_ClickPanSensitivitySlider(Widget *w)
{
	if (Slider_Click(w)) {
		const SliderData *data = w->data;

		g_gameConfig.panSensitivity = 0.25f * data->curr;
	}

	return true;
}

static bool
MenuBar_ClickMenubarScaleSlider(Widget *w)
{
	if (Slider_Click(w)) {
		const SliderData *data = w->data;

		g_screenDiv[SCREENDIV_MENUBAR].scalex = data->curr;
		A5_InitTransform(false);
		GameLoop_TweakWidgetDimensions();
	}

	return true;
}

static bool
MenuBar_ClickSidebarScaleSlider(Widget *w)
{
	if (Slider_Click(w)) {
		const SliderData *data = w->data;

		g_screenDiv[SCREENDIV_SIDEBAR].scalex = data->curr;
		A5_InitTransform(false);
		GameLoop_TweakWidgetDimensions();
	}

	return true;
}

static bool
MenuBar_ClickViewportScaleSlider(Widget *w)
{
	if (Slider_Click(w)) {
		const SliderData *data = w->data;

		g_screenDiv[SCREENDIV_VIEWPORT].scalex = data->curr;
		A5_InitTransform(false);
		GameLoop_TweakWidgetDimensions();
	}

	return true;
}

static const int s_house_colour_presets[] = {
	0xE02020, 0xF08000, 0xF0D020, 0x30B040,
	0x20C0D0, 0x3060F0, 0xA040E0, 0xE0E0E0,

	/* Second row: darker and different shades. */
	0x801818, 0x8A5A2B, 0x808020, 0x146040,
	0x108080, 0x182880, 0xF050A0, 0x606060,
};

#define HOUSE_COLOUR_NUM_PRESETS ((int)lengthof(s_house_colour_presets))
#define HOUSE_COLOUR_PRESETS_PER_ROW 8
#define HOUSE_COLOUR_PREVIEW_W 20
#define HOUSE_COLOUR_PRESET_W 8
#define HOUSE_COLOUR_PRESET_H 6
#define HOUSE_COLOUR_PRESET_GAP 1
#define HOUSE_COLOUR_ROW_GAP 2
#define HOUSE_COLOUR_PICKER_H 14 /* Two rows of presets. */

/* Apply a custom house colour, and move the sliders to match. */
static void
MenuBar_SetHouseColour(int rgb)
{
	Video_SetCustomHouseColour(rgb);
	snprintf(g_house_colour, sizeof(g_house_colour), "%06X", rgb);

	g_gameConfig.houseColorPalette = HOUSE_COLOUR_CUSTOM;
	GUI_Palette_CreateRemap(g_playerHouseID);
	VideoA5_RebuildHousePalette(g_playerHouseID);
	GUI_Palette_RemapScreen(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, g_screenActiveID, g_remap);

	for (int i = 0; i < 3; i++) {
		Widget *slider = GUI_Widget_Get_ByIndex(g_widgetLinkedListTail, 130 + i);
		if (slider != NULL)
			((SliderData *)slider->data)->curr = ((rgb >> (16 - 8 * i)) & 0xFF) * 63 / 255;
	}
}

/* Red, green and blue sliders (indices 130-132) of the custom house colour.
 * They work in 6-bit steps (0-63), the depth of the VGA palette.
 */
static bool
MenuBar_ClickHouseColourSlider(Widget *w)
{
	if (Slider_Click(w)) {
		const SliderData *data = w->data;
		const int shift = (w->index == 130) ? 16 : (w->index == 131) ? 8 : 0;
		const int value = data->curr * 255 / 63;

		MenuBar_SetHouseColour((Video_GetCustomHouseColour() & ~(0xFF << shift)) | (value << shift));
	}

	return true;
}

/* Preview swatch of the custom colour, followed by the preset colour boxes. */
static void
MenuBar_DrawHouseColourPicker(Widget *w)
{
	const WidgetProperties *wi = &g_widgetProperties[w->parentID];
	const int x = wi->xBase + w->offsetX;
	const int y = wi->yBase + w->offsetY;
	const int current = Video_GetCustomHouseColour();

	Prim_FillRect_RGBA(x - 1, y - 1, x + HOUSE_COLOUR_PREVIEW_W + 1, y + HOUSE_COLOUR_PICKER_H + 1, 0, 0, 0, 0xFF);
	Prim_FillRect_RGBA(x, y, x + HOUSE_COLOUR_PREVIEW_W, y + HOUSE_COLOUR_PICKER_H,
			(current >> 16) & 0xFF, (current >> 8) & 0xFF, current & 0xFF, 0xFF);

	for (int i = 0; i < HOUSE_COLOUR_NUM_PRESETS; i++) {
		const int rgb = s_house_colour_presets[i];
		const int x1 = x + HOUSE_COLOUR_PREVIEW_W + 6
				+ (i % HOUSE_COLOUR_PRESETS_PER_ROW) * (HOUSE_COLOUR_PRESET_W + HOUSE_COLOUR_PRESET_GAP);
		const int y1 = y + (i / HOUSE_COLOUR_PRESETS_PER_ROW) * (HOUSE_COLOUR_PRESET_H + HOUSE_COLOUR_ROW_GAP);
		const int x2 = x1 + HOUSE_COLOUR_PRESET_W;
		const int y2 = y1 + HOUSE_COLOUR_PRESET_H;
		const bool selected = (rgb == current);

		/* Outline: white if it is the current colour, black otherwise. */
		const unsigned char outline = selected ? 0xFF : 0x00;
		Prim_FillRect_RGBA(x1 - 1, y1 - 1, x2 + 1, y2 + 1, outline, outline, outline, 0xFF);
		Prim_FillRect_RGBA(x1, y1, x2, y2,
				(rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 0xFF);
	}
}

static bool
MenuBar_ClickHouseColourPresets(Widget *w)
{
	const WidgetProperties *wi = &g_widgetProperties[w->parentID];
	const int x = wi->xBase + w->offsetX + HOUSE_COLOUR_PREVIEW_W + 6;
	const int y = wi->yBase + w->offsetY;
	const int step = HOUSE_COLOUR_PRESET_W + HOUSE_COLOUR_PRESET_GAP;
	const int rowStep = HOUSE_COLOUR_PRESET_H + HOUSE_COLOUR_ROW_GAP;
	const int dx = g_mouseX - x;
	const int dy = g_mouseY - y;

	if (dx >= 0 && dy >= 0) {
		const int col = dx / step;
		const int i = (dy / rowStep) * HOUSE_COLOUR_PRESETS_PER_ROW + col;

		if (col < HOUSE_COLOUR_PRESETS_PER_ROW && i < HOUSE_COLOUR_NUM_PRESETS
				&& (dx % step) < HOUSE_COLOUR_PRESET_W + 1
				&& (dy % rowStep) < HOUSE_COLOUR_PRESET_H + 1
				&& s_house_colour_presets[i] != Video_GetCustomHouseColour())
			MenuBar_SetHouseColour(s_house_colour_presets[i]);
	}

	return true;
}

static void
MenuBar_CreateGameControls(void)
{
	/* 20, 30, 100  -- music label, on/off, slider.
	 * 21, 31, 101  -- sound label, on/off, slider.
	 * 22, 32       -- game speed label, button.
	 * 23, 33       -- hint label, on/off.
	 * 24, 34       -- subtitles label, on/off.
	 * 25, 35       -- house colour label and cycle button.
	 * 27, 28, 29, 130, 131, 132 -- custom house colour red/green/blue labels and sliders.
	 * 19, 133      -- custom house colour preview and preset boxes.
	 * 26           -- previous.
	 *
	 * 40, 50       -- control style label, mode.
	 * 41, 51       -- mouse wheel label, mode.
	 * 42, 52       -- scrolling edge label, on/off.
	 * 43, 53, 113  -- auto scroll label, on/off, slider.
	 * 44, 54, 114  -- pan sensitivity label, slider.
	 *
	 * 45, 55, 115  -- menubar scale label, slider.
	 * 46, 56, 116  -- sidebar scale label, slider.
	 * 47, 57, 117  -- viewport scale label, slider.
	 * 48, 58       -- hardware effects label, off/on.  Only with the OpenGL driver.
	 * 49, 59       -- radio voices label, off/on.
	 * 66, 76, 67, 77 -- day and night label, off/on; sandstorms label, off/on.
	 *
	 * 60, 70   -- health bars label, off/on/always on.
	 * 61, 71   -- hi-res overlays label, off/on.
	 * 62, 72   -- smooth unit animation label, off/on.
	 * 63, 73   -- infantry squad death label, off/on.
	 * 64, 74   -- unit target lines label, off/on.
	 *
	 * 90, 91, 92, 93, 94   -- radio buttons.
	 */

	const WindowDesc *desc = &g_gameControlWindowDesc;
	Widget *w;

	GUI_Window_Create(&g_gameControlWindowDesc);

	/* Radio buttons. */
	for (int page = 0; page < 6; page++) {
		w = GUI_Widget_Allocate(90 + page, 0, 8 + 10 * page, g_widgetProperties[g_gameControlWindowDesc.index].height - 17, SHAPE_RADIO_BUTTON_OFF, STR_NULL);
		w->parentID = g_gameControlWindowDesc.index;
		w->clickProc = MenuBar_ClickRadioButton;
		w->state.selected = 0;
		g_widgetLinkedListTail = GUI_Widget_Link(g_widgetLinkedListTail, w);
	}

	/* Labels. */
	const struct {
		uint16 index;
		uint16 position;
		const char *str;
	} label[] = {
		{ 20, 0, String_Get_ByIndex(STR_MUSIC_IS) },
		{ 21, 1, String_Get_ByIndex(STR_SOUNDS_ARE) },
		{ 22, 2, String_Get_ByIndex(STR_GAME_SPEED) },
		{ 23, 3, String_Get_ByIndex(STR_HINTS_ARE) },
		{ 24, 4, "Subtitles are" },
		{ 25, 0, "House Color" },
		{ 27, 1, "Red" },
		{ 28, 2, "Green" },
		{ 29, 3, "Blue" },
		{ 19, 4, "Preview" },
		{ 40, 0, "Control style" },
		{ 41, 1, "Mouse wheel" },
		{ 42, 2, "Scrolling edge" },
		{ 43, 3, String_Get_ByIndex(STR_AUTO_SCROLL_IS) },
		{ 44, 4, "Pan sensitivity" },
		{ 45, 0, "Menubar Scale" },
		{ 46, 1, "Sidebar Scale" },
		{ 47, 2, "Viewport Scale" },
		{ 48, 3, "Hardware effects" },
		{ 49, VideoA5_HardwareEffectsAvailable() ? 4 : 3, "Radio voices" },
		{ 66, 0, "Day and night" },
		{ 67, 1, "Sandstorms" },
		{ 60, 0, "Health bars" },
		{ 61, 1, "Hi-res overlays" },
		{ 62, 2, "Smooth unit animation" },
		{ 63, 3, "Infantry squad corpses" },
		{ 64, 4, "Unit target lines" },
	};

	for (unsigned int i = 0; i < lengthof(label); i++) {
		if (label[i].index == 48 && !VideoA5_HardwareEffectsAvailable())
			continue;

		const uint16 x = desc->widgets[label[i].position].offsetX + (label[i].index >= 60 ? 40 : 0);
		const uint16 y = desc->widgets[label[i].position].offsetY;

		w = GUI_Widget_Allocate(label[i].index, 0, x, y, -2, STR_NULL);
		w->parentID = g_gameControlWindowDesc.index;
		w->data = (void *)label[i].str;
		w->drawParameterDown.proc = MenuBar_DrawGameControlLabel;
		w->drawParameterNormal.proc = MenuBar_DrawGameControlLabel;
		w->drawParameterSelected.proc = MenuBar_DrawGameControlLabel;

		g_widgetLinkedListTail = GUI_Widget_Link(g_widgetLinkedListTail, w);
	}

	/* Buttons. */
	const struct {
		uint16 index;
		uint16 position, w;
	} button[] = {
		{ 35, 0, 104 }, /* House color palette. */
		{ 50, 0, 104 }, /* Control style. */
		{ 51, 1, 104 }, /* Mouse wheel. */
		{ 52, 2, 104 }, /* Scrolling edge. */
		{ 53, 3,  46 }, /* Auto scroll. */
		{ 58, 3,  64 }, /* Hardware effects. */
		{ 59, VideoA5_HardwareEffectsAvailable() ? 4 : 3, 64 }, /* Radio voices. */
		{ 76, 0,  64 }, /* Day and night. */
		{ 77, 1,  64 }, /* Sandstorms. */
		{ 70, 0,  64 }, /* Health bars. */
		{ 71, 1,  64 }, /* Hi-res vector graphics. */
		{ 72, 2,  64 }, /* Smooth unit animation. */
		{ 73, 3,  64 }, /* Infantry squad corpses. */
		{ 74, 4,  64 }, /* Selected unit target lines. */
	};

	for (unsigned int i = 0; i < lengthof(button); i++) {
		if (button[i].index == 58 && !VideoA5_HardwareEffectsAvailable())
			continue;

		const uint16 x = desc->widgets[button[i].position].offsetX + (button[i].index >= 60 ? 40 : 0);
		const uint16 y = desc->widgets[button[i].position].offsetY;

		w = GUI_Widget_Allocate(button[i].index, 0, x, y, -2, -button[i].index);
		w->parentID = g_gameControlWindowDesc.index;
		w->flags = g_table_windowWidgets[0].flags;
		w->width = button[i].w;
		w->height = 15;
		w->drawParameterNormal.proc = GUI_Widget_TextButton_Draw;
		w->drawParameterSelected.proc = GUI_Widget_TextButton_Draw;
		w->drawParameterDown.proc = GUI_Widget_TextButton_Draw;

		g_widgetLinkedListTail = GUI_Widget_Link(g_widgetLinkedListTail, w);
	}

	w = GUI_Widget_Allocate(26, SCANCODE_ESCAPE, 96, 110, -2, STR_PREVIOUS);
	w->parentID = g_gameControlWindowDesc.index;
	w->flags = g_table_windowWidgets[0].flags;
	w->width = 136;
	w->height = 15;
	w->drawParameterNormal.proc = GUI_Widget_TextButton_Draw;
	w->drawParameterSelected.proc = GUI_Widget_TextButton_Draw;
	w->drawParameterDown.proc = GUI_Widget_TextButton_Draw;
	g_widgetLinkedListTail = GUI_Widget_Link(g_widgetLinkedListTail, w);

	/* Sliders. */
	const struct {
		uint16 index;
		uint16 x, position;
		int min, max, tics;
		bool (*clickProc)(Widget *widget);
	} slider[] = {
		{ 100, 180, 0, 0, 20, 2, MenuBar_ClickMusicVolumeSlider },
		{ 101, 180, 1, 0, 20, 2, MenuBar_ClickSoundVolumeSlider },
		{ 113, 180, 3, 0,  4, 1, MenuBar_ClickScrollSpeedSlider },
		{ 114, 131, 4, 2,  8, 1, MenuBar_ClickPanSensitivitySlider },
		{ 115, 131, 0, 1,  8, 1, MenuBar_ClickMenubarScaleSlider },
		{ 116, 131, 1, 1,  8, 1, MenuBar_ClickSidebarScaleSlider },
		{ 117, 131, 2, 1,  8, 1, MenuBar_ClickViewportScaleSlider },
		{ 130, 131, 1, 0, 63, 9, MenuBar_ClickHouseColourSlider },
		{ 131, 131, 2, 0, 63, 9, MenuBar_ClickHouseColourSlider },
		{ 132, 131, 3, 0, 63, 9, MenuBar_ClickHouseColourSlider },
		{ 133, 131, 4, 0,  1, 1, MenuBar_ClickHouseColourPresets },
	};

	for (unsigned int i = 0; i < lengthof(slider); i++) {
		const uint16 width = 50 + (180 - slider[i].x);
		const uint16 y = 24 + 17 * slider[i].position;
		SliderData *data;

		w = Slider_Allocate(slider[i].index, g_gameControlWindowDesc.index, slider[i].x, y, width,
				(slider[i].index == 133) ? HOUSE_COLOUR_PICKER_H : 12);
		w->clickProc = slider[i].clickProc;

		data = w->data;
		data->min = slider[i].min;
		data->max = slider[i].max;
		data->tics = slider[i].tics;

		switch (w->index) {
			case 100: data->curr = 20 * music_volume; break;
			case 101: data->curr = 20 * sound_volume; break;
			case 113: data->curr = clamp(data->min, (g_gameConfig.scrollSpeed / 4), data->max); break;
			case 114: data->curr = clamp(data->min, g_gameConfig.panSensitivity / 0.25f, data->max); break;
			case 115: data->curr = clamp(data->min, g_screenDiv[SCREENDIV_MENUBAR].scalex, data->max); break;
			case 116: data->curr = clamp(data->min, g_screenDiv[SCREENDIV_SIDEBAR].scalex, data->max); break;
			case 117: data->curr = clamp(data->min, g_screenDiv[SCREENDIV_VIEWPORT].scalex, data->max); break;
			case 130: data->curr = ((Video_GetCustomHouseColour() >> 16) & 0xFF) * 63 / 255; break;
			case 131: data->curr = ((Video_GetCustomHouseColour() >> 8) & 0xFF) * 63 / 255; break;
			case 132: data->curr = (Video_GetCustomHouseColour() & 0xFF) * 63 / 255; break;
			case 133:
				/* Preview and preset boxes, not a real slider. */
				data->curr = 0;
				w->drawParameterNormal.proc = MenuBar_DrawHouseColourPicker;
				w->drawParameterSelected.proc = MenuBar_DrawHouseColourPicker;
				w->drawParameterDown.proc = MenuBar_DrawHouseColourPicker;
				break;
			default: assert(false); break;
		}

		g_widgetLinkedListTail = GUI_Widget_Link(g_widgetLinkedListTail, w);
	}

	GUI_Widget_MakeSelected(GUI_Widget_Get_ByIndex(g_widgetLinkedListTail, 90), true);
}

static void
MenuBar_UninitGameControls(void)
{
	Widget *w = g_widgetLinkedListTail;
	while (w != NULL) {
		Widget *next = GUI_Widget_GetNext(w);

		/* Widgets stored in g_table_windowWidgets.  Not tested by index, as
		 * the house colour button (35) is allocated and must be freed.
		 */
		if (w >= g_table_windowWidgets && w < g_table_windowWidgets + 9) {
		} else if (w->index >= 100) { /* Sliders. */
			Slider_Free(w);
		} else { /* Regular widgets. */
			free(w);
		}

		w = next;
	}
}

static void
MenuBar_TickOptions(void)
{
	const int widgetID = GUI_Widget_HandleEvents(g_widgetLinkedListTail);
	switch (widgetID) {
		case 0x8000 | 30: /* STR_LOAD_A_GAME */
			g_gameOverlay = GAMEOVERLAY_LOAD_GAME;
			SaveMenu_InitSaveLoad(false);
			break;

		case 0x8000 | 31: /* STR_SAVE_THIS_GAME */
			g_gameOverlay = GAMEOVERLAY_SAVE_GAME;
			SaveMenu_InitSaveLoad(true);
			break;

		case 0x8000 | 32: /* STR_GAME_CONTROLS */
			g_gameOverlay = GAMEOVERLAY_GAME_CONTROLS;
			MenuBar_CreateGameControls();
			break;

		case 0x8000 | 33: /* STR_RESTART_SCENARIO */
			g_gameOverlay = GAMEOVERLAY_CONFIRM_RESTART;
			g_yesNoWindowDesc.stringID = STR_ARE_YOU_SURE_YOU_WISH_TO_RESTART;
			GUI_Window_Create(&g_yesNoWindowDesc);
			break;

		case 0x8000 | 34: /* STR_PICK_ANOTHER_HOUSE */
			g_gameOverlay = GAMEOVERLAY_CONFIRM_PICK_HOUSE;
			g_yesNoWindowDesc.stringID = STR_ARE_YOU_SURE_YOU_WISH_TO_PICK_A_NEW_HOUSE;
			GUI_Window_Create(&g_yesNoWindowDesc);
			break;

		case 0x8000 | 35: /* STR_CONTINUE_GAME */
			g_gameOverlay = GAMEOVERLAY_NONE;
			break;

		case 0x8000 | 36: /* STR_QUIT_PLAYING */
			g_gameOverlay = GAMEOVERLAY_CONFIRM_QUIT;
			g_yesNoWindowDesc.stringID = STR_ARE_YOU_SURE_YOU_WANT_TO_QUIT_PLAYING;
			GUI_Window_Create(&g_yesNoWindowDesc);
			break;

		default:
			break;
	}
}

static void
MenuBar_TickSaveLoadGame(enum GameOverlay overlay)
{
	if (overlay == GAMEOVERLAY_SAVE_ENTRY) {
		const int ret = SaveMenu_Savegame_Click(s_save_entry);

		if (ret == -1) {
			g_gameOverlay = GAMEOVERLAY_OPTIONS;
			GUI_Window_Create(&g_optionsWindowDesc);
		} else if (ret == -2) {
			g_gameOverlay = GAMEOVERLAY_NONE;
		}
	} else {
		const bool save = (overlay == GAMEOVERLAY_SAVE_GAME);
		const int ret = SaveMenu_SaveLoad_Click(save);

		if (ret == -1) {
			g_gameOverlay = GAMEOVERLAY_OPTIONS;
			GUI_Window_Create(&g_optionsWindowDesc);
		} else if (ret == -2) {
			g_gameOverlay = GAMEOVERLAY_NONE;
		} else if (ret > 0) {
			g_gameOverlay = GAMEOVERLAY_SAVE_ENTRY;
			GUI_Window_Create(&g_savegameNameWindowDesc);
			s_save_entry = ret - 0x1E;

			char *saveDesc = g_savegameDesc[s_save_entry];
			if (*saveDesc == '[') *saveDesc = '\0';
		}
	}
}

static void
MenuBar_TickGameControls(void)
{
	const int widgetID = GUI_Widget_HandleEvents(g_widgetLinkedListTail);
	Widget *w;

	switch (widgetID) {
		case 0x8000 | 30: /* STR_MUSIC_IS */
			g_enable_music = !g_enable_music;
			if (!g_enable_music)
				Audio_PlayMusic(MUSIC_STOP);
			break;

		case 0x8000 | 31: /* STR_SOUNDS_ARE */
			w = GUI_Widget_Get_ByIndex(g_widgetLinkedListTail, 31);
			if (w->state.buttonState & 0x04) {
				g_enable_sound_effects++;
				if (g_enable_sound_effects > SOUNDEFFECTS_SYNTH_AND_SAMPLES)
					g_enable_sound_effects = SOUNDEFFECTS_NONE;
			} else {
				if (g_enable_sound_effects == SOUNDEFFECTS_NONE) {
					g_enable_sound_effects = SOUNDEFFECTS_SYNTH_AND_SAMPLES;
				} else {
					g_enable_sound_effects--;
				}
			}
			g_enable_voices = !(g_enable_sound_effects == SOUNDEFFECTS_NONE || g_enable_sound_effects == SOUNDEFFECTS_SYNTH_ONLY);
			break;

		case 0x8000 | 32: /* STR_GAME_SPEED */
			w = GUI_Widget_Get_ByIndex(g_widgetLinkedListTail, 32);
			if (w->state.buttonState & 0x04) {
				if (++g_gameConfig.gameSpeed >= 5)
					g_gameConfig.gameSpeed = 0;
			} else {
				if (--g_gameConfig.gameSpeed < 0)
					g_gameConfig.gameSpeed = 4;
			}
			break;

		case 0x8000 | 33: g_gameConfig.hints ^= 0x1; break;
		case 0x8000 | 34: g_enable_subtitles ^= 0x1; break;

		case 0x8000 | 35: /* House color palette */
			w = GUI_Widget_Get_ByIndex(g_widgetLinkedListTail, 35);
			if (w->state.buttonState & 0x04) {
				if (++g_gameConfig.houseColorPalette >= 8)
					g_gameConfig.houseColorPalette = 0;
			} else {
				if (g_gameConfig.houseColorPalette == 0)
					g_gameConfig.houseColorPalette = 7;
				else
					g_gameConfig.houseColorPalette--;
			}
			GUI_Palette_CreateRemap(g_playerHouseID);
			VideoA5_RebuildHousePalette(g_playerHouseID);
			GUI_Palette_RemapScreen(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, g_screenActiveID, g_remap);
			break;

		case 0x8000 | 26: /* STR_PREVIOUS */
			g_gameOverlay = GAMEOVERLAY_OPTIONS;
			MenuBar_UninitGameControls();
			GameLoop_TweakWidgetDimensions();
			GUI_Window_Create(&g_optionsWindowDesc);
			break;

		case 0x8000 | 50: g_gameConfig.leftClickOrders ^= 0x1; break;
		case 0x8000 | 51: g_gameConfig.holdControlToZoom ^= 0x1; break;
		case 0x8000 | 52: g_gameConfig.scrollAlongScreenEdge ^= 0x1; break;
		case 0x8000 | 53: g_gameConfig.autoScroll ^= 0x1; break;
		case 0x8000 | 58: g_hardware_effects = !g_hardware_effects; break;
		case 0x8000 | 59: g_radio_voices = !g_radio_voices; break;
		case 0x8000 | 76: g_day_night = !g_day_night; break;
		case 0x8000 | 77: g_sandstorms = !g_sandstorms; break;

		case 0x8000 | 70: /* Health bars. */
			w = GUI_Widget_Get_ByIndex(g_widgetLinkedListTail, 70);
			if (w->state.buttonState & 0x04) {
				enhancement_draw_health_bars = (enhancement_draw_health_bars + 1) % NUM_HEALTH_BAR_MODES;
			} else {
				enhancement_draw_health_bars = (enhancement_draw_health_bars + NUM_HEALTH_BAR_MODES - 1) % NUM_HEALTH_BAR_MODES;
			}
			break;

		case 0x8000 | 71: enhancement_high_res_overlays ^= 0x1; break;
		case 0x8000 | 72: enhancement_smooth_unit_animation = (enhancement_smooth_unit_animation == SMOOTH_UNIT_ANIMATION_DISABLE) ? SMOOTH_UNIT_ANIMATION_ENABLE : SMOOTH_UNIT_ANIMATION_DISABLE; break;
		case 0x8000 | 73: enhancement_infantry_squad_death_animations ^= 0x1; break;
		case 0x8000 | 74: enhancement_draw_target_lines ^= 0x1; break;

		default:
			break;
	}
}

static void
MenuBar_TickConfirmation(enum GameOverlay overlay)
{
	const int widgetID = GUI_Widget_HandleEvents(g_widgetLinkedListTail);
	switch (widgetID) {
		case 0x8000 | 30: /* Yes */
			if (overlay == GAMEOVERLAY_CONFIRM_RESTART) {
				g_gameMode = GM_RESTART;
			} else if (overlay == GAMEOVERLAY_CONFIRM_PICK_HOUSE) {
				g_gameMode = GM_PICKHOUSE;
			} else {
				g_gameMode = GM_QUITGAME;
			}
			break;

		case 0x8000 | 31: /* No */
			g_gameOverlay = GAMEOVERLAY_OPTIONS;
			GUI_Window_Create(&g_optionsWindowDesc);
			break;

		default:
			break;
	}
}

/* Debug test (--test-colour-menu): open the Game Controls menu on its last
 * page, with the house colour picker, and take a screenshot of it.  It counts
 * frames, as the game is not running while the menu is open.
 *
 * --test-options-previous instead clicks the yellow preset and then Previous,
 * which hung the game (see ActionPanel_GetLocateButton).
 */
static void
MenuBar_DebugPrintMentat(const char *when)
{
	const Widget *m = GUI_Widget_Get_ByIndex(g_widgetLinkedListHead, 1);

	if (m == NULL) {
		fprintf(stdout, "test: mentat widget %s: MISSING\n", when);
	} else {
		fprintf(stdout, "test: mentat widget %s: x=%d y=%d w=%d h=%d div=%d invisible=%d\n", when,
				m->offsetX, m->offsetY, m->width, m->height, (int)m->div, (int)m->flags.invisible);
	}
	fflush(stdout);
}

void
MenuBar_DebugTestColourMenu(void)
{
	static int state = 0;
	static int frames = 0;

	frames++;

	if (g_debugTestColourMenu == 7) {
		/* --test-colour-mouse: change the house colour in the settings by clicking
		 * with the mouse (events injected as Allegro delivers them), with a unit
		 * selected, and take screenshots.
		 */
		static const struct { int frame; int what; } script[] = {
			{ 240, 100 },  /* Select a unit. */
			{ 260,   1 },  /* Options button (head list). */
			{ 285, 200 },  /* Screenshot (plain Options window). */
		{ 300,  32 },  /* Game Controls. */
			{ 340,  94 },  /* Radio button: colour page. */
			{ 380,  35 },  /* House Color button, x3. */
			{ 410,  35 },
			{ 440,  35 },
			{ 470, 133 },  /* Preset swatch. */
			{ 500, 131 },  /* Green slider. */
			{ 540, 200 },  /* Screenshot (settings open). */
			{ 560,  26 },  /* Previous. */
			{ 600,  35 },  /* Continue game. */
			{ 660, 200 },  /* Screenshot (back in game). */
			{ 680,   2 },  /* Hover Mentat. */
			{ 740, 200 },
			{ 760,   3 },  /* Click Mentat. */
			{ 820, 200 },
			{ 860, 300 },  /* Exit. */
		};
		static int next, click_step, cx, cy;
		static bool click_overlay;

		if (next < (int)lengthof(script) && frames == script[next].frame) {
			const int what = script[next].what;
			next++;

			if (what == 100) {
				PoolFindStruct find;
				Unit *u = Unit_FindFirst(&find, g_playerHouseID, UNIT_INVALID);

				if (u != NULL) {
					Unit_Select(u);
					GUI_ChangeSelectionType(SELECTIONTYPE_UNIT);
				}
			} else if (what == 200) {
				VideoA5_CaptureScreenshot();
			} else if (what == 300) {
				exit(0);
			} else if (what == 1 || what == 2 || what == 3) {
				/* Mentat (2 = hover only, 3 = click) and Options are head-list buttons. */
				const ScreenDiv *mb = &g_screenDiv[SCREENDIV_MENUBAR];
				const int index = (what == 1) ? 2 : 1;
				const Widget *w = GUI_Widget_Get_ByIndex(g_widgetLinkedListHead, index);

				cx = mb->x + mb->scalex * (w->offsetX + w->width / 2);
				cy = mb->y + mb->scaley * (w->offsetY + w->height / 2);
				click_overlay = false;
				click_step = (what == 2) ? 4 : 1;
			} else {
				const Widget *w = GUI_Widget_Get_ByIndex(g_widgetLinkedListTail, what);
				const ScreenDiv *md = &g_screenDiv[SCREENDIV_MENU];

				if (w == NULL) {
					fprintf(stdout, "test: widget %d not found\n", what);
				} else {
					const WidgetProperties *wp = &g_widgetProperties[w->parentID];
					int ox = w->offsetX;
					int oy = w->offsetY;
					int dx = w->width / 2;
					int dy = w->height / 2;

					if (oy < 0)
						oy += wp->height;

					if (what == 133)
						dx = HOUSE_COLOUR_PREVIEW_W + 6 + 2 * (HOUSE_COLOUR_PRESET_W + HOUSE_COLOUR_PRESET_GAP) + 2;
					else if (what >= 130)
						dx = w->width / 3;

					cx = md->x + md->scalex * (wp->xBase + ox + dx);
					cy = md->y + md->scaley * (wp->yBase + oy + dy);
					click_overlay = true;
					click_step = 1;
				}
			}
		}

		if (click_step == 1) {
			Mouse_EventHandler(click_overlay, cx, cy, 0, 0);
			click_step = 2;
		} else if (click_step == 2) {
			Mouse_EventHandler(click_overlay, cx, cy, 0, SCANCODE_MOUSE_LMB);
			click_step = 3;
		} else if (click_step == 3) {
			Mouse_EventHandler(click_overlay, cx, cy, 0, SCANCODE_MOUSE_LMB | SCANCODE_RELEASE);
			click_step = 0;
		} else if (click_step == 4) {
			Mouse_EventHandler(click_overlay, cx, cy, 0, 0); /* Hover. */
			click_step = 0;
		}

		if (frames % 60 == 0) {
			fprintf(stdout, "test: frame %d overlay=%d\n", frames, (int)g_gameOverlay);
			fflush(stdout);
		}

		return;
	}

	if (g_debugTestColourMenu == 6) {
		/* --test-colour-unit: select a unit, change the house colour, look at the panels. */
		if (frames == 240) {
			PoolFindStruct find;
			Unit *u = Unit_FindFirst(&find, g_playerHouseID, UNIT_INVALID);

			if (u != NULL) {
				Unit_Select(u);
				GUI_ChangeSelectionType(SELECTIONTYPE_UNIT);
			}
		} else if (frames == 300) {
			VideoA5_CaptureScreenshot();
		} else if (frames == 320) {
			for (int i = 0; i < 2; i++) {
				if (++g_gameConfig.houseColorPalette >= 8)
					g_gameConfig.houseColorPalette = 0;

				GUI_Palette_CreateRemap(g_playerHouseID);
				VideoA5_RebuildHousePalette(g_playerHouseID);
				GUI_Palette_RemapScreen(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, g_screenActiveID, g_remap);
			}
		} else if (frames == 400) {
			VideoA5_CaptureScreenshot();
		} else if (frames >= 410 && frames <= 520) {
			/* Hover over, then hold a click on, the Mentat button. */
			const ScreenDiv *mb = &g_screenDiv[SCREENDIV_MENUBAR];
			const int x = mb->x + mb->scalex * (16 + 40);
			const int y = mb->y + mb->scaley * (1 + 8);

			if (frames == 410 || frames == 450)
				Mouse_EventHandler(false, x, y, 0, 0);
			else if (frames == 452)
				Mouse_EventHandler(false, x, y, 0, SCANCODE_MOUSE_LMB);
			else if (frames == 480)
				VideoA5_CaptureScreenshot();
			else if (frames == 500)
				Mouse_EventHandler(false, x, y, 0, SCANCODE_MOUSE_LMB | SCANCODE_RELEASE);
		} else if (frames == 560) {
			VideoA5_CaptureScreenshot();
		} else if (frames == 600) {
			exit(0);
		}

		return;
	}

	if (g_debugTestColourMenu == 5) {
		/* --test-options-continue: click Options, Continue game, then Mentat,
		 * with mouse events injected as Allegro delivers them.
		 */
		const ScreenDiv *mb = &g_screenDiv[SCREENDIV_MENUBAR];
		static int x, y, step;
		static bool overlay_click;

		if (frames == 240) {
			MenuBar_DebugPrintMentat("before Options");
			x = mb->x + mb->scalex * (104 + 40); /* Options button. */
			y = mb->y + mb->scaley * (1 + 8);
			overlay_click = false;
			step = 1;
		} else if (frames == 300) {
			const Widget *cont = GUI_Widget_Get_ByIndex(g_widgetLinkedListTail, 35);
			const ScreenDiv *md = &g_screenDiv[SCREENDIV_MENU];

			fprintf(stdout, "test: overlay=%d before Continue click\n", (int)g_gameOverlay);
			if (cont != NULL) {
				const WidgetProperties *wp = &g_widgetProperties[cont->parentID];

				x = md->x + md->scalex * (wp->xBase + cont->offsetX + cont->width / 2);
				y = md->y + md->scaley * (wp->yBase + cont->offsetY + cont->height / 2);
			}
			overlay_click = true;
			step = 1;
		} else if (frames == 360) {
			fprintf(stdout, "test: overlay=%d after Continue click, mouse %d,%d\n",
					(int)g_gameOverlay, g_mouseX, g_mouseY);
			MenuBar_DebugPrintMentat("after Continue");
			x = mb->x + mb->scalex * (16 + 40); /* Mentat button. */
			y = mb->y + mb->scaley * (1 + 8);
			overlay_click = false;
			step = 1;
		} else if (frames == 420) {
			fprintf(stdout, "test: mentat click after Options/Continue %s (overlay=%d)\n",
					(g_gameOverlay == GAMEOVERLAY_MENTAT) ? "WORKED" : "FAILED", (int)g_gameOverlay);
			fflush(stdout);
		} else if (frames == 480) {
			VideoA5_CaptureScreenshot();
		} else if (frames == 520) {
			exit(0);
		}

		/* DD_TEST_KEYS: open with F2, close with Escape, instead of clicking. */
		if (getenv("DD_TEST_KEYS")) {
			if (frames == 240 || frames == 300) {
				Input_EventHandler(frames == 240 ? SCANCODE_F2 : SCANCODE_ESCAPE);
			} else if (frames == 243 || frames == 303) {
				Input_EventHandler((frames == 243 ? SCANCODE_F2 : SCANCODE_ESCAPE) | SCANCODE_RELEASE);
			}

			if (frames == 240 || frames == 300)
				step = 0;
		}

		if (step == 1) {
			Mouse_EventHandler(overlay_click, x, y, 0, 0);
			step = 2;
		} else if (step == 2) {
			Mouse_EventHandler(overlay_click, x, y, 0, SCANCODE_MOUSE_LMB);
			step = 3;
		} else if (step == 3) {
			Mouse_EventHandler(overlay_click, x, y, 0, SCANCODE_MOUSE_LMB | SCANCODE_RELEASE);
			step = 0;
		}

		return;
	}

	if (state == 0 && frames > 240) {
		if (g_debugTestColourMenu == 2) {
			/* --test-mentat: the in-game Mentat screen. */
			MenuBar_ClickMentat(NULL);
		} else {
			Widget *radio;

			MenuBar_DebugPrintMentat("before");
			g_gameOverlay = GAMEOVERLAY_GAME_CONTROLS;
			MenuBar_CreateGameControls();

			radio = GUI_Widget_Get_ByIndex(g_widgetLinkedListTail, 94);
			if (radio != NULL)
				MenuBar_ClickRadioButton(radio);

			if (g_debugTestColourMenu == 4) {
				/* --test-options-previous: click the yellow preset, as the player would. */
				Widget *presets = GUI_Widget_Get_ByIndex(g_widgetLinkedListTail, 133);
				const WidgetProperties *wi = &g_widgetProperties[presets->parentID];

				g_mouseX = wi->xBase + presets->offsetX + HOUSE_COLOUR_PREVIEW_W + 6
					+ 2 * (HOUSE_COLOUR_PRESET_W + HOUSE_COLOUR_PRESET_GAP) + 2;
				MenuBar_ClickHouseColourPresets(presets);
			}
		}

		state = 1;
		frames = 0;
	} else if (state == 1 && g_debugTestColourMenu == 4 && frames > 30) {
		/* Cycle the house colour button as the player would. */
		for (int i = 0; i < (getenv("DD_TEST_NOCYCLE") ? 0 : 10); i++) {
			if (++g_gameConfig.houseColorPalette >= 8)
				g_gameConfig.houseColorPalette = 0;

			GUI_Palette_CreateRemap(g_playerHouseID);
			VideoA5_RebuildHousePalette(g_playerHouseID);
			GUI_Palette_RemapScreen(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, g_screenActiveID, g_remap);
		}

		MenuBar_DebugPrintMentat("after colour cycling");

		/* Previous: the same as MenuBar_TickGameControls does. */
		g_gameOverlay = GAMEOVERLAY_OPTIONS;
		MenuBar_UninitGameControls();
		GameLoop_TweakWidgetDimensions();
		GUI_Window_Create(&g_optionsWindowDesc);

		state = 2;
		frames = 0;
	} else if (state == 2 && g_debugTestColourMenu == 4) {
		/* The game used to hang here, drawing, so reaching the end is the test. */
		if (frames == 5)
			MenuBar_DebugPrintMentat("after Previous");

		/* Continue game, then look at the menubar. */
		if (frames == 20)
			g_gameOverlay = GAMEOVERLAY_NONE;

		if (frames == 80)
			VideoA5_CaptureScreenshot();

		/* Click the Mentat button with real input events. */
		if (frames >= 40 && frames <= 46) {
			const ScreenDiv *mb = &g_screenDiv[SCREENDIV_MENUBAR];

			g_mouseX = mb->x + mb->scalex * (16 + 40);
			g_mouseY = mb->y + mb->scaley * (1 + 8);

			if (frames == 42)
				Input_EventHandler(SCANCODE_MOUSE_LMB);
			else if (frames == 44)
				Input_EventHandler(SCANCODE_MOUSE_LMB | SCANCODE_RELEASE);
		}

		if (frames == 70) {
			fprintf(stdout, "test: mentat click %s (overlay=%d)\n",
					(g_gameOverlay == GAMEOVERLAY_MENTAT) ? "WORKED" : "FAILED", (int)g_gameOverlay);
			fflush(stdout);
		}

		if (frames > 120) {
			fprintf(stdout, "test: survived Previous\n");
			fflush(stdout);
			exit(0);
		}
	} else if (state == 1 && frames > 90) {
		VideoA5_CaptureScreenshot();
		state = 2;
		frames = 0;
	} else if (state == 2 && frames > 90) {
		exit(0);
	}
}

void
MenuBar_TickOptionsOverlay(void)
{
	switch (g_gameOverlay) {
		case GAMEOVERLAY_OPTIONS:
			MenuBar_TickOptions();
			break;

		case GAMEOVERLAY_LOAD_GAME:
		case GAMEOVERLAY_SAVE_GAME:
		case GAMEOVERLAY_SAVE_ENTRY:
			MenuBar_TickSaveLoadGame(g_gameOverlay);
			break;

		case GAMEOVERLAY_GAME_CONTROLS:
			MenuBar_TickGameControls();
			break;

		case GAMEOVERLAY_CONFIRM_RESTART:
		case GAMEOVERLAY_CONFIRM_PICK_HOUSE:
		case GAMEOVERLAY_CONFIRM_QUIT:
			MenuBar_TickConfirmation(g_gameOverlay);
			break;

		default:
			break;
	}

	if (g_gameOverlay == GAMEOVERLAY_NONE) {
		Timer_SetTimer(TIMER_GAME, true);
		Video_GrabCursor();
		Structure_Recount();
		Unit_Recount();

		Mouse_TransformFromDiv(SCREENDIV_MENU, &g_mouseX, &g_mouseY);
	}
}

void
MenuBar_DrawOptionsOverlay(void)
{
	A5_UseTransform(SCREENDIV_MENU);

	Video_ShadeScreen(128);

	switch (g_gameOverlay) {
		case GAMEOVERLAY_OPTIONS:
			GUI_Widget_DrawWindow(&g_optionsWindowDesc);
			break;

		case GAMEOVERLAY_LOAD_GAME:
		case GAMEOVERLAY_SAVE_GAME:
			GUI_Widget_DrawWindow(&g_saveLoadWindowDesc);
			break;

		case GAMEOVERLAY_SAVE_ENTRY:
			GUI_Widget_DrawWindow(&g_savegameNameWindowDesc);
			GUI_Widget_Savegame_Draw(s_save_entry);
			break;

		case GAMEOVERLAY_GAME_CONTROLS:
			GUI_Widget_DrawWindow(&g_gameControlWindowDesc);
			break;

		case GAMEOVERLAY_CONFIRM_RESTART:
		case GAMEOVERLAY_CONFIRM_PICK_HOUSE:
		case GAMEOVERLAY_CONFIRM_QUIT:
			GUI_Widget_DrawWindow(&g_yesNoWindowDesc);
			break;

		default:
			break;
	}

	GUI_Widget_DrawAll(g_widgetLinkedListTail);
}

/*--------------------------------------------------------------*/

uint16
GUI_DisplayModalMessage(const char *str, uint16 shapeID, ...)
{
	const enum ScreenDivID divID = A5_SaveTransform();

	va_list ap;

	va_start(ap, shapeID);
	vsnprintf(s_modal_message_buf, sizeof(s_modal_message_buf), str, ap);
	va_end(ap);

	MenuBar_PrepareModalMessage(shapeID);

	bool redraw = true;
	while (true) {
		if (redraw) {
			redraw = false;

			A5_UseTransform(SCREENDIV_MAIN);

			GUI_DrawInterfaceAndRadar();
			Video_ShadeScreen(128);

			A5_UseTransform(SCREENDIV_MENU);

			MenuBar_DrawModalMessage();

			Video_Tick();
		}

		if (Input_Tick(true))
			redraw = true;

		if (MenuBar_TickModalMessage())
			break;

		sleepIdle();
	}

	A5_UseTransform(divID);

	/* Not sure. */
	return 0;
}
