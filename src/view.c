#include <stdlib.h>
#include <string.h>
#include <exhibit.h>

// zooming steps through these, best fit can land anywhere in between
static const double steps[] = {
    0.02, 0.03, 0.05, 0.0625, 0.1, 0.125, 0.167, 0.25, 0.333, 0.5, 0.667,
    1, 1.5, 2, 3, 4, 6, 8, 12, 16, 24, 32,
};
#define STEPS   (int)(sizeof(steps) / sizeof(steps[0]))

static ui_rect_t  area;
static picture_t *pic;
static double     zoom = 1;
static int        fitting = 1;
static double     pan_x, pan_y;

static int        dragging;
static int        drag_x, drag_y;

static ui_image_t *shrunk;
static int         shrunk_count;
static double      shrunk_zoom;
static int         shrunk_turned;

static int *columns;
static int  column_cap;

static void cache_drop(void) {
    for (int i = 0; i < shrunk_count; i++)
        ui_image_free(&shrunk[i]);
    free(shrunk);
    shrunk       = NULL;
    shrunk_count = 0;
}

static int shown_w(void) {
    int w = (int)(pic->w * zoom + 0.5);
    return w < 1 ? 1 : w;
}

static int shown_h(void) {
    int h = (int)(pic->h * zoom + 0.5);
    return h < 1 ? 1 : h;
}

// smaller than the view its centred, bigger and it cant be pulled away from the edges
static void clamp(void) {
    if (!pic || !pic->frame)
        return;
    int sw = shown_w(), sh = shown_h();

    if (sw <= area.w)
        pan_x = (area.w - sw) / 2;
    else if (pan_x > 0)
        pan_x = 0;
    else if (pan_x < area.w - sw)
        pan_x = area.w - sw;

    if (sh <= area.h)
        pan_y = (area.h - sh) / 2;
    else if (pan_y > 0)
        pan_y = 0;
    else if (pan_y < area.h - sh)
        pan_y = area.h - sh;
}

// the whole picture in view, but never blown up past its own size
static void apply_fit(void) {
    if (!pic || !pic->frame || area.w <= 0 || area.h <= 0)
        return;
    double zx = (double)area.w / pic->w, zy = (double)area.h / pic->h;
    zoom = zx < zy ? zx : zy;
    if (zoom > 1)
        zoom = 1;
    clamp();
}

void view_layout(ui_rect_t r) {
    // keep whats in the middle in the middle when the window changes size
    pan_x += (r.w - area.w) / 2.0;
    pan_y += (r.h - area.h) / 2.0;
    area = r;
    if (fitting)
        apply_fit();
    else
        clamp();
}

void view_show(picture_t *p) {
    cache_drop();
    pic     = p;
    fitting = 1;
    apply_fit();
}

void view_turned(void) {
    cache_drop();
    if (fitting)
        apply_fit();
    else
        clamp();
}

int view_fitting(void) {
    return fitting;
}

double view_zoom(void) {
    return zoom;
}

void view_fit(void) {
    fitting = 1;
    apply_fit();
}

static void zoom_to(double z, int x, int y) {
    if (!pic || !pic->frame)
        return;
    if (x < 0) {
        x = area.x + area.w / 2;
        y = area.y + area.h / 2;
    }
    // the picture point under x y stays under it
    double px = (x - area.x - pan_x) / zoom;
    double py = (y - area.y - pan_y) / zoom;
    zoom    = z;
    fitting = 0;
    pan_x   = x - area.x - px * zoom;
    pan_y   = y - area.y - py * zoom;
    clamp();
}

void view_actual(void) {
    zoom_to(1, -1, -1);
}

void view_zoom_step(int dir, int x, int y) {
    double z = zoom;
    if (dir > 0) {
        for (int i = 0; i < STEPS; i++) {
            if (steps[i] > zoom * 1.001) {
                z = steps[i];
                break;
            }
        }
    } else {
        for (int i = STEPS - 1; i >= 0; i--) {
            if (steps[i] < zoom * 0.999) {
                z = steps[i];
                break;
            }
        }
    }
    zoom_to(z, x, y);
}

int view_drag(int x, int y, int left) {
    if (!left) {
        dragging = 0;
        return 0;
    }
    if (!dragging) {
        dragging = ui_rect_contains(area, x, y);
        drag_x   = x;
        drag_y   = y;
        return 0;
    }

    double was_x = pan_x, was_y = pan_y;
    pan_x += x - drag_x;
    pan_y += y - drag_y;
    drag_x = x;
    drag_y = y;
    clamp();
    return pan_x != was_x || pan_y != was_y;
}

static const ui_image_t *shrunk_frame(int frame) {
    if (shrunk && (shrunk_zoom != zoom || shrunk_turned != pic->turned))
        cache_drop();
    if (!shrunk) {
        shrunk = calloc((size_t)pic->frames, sizeof(ui_image_t));
        if (!shrunk)
            return NULL;
        shrunk_count  = pic->frames;
        shrunk_zoom   = zoom;
        shrunk_turned = pic->turned;
    }

    ui_image_t *img = &shrunk[frame];
    if (!img->pixels) {
        size_t bytes = (size_t)pic->w * pic->h * sizeof(uint32_t);
        if (!(img->pixels = malloc(bytes)))
            return NULL;
        memcpy(img->pixels, pic->frame[frame], bytes);
        img->w = pic->w;
        img->h = pic->h;
        if (ui_image_scale(img, shown_w(), shown_h()) < 0) {
            ui_image_free(img);
            return NULL;
        }
    }
    return img;
}

static uint32_t blend(uint32_t over, uint32_t under) {
    uint32_t a = over >> 24;
    if (a == 255)
        return over & 0xFFFFFF;
    uint32_t out = 0;
    for (int shift = 0; shift < 24; shift += 8) {
        uint32_t o = (over >> shift) & 0xFF, u = (under >> shift) & 0xFF;
        out |= ((o * a + u * (255 - a) + 127) / 255) << shift;
    }
    return out;
}

static void draw_picked(ui_canvas_t *c, const uint32_t *src, int x0, int y0, ui_rect_t vis) {
    if (vis.w > column_cap) {
        int *more = realloc(columns, (size_t)vis.w * sizeof(int));
        if (!more)
            return;
        columns    = more;
        column_cap = vis.w;
    }
    for (int i = 0; i < vis.w; i++) {
        int sx = (int)((vis.x + i - x0) / zoom);
        columns[i] = sx < pic->w ? sx : pic->w - 1;
    }

    for (int row = vis.y; row < vis.y + vis.h; row++) {
        int sy = (int)((row - y0) / zoom);
        if (sy >= pic->h)
            sy = pic->h - 1;
        const uint32_t *line = src + (size_t)sy * pic->w;
        uint32_t       *dst  = c->pixels + (size_t)row * c->pitch + vis.x;
        for (int i = 0; i < vis.w; i++)
            dst[i] = blend(line[columns[i]], dst[i]);
    }
}

void view_draw(ui_canvas_t *c, int frame) {
    ui_fill(c, area, VIEW_BACK);
    if (!pic || !pic->frame)
        return;
    if (frame < 0 || frame >= pic->frames)
        frame = 0;

    int       x0  = area.x + (int)pan_x, y0 = area.y + (int)pan_y;
    ui_rect_t vis = ui_rect_clip((ui_rect_t){ x0, y0, shown_w(), shown_h() }, ui_rect_clip(area, c->clip));
    if (vis.w <= 0 || vis.h <= 0)
        return;

    if (pic->see_through) {
        for (int row = vis.y; row < vis.y + vis.h; row++) {
            uint32_t *dst  = c->pixels + (size_t)row * c->pitch;
            int       band = (row - y0) / CHECK_SIZE;
            for (int col = vis.x; col < vis.x + vis.w; col++)
                dst[col] = (((col - x0) / CHECK_SIZE + band) & 1) ? CHECK_DARK : CHECK_LIGHT;
        }
    }

    const ui_image_t *small = zoom < 1 ? shrunk_frame(frame) : NULL;
    if (small) {
        ui_rect_t old = c->clip;
        ui_clip(c, vis);
        ui_image_draw(c, small, x0, y0);
        c->clip = old;
    } else {
        draw_picked(c, pic->frame[frame], x0, y0, vis);
    }
}
