#include <stdio.h>
#include <stdlib.h>
#include <syscalls.h>
#include <crime/crime.h>
#include <exhibit.h>

// the toolbars pictures are pngs in res/icons, built into the binary as .bleed.exh.<file name>
static const char *glyph_files[GLYPHS] = {
    "prev", "next", "zoomin", "zoomout", "fit", "actual", "rotleft", "rotright", "delete",
};

static ui_image_t glyphs[GLYPHS];

const ui_image_t *glyph(int which) {
    return which >= 0 && which < GLYPHS ? &glyphs[which] : NULL;
}

void icons_init(void) {
    char self[EXH_PATH_MAX];
    task_path((uint64_t)_getpid(), self, sizeof(self));

    for (int g = 0; g < GLYPHS; g++) {
        char section[64];
        snprintf(section, sizeof(section), ".bleed.exh.tool-%s", glyph_files[g]);

        size_t   size;
        uint8_t *png = cs_res_read(self, section, &size);
        if (!png)
            continue;
        ui_image_decode(&glyphs[g], png, size);
        free(png);
    }
}
