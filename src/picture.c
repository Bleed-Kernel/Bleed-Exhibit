#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <exhibit.h>
#include <stb_image.h>

#define BYTES_MAX       (512UL << 20)
#define GIF_SLOWEST     20              // browsers treat anything quicker as broken and use 100ms
#define GIF_DEFAULT     100
#define FOLDER_MAX      4096
#define DIRENT_FILE     1

static const char *const exts[] = {
    "png", "jpg", "jpeg", "jpe", "jfif", "gif", "bmp", "tga", "psd", "pnm", "ppm", "pgm", "hdr", "pic", NULL
};

int picture_name_ok(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name)
        return 0;
    for (int i = 0; exts[i]; i++) {
        if (!strcasecmp(dot + 1, exts[i]))
            return 1;
    }
    return 0;
}

static void format_of(const uint8_t *b, size_t n, const char *path, char *out) {
    const char *f = NULL;
    if (n >= 8 && !memcmp(b, "\x89PNG", 4))                  f = "PNG";
    else if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8)         f = "JPEG";
    else if (n >= 6 && !memcmp(b, "GIF8", 4))                f = "GIF";
    else if (n >= 2 && b[0] == 'B' && b[1] == 'M')           f = "BMP";
    else if (n >= 4 && !memcmp(b, "8BPS", 4))                f = "PSD";
    else if (n >= 2 && b[0] == '#' && b[1] == '?')           f = "HDR";
    else if (n >= 4 && !memcmp(b, "\x53\x80\xF6\x34", 4))    f = "PIC";
    else if (n >= 2 && b[0] == 'P' && (b[1] == '5' || b[1] == '6')) f = "PNM";

    if (f) {
        strcpy(out, f);
        return;
    }
    // tga has no magic, its name is all there is to go on great design guys
    const char *dot = strrchr(path, '.');
    snprintf(out, 8, "%s", dot ? dot + 1 : "?");
    for (char *c = out; *c; c++) {
        if (*c >= 'a' && *c <= 'z')
            *c -= 32;
    }
}

static uint8_t *read_all(const char *path, size_t *size) {
    *size = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;

    uint8_t *data = NULL;
    long     len  = -1;
    if (fseek(f, 0, SEEK_END) == 0)
        len = ftell(f);
    if (len > 0 && (size_t)len <= BYTES_MAX && fseek(f, 0, SEEK_SET) == 0 && (data = malloc((size_t)len))) {
        if (fread(data, 1, (size_t)len, f) != (size_t)len) {
            free(data);
            data = NULL;
        }
    }
    fclose(f);
    *size = len > 0 ? (size_t)len : 0;
    return data;
}

void picture_free(picture_t *p) {
    if (p->frame)
        free(p->frame[0]);
    free(p->frame);
    free(p->delay);
    memset(p, 0, sizeof(*p));
}

// rgba bytes as stb gives them into the 0xAARRGGBB the rest of the desktop draws with
static int take_frames(picture_t *p, const uint8_t *rgba, int w, int h, int frames) {
    size_t    count = (size_t)w * h;
    uint32_t *all   = malloc(count * frames * sizeof(uint32_t));
    p->frame        = calloc((size_t)frames, sizeof(uint32_t *));
    p->delay        = calloc((size_t)frames, sizeof(int));
    if (!all || !p->frame || !p->delay) {
        free(all);
        return -1;
    }

    for (size_t i = 0; i < count * frames; i++, rgba += 4) {
        all[i] = (uint32_t)rgba[3] << 24 | (uint32_t)rgba[0] << 16 | (uint32_t)rgba[1] << 8 | rgba[2];
        if (rgba[3] != 255)
            p->see_through = 1;
    }
    for (int f = 0; f < frames; f++)
        p->frame[f] = all + count * f;

    p->w      = w;
    p->h      = h;
    p->frames = frames;
    return 0;
}

int picture_load(picture_t *p, const char *path) {
    memset(p, 0, sizeof(*p));

    size_t   size;
    uint8_t *file = read_all(path, &size);
    if (!file) {
        snprintf(p->error, sizeof(p->error), size > BYTES_MAX ? "The file is too big to open." : "The file couldnt be read.");
        return -1;
    }
    p->size = size;
    format_of(file, size, path, p->format);

    int      w = 0, h = 0, comp, frames = 1;
    int     *delays = NULL;
    uint8_t *rgba;
    if (!strcmp(p->format, "GIF"))
        rgba = stbi_load_gif_from_memory(file, (int)size, &delays, &w, &h, &frames, &comp, 4);
    else
        rgba = stbi_load_from_memory(file, (int)size, &w, &h, &comp, 4);
    free(file);

    if (!rgba) {
        const char *why = stbi_failure_reason();
        snprintf(p->error, sizeof(p->error), "%s", why ? why : "Its not a picture Exhibit knows.");
        if (p->error[0] >= 'a' && p->error[0] <= 'z')
            p->error[0] -= 32;
        free(delays);
        return -1;
    }

    if ((uint64_t)w * h * frames * 4 > BYTES_MAX || take_frames(p, rgba, w, h, frames) < 0) {
        stbi_image_free(rgba);
        free(delays);
        picture_free(p);
        p->size = size;
        snprintf(p->error, sizeof(p->error), "Theres not enough memory to show a picture this big.");
        return -1;
    }
    stbi_image_free(rgba);

    for (int f = 0; f < frames; f++) {
        int d = delays ? delays[f] : 0;
        p->delay[f] = d < GIF_SLOWEST ? GIF_DEFAULT : d;
    }
    free(delays);
    return 0;
}

// a quarter turn of every frame, into a new block since the shape changes
void picture_rotate(picture_t *p, int clockwise) {
    if (!p->frame)
        return;

    int       w = p->w, h = p->h;
    size_t    count = (size_t)w * h;
    uint32_t *all   = malloc(count * p->frames * sizeof(uint32_t));
    if (!all)
        return;

    for (int f = 0; f < p->frames; f++) {
        const uint32_t *src = p->frame[f];
        uint32_t       *dst = all + count * f;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                size_t to = clockwise ? (size_t)x * h + (h - 1 - y) : (size_t)(w - 1 - x) * h + y;
                dst[to] = src[(size_t)y * w + x];
            }
        }
    }
    free(p->frame[0]);
    for (int f = 0; f < p->frames; f++)
        p->frame[f] = all + count * f;
    p->w = h;
    p->h = w;
    p->turned++;
}

static char  folder[EXH_PATH_MAX];
static char (*names)[EXH_NAME_MAX];
static int   name_count;
static int   current = -1;

static int by_name(const void *a, const void *b) {
    return strcasecmp(a, b);
}

int folder_count(void) {
    return name_count;
}

int folder_index(void) {
    return current;
}

void folder_path(int i, char *out, size_t size) {
    if (i < 0 || i >= name_count) {
        out[0] = '\0';
        return;
    }
    snprintf(out, size, "%s%s%s", folder, strcmp(folder, "/") ? "/" : "", names[i]);
    current = i;
}

// after a delete, so the list doesnt have to be read again
void folder_forget(int i) {
    if (i < 0 || i >= name_count)
        return;
    memmove(names[i], names[i + 1], (size_t)(name_count - i - 1) * EXH_NAME_MAX);
    name_count--;
    if (current >= name_count)
        current = name_count - 1;
}

void folder_scan(const char *path) {
    free(names);
    names      = NULL;
    name_count = 0;
    current    = -1;

    const char *slash = strrchr(path, '/');
    const char *base  = slash ? slash + 1 : path;
    if (!slash)
        strcpy(folder, ".");
    else if (slash == path)
        strcpy(folder, "/");
    else
        snprintf(folder, sizeof(folder), "%.*s", (int)(slash - path), path);

    DIR *dir = opendir(folder);
    if (!dir)
        return;
    names = malloc((size_t)FOLDER_MAX * EXH_NAME_MAX);
    struct dirent *e;
    while (names && name_count < FOLDER_MAX && (e = readdir(dir))) {
        if (e->d_type == DIRENT_FILE && picture_name_ok(e->d_name)) {
            snprintf(names[name_count], EXH_NAME_MAX, "%s", e->d_name);
            name_count++;
        }
    }
    closedir(dir);

    qsort(names, (size_t)name_count, EXH_NAME_MAX, by_name);
    for (int i = 0; i < name_count; i++) {
        if (!strcmp(names[i], base))
            current = i;
    }
}
