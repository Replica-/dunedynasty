#ifndef VIDEO_VIDEOA5_H
#define VIDEO_VIDEOA5_H

#include "video.h"
#include "../file.h"

enum GraphicsDriver {
	GRAPHICS_DRIVER_OPENGL,
	GRAPHICS_DRIVER_DIRECT3D,
};

typedef struct DisplayMode {
   int width;          // Screen width
   int height;         // Screen height
} DisplayMode;

#define DISPLAY_MODE_INITIALIZER { .width = 0, .height = 0 }

extern enum GraphicsDriver g_graphics_driver;
extern bool g_gl_post_fx;     /* [graphics] gl_post_fx (default on): GLSL post-processing of the final frame. */
extern bool g_hardware_effects; /* [graphics] hardware_effects (default on): procedural explosions and smoke.  OpenGL only. */
extern bool g_show_fps;        /* [graphics] show_fps (default on): frame rate and slowest frame, top left.  F10 toggles it. */
extern bool VideoA5_HardwareEffectsAvailable(void); /* True with the OpenGL driver. */
extern bool VideoA5_HardwareEffectsActive(void);    /* Available and enabled. */
extern bool g_gl_post_fx_cli; /* From --gl-post. */
extern bool g_gl_post_fx_off_cli; /* From --no-gl-post. */

extern bool VideoA5_Init(void);
extern void VideoA5_Uninit(void);
extern void VideoA5_ToggleFullscreen(void);
extern void VideoA5_ToggleFPS(void);
extern void VideoA5_CaptureScreenshot(void);
extern void VideoA5_Tick(void);

extern void VideoA5_InitSprites(void);
extern void VideoA5_DisplayFound(void);
extern void VideoA5_DrawCPS(enum SearchDirectory dir, const char *filename);
extern void VideoA5_DrawCPSFill(enum SearchDirectory dir, const char *filename);
extern void VideoA5_DrawCPSCoordinates(enum SearchDirectory dir, const char *filename, int dx, int dy);
extern void VideoA5_DrawCPSRegion(enum SearchDirectory dir, const char *filename, int sx, int sy, int dx, int dy, int w, int h);
extern void VideoA5_DrawCPSSpecial(enum CPSID cpsID, enum HouseType houseID, int x, int y);
extern void VideoA5_DrawCPSSpecialScale(enum CPSID cpsID, enum HouseType houseID, int x, int y, float scale);
extern void VideoA5_DrawIcon(uint16 iconID, enum HouseType houseID, int x, int y);
extern void VideoA5_DrawIconAlpha(uint16 iconID, int x, int y, unsigned char alpha);
extern void VideoA5_DrawRectCross(int x1, int y1, int w, int h, unsigned char c);
extern void VideoA5_DrawShape(enum ShapeID shapeID, enum HouseType houseID, int x, int y, int flags);
extern void VideoA5_DrawShapeRotate(enum ShapeID shapeID, enum HouseType houseID, int x, int y, int orient256, int flags);
extern void VideoA5_DrawShapeScale(enum ShapeID shapeID, int x, int y, int w, int h, int flags);

/* Copies of a shape or an icon, in bitmaps of their own (the caller destroys them). */
extern struct ALLEGRO_BITMAP *VideoA5_CopyShape(enum ShapeID shapeID, enum HouseType houseID);
extern struct ALLEGRO_BITMAP *VideoA5_CopyIcon(uint16 iconID, enum HouseType houseID);
extern void VideoA5_DrawShapeGrey(enum ShapeID shapeID, int x, int y, int flags);
extern void VideoA5_DrawShapeGreyScale(enum ShapeID shapeID, int x, int y, int w, int h, int flags);
extern void VideoA5_DrawShapeTint(enum ShapeID shapeID, int x, int y, unsigned char c, int flags);
extern void VideoA5_DrawChar(unsigned char c, const uint8 *pal, int x, int y);
extern void VideoA5_DrawCharAlpha(unsigned char c, const uint8 *pal, int x, int y, unsigned char alpha);
extern bool VideoA5_DrawWSA(void *wsa, int frame, int sx, int sy, int dx, int dy, int w, int h);
extern void VideoA5_DrawWSAStatic(int frame, int x, int y);
extern void VideoA5_RebuildPlayerHouse(void);
extern void VideoA5_RebuildHousePalette(enum HouseType houseID);

extern int VideoA5_GetDesktopWidth(void);
extern int VideoA5_GetDesktopHeight(void);

extern int VideoA5_GetHeight(enum ShapeID shapeID);
extern int VideoA5_GetWidth(enum ShapeID shapeID);

struct DisplayMode* VideoA5_GetDisplayModes(void);
int VideoA5_GetNumDisplayModes(void);
int VideoA5_GetCurrentDisplayMode(void);

#endif
