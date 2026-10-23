#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <keyboard.h>
#include <crime/crime.h>
#include <exhibit.h>

#define WINDOW_MIN_W    440
#define WINDOW_MIN_H    320
#define WINDOW_MAX_W    1000
#define WINDOW_MAX_H    700
#define PAD             6
#define TOOL_H          32
#define BUTTON_W        28
#define BUTTON_H        26
#define BUTTON_GAP      2
#define GROUP_GAP       10
#define STATUS_H        22
#define STATUS_RIGHT_W  150
#define DOUBLE_CLICK_MS 400
#define CABINET         "/initrd/bin/cabinet"

#define KEY_ESCAPE      0x01
#define KEY_DELETE      0x53

enum {
    BTN_PREV,
    BTN_NEXT,
    BTN_ZOOM_IN,
    BTN_ZOOM_OUT,
    BTN_FIT,
    BTN_ACTUAL,
    BTN_ROTATE_LEFT,
    BTN_ROTATE_RIGHT,
    BTN_DELETE,
    BTN_COUNT,
};

static const int   btn_group[] = { BTN_ZOOM_IN, BTN_ROTATE_LEFT, BTN_DELETE };
#define GROOVES     (int)(sizeof(btn_group) / sizeof(btn_group[0]))

static const char *btn_hint[BTN_COUNT] = {
    "Previous Picture", "Next Picture", "Zoom In", "Zoom Out", "Best Fit", "Actual Size",
    "Rotate Anticlockwise", "Rotate Clockwise", "Delete",
};

enum {
    ACT_ZOOM_IN,
    ACT_ZOOM_OUT,
    ACT_FIT,
    ACT_ACTUAL,
    ACT_ROTATE_RIGHT,
    ACT_ROTATE_LEFT,
    ACT_FOLDER,
    ACT_DELETE,
    ACT_LINE,
};

static cs_window_t *win;
static ui_button_t  buttons[BTN_COUNT];
static int          grooves[GROOVES];
static ui_rect_t    view_r, status_r;
static uint32_t     mouse_was;
static int          quit;

static ui_menu_t    menu;
static int          menu_acts[UI_MENU_ITEMS];

static picture_t    pic;
static char         path[EXH_PATH_MAX];
static int          frame;
static uint64_t     frame_due;

static uint64_t     last_click;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static ui_canvas_t canvas(void) {
    return ui_canvas(win->pixels, win->width, win->height, win->pitch);
}

static int showing(void) {
    return pic.frame != NULL;
}

static const char *base_name(const char *p) {
    const char *slash = strrchr(p, '/');
    return slash ? slash + 1 : p;
}

static void layout(void) {
    int x = PAD;
    for (int i = 0, g = 0; i < BTN_COUNT; i++) {
        if (g < GROOVES && i == btn_group[g]) {
            grooves[g++] = x + GROUP_GAP / 2 - 2;
            x += GROUP_GAP;
        }
        buttons[i].rect = (ui_rect_t){ x, (TOOL_H - BUTTON_H) / 2, BUTTON_W, BUTTON_H };
        x += BUTTON_W + BUTTON_GAP;
    }

    status_r = (ui_rect_t){ 0, win->height - STATUS_H, win->width, STATUS_H };
    // the picture goes edge to edge inside a sunken frame, the frame is 2 wide
    view_r   = (ui_rect_t){ 2, TOOL_H + 3, win->width - 4, status_r.y - TOOL_H - 5 };
    if (view_r.h < 0)
        view_r.h = 0;
    view_layout(view_r);
}

static void size_text(uint64_t size, char *out, size_t len) {
    if (size < 1024)
        snprintf(out, len, "%lu bytes", (unsigned long)size);
    else if (size < 1024 * 1024)
        snprintf(out, len, "%lu.%02luKB", (unsigned long)(size / 1024), (unsigned long)(size % 1024 * 100 / 1024));
    else
        snprintf(out, len, "%lu.%02luMB", (unsigned long)(size >> 20), (unsigned long)((size & 0xFFFFF) * 100 >> 20));
}

static void status_draw(ui_canvas_t *c) {
    char left[96] = "", right[48] = "";
    int  hint = -1;
    for (int b = 0; b < BTN_COUNT; b++) {
        if (buttons[b].hover)
            hint = b;
    }

    if (hint >= 0) {
        snprintf(left, sizeof(left), "%s", btn_hint[hint]);
    } else if (showing()) {
        char size[32];
        size_text(pic.size, size, sizeof(size));
        if (pic.frames > 1)
            snprintf(left, sizeof(left), "%d x %d  %s, %d frames  %s", pic.w, pic.h, pic.format, pic.frames, size);
        else
            snprintf(left, sizeof(left), "%d x %d  %s  %s", pic.w, pic.h, pic.format, size);
    }

    if (showing()) {
        int pct = (int)(view_zoom() * 100 + 0.5);
        if (folder_count() > 1)
            snprintf(right, sizeof(right), "%d%%    %d of %d", pct, folder_index() + 1, folder_count());
        else
            snprintf(right, sizeof(right), "%d%%", pct);
    }

    ui_fill(c, status_r, UI_COLOUR_FACE);
    ui_rect_t l = { 2, status_r.y + 2, status_r.w - STATUS_RIGHT_W - 6, STATUS_H - 4 };
    ui_rect_t r = { status_r.w - STATUS_RIGHT_W - 2, status_r.y + 2, STATUS_RIGHT_W, STATUS_H - 4 };
    ui_edge(c, l, UI_COLOUR_SHADOW, UI_COLOUR_HILIGHT);
    ui_edge(c, r, UI_COLOUR_SHADOW, UI_COLOUR_HILIGHT);
    ui_text_in(c, &ui_font_text, (ui_rect_t){ l.x + 4, l.y, l.w - 8, l.h }, left, UI_COLOUR_TEXT, UI_ALIGN_LEFT);
    ui_text_in(c, &ui_font_text, (ui_rect_t){ r.x + 4, r.y, r.w - 8, r.h }, right, UI_COLOUR_TEXT, UI_ALIGN_LEFT);
}

// whats said in the middle when theres no picture to show
static void notice_draw(ui_canvas_t *c) {
    char text[EXH_NAME_MAX + 192];
    if (!path[0])
        snprintf(text, sizeof(text), "No image file");
    else
        snprintf(text, sizeof(text), "Exhibit cant show '%s'.\n%s", base_name(path), pic.error);

    ui_rect_t r = ui_rect_shrink(view_r, 16);
    int       w, h;
    ui_text_measure(&ui_font_text, r.w, text, &w, &h);
    r.y += (r.h - h) / 2;
    r.h  = h;
    ui_text_wrap(c, &ui_font_text, r, text, UI_COLOUR_BACK_TEXT, UI_ALIGN_CENTRE);
}

static void picture_draw(ui_canvas_t *c) {
    view_draw(c, frame);
    if (!showing())
        notice_draw(c);
}

static void draw_all(void) {
    ui_canvas_t c = canvas();
    ui_fill(&c, (ui_rect_t){ 0, 0, win->width, win->height }, UI_COLOUR_FACE);

    int many = folder_count() > 1;
    buttons[BTN_PREV].disabled         = !many;
    buttons[BTN_NEXT].disabled         = !many;
    buttons[BTN_ZOOM_IN].disabled      = !showing();
    buttons[BTN_ZOOM_OUT].disabled     = !showing();
    buttons[BTN_FIT].disabled          = !showing() || view_fitting();
    buttons[BTN_ACTUAL].disabled       = !showing() || view_zoom() == 1;
    buttons[BTN_ROTATE_LEFT].disabled  = !showing();
    buttons[BTN_ROTATE_RIGHT].disabled = !showing();
    buttons[BTN_DELETE].disabled       = !path[0];
    for (int i = 0; i < BTN_COUNT; i++)
        ui_button_draw(&c, &buttons[i]);
    for (int g = 0; g < GROOVES; g++) {
        ui_fill(&c, (ui_rect_t){ grooves[g], 4, 1, TOOL_H - 8 }, UI_COLOUR_SHADOW);
        ui_fill(&c, (ui_rect_t){ grooves[g] + 1, 4, 1, TOOL_H - 8 }, UI_COLOUR_HILIGHT);
    }
    ui_separator(&c, 0, TOOL_H, win->width);

    ui_panel(&c, ui_rect_shrink(view_r, -2), UI_SUNKEN);
    picture_draw(&c);
    status_draw(&c);

    ui_menu_draw(&c, &menu);
    cs_window_commit(win, 0, 0, win->width, win->height);
}

// a gif moving on, only its own part of the window changes
static void frame_draw(void) {
    ui_canvas_t c = canvas();
    ui_clip(&c, view_r);
    picture_draw(&c);
    ui_unclip(&c);
    if (menu.open)
        ui_menu_draw(&c, &menu);
    cs_window_commit(win, view_r.x, view_r.y, view_r.w, view_r.h);
}

static void title_update(void) {
    char title[CS_TITLE_MAX], name[CS_TITLE_MAX];
    if (path[0]) {
        ui_text_ascii(name, sizeof(name), base_name(path));
        snprintf(title, sizeof(title), "%s Exhibit", name);
    } else {
        snprintf(title, sizeof(title), "Exhibit");
    }
    cs_window_title(win, title);
}

static void animation_start(void) {
    frame     = 0;
    frame_due = showing() && pic.frames > 1 ? now_ms() + (uint64_t)pic.delay[0] : 0;
}

static void load(const char *p) {
    picture_free(&pic);
    snprintf(path, sizeof(path), "%s", p);
    if (path[0])
        picture_load(&pic, path);
    view_show(&pic);
    animation_start();
}

static void open_index(int i) {
    int n = folder_count();
    if (n <= 0)
        return;
    i = ((i % n) + n) % n;

    char p[EXH_PATH_MAX];
    folder_path(i, p, sizeof(p));
    load(p);
    title_update();
}

static void rotate(int clockwise) {
    if (!showing())
        return;
    picture_rotate(&pic, clockwise);
    view_turned();
}

static void delete_current(void) {
    if (!path[0])
        return;

    char text[EXH_NAME_MAX + 64];
    snprintf(text, sizeof(text), "Are you sure you want to delete '%s'?", base_name(path));
    if (!ui_ask("Confirm File Delete", text))
        return;

    if (unlink(path) < 0) {
        snprintf(text, sizeof(text), "Cannot delete '%s'.\nIt may be read only or in use by something else", base_name(path));
        ui_message("Error Deleting File", text, UI_ICON_ERROR);
        return;
    }

    int i = folder_index();
    folder_forget(i);
    if (folder_count() > 0) {
        open_index(i < folder_count() ? i : 0);
    } else {
        load("");
        title_update();
    }
}

static void open_folder(void) {
    char dir[EXH_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return;
    if (slash == dir)
        slash[1] = '\0';
    else
        *slash = '\0';

    const char *argv[] = { CABINET, dir, NULL };
    if (cs_run(argv) < 0)
        ui_message("Exhibit", "Exhibit couldnt start Cabinet.", UI_ICON_ERROR);
}

static void button_clicked(int i) {
    switch (i) {
    case BTN_PREV:         open_index(folder_index() - 1); break;
    case BTN_NEXT:         open_index(folder_index() + 1); break;
    case BTN_ZOOM_IN:      view_zoom_step(1, -1, -1); break;
    case BTN_ZOOM_OUT:     view_zoom_step(-1, -1, -1); break;
    case BTN_FIT:          view_fit(); break;
    case BTN_ACTUAL:       view_actual(); break;
    case BTN_ROTATE_LEFT:  rotate(0); break;
    case BTN_ROTATE_RIGHT: rotate(1); break;
    case BTN_DELETE:       delete_current(); break;
    }
}

static void menu_add(const char *label, int act, int disabled) {
    menu.items[menu.count]    = act == ACT_LINE ? UI_MENU_LINE : label;
    menu.disabled[menu.count] = disabled;
    menu_acts[menu.count++]   = act;
}

static void menu_show(int x, int y) {
    int none = !showing();
    menu.count = 0;
    menu_add("Zoom In", ACT_ZOOM_IN, none);
    menu_add("Zoom Out", ACT_ZOOM_OUT, none);
    menu_add("Best Fit", ACT_FIT, none || view_fitting());
    menu_add("Actual Size", ACT_ACTUAL, none || view_zoom() == 1);
    menu_add(NULL, ACT_LINE, 0);
    menu_add("Rotate Clockwise", ACT_ROTATE_RIGHT, none);
    menu_add("Rotate Anticlockwise", ACT_ROTATE_LEFT, none);
    menu_add(NULL, ACT_LINE, 0);
    menu_add("Open Containing Folder", ACT_FOLDER, !path[0]);
    menu_add(NULL, ACT_LINE, 0);
    menu_add("Delete", ACT_DELETE, !path[0]);
    ui_menu_open(&menu, x, y, (ui_rect_t){ 0, 0, win->width, win->height });
}

static void menu_do(int act) {
    switch (act) {
    case ACT_ZOOM_IN:      view_zoom_step(1, -1, -1); break;
    case ACT_ZOOM_OUT:     view_zoom_step(-1, -1, -1); break;
    case ACT_FIT:          view_fit(); break;
    case ACT_ACTUAL:       view_actual(); break;
    case ACT_ROTATE_RIGHT: rotate(1); break;
    case ACT_ROTATE_LEFT:  rotate(0); break;
    case ACT_FOLDER:       open_folder(); break;
    case ACT_DELETE:       delete_current(); break;
    }
}

static void on_mouse(const cs_event_t *ev) {
    int left      = (ev->buttons & CS_BUTTON_LEFT) != 0;
    int was_left  = (mouse_was & CS_BUTTON_LEFT) != 0;
    int right     = (ev->buttons & CS_BUTTON_RIGHT) != 0;
    int was_right = (mouse_was & CS_BUTTON_RIGHT) != 0;
    mouse_was     = ev->buttons;

    if (menu.open) {
        int r = ui_menu_mouse(&menu, ev->x, ev->y, left, right);
        if (r & UI_PICKED)
            menu_do(menu_acts[menu.picked]);
        if (r & UI_CHANGED)
            draw_all();
        return;
    }
    if (right && !was_right && ui_rect_contains(view_r, ev->x, ev->y)) {
        menu_show(ev->x, ev->y);
        draw_all();
        return;
    }

    for (int i = 0; i < BTN_COUNT; i++) {
        int r = ui_button_mouse(&buttons[i], ev->x, ev->y, left);
        if (r & UI_CLICKED)
            button_clicked(i);
        if (r)
            draw_all();
    }

    // a double click flips between the whole picture and every pixel of it
    if (left && !was_left && showing() && ui_rect_contains(view_r, ev->x, ev->y)) {
        uint64_t now = now_ms();
        if (now - last_click < DOUBLE_CLICK_MS) {
            last_click = 0;
            if (view_fitting())
                view_actual();
            else
                view_fit();
            draw_all();
            return;
        }
        last_click = now;
    }
    if (view_drag(ev->x, ev->y, left))
        draw_all();
}

static void on_key(const cs_event_t *ev) {
    if (ev->key_action != KEY_DOWN)
        return;

    if (menu.open) {
        int r = ui_menu_key(&menu, ev->keycode);
        if (r & UI_PICKED)
            menu_do(menu_acts[menu.picked]);
        if (r & UI_CHANGED)
            draw_all();
        return;
    }

    keyboard_event_t key = { .keycode = ev->keycode, .action = ev->key_action, .keymod = ev->keymod };
    char             ch  = tty_key_to_ascii(&key);

    switch (ev->keycode) {
    case ArrowLeft:
    case ArrowUp:
    case PageUp:     open_index(folder_index() - 1); break;
    case ArrowRight:
    case ArrowDown:
    case PageDown:   open_index(folder_index() + 1); break;
    case Home:       open_index(0); break;
    case End:        open_index(folder_count() - 1); break;
    case KEY_DELETE: delete_current(); break;
    case KEY_ESCAPE: quit = 1; return;
    default:
        switch (ch) {
        case ' ':           open_index(folder_index() + 1); break;
        case '+': case '=': view_zoom_step(1, -1, -1); break;
        case '-': case '_': view_zoom_step(-1, -1, -1); break;
        case '0': case 'f': view_fit(); break;
        case '1':           view_actual(); break;
        case 'r':           rotate(1); break;
        case 'l':           rotate(0); break;
        default:            return;
        }
    }
    draw_all();
}

static void on_event(const cs_event_t *ev) {
    switch (ev->type) {
    case CS_EV_CLOSE_REQUEST:
        quit = 1;
        break;
    case CS_EV_RESIZE:
        layout();
        draw_all();
        break;
    case CS_EV_MOUSE:
        on_mouse(ev);
        break;
    case CS_EV_WHEEL:
        if (showing() && ev->wheel) {
            view_zoom_step(ev->wheel > 0 ? 1 : -1, ev->x, ev->y);
            draw_all();
        }
        break;
    case CS_EV_KEY:
        on_key(ev);
        break;
    }
}

// a gif thats due its next frame, and how long till the one after
static int animate(void) {
    if (!frame_due)
        return -1;
    uint64_t now = now_ms();
    if (now >= frame_due) {
        frame = (frame + 1) % pic.frames;
        frame_due += (uint64_t)pic.delay[frame];
        if (frame_due < now)
            frame_due = now + (uint64_t)pic.delay[frame];
        frame_draw();
    }
    now = now_ms();
    return frame_due > now ? (int)(frame_due - now) : 0;
}

static int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

int main(int argc, char **argv) {
    if (cs_connect() < 0) {
        printf("exhibit: needs the desktop running\n");
        return 1;
    }
    if (ui_init() < 0) {
        printf("exhibit: no font ending in " UI_FONT_REGULAR " in " UI_FONT_DIR "\n");
        return 1;
    }

    // read before the window opens so the window can be the pictures size
    if (argc > 1) {
        char full[EXH_PATH_MAX];
        if (argv[1][0] == '/') {
            snprintf(full, sizeof(full), "%s", argv[1]);
        } else {
            char cwd[EXH_PATH_MAX];
            if (!getcwd(cwd, sizeof(cwd)))
                strcpy(cwd, "/");
            snprintf(full, sizeof(full), "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", argv[1]);
        }
        folder_scan(full);
        load(full);
    }

    int w = WINDOW_MIN_W, h = WINDOW_MIN_H;
    if (showing()) {
        w = clamp_int(pic.w + 4, WINDOW_MIN_W, WINDOW_MAX_W);
        h = clamp_int(pic.h + TOOL_H + STATUS_H + 5, WINDOW_MIN_H, WINDOW_MAX_H);
    }
    win = cs_window_open(w, h, "Exhibit");
    if (!win) {
        printf("exhibit: the desktop wouldnt give us a window\n");
        return 1;
    }

    icons_init();
    static const int btn_glyph[BTN_COUNT] = {
        GLYPH_PREV, GLYPH_NEXT, GLYPH_ZOOM_IN, GLYPH_ZOOM_OUT, GLYPH_FIT, GLYPH_ACTUAL,
        GLYPH_ROTATE_LEFT, GLYPH_ROTATE_RIGHT, GLYPH_DELETE,
    };
    for (int i = 0; i < BTN_COUNT; i++) {
        buttons[i].image = glyph(btn_glyph[i]);
        buttons[i].flat  = 1;
    }

    layout();
    view_fit();
    animation_start();
    title_update();
    draw_all();

    cs_event_t ev;
    while (!quit) {
        int wait = animate();
        if (cs_event_wait(&ev, wait) > 0)
            on_event(&ev);
    }

    cs_window_close(win);
    return 0;
}
