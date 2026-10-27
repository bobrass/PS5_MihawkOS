/* SPDX-License-Identifier: GPL-3.0-or-later
 * src/pw_loading_art.c: the still picture and the animation stay inside their rectangle, the globe, sky and meadow
 * have their colours, and the bar's liquid moves from one frame to the next. With an argument, the folder to write
 * frames to (PPM) for a look by eye. */
#include "../src/pw_loading_art.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W PW_LOADING_WIDTH
#define H PW_LOADING_HEIGHT

static unsigned red(uint32_t p) { return (p >> 16) & 0xff; }
static unsigned green(uint32_t p) { return (p >> 8) & 0xff; }
static unsigned blue(uint32_t p) { return p & 0xff; }

static void write_ppm(const char *folder, const char *name, const uint32_t *canvas)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", folder, name);
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P6\n%d %d\n255\n", W, H);
    for (size_t i = 0; i < (size_t)W * H; ++i) {
        const unsigned char px[3] = {(unsigned char)red(canvas[i]), (unsigned char)green(canvas[i]), (unsigned char)blue(canvas[i])};
        fwrite(px, 1, 3, file);
    }
    fclose(file);
}

int main(int argc, char **argv)
{
    uint32_t *base = malloc((size_t)W * H * 4), *a = malloc((size_t)W * H * 4), *b = malloc((size_t)W * H * 4);
    assert(base && a && b);
    pw_loading_art_base(base);
    memcpy(a, base, (size_t)W * H * 4);
    memcpy(b, base, (size_t)W * H * 4);
    pw_loading_art_frame(a, base, 0);
    pw_loading_art_frame(b, base, 25);

    /* every pixel is opaque, and a frame changes nothing outside its rectangle */
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const size_t i = (size_t)y * W + (size_t)x;
            assert((base[i] >> 24) == 0xff && (a[i] >> 24) == 0xff);
            if (x < PW_LOADING_DYN_X0 || x >= PW_LOADING_DYN_X1 || y < PW_LOADING_DYN_Y0 || y >= PW_LOADING_DYN_Y1)
                assert(a[i] == base[i] && b[i] == base[i]);
        }
    /* the sky at the top is deep blue, the meadow at the bottom green, the globe's centre bright aqua */
    const uint32_t sky = base[(size_t)40 * W + 960], meadow = base[(size_t)1060 * W + 1300], core = a[(size_t)330 * W + 945];
    assert(blue(sky) > 150 && blue(sky) > red(sky) + 80);
    assert(green(meadow) > red(meadow) + 40 && green(meadow) > blue(meadow) + 40);
    assert(blue(core) > 200 && green(core) > 150 && red(core) < 200);
    /* the liquid has moved: the bar's middle row differs between the two frames */
    unsigned changed = 0;
    for (int x = 600; x < 1320; ++x) changed += a[(size_t)714 * W + (size_t)x] != b[(size_t)714 * W + (size_t)x];
    assert(changed > 100);
    /* the tip card is lighter than the backdrop beside it */
    const uint32_t card = base[(size_t)940 * W + 400], beside = base[(size_t)940 * W + 860];
    assert(red(card) + green(card) + blue(card) > red(beside) + green(beside) + blue(beside));

    /* a determinate tube is green up to its fraction and clear after it */
    memcpy(a, base, (size_t)W * H * 4);
    pw_loading_art_tube(a, 660, 520, 1260, 548, 0.5f, 0);
    const uint32_t full = a[(size_t)534 * W + 800], empty = a[(size_t)534 * W + 1150];
    assert(green(full) > red(full) + 60 && green(full) > blue(full) + 60);
    assert(!(green(empty) > red(empty) + 60 && green(empty) > blue(empty) + 60));
    /* the small spinner stays inside its box: 2.1 radii wide and 0.65 high each side, plus its bubbles */
    memcpy(b, base, (size_t)W * H * 4);
    pw_loading_art_spinner(b, 960, 228, 16, 7);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (b[(size_t)y * W + (size_t)x] != base[(size_t)y * W + (size_t)x])
                assert(x > 960 - 16 * 2.3 && x < 960 + 16 * 2.3 && y > 228 - 16 * 1.2 && y < 228 + 16 * 1.2);

    if (argc > 1) {
        write_ppm(argv[1], "loading-0", a);
        write_ppm(argv[1], "loading-25", b);
    }
    free(base); free(a); free(b);
    puts("test_pw_loading_art: ok");
    return 0;
}
