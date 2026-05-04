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
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <ncurses.h>

#include "deck.h"
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

    return "(unknown artist)";
}

static void format_record_label(const struct record *r, char *buf, size_t len)
{
    assert(buf != NULL);
    assert(len != 0);

    if (r == NULL || r->pathname == NULL) {
        snprintf(buf, len, "(no track loaded)");
    } else if (r->artist[0] != '\0' && r->title[0] != '\0') {
        snprintf(buf, len, "%s - %s", r->artist, r->title);
    } else if (r->title[0] != '\0') {
        snprintf(buf, len, "%s", r->title);
    } else if (r->artist[0] != '\0') {
        snprintf(buf, len, "%s", r->artist);
    } else {
        snprintf(buf, len, "%s", pathname_basename(r->pathname));
    }
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

static void draw_screen(void)
{
    int rows, cols;
    struct deck *d;
    struct record *r;
    const char *message;
    const char *title, *artist;
    char label[512], timebuf[80];
    int i, list_start, list_height, tx, title_width, artist_width;

    getmaxyx(stdscr, rows, cols);
    erase();

    d = &deck[0];

    /* Header: loaded track */

    title = record_title(d->record);
    artist = record_artist(d->record);
    format_track_time(d, timebuf, sizeof timebuf);

    tx = cols - (int)strlen(timebuf);
    if (tx < 9)
        tx = 9;

    title_width = tx - 10;
    if (title_width < 0)
        title_width = 0;

    artist_width = cols - 10;
    if (artist_width < 0)
        artist_width = 0;

    attron(A_BOLD);
    mvprintw(0, 0, " Track:");
    mvprintw(1, 0, "Artist:");
    attroff(A_BOLD);

    if (d->record && d->record->pathname != NULL) {
        mvaddnstr(0, 8, " ", 1);
        mvaddnstr(0, 9, title, title_width);
        mvprintw(0, tx, "%s", timebuf);
        mvaddnstr(1, 8, " ", 1);
        mvaddnstr(1, 9, artist, artist_width);
    } else {
        attron(A_DIM);
        mvaddnstr(0, 8, " ", 1);
        mvaddnstr(0, 9, title, title_width);
        mvprintw(0, tx, "%s", timebuf);
        mvaddnstr(1, 8, " ", 1);
        mvaddnstr(1, 9, artist, artist_width);
        attroff(A_DIM);
    }

    mvhline(2, 0, ACS_HLINE, cols);

    /* Track list */

    list_start = 3;
    list_height = rows - list_start - 1;
    if (list_height < 1)
        list_height = 1;

    selector_set_lines(&selector, list_height);

    for (i = 0; i < list_height; i++) {
        int entry = listbox_map(&selector.records, i);
        if (entry == -1)
            break;

        r = selector.view_index->record[entry];
        format_record_label(r, label, sizeof label);

        if (entry == listbox_current(&selector.records)) {
            attron(A_REVERSE);
            mvhline(list_start + i, 0, ' ', cols);
            mvprintw(list_start + i, 0, " > %s", label);
            attroff(A_REVERSE);
        } else {
            mvprintw(list_start + i, 0, "   %s", label);
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

        draw_screen();
        ch = getch();
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

    status_set_output(false);
    selector_init(&selector, lib);

    running = true;

    if (pthread_create(&ph, NULL, launch, NULL)) {
        perror("pthread_create");
        selector_clear(&selector);
        status_set_output(true);
        endwin();
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
    status_set_output(true);
}
