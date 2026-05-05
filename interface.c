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
 * Simple ncurses TUI for a single deck
 *
 * Top: currently loaded track
 * Bottom: scrollable list of available tracks
 * Keys: Up/Down to navigate, Right/Enter to load, q to quit
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ncurses.h>

#include "deck.h"
#include "external.h"
#include "interface.h"
#include "library.h"
#include "listbox.h"
#include "rig.h"
#include "selector.h"
#include "status.h"
#include "xwax.h"

#define REFRESH_MS 100

static struct selector selector;
static pthread_t ph;
static volatile bool running;
static int stderr_fd = -1, stderr_save = -1;
static struct rb stderr_rb;

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

    if (r->title[0] != '\0')
        return r->title;

    return pathname_basename(r->pathname);
}

static const char *record_artist(const struct record *r)
{
    if (r == NULL || r->pathname == NULL)
        return "";

    if (r->artist[0] != '\0')
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
                size_t chunk;
                size_t remain;

                remain = sizeof(stderr_rb.buf) - stderr_rb.len;
                if (remain == 0) {
                    status_set(STATUS_ALERT, "Error output truncated");
                    rb_reset(&stderr_rb);
                    remain = sizeof(stderr_rb.buf);
                }

                chunk = z - off;
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

static void draw_screen(void)
{
    enum {
        HEADER_VALUE_COL = 9,
        LIST_PREFIX_COL = 3,
        LIST_GAP = 2,
        RIGHT_MARGIN = 1
    };

    int rows, cols;
    struct deck *d;
    struct record *r;
    const char *message;
    const char *title, *artist, *list_title, *list_artist;
    char timebuf[80];
    int i, list_header_row, list_start, list_height, tx;
    int title_width, artist_width, time_width;
    int list_cols, list_title_width, list_artist_col, list_artist_width;
    bool show_list_headers;

    getmaxyx(stdscr, rows, cols);
    erase();

    d = &deck[0];

    /* Header: loaded track */

    title = record_title(d->record);
    artist = record_artist(d->record);
    format_track_time(d, timebuf, sizeof timebuf);

    tx = cols - (int)strlen(timebuf) - RIGHT_MARGIN;
    if (tx < HEADER_VALUE_COL)
        tx = HEADER_VALUE_COL;

    title_width = tx - HEADER_VALUE_COL - 1;
    if (title_width < 0)
        title_width = 0;

    artist_width = cols - HEADER_VALUE_COL - RIGHT_MARGIN;
    if (artist_width < 0)
        artist_width = 0;

    time_width = cols - tx - RIGHT_MARGIN;
    if (time_width < 0)
        time_width = 0;

    mvhline(0, 0, ' ', cols);
    mvhline(1, 0, ' ', cols);

    attron(A_BOLD);
    mvprintw(0, 0, " Track:");
    mvprintw(1, 0, "Artist:");
    attroff(A_BOLD);

    if (d->record && d->record->pathname != NULL) {
        mvaddnstr(0, HEADER_VALUE_COL, title, title_width);
        mvaddnstr(0, tx, timebuf, time_width);
        mvaddnstr(1, HEADER_VALUE_COL, artist, artist_width);
    } else {
        attron(A_DIM);
        mvaddnstr(0, HEADER_VALUE_COL, title, title_width);
        mvaddnstr(0, tx, timebuf, time_width);
        mvaddnstr(1, HEADER_VALUE_COL, artist, artist_width);
        attroff(A_DIM);
    }

    mvhline(2, 0, ACS_HLINE, cols);

    /* Track list */

    show_list_headers = rows >= 6;
    if (show_list_headers) {
        list_header_row = 3;
        list_start = 4;
    } else {
        list_header_row = -1;
        list_start = 3;
    }

    list_height = rows - list_start - 1;
    if (list_height < 1)
        list_height = 1;

    list_cols = cols - LIST_PREFIX_COL - RIGHT_MARGIN;
    if (list_cols < 0)
        list_cols = 0;

    if (list_cols > LIST_GAP) {
        list_title_width = (list_cols - LIST_GAP) * 2 / 3;
        list_artist_width = list_cols - LIST_GAP - list_title_width;
    } else {
        list_title_width = list_cols;
        list_artist_width = 0;
    }

    list_artist_col = LIST_PREFIX_COL + list_title_width + LIST_GAP;

    selector_set_lines(&selector, list_height);

    if (show_list_headers) {
        mvhline(list_header_row, 0, ' ', cols);
        attron(A_BOLD);
        mvaddnstr(list_header_row, LIST_PREFIX_COL, "Title", list_title_width);
        if (list_artist_width > 0 && list_artist_col < cols)
            mvaddnstr(list_header_row, list_artist_col, "Artist",
                      list_artist_width);
        attroff(A_BOLD);
    }

    for (i = 0; i < list_height; i++) {
        int row;
        int entry = listbox_map(&selector.records, i);
        if (entry == -1)
            break;

        row = list_start + i;
        r = selector.view_index->record[entry];
        list_title = record_title(r);
        list_artist = record_artist(r);

        if (entry == listbox_current(&selector.records)) {
            attron(A_REVERSE);
            mvhline(row, 0, ' ', cols);
            mvaddstr(row, 0, " > ");
            mvaddnstr(row, LIST_PREFIX_COL, list_title, list_title_width);
            if (list_artist_width > 0 && list_artist_col < cols)
                mvaddnstr(row, list_artist_col, list_artist,
                          list_artist_width);
            attroff(A_REVERSE);
        } else {
            mvhline(row, 0, ' ', cols);
            mvaddstr(row, 0, "   ");
            mvaddnstr(row, LIST_PREFIX_COL, list_title, list_title_width);
            if (list_artist_width > 0 && list_artist_col < cols)
                mvaddnstr(row, list_artist_col, list_artist,
                          list_artist_width);
        }
    }

    /* Key hints at bottom */

    mvhline(rows - 1, 0, ' ', cols);

    message = status();
    if (message[0] != '\0') {
        int attr = A_DIM;

        if (status_level() >= STATUS_WARN)
            attr = A_BOLD;

        attron(attr);
        mvprintw(rows - 1, 0, " %s", message);
        attroff(attr);
    } else {
        attron(A_DIM);
        mvprintw(rows - 1, 0,
                 " q:quit  Up/Down:navigate  Right/Enter:load track");
        attroff(A_DIM);
    }

    refresh();
}

static void handle_key(int ch)
{
    switch (ch) {
    case KEY_UP:
        selector_up(&selector);
        break;

    case KEY_DOWN:
        selector_down(&selector);
        break;

    case KEY_PPAGE:
        selector_page_up(&selector);
        break;

    case KEY_NPAGE:
        selector_page_down(&selector);
        break;

    case KEY_RIGHT:
    case '\n': {
        struct record *r = selector_current(&selector);
        if (r != NULL)
            deck_load(&deck[0], r);
        break;
    }

    case 'q':
        running = false;
        rig_quit();
        break;
    }
}

static void *launch(void *arg)
{
    (void)arg;

    while (running) {
        int ch;

        pump_stderr();
        draw_screen();
        ch = getch();
        pump_stderr();
        if (ch != ERR)
            handle_key(ch);
    }

    return NULL;
}

/*
 * Start the TUI interface
 */

int interface_start(struct library *lib, const char *geo, bool decor)
{
    (void)geo;
    (void)decor;

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    timeout(REFRESH_MS);
    curs_set(0);

    if (begin_stderr_capture() == -1) {
        endwin();
        return -1;
    }

    status_set_output(false);
    selector_init(&selector, lib);

    running = true;

    if (pthread_create(&ph, NULL, launch, NULL)) {
        perror("pthread_create");
        selector_clear(&selector);
        endwin();
        end_stderr_capture();
        status_set_output(true);
        return -1;
    }

    return 0;
}

/*
 * Synchronise with the TUI interface and exit
 */

void interface_stop(void)
{
    running = false;

    if (pthread_join(ph, NULL) != 0)
        abort();

    selector_clear(&selector);
    endwin();
    end_stderr_capture();
    status_set_output(true);
}
