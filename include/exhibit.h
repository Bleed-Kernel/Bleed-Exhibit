#pragma once

#include <stdint.h>
#include <stddef.h>
#include <bleed_uikit.h>

#define EXH_PATH_MAX    UI_PATH_MAX
#define EXH_NAME_MAX    UI_NAME_MAX

#define VIEW_BACK       UI_COLOUR_SHADOW
#define CHECK_LIGHT     0x9A9AA0
#define CHECK_DARK      0x6E6E75
#define CHECK_SIZE      8

// picture.c

typedef struct {
    int        w, h;            // as shown, so after any turning
    int        frames;          // more than one for an animated gif
    uint32_t **frame;           // 0xAARRGGBB, w * h each
    int       *delay;           // ms each frame stays up
    int        see_through;     // some pixel isnt fully solid, so the checks go under it
    int        turned;          // bumped every rotate, anything cached off the pixels is stale
    uint64_t   size;
    char       format[8];
    char       error[128];      // set when it couldnt be read, nothing else is then
} picture_t;

int  picture_load(picture_t *p, const char *path);
void picture_free(picture_t *p);
void picture_rotate(picture_t *p, int clockwise);

// is this a name we try to open
int  picture_name_ok(const char *name);

// the other pictures in the folder path is in, sorted like cabinet sorts them
void folder_scan(const char *path);
int  folder_count(void);
int  folder_index(void);
void folder_path(int i, char *out, size_t size);
void folder_forget(int i);

// view.c

void view_layout(ui_rect_t r);
void view_show(picture_t *p);   // a new picture, back to best fit
void view_draw(ui_canvas_t *c, int frame);

int    view_fitting(void);
double view_zoom(void);
void   view_fit(void);
void   view_actual(void);
void   view_zoom_step(int dir, int x, int y);  // in or out a step, keeping x y where it is
void   view_turned(void);

// pan with the picture under the pointer, 1 if anything moved
int  view_drag(int x, int y, int left);

// icons.c

enum {
    GLYPH_PREV,
    GLYPH_NEXT,
    GLYPH_ZOOM_IN,
    GLYPH_ZOOM_OUT,
    GLYPH_FIT,
    GLYPH_ACTUAL,
    GLYPH_ROTATE_LEFT,
    GLYPH_ROTATE_RIGHT,
    GLYPH_DELETE,
    GLYPHS,
};

void icons_init(void);
const ui_image_t *glyph(int which);
