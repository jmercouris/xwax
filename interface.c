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
#include "xwax.h"

#define REFRESH_MS 100

static struct selector selector;
static pthread_t ph;
static volatile bool running;

static void draw_screen(void)
{
    int rows, cols;
    struct deck *d;
    struct record *r;
    int i, list_start, list_height;

    getmaxyx(stdscr, rows, cols);
    erase();

    d = &deck[0];

    /* Header: loaded track */

    attron(A_BOLD);
    mvprintw(0, 0, " Loaded:");
    attroff(A_BOLD);

    if (d->record && d->record->artist[0] != '\0') {
        mvprintw(0, 9, " %s - %s", d->record->artist, d->record->title);
    } else {
        attron(A_DIM);
        mvprintw(0, 9, " (no track loaded)");
        attroff(A_DIM);
    }

    mvhline(1, 0, ACS_HLINE, cols);

    /* Track list */

    attron(A_BOLD);
    mvprintw(2, 0, " Library");
    attroff(A_BOLD);

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

        if (entry == listbox_current(&selector.records)) {
            attron(A_REVERSE);
            mvhline(list_start + i, 0, ' ', cols);
            mvprintw(list_start + i, 0, " > %s - %s", r->artist, r->title);
            attroff(A_REVERSE);
        } else {
            mvprintw(list_start + i, 0, "   %s - %s", r->artist, r->title);
        }
    }

    /* Key hints at bottom */

    attron(A_DIM);
    mvhline(rows - 1, 0, ' ', cols);
    mvprintw(rows - 1, 0, " q:quit  Up/Down:navigate  Right/Enter:load track");
    attroff(A_DIM);

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

    selector_init(&selector, lib);

    running = true;

    if (pthread_create(&ph, NULL, launch, NULL)) {
        perror("pthread_create");
        selector_clear(&selector);
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
}
