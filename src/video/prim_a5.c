/* prim_a5.c */

#include <assert.h>
#include <math.h>
#include <allegro5/allegro.h>
#include <allegro5/allegro_primitives.h>

#include "prim.h"

extern ALLEGRO_COLOR paltoRGB[256];

/*--------------------------------------------------------------*/

void
Prim_Line(float x1, float y1, float x2, float y2, uint8 c, float thickness)
{
	al_draw_line(x1, y1, x2, y2, paltoRGB[c], thickness);
}

void
Prim_Hline(int x1, int y, int x2, uint8 c)
{
	assert(x1 <= x2);
	al_draw_line(x1, y + 0.5f, x2 + 0.99f, y + 0.5f, paltoRGB[c], 1.0f);
}

void
Prim_Vline(int x, int y1, int y2, uint8 c)
{
	assert(y1 <= y2);
	al_draw_line(x + 0.5f, y1, x + 0.5f, y2 + 0.99f, paltoRGB[c], 1.0f);
}

/*--------------------------------------------------------------*/

void
Prim_Circle(float cx, float cy, float rd, uint8 c, float thickness)
{
	al_draw_circle(cx, cy, rd, paltoRGB[c], thickness);
}

void
Prim_Circle_i(int cx, int cy, int rd, uint8 c)
{
	al_draw_circle(cx + 0.5f, cy + 0.5f, rd + 0.5f, paltoRGB[c], 1.0f);
}

void
Prim_Circle_RGBA(float cx, float cy, float rd, unsigned char r, unsigned char g, unsigned char b, unsigned char alpha, float thickness)
{
	al_draw_circle(cx, cy, rd, al_map_rgba(r, g, b, alpha), thickness);
}

/*--------------------------------------------------------------*/

void
Prim_FillCircle(float cx, float cy, float rd, uint8 c)
{
	al_draw_filled_circle(cx, cy, rd, paltoRGB[c]);
}

void
Prim_FillCircle_i(int cx, int cy, int rd, uint8 c)
{
	al_draw_filled_circle(cx + 0.5f, cy + 0.5f, rd + 0.5f, paltoRGB[c]);
}

void
Prim_FillCircle_RGBA(float cx, float cy, float rd, unsigned char r, unsigned char g, unsigned char b, unsigned char alpha)
{
	al_draw_filled_circle(cx, cy, rd, al_map_rgba(r, g, b, alpha));
}

/*--------------------------------------------------------------*/

void
Prim_Rect(float x1, float y1, float x2, float y2, uint8 c, float thickness)
{
	al_draw_rectangle(x1, y1, x2, y2, paltoRGB[c], thickness);
}

void
Prim_Rect_i(int x1, int y1, int x2, int y2, uint8 c)
{
	assert(x1 <= x2);
	assert(y1 <= y2);

	al_draw_rectangle(x1 + 0.5f, y1 + 0.5f, x2 + 0.5f, y2 + 0.5f, paltoRGB[c], 1.0f);
}

void
Prim_Rect_RGBA(float x1, float y1, float x2, float y2, unsigned char r, unsigned char g, unsigned char b, unsigned char alpha, float thickness)
{
	al_draw_rectangle(x1, y1, x2, y2, al_map_rgba(r, g, b, alpha), thickness);
}

/*--------------------------------------------------------------*/

void
Prim_FillRect(float x1, float y1, float x2, float y2, uint8 c)
{
	al_draw_filled_rectangle(x1, y1, x2, y2, paltoRGB[c]);
}

void
Prim_FillRect_i(int x1, int y1, int x2, int y2, uint8 c)
{
	assert(x1 <= x2);
	assert(y1 <= y2);

	al_draw_filled_rectangle(x1 + 0.01f, y1 + 0.01f, x2 + 0.99f, y2 + 0.99f, paltoRGB[c]);
}

void
Prim_FillRect_RGBA(float x1, float y1, float x2, float y2,
		unsigned char r, unsigned char g, unsigned char b, unsigned char alpha)
{
	assert(x1 <= x2);
	assert(y1 <= y2);

	al_draw_filled_rectangle(x1, y1, x2, y2, al_map_rgba(r, g, b, alpha));
}

void
Prim_SetAdditive(bool additive)
{
	al_set_blender(ALLEGRO_ADD, ALLEGRO_ALPHA, additive ? ALLEGRO_ONE : ALLEGRO_INVERSE_ALPHA);
}


void
Prim_SetBlend(enum PrimBlend mode)
{
	switch (mode) {
		case PRIM_BLEND_NORMAL:
			al_set_blender(ALLEGRO_ADD, ALLEGRO_ALPHA, ALLEGRO_INVERSE_ALPHA);
			break;

		case PRIM_BLEND_ADDITIVE:
			al_set_blender(ALLEGRO_ADD, ALLEGRO_ALPHA, ALLEGRO_ONE);
			break;

		case PRIM_BLEND_REPLACE:
			/* Overlapping shapes do not add up: the last one wins. */
			al_set_blender(ALLEGRO_ADD, ALLEGRO_ONE, ALLEGRO_ZERO);
			break;

		case PRIM_BLEND_PUNCH:
			/* Leaves the colour alone, and takes the shape's alpha away
			 * from the alpha already there.  Makes holes in a layer.
			 */
			al_set_separate_blender(ALLEGRO_ADD, ALLEGRO_ZERO, ALLEGRO_ONE,
					ALLEGRO_DEST_MINUS_SRC, ALLEGRO_ONE, ALLEGRO_ONE);
			break;
	}
}

/*--------------------------------------------------------------*/

/* Offscreen layers, in the pixels of the viewport, which are drawn over the
 * scene afterwards.  Used for shadows and for the darkness of the night.
 */

static ALLEGRO_BITMAP *s_layer[PRIM_NUM_LAYERS];
static ALLEGRO_BITMAP *s_layerPrevTarget;
static ALLEGRO_TRANSFORM s_layerPrevTransform;
static int s_layerClip[4];

bool
Prim_LayerBegin(enum PrimLayer layer, int w, int h, unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
	ALLEGRO_BITMAP **bmp = &s_layer[layer];

	if (*bmp != NULL && (al_get_bitmap_width(*bmp) != w || al_get_bitmap_height(*bmp) != h)) {
		al_destroy_bitmap(*bmp);
		*bmp = NULL;
	}

	if (*bmp == NULL) {
		const int old_flags = al_get_new_bitmap_flags();

		al_set_new_bitmap_flags(ALLEGRO_VIDEO_BITMAP | ALLEGRO_NO_PRESERVE_TEXTURE);
		*bmp = al_create_bitmap(w, h);
		al_set_new_bitmap_flags(old_flags);

		if (*bmp == NULL)
			return false;
	}

	s_layerPrevTarget = al_get_target_bitmap();
	al_copy_transform(&s_layerPrevTransform, al_get_current_transform());
	al_get_clipping_rectangle(&s_layerClip[0], &s_layerClip[1], &s_layerClip[2], &s_layerClip[3]);

	al_set_target_bitmap(*bmp);

	ALLEGRO_TRANSFORM identity;
	al_identity_transform(&identity);
	al_use_transform(&identity);
	al_set_clipping_rectangle(0, 0, w, h);

	al_clear_to_color(al_map_rgba(r, g, b, a));
	return true;
}

void
Prim_LayerEnd(void)
{
	Prim_SetBlend(PRIM_BLEND_NORMAL);

	al_set_target_bitmap(s_layerPrevTarget);
	al_use_transform(&s_layerPrevTransform);
	al_set_clipping_rectangle(s_layerClip[0], s_layerClip[1], s_layerClip[2], s_layerClip[3]);
}

void
Prim_LayerDraw(enum PrimLayer layer, int x, int y)
{
	if (s_layer[layer] != NULL)
		al_draw_bitmap(s_layer[layer], x, y, 0);
}
/*--------------------------------------------------------------*/

#if 0
void
Prim_FillTriangle(float x1, float y1, float x2, float y2, float x3, float y3, uint8 c)
{
	al_draw_filled_triangle(x1, y1, x2, y2, x3, y3, paltoRGB[c]);
}
#endif

/*--------------------------------------------------------------*/

extern uint8 s_colourBorderSchema[5][4];

void
Prim_DrawBorder(float x, float y, float w, float h, int thickness, bool outline, bool fill, int colour_scheme)
{
	const int idx[60] = {
		/* Black outline. */
		0, 1, 2,
		0, 2, 3,

		/* Background. */
		4, 5, 6,
		4, 6, 7,

		/* Top, left borders. */
		8,  9, 10,
		8, 10, 11,
		8, 11, 12,
		8, 12, 13,

		/* Bottom, right borders. */
		14, 15, 16,
		14, 16, 17,
		14, 17, 18,
		14, 18, 19,

		/* Corners. */
		20, 21, 22,
		20, 22, 23,
		24, 25, 26,
		24, 26, 27,

		/* Corners, when thickness = 2. */
		28, 23, 29,
		28, 29, 30,
		31, 32, 33,
		31, 33, 25,
	};

	const uint8 *scheme = s_colourBorderSchema[colour_scheme];

	ALLEGRO_VERTEX vtx[34];
	float sz = thickness;
	int start = 0;
	int end = 60;

	memset(vtx, 0, sizeof(vtx));

	if (!fill) {
		start += 12;

		if (outline)
			Prim_Rect(x - 0.5f, y - 0.5f, x + w + 0.5f, y + h + 0.5f, 12, 1.0f);
	} else if (!outline) {
		start += 6;
	} else {
		/* Black outline. */
		for (int i = 0; i <= 3; i++)
			vtx[i].color = paltoRGB[12];

		vtx[0].x = x     - 1.0f, vtx[0].y = y - 1.0f;
		vtx[1].x = x + w + 1.0f, vtx[1].y = y - 1.0f;
		vtx[2].x = x + w + 1.0f, vtx[2].y = y + h + 1.0f;
		vtx[3].x = x     - 1.0f, vtx[3].y = y + h + 1.0f;
	}

	if (fill) {
		/* Background. */
		for (int i = 4; i <= 7; i++)
			vtx[i].color = paltoRGB[scheme[0]];

		vtx[4].x = x,       vtx[4].y = y;
		vtx[5].x = x + w,   vtx[5].y = y;
		vtx[6].x = x + w,   vtx[6].y = y + h;
		vtx[7].x = x,       vtx[7].y = y + h;
	}

	/* Top, left borders. */
	for (int i = 8; i <= 13; i++)
		vtx[i].color = paltoRGB[scheme[2]];

	vtx[ 8].x = x,          vtx[ 8].y = y;
	vtx[ 9].x = x + w,      vtx[ 9].y = y;
	vtx[10].x = x + w - sz, vtx[10].y = y + sz;
	vtx[11].x = x + sz,     vtx[11].y = y + sz;
	vtx[12].x = x + sz,     vtx[12].y = y + h - sz;
	vtx[13].x = x,          vtx[13].y = y + h;

	/* Bottom, right borders. */
	for (int i = 14; i <= 19; i++)
		vtx[i].color = paltoRGB[scheme[1]];

	vtx[14].x = x + w,      vtx[14].y = y + h;
	vtx[15].x = vtx[ 9].x,  vtx[15].y = vtx[ 9].y;
	vtx[16].x = vtx[10].x,  vtx[16].y = vtx[10].y;
	vtx[17].x = x + w - sz, vtx[17].y = y + h - sz;
	vtx[18].x = vtx[12].x,  vtx[18].y = vtx[12].y;
	vtx[19].x = vtx[13].x,  vtx[19].y = vtx[13].y;

	/* Corners. */
	vtx[20].x = x + w - 1.0f,   vtx[20].y = y;
	vtx[21].x = x + w,          vtx[21].y = y;
	vtx[22].x = x + w,          vtx[22].y = y + 1.0f;
	vtx[23].x = x + w - 1.0f,   vtx[23].y = y + 1.0f;

	vtx[24].x = x,          vtx[24].y = y + h - 1.0f;
	vtx[25].x = x + 1.0f,   vtx[25].y = y + h - 1.0f;
	vtx[26].x = x + 1.0f,   vtx[26].y = y + h;
	vtx[27].x = x,          vtx[27].y = y + h;

	if (thickness <= 1) {
		for (int i = 20; i <= 27; i++)
			vtx[i].color = paltoRGB[scheme[3]];

		end -= 12;
	} else {
		for (int i = 20; i <= 33; i++)
			vtx[i].color = paltoRGB[scheme[3]];

		vtx[28].x = x + w - 2.0f,   vtx[28].y = y + 1.0f;
		vtx[29].x = x + w - 1.0f,   vtx[29].y = y + 2.0f;
		vtx[30].x = x + w - 2.0f,   vtx[30].y = y + 2.0f;

		vtx[31].x = x + 1.0f,   vtx[31].y = y + h - 2.0f;
		vtx[32].x = x + 2.0f,   vtx[32].y = y + h - 2.0f;
		vtx[33].x = x + 2.0f,   vtx[33].y = y + h - 1.0f;
	}

	al_draw_indexed_prim(vtx, NULL, NULL, &idx[start], end - start, ALLEGRO_PRIM_TRIANGLE_LIST);
}

/*--------------------------------------------------------------*/

/* Reading pixels back from what has been drawn so far.  A rectangle of the
 * current target, given in the units of the current transform, is locked, and
 * single pixels can then be read from it until Prim_GrabEnd().  Nothing can be
 * drawn in between.
 */

static ALLEGRO_BITMAP *s_grabBitmap;
static ALLEGRO_TRANSFORM s_grabTransform;
static int s_grabRegion[4];

bool
Prim_GrabBegin(int x, int y, int w, int h)
{
	ALLEGRO_BITMAP *target = al_get_target_bitmap();

	if (target == NULL || s_grabBitmap != NULL)
		return false;

	al_copy_transform(&s_grabTransform, al_get_current_transform());

	float x1 = (float)x, y1 = (float)y, x2 = (float)(x + w), y2 = (float)(y + h);
	al_transform_coordinates(&s_grabTransform, &x1, &y1);
	al_transform_coordinates(&s_grabTransform, &x2, &y2);

	int rx1 = (int)floorf(x1 < x2 ? x1 : x2), ry1 = (int)floorf(y1 < y2 ? y1 : y2);
	int rx2 = (int)ceilf(x1 < x2 ? x2 : x1), ry2 = (int)ceilf(y1 < y2 ? y2 : y1);

	if (rx1 < 0) rx1 = 0;
	if (ry1 < 0) ry1 = 0;
	if (rx2 > al_get_bitmap_width(target)) rx2 = al_get_bitmap_width(target);
	if (ry2 > al_get_bitmap_height(target)) ry2 = al_get_bitmap_height(target);

	if (rx2 <= rx1 || ry2 <= ry1)
		return false;

	if (al_lock_bitmap_region(target, rx1, ry1, rx2 - rx1, ry2 - ry1, ALLEGRO_PIXEL_FORMAT_ANY, ALLEGRO_LOCK_READONLY) == NULL)
		return false;

	s_grabBitmap = target;
	s_grabRegion[0] = rx1; s_grabRegion[1] = ry1; s_grabRegion[2] = rx2; s_grabRegion[3] = ry2;
	return true;
}

bool
Prim_GrabPixel(float x, float y, unsigned char *rgb)
{
	if (s_grabBitmap == NULL)
		return false;

	al_transform_coordinates(&s_grabTransform, &x, &y);

	const int px = (int)floorf(x), py = (int)floorf(y);
	if (px < s_grabRegion[0] || py < s_grabRegion[1] || px >= s_grabRegion[2] || py >= s_grabRegion[3])
		return false;

	al_unmap_rgb(al_get_pixel(s_grabBitmap, px, py), &rgb[0], &rgb[1], &rgb[2]);
	return true;
}

void
Prim_GrabEnd(void)
{
	if (s_grabBitmap != NULL) {
		al_unlock_bitmap(s_grabBitmap);
		s_grabBitmap = NULL;
	}
}

/*--------------------------------------------------------------*/

/* A grid of cols x rows cells, each cell x cell pixels, with the top left at x,y.  The
 * alpha is given at the corners, (cols + 1) x (rows + 1) of them, and the graphics card
 * blends it smoothly across each cell.  Everything is kept inside the box bx1,by1,bx2,by2.
 */
void
Prim_AlphaGrid(int cols, int rows, const float *cornerAlpha, float x, float y, float cell,
		unsigned char r, unsigned char g, unsigned char b,
		float bx1, float by1, float bx2, float by2)
{
	enum { MAX_COLS = 160, MAX_ROWS = 100 };
	static ALLEGRO_VERTEX vertices[(MAX_COLS + 1) * (MAX_ROWS + 1)];
	static int indices[MAX_COLS * MAX_ROWS * 6];
	int num = 0;

	if (cols > MAX_COLS || rows > MAX_ROWS)
		return;

	for (int j = 0; j <= rows; j++) {
		for (int i = 0; i <= cols; i++) {
			ALLEGRO_VERTEX *v = &vertices[j * (cols + 1) + i];
			const float px = x + i * cell, py = y + j * cell;

			v->x = (px < bx1) ? bx1 : (px > bx2) ? bx2 : px;
			v->y = (py < by1) ? by1 : (py > by2) ? by2 : py;
			v->z = 0.0f;
			v->u = v->v = 0.0f;
			v->color = al_map_rgba(r, g, b, (unsigned char)(cornerAlpha[j * (cols + 1) + i] * 255.0f + 0.5f));
		}
	}

	for (int j = 0; j < rows; j++) {
		for (int i = 0; i < cols; i++) {
			const int a = j * (cols + 1) + i, bb = a + 1, c = a + (cols + 1), d = c + 1;

			if (cornerAlpha[a] <= 0.0f && cornerAlpha[bb] <= 0.0f && cornerAlpha[c] <= 0.0f && cornerAlpha[d] <= 0.0f)
				continue;

			indices[num++] = a; indices[num++] = bb; indices[num++] = c;
			indices[num++] = bb; indices[num++] = d; indices[num++] = c;
		}
	}

	if (num > 0)
		al_draw_indexed_prim(vertices, NULL, NULL, indices, num, ALLEGRO_PRIM_TRIANGLE_LIST);
}

void
Prim_FillTriangle_RGBA(float x1, float y1, float x2, float y2, float x3, float y3,
		unsigned char r, unsigned char g, unsigned char b, unsigned char alpha)
{
	al_draw_filled_triangle(x1, y1, x2, y2, x3, y3, al_map_rgba(r, g, b, alpha));
}
