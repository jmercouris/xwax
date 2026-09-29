/*
 * Copyright (C) 2026 Mark Hills <mark@xwax.org>
 *
 * This file is part of "xwax".
 *
 * "xwax" is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, version 3 as
 * published by the Free Software Foundation.
 *
 * "xwax" is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 *
 */

/*
 * SDL library browser for the first deck
 *
 * The window is a full-width track list: the loaded record at the
 * top, one row per track, a status line, then Load and Quit.
 * Keys match the old terminal browser. A finger tap loads the row
 * under it; a vertical drag scrolls. SDL reads the touchscreen
 * itself, including on the kmsdrm console driver.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <SDL.h>
#include <SDL_ttf.h>

#include "external.h"
#include "interface.h"
#include "rig.h"
#include "selector.h"
#include "status.h"
#include "xwax.h"

#define REFRESH_MS 100

#define FONT "DejaVuSans.ttf"
#define BOLD_FONT "DejaVuSans-Bold.ttf"
#define FONT_SIZE 20

#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720

#define EVENT_TICKER   (SDL_USEREVENT)
#define EVENT_QUIT     (SDL_USEREVENT + 1)
#define EVENT_REDRAW   (SDL_USEREVENT + 2)

#define GESTURE_NONE   0
#define GESTURE_FINGER 1
#define GESTURE_MOUSE  2

#define HIT_NONE  0
#define HIT_LIST  1
#define HIT_LOAD  2
#define HIT_QUIT  3

struct hit {
    int where;
    int row;
};

static const char *font_dirs[] = {
    "/usr/X11R6/lib/X11/fonts/TTF",
    "/usr/share/fonts/truetype/ttf-dejavu/",
    "/usr/share/fonts/ttf-dejavu",
    "/usr/share/fonts/dejavu",
    "/usr/share/fonts/TTF",
    "/usr/share/fonts/truetype",
    "/usr/share/fonts/truetype/dejavu",
    "/usr/share/fonts/truetype/ttf-dejavu",
    NULL
};

static const SDL_Color background_col = {0, 0, 0, 255},
    header_col = {16, 16, 16, 255},
    text_col = {224, 224, 224, 255},
    dim_col = {128, 128, 128, 255},
    selected_col = {0, 48, 64, 255},
    action_col = {28, 28, 28, 255},
    action_down_col = {0, 64, 80, 255},
    rule_col = {64, 64, 64, 255},
    warn_col = {192, 64, 0, 255};

static TTF_Font *font, *bold;
static float scale = 1.0;
static iconv_t utf = (iconv_t)-1;
static pthread_t ph;
static SDL_Window *window;
static struct selector selector;
static struct observer on_status, on_selector;
static bool observers;

static int stderr_fd = -1, stderr_save = -1;
static struct rb stderr_rb;

/* Geometry of the current frame, in surface pixels */

static struct {
    int w, h;
    int row_h;
    int margin, gap;
    int label_w;
    int prefix_w;
    int title_x, title_w;
    int artist_x, artist_w;
    int list_header_y;
    int list_y;
    int list_rows;
    int status_y;
    int action_y;
} screen;

/*
 * One finger or mouse button. A short press is a tap; travel of
 * half a row, or a full row of scrolling, is a drag.
 */

static struct {
    int source;
    SDL_FingerID finger;
    float x, y;
    float last_y;
    float carry;
    int where;
    int row;
    bool dragged;
} gesture;

/*
 * Scale a pixel count by the geometry zoom
 */

static int zoom(int d)
{
    int z;

    z = d * scale;
    if (d > 0 && z < 1)
        z = 1;

    return z;
}

static TTF_Font* open_font(const char *name, int size)
{
    int pt;
    char buf[256];
    const char **dir;

    pt = zoom(size);
    if (pt < 10)
        pt = 10;

    for (dir = font_dirs; *dir != NULL; dir++) {
        struct stat st;
        TTF_Font *face;

        sprintf(buf, "%s/%s", *dir, name);
        if (stat(buf, &st) == -1) {
            if (errno != ENOENT)
                perror("stat");
            continue;
        }

        fprintf(stderr, "Loading font '%s', %dpt...\n", buf, pt);
        face = TTF_OpenFont(buf, pt);
        if (face == NULL) {
            fprintf(stderr, "Font error: %s\n", TTF_GetError());
            return NULL;
        }

        TTF_SetFontHinting(face, TTF_HINTING_NONE);
        return face;
    }

    fprintf(stderr, "Font '%s' cannot be found.\n", name);
    return NULL;
}

static int load_fonts(void)
{
    font = open_font(FONT, FONT_SIZE);
    if (font == NULL)
        return -1;

    bold = open_font(BOLD_FONT, FONT_SIZE);
    if (bold == NULL)
        return -1;

    return 0;
}

static void clear_fonts(void)
{
    if (bold != NULL)
        TTF_CloseFont(bold);
    if (font != NULL)
        TTF_CloseFont(font);
    bold = NULL;
    font = NULL;
}

/*
 * Convert a locale string into UTF-8 for SDL_ttf
 *
 * Truncates to the output buffer. On a hard conversion failure the
 * original bytes are copied through.
 */

static void locale_to_utf8(const char *in, char *out, size_t outlen)
{
    char raw[1024];
    char *ip, *op;
    size_t n, ilen, olen;

    assert(outlen > 1);

    n = strlen(in);
    if (n >= sizeof raw)
        n = sizeof raw - 1;
    memcpy(raw, in, n);
    raw[n] = '\0';

    op = out;
    olen = outlen - 1;
    if (iconv(utf, NULL, NULL, &op, &olen) == (size_t)-1)
        abort();

    ip = raw;
    ilen = n;
    if (iconv(utf, &ip, &ilen, &op, &olen) == (size_t)-1 && op == out) {
        if (n >= outlen)
            n = outlen - 1;
        memcpy(out, raw, n);
        out[n] = '\0';
        return;
    }

    *op = '\0';
}

static Uint32 map_col(SDL_Surface *sf, SDL_Color col)
{
    return SDL_MapRGB(sf->format, col.r, col.g, col.b);
}

static void fill_rect(SDL_Surface *sf, int x, int y, int w, int h,
                      SDL_Color col)
{
    SDL_Rect r;

    if (w <= 0 || h <= 0)
        return;

    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    SDL_FillRect(sf, &r, map_col(sf, col));
}

/*
 * Draw locale text, clipped to max_w. The caller has already filled
 * the row; the shaded background matches that fill.
 */

static void blit_text(SDL_Surface *sf, int x, int y, int max_w,
                      const char *text, TTF_Font *face,
                      SDL_Color fg, SDL_Color bg)
{
    char utf8[1024];
    SDL_Surface *rendered;
    SDL_Rect src, dst;

    if (text == NULL || text[0] == '\0' || max_w <= 0)
        return;

    locale_to_utf8(text, utf8, sizeof utf8);
    if (utf8[0] == '\0')
        return;

    rendered = TTF_RenderUTF8_Shaded(face, utf8, fg, bg);
    if (rendered == NULL)
        return;

    src.x = 0;
    src.y = 0;
    src.w = rendered->w < max_w ? rendered->w : max_w;
    src.h = rendered->h;

    dst.x = x;
    dst.y = y;

    SDL_BlitSurface(rendered, &src, sf, &dst);
    SDL_FreeSurface(rendered);
}

static int text_y(int row_y, TTF_Font *face)
{
    int dy;

    dy = (screen.row_h - TTF_FontHeight(face)) / 2;
    if (dy < 0)
        dy = 0;

    return row_y + dy;
}

static const char *pathname_basename(const char *pathname)
{
    const char *base;

    base = strrchr(pathname, '/');
    if (base == NULL)
        return pathname;

    return base + 1;
}

static const char *record_title(const struct record *r)
{
    if (r == NULL || r->pathname == NULL)
        return "(no track loaded)";

    if (r->title != NULL && r->title[0] != '\0')
        return r->title;

    return pathname_basename(r->pathname);
}

static const char *record_artist(const struct record *r)
{
    if (r == NULL || r->pathname == NULL)
        return "";

    if (r->artist != NULL && r->artist[0] != '\0')
        return r->artist;

    return "-";
}

static void format_duration(double seconds, char *buf, size_t len)
{
    unsigned int s;

    assert(buf != NULL);
    assert(len != 0);

    if (seconds < 0.0)
        seconds = 0.0;

    s = seconds;

    if (s >= 60 * 60) {
        unsigned int h, m;

        h = s / (60 * 60);
        s %= 60 * 60;
        m = s / 60;
        s %= 60;

        snprintf(buf, len, "%u:%02u:%02u", h, m, s);
    } else {
        snprintf(buf, len, "%u:%02u", s / 60, s % 60);
    }
}

static void format_track_time(struct deck *d, char *buf, size_t len)
{
    double elapsed, total;
    char elapsed_buf[32], total_buf[32];

    assert(buf != NULL);
    assert(len != 0);

    if (d->record == NULL || d->record->pathname == NULL) {
        snprintf(buf, len, "--:-- / --:--");
        return;
    }

    elapsed = player_get_elapsed(&d->player);
    total = 0.0;
    if (d->player.track != NULL && d->player.track->rate > 0)
        total = (double)d->player.track->length / d->player.track->rate;

    format_duration(elapsed, elapsed_buf, sizeof elapsed_buf);
    format_duration(total, total_buf, sizeof total_buf);
    snprintf(buf, len, "%s / %s", elapsed_buf, total_buf);
}

static int make_nonblocking(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL);
    if (flags == -1) {
        perror("fcntl");
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl");
        return -1;
    }

    return 0;
}

/*
 * Send process stderr to the status line
 *
 * A kmsdrm console has no separate terminal once the display is
 * taken, so importer and scanner messages are captured here.
 */

static int begin_stderr_capture(void)
{
    int pp[2];

    if (pipe(pp) == -1) {
        perror("pipe");
        return -1;
    }

    if (make_nonblocking(pp[0]) == -1)
        goto fail;

    stderr_save = dup(STDERR_FILENO);
    if (stderr_save == -1) {
        perror("dup");
        goto fail;
    }

    if (dup2(pp[1], STDERR_FILENO) == -1) {
        perror("dup2");
        goto fail_save;
    }

    if (close(pp[1]) == -1)
        abort();

    stderr_fd = pp[0];
    rb_reset(&stderr_rb);
    return 0;

fail_save:
    if (close(stderr_save) == -1)
        abort();
    stderr_save = -1;
fail:
    if (close(pp[0]) == -1)
        abort();
    if (close(pp[1]) == -1)
        abort();
    return -1;
}

static void emit_stderr_message(const char *line, size_t len)
{
    char *msg;

    msg = strndup(line, len);
    if (msg == NULL) {
        status_set(STATUS_ALERT, "Out of memory reading stderr");
        return;
    }

    if (msg[0] != '\0')
        status_printf(STATUS_ALERT, "%s", msg);

    free(msg);
}

static void flush_stderr_buffer(void)
{
    if (stderr_rb.len == 0)
        return;

    emit_stderr_message(stderr_rb.buf, stderr_rb.len);
    rb_reset(&stderr_rb);
}

static void split_stderr_buffer(void)
{
    for (;;) {
        char *eol;
        size_t len;

        eol = memchr(stderr_rb.buf, '\n', stderr_rb.len);
        if (eol == NULL)
            return;

        len = eol - stderr_rb.buf;
        emit_stderr_message(stderr_rb.buf, len);
        memmove(stderr_rb.buf, eol + 1, stderr_rb.len - len - 1);
        stderr_rb.len -= len + 1;
    }
}

static void pump_stderr(void)
{
    if (stderr_fd == -1)
        return;

    for (;;) {
        char buf[512];
        ssize_t z;

        z = read(stderr_fd, buf, sizeof buf);
        if (z > 0) {
            size_t off;

            off = 0;
            while (off < (size_t)z) {
                size_t chunk, remain;

                remain = sizeof stderr_rb.buf - stderr_rb.len;
                if (remain == 0) {
                    status_set(STATUS_ALERT, "Error output truncated");
                    rb_reset(&stderr_rb);
                    remain = sizeof stderr_rb.buf;
                }

                chunk = (size_t)z - off;
                if (chunk > remain)
                    chunk = remain;

                memcpy(stderr_rb.buf + stderr_rb.len, buf + off, chunk);
                stderr_rb.len += chunk;
                off += chunk;
                split_stderr_buffer();
            }
            continue;
        }

        if (z == 0) {
            flush_stderr_buffer();
            return;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;

        status_printf(STATUS_ALERT, "Error reading stderr: %s",
                      strerror(errno));
        return;
    }
}

static void end_stderr_capture(void)
{
    if (stderr_save == -1)
        return;

    pump_stderr();

    if (dup2(stderr_save, STDERR_FILENO) == -1)
        abort();
    if (close(stderr_save) == -1)
        abort();
    stderr_save = -1;

    pump_stderr();
    flush_stderr_buffer();

    if (close(stderr_fd) == -1)
        abort();
    stderr_fd = -1;
}

/*
 * Lay out rows from the current surface size
 *
 * List rows take whatever is left after the header, the status
 * line and the two action rows.
 */

static void measure(SDL_Surface *sf)
{
    int tw = 0, th = 0;
    int top, bottom, list_h, inner;

    screen.w = sf->w;
    screen.h = sf->h;

    screen.row_h = TTF_FontHeight(font) + zoom(14);
    if (screen.row_h < zoom(44))
        screen.row_h = zoom(44);

    screen.margin = zoom(16);
    screen.gap = zoom(12);

    if (TTF_SizeUTF8(bold, "Artist ", &tw, &th) == 0)
        screen.label_w = tw;
    else
        screen.label_w = zoom(88);

    screen.prefix_w = zoom(28);
    screen.title_x = screen.margin + screen.prefix_w;

    inner = screen.w - screen.margin * 2 - screen.prefix_w;
    if (inner < 0)
        inner = 0;

    if (inner > screen.gap) {
        screen.title_w = (inner - screen.gap) * 2 / 3;
        screen.artist_w = inner - screen.gap - screen.title_w;
    } else {
        screen.title_w = inner;
        screen.artist_w = 0;
    }
    screen.artist_x = screen.title_x + screen.title_w + screen.gap;

    /* track, artist, 2px rule, column headings */
    top = screen.row_h * 3 + 2;
    bottom = screen.row_h * 3;
    list_h = screen.h - top - bottom;
    screen.list_rows = list_h / screen.row_h;
    if (screen.list_rows < 1)
        screen.list_rows = 1;

    screen.list_header_y = screen.row_h * 2 + 2;
    screen.list_y = screen.list_header_y + screen.row_h;
    screen.status_y = screen.list_y + screen.list_rows * screen.row_h;
    screen.action_y = screen.status_y + screen.row_h;

    selector_set_lines(&selector, screen.list_rows);
}

static struct hit hit_test(float x, float y)
{
    struct hit hit;
    int iy;

    (void)x;

    hit.where = HIT_NONE;
    hit.row = -1;
    iy = y;

    if (iy >= screen.list_y
        && iy < screen.list_y + screen.list_rows * screen.row_h) {
        hit.where = HIT_LIST;
        hit.row = (iy - screen.list_y) / screen.row_h;
        return hit;
    }

    if (iy >= screen.action_y && iy < screen.action_y + screen.row_h) {
        hit.where = HIT_LOAD;
        return hit;
    }

    if (iy >= screen.action_y + screen.row_h
        && iy < screen.action_y + 2 * screen.row_h) {
        hit.where = HIT_QUIT;
        return hit;
    }

    return hit;
}

static void draw_field(SDL_Surface *sf, int y, const char *label,
                       const char *value, const char *right, bool dimmed)
{
    SDL_Color fg;
    int ty, value_w, right_w;

    fg = dimmed ? dim_col : text_col;
    fill_rect(sf, 0, y, screen.w, screen.row_h, header_col);
    ty = text_y(y, font);

    blit_text(sf, screen.margin, ty, screen.label_w, label, bold,
              dim_col, header_col);

    right_w = 0;
    if (right != NULL && right[0] != '\0') {
        char utf8[64];
        int tw = 0, th = 0;

        locale_to_utf8(right, utf8, sizeof utf8);
        if (TTF_SizeUTF8(font, utf8, &tw, &th) == 0)
            right_w = tw;
        blit_text(sf, screen.w - screen.margin - right_w, ty, right_w,
                  right, font, fg, header_col);
    }

    value_w = screen.w - screen.margin * 2 - screen.label_w - right_w - screen.gap;
    if (value_w < 0)
        value_w = 0;

    blit_text(sf, screen.margin + screen.label_w, ty, value_w,
              value, font, fg, header_col);
}

static void draw_button(SDL_Surface *sf, int y, const char *label,
                        bool pressed)
{
    SDL_Color bg;
    char utf8[32];
    int tw = 0, th = 0, x;

    bg = pressed ? action_down_col : action_col;
    fill_rect(sf, 0, y, screen.w, screen.row_h, bg);

    locale_to_utf8(label, utf8, sizeof utf8);
    if (TTF_SizeUTF8(bold, utf8, &tw, &th) != 0)
        return;

    x = (screen.w - tw) / 2;
    if (x < screen.margin)
        x = screen.margin;

    blit_text(sf, x, text_y(y, bold), tw, label, bold, text_col, bg);
}

static bool button_pressed(int where)
{
    return gesture.source != GESTURE_NONE
        && !gesture.dragged
        && gesture.where == where;
}

static void draw(SDL_Surface *sf)
{
    struct deck *d;
    const char *message;
    char timebuf[80];
    SDL_Color status_fg;
    int i, ty;
    bool loaded;

    measure(sf);

    if (SDL_MUSTLOCK(sf))
        SDL_LockSurface(sf);

    fill_rect(sf, 0, 0, screen.w, screen.h, background_col);

    d = &deck[0];
    loaded = d->record != NULL && d->record->pathname != NULL;
    format_track_time(d, timebuf, sizeof timebuf);

    draw_field(sf, 0, "Track", record_title(d->record), timebuf, !loaded);
    draw_field(sf, screen.row_h, "Artist", record_artist(d->record),
               NULL, !loaded);
    fill_rect(sf, 0, screen.row_h * 2, screen.w, 2, rule_col);

    ty = text_y(screen.list_header_y, bold);
    fill_rect(sf, 0, screen.list_header_y, screen.w, screen.row_h,
              background_col);
    blit_text(sf, screen.title_x, ty, screen.title_w, "Title", bold,
              dim_col, background_col);
    if (screen.artist_w > 0) {
        blit_text(sf, screen.artist_x, ty, screen.artist_w, "Artist",
                  bold, dim_col, background_col);
    }

    for (i = 0; i < screen.list_rows; i++) {
        int entry, row_y;
        struct record *r;
        bool selected;
        SDL_Color fg, bg, artist_fg;

        entry = listbox_map(&selector.records, i);
        if (entry == -1)
            break;

        r = selector.view_index->record[entry];
        selected = entry == listbox_current(&selector.records);
        bg = selected ? selected_col : background_col;
        fg = text_col;
        artist_fg = selected ? text_col : dim_col;
        row_y = screen.list_y + i * screen.row_h;

        fill_rect(sf, 0, row_y, screen.w, screen.row_h, bg);
        ty = text_y(row_y, font);

        if (selected) {
            blit_text(sf, screen.margin, ty, screen.prefix_w, ">",
                      bold, fg, bg);
        }

        blit_text(sf, screen.title_x, ty, screen.title_w,
                  record_title(r), font, fg, bg);
        if (screen.artist_w > 0) {
            blit_text(sf, screen.artist_x, ty, screen.artist_w,
                      record_artist(r), font, artist_fg, bg);
        }
    }

    fill_rect(sf, 0, screen.status_y, screen.w, screen.row_h, background_col);
    message = status();
    status_fg = dim_col;
    if (message[0] == '\0') {
        message = "Swipe to scroll. Tap a track to load.";
    } else if (status_level() >= STATUS_WARN) {
        status_fg = warn_col;
    } else if (status_level() >= STATUS_INFO) {
        status_fg = text_col;
    }

    blit_text(sf, screen.margin, text_y(screen.status_y, font),
              screen.w - screen.margin * 2, message, font,
              status_fg, background_col);

    draw_button(sf, screen.action_y, "Load", button_pressed(HIT_LOAD));
    draw_button(sf, screen.action_y + screen.row_h, "Quit",
                button_pressed(HIT_QUIT));

    if (SDL_MUSTLOCK(sf))
        SDL_UnlockSurface(sf);
}

static void load_current(void)
{
    struct record *r;

    if (ndeck == 0)
        return;

    r = selector_current(&selector);
    if (r != NULL)
        deck_load(&deck[0], r);
}

/*
 * Select the on-screen row and load it
 *
 * The row is already visible, so the list offset stays put.
 */

static void load_row(int row)
{
    int entry;

    entry = listbox_map(&selector.records, row);
    if (entry == -1)
        return;

    selector.records.selected = entry;
    load_current();
}

static void scroll_by_pixels(float dy)
{
    if (selector.records.entries <= 0)
        return;

    gesture.carry += dy;

    while (gesture.carry >= screen.row_h) {
        selector_up(&selector);
        gesture.carry -= screen.row_h;
        gesture.dragged = true;
    }

    while (gesture.carry <= -screen.row_h) {
        selector_down(&selector);
        gesture.carry += screen.row_h;
        gesture.dragged = true;
    }
}

static void gesture_begin(int source, SDL_FingerID finger, float x, float y)
{
    struct hit hit;

    if (gesture.source != GESTURE_NONE)
        return;

    hit = hit_test(x, y);
    gesture.source = source;
    gesture.finger = finger;
    gesture.x = x;
    gesture.y = y;
    gesture.last_y = y;
    gesture.carry = 0.0;
    gesture.where = hit.where;
    gesture.row = hit.row;
    gesture.dragged = false;
}

static void gesture_move(float x, float y)
{
    float dy;

    dy = y - gesture.last_y;
    gesture.last_y = y;

    if (gesture.where == HIT_LIST) {
        scroll_by_pixels(dy);
        return;
    }

    /* A slide off a button is not a tap */
    if (fabsf(x - gesture.x) >= screen.row_h / 2.0f
        || fabsf(y - gesture.y) >= screen.row_h / 2.0f)
        gesture.dragged = true;
}

/*
 * Return: false if the interface should exit
 */

static bool gesture_end(void)
{
    bool live;

    live = true;

    if (gesture.source == GESTURE_NONE)
        return true;

    if (!gesture.dragged) {
        switch (gesture.where) {
        case HIT_LIST:
            load_row(gesture.row);
            break;
        case HIT_LOAD:
            load_current();
            break;
        case HIT_QUIT:
            rig_quit();
            live = false;
            break;
        default:
            break;
        }
    }

    gesture.source = GESTURE_NONE;
    return live;
}

/*
 * Return: false if the interface should exit
 */

static bool handle_key(SDL_Keycode key)
{
    switch (key) {
    case SDLK_UP:
        selector_up(&selector);
        break;

    case SDLK_DOWN:
        selector_down(&selector);
        break;

    case SDLK_PAGEUP:
        selector_page_up(&selector);
        break;

    case SDLK_PAGEDOWN:
        selector_page_down(&selector);
        break;

    case SDLK_RIGHT:
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        load_current();
        break;

    case SDLK_q:
    case SDLK_ESCAPE:
        rig_quit();
        return false;

    default:
        break;
    }

    return true;
}

static void push_event(int t)
{
    SDL_Event e;

    if (!SDL_PeepEvents(&e, 1, SDL_PEEKEVENT, t, t)) {
        e.type = t;
        if (SDL_PushEvent(&e) == -1)
            abort();
    }
}

static Uint32 ticker(Uint32 interval, void *p)
{
    (void)p;
    push_event(EVENT_TICKER);
    return interval;
}

static void defer_redraw(struct observer *o, void *x)
{
    (void)o;
    (void)x;
    push_event(EVENT_REDRAW);
}

/*
 * Map a window position into surface pixels
 */

static void to_surface(float x, float y, float *sx, float *sy)
{
    int ww, wh;

    SDL_GetWindowSize(window, &ww, &wh);
    if (ww <= 0 || wh <= 0 || screen.w <= 0 || screen.h <= 0) {
        *sx = x;
        *sy = y;
        return;
    }

    *sx = x * screen.w / ww;
    *sy = y * screen.h / wh;
}

/*
 * Handle one SDL event
 *
 * Return: false if the interface thread should finish
 */

static bool handle_event(SDL_Event *event, SDL_Surface **surface)
{
    switch (event->type) {
    case SDL_QUIT:
        rig_quit();
        return false;

    case SDL_WINDOWEVENT:
        switch (event->window.event) {
        case SDL_WINDOWEVENT_RESIZED:
        case SDL_WINDOWEVENT_SIZE_CHANGED:
            *surface = SDL_GetWindowSurface(window);
            if (*surface == NULL)
                return false;
            break;

        default:
            break;
        }
        break;

    case EVENT_QUIT:
        return false;

    case EVENT_TICKER:
    case EVENT_REDRAW:
        break;

    case SDL_KEYDOWN:
        if (!handle_key(event->key.keysym.sym))
            return false;
        break;

    case SDL_MOUSEWHEEL:
        if (event->wheel.y > 0)
            selector_up(&selector);
        else if (event->wheel.y < 0)
            selector_down(&selector);
        break;

    case SDL_MOUSEBUTTONDOWN:
        if (event->button.which != SDL_TOUCH_MOUSEID
            && event->button.button == SDL_BUTTON_LEFT) {
            float x, y;

            to_surface(event->button.x, event->button.y, &x, &y);
            gesture_begin(GESTURE_MOUSE, 0, x, y);
        }
        break;

    case SDL_MOUSEMOTION:
        if (gesture.source == GESTURE_MOUSE) {
            float x, y;

            to_surface(event->motion.x, event->motion.y, &x, &y);
            gesture_move(x, y);
        }
        break;

    case SDL_MOUSEBUTTONUP:
        if (gesture.source == GESTURE_MOUSE
            && event->button.button == SDL_BUTTON_LEFT) {
            if (!gesture_end())
                return false;
        }
        break;

    case SDL_FINGERDOWN: {
        float x, y;

        x = event->tfinger.x * screen.w;
        y = event->tfinger.y * screen.h;
        gesture_begin(GESTURE_FINGER, event->tfinger.fingerId, x, y);
        break;
    }

    case SDL_FINGERMOTION:
        if (gesture.source == GESTURE_FINGER
            && gesture.finger == event->tfinger.fingerId) {
            float x, y;

            x = event->tfinger.x * screen.w;
            y = event->tfinger.y * screen.h;
            gesture_move(x, y);
        }
        break;

    case SDL_FINGERUP:
        if (gesture.source == GESTURE_FINGER
            && gesture.finger == event->tfinger.fingerId) {
            if (!gesture_end())
                return false;
        }
        break;

    default:
        break;
    }

    return true;
}

static int interface_main(void)
{
    SDL_TimerID timer;
    SDL_Surface *surface;

    surface = SDL_GetWindowSurface(window);
    if (surface == NULL) {
        fprintf(stderr, "%s\n", SDL_GetError());
        return -1;
    }

    rig_lock();
    draw(surface);
    rig_unlock();
    SDL_UpdateWindowSurface(window);

    timer = SDL_AddTimer(REFRESH_MS, ticker, NULL);

    for (;;) {
        SDL_Event event;

        if (SDL_WaitEvent(&event) == 0)
            break;

        rig_lock();
        pump_stderr();

        do {
            if (!handle_event(&event, &surface))
                goto finish;
        } while (SDL_PollEvent(&event) > 0);

        if (surface != NULL)
            draw(surface);

        rig_unlock();

        if (surface != NULL)
            SDL_UpdateWindowSurface(window);
        continue;

    finish:
        rig_unlock();
        break;
    }

    SDL_RemoveTimer(timer);
    return 0;
}

static void *launch(void *p)
{
    (void)p;
    interface_main();
    return NULL;
}

/*
 * Parse a geometry string into size, position and scale
 *
 * The format is "[<n>x<n>][+<n>+<n>][/<f>]".
 *
 * Return: -1 if the string could not be actioned, otherwise 0
 */

static int parse_geometry(const char *s, int *x, int *y,
                          int *width, int *height, float *scale_out)
{
    int n, len;
    char buf[128];

    n = sscanf(s, "%[0-9]x%d%n", buf, height, &len);
    switch (n) {
    case EOF:
        return 0;
    case 0:
        break;
    case 2:
        *width = atoi(buf);
        s += len;
        break;
    default:
        return -1;
    }

    n = sscanf(s, "+%d+%d%n", x, y, &len);
    switch (n) {
    case EOF:
        return 0;
    case 0:
        break;
    case 2:
        s += len;
        break;
    default:
        return -1;
    }

    n = sscanf(s, "/%f%n", scale_out, &len);
    switch (n) {
    case EOF:
        return 0;
    case 0:
        break;
    case 1:
        if (*scale_out <= 0.0)
            return -1;
        s += len;
        break;
    default:
        return -1;
    }

    if (*s != '\0')
        return -1;

    return 0;
}

static void cleanup(void)
{
    if (observers) {
        ignore(&on_status);
        ignore(&on_selector);
        selector_clear(&selector);
        observers = false;
    }

    clear_fonts();

    if (utf != (iconv_t)-1) {
        if (iconv_close(utf) == -1)
            abort();
        utf = (iconv_t)-1;
    }

    if (window != NULL) {
        SDL_DestroyWindow(window);
        window = NULL;
    }

    TTF_Quit();
    SDL_Quit();

    end_stderr_capture();
    status_set_output(true);

    if (status()[0] != '\0')
        fprintf(stderr, "%s\n", status());
}

/*
 * Start the SDL interface
 */

int interface_start(struct library *lib, const char *geo, bool decor)
{
    int x = SDL_WINDOWPOS_UNDEFINED,
        y = SDL_WINDOWPOS_UNDEFINED,
        width = DEFAULT_WIDTH,
        height = DEFAULT_HEIGHT;
    Uint32 window_flags = SDL_WINDOW_RESIZABLE;

    if (ndeck == 0) {
        fprintf(stderr, "No deck to display.\n");
        return -1;
    }

    if (parse_geometry(geo, &x, &y, &width, &height, &scale) == -1) {
        fprintf(stderr, "Window geometry ('%s') is not valid.\n", geo);
        return -1;
    }

    if (!decor)
        window_flags |= SDL_WINDOW_BORDERLESS;

    fprintf(stderr, "Initialising fonts...\n");

    if (TTF_Init() == -1) {
        fprintf(stderr, "%s\n", TTF_GetError());
        return -1;
    }

    if (load_fonts() == -1)
        goto fail_fonts;

    fprintf(stderr, "Initialising SDL...\n");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
        fprintf(stderr, "%s\n", SDL_GetError());
        goto fail_fonts;
    }

    window = SDL_CreateWindow(banner, x, y, width, height, window_flags);
    if (window == NULL) {
        fprintf(stderr, "%s\n", SDL_GetError());
        goto fail_sdl;
    }

    utf = iconv_open("UTF8", "");
    if (utf == (iconv_t)-1) {
        perror("iconv_open");
        goto fail_sdl;
    }

    if (begin_stderr_capture() == -1)
        goto fail_iconv;

    status_set_output(false);
    selector_init(&selector, lib);
    watch(&on_status, &status_changed, defer_redraw);
    watch(&on_selector, &selector.changed, defer_redraw);
    observers = true;

    if (pthread_create(&ph, NULL, launch, NULL) != 0) {
        perror("pthread_create");
        cleanup();
        return -1;
    }

    return 0;

fail_iconv:
    if (iconv_close(utf) == -1)
        abort();
    utf = (iconv_t)-1;
fail_sdl:
    SDL_Quit();
fail_fonts:
    clear_fonts();
    TTF_Quit();
    return -1;
}

/*
 * Synchronise with the SDL interface and exit
 */

void interface_stop(void)
{
    push_event(EVENT_QUIT);

    if (pthread_join(ph, NULL) != 0)
        abort();

    cleanup();
}
