#ifndef VIDEO_PRIM_H
#define VIDEO_PRIM_H

#include "types.h"

extern void Prim_Line(float x1, float y1, float x2, float y2, uint8 c, float thickness);
extern void Prim_Hline(int x1, int y, int x2, uint8 c);
extern void Prim_Vline(int x, int y1, int y2, uint8 c);

extern void Prim_Circle(float cx, float cy, float rd, uint8 c, float thickness);
extern void Prim_Circle_i(int cx, int cy, int rd, uint8 c);
extern void Prim_Circle_RGBA(float cx, float cy, float rd, unsigned char r, unsigned char g, unsigned char b, unsigned char alpha, float thickness);

extern void Prim_FillCircle(float cx, float cy, float rd, uint8 c);
extern void Prim_FillCircle_i(int cx, int cy, int rd, uint8 c);
extern void Prim_FillCircle_RGBA(float cx, float cy, float rd, unsigned char r, unsigned char g, unsigned char b, unsigned char alpha);

extern void Prim_Rect(float x1, float y1, float x2, float y2, uint8 c, float thickness);
extern void Prim_Rect_i(int x1, int y1, int x2, int y2, uint8 c);
extern void Prim_Rect_RGBA(float x1, float y1, float x2, float y2, unsigned char r, unsigned char g, unsigned char b, unsigned char alpha, float thickness);

extern void Prim_FillRect(float x1, float y1, float x2, float y2, uint8 c);
extern void Prim_FillRect_i(int x1, int y1, int x2, int y2, uint8 c);
extern void Prim_FillRect_RGBA(float x1, float y1, float x2, float y2, unsigned char r, unsigned char g, unsigned char b, unsigned char alpha);

/* Additive blending (glowing fire) for the following primitives, or back to normal alpha blending. */
extern void Prim_SetAdditive(bool additive);

enum PrimBlend {
	PRIM_BLEND_NORMAL,
	PRIM_BLEND_ADDITIVE,
	PRIM_BLEND_REPLACE,
	PRIM_BLEND_PUNCH
};

extern void Prim_SetBlend(enum PrimBlend mode);

enum PrimLayer {
	PRIM_LAYER_SHADOW,
	PRIM_LAYER_DARK,
	PRIM_NUM_LAYERS
};

/* Draw into a layer of w x h pixels, cleared to the colour, until Prim_LayerEnd(). */
extern bool Prim_LayerBegin(enum PrimLayer layer, int w, int h, unsigned char r, unsigned char g, unsigned char b, unsigned char a);
extern void Prim_LayerEnd(void);
extern void Prim_LayerDraw(enum PrimLayer layer, int x, int y);

extern bool Prim_GrabBegin(int x, int y, int w, int h);
extern bool Prim_GrabPixel(float x, float y, unsigned char *rgb);
extern void Prim_GrabEnd(void);

extern void Prim_DrawBorder(float x, float y, float w, float h, int thickness, bool outline, bool fill, int colour_scheme);

#endif
