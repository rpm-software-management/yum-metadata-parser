/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/* This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License,
 * version 2, as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#include "debug.h"

typedef struct {
    DebugFn fn;
    gpointer user_data;
    guint id;
} DebugHandler;

static GSList *handlers = NULL;

guint
debug_add_handler (DebugFn fn, gpointer user_data)
{
    DebugHandler *handler;

    g_return_val_if_fail (fn != NULL, 0);

    handler = g_new0 (DebugHandler, 1);
    handler->fn = fn;
    handler->user_data = user_data;

    if (handlers)
        handler->id = ((DebugHandler *) handlers->data)->id + 1;
    else
        handler->id = 1;

    handlers = g_slist_prepend (handlers, handler);

    return handler->id;
}

void
debug_remove_handler (guint id)
{
    GSList *iter;

    for (iter = handlers; iter; iter = iter->next) {
        DebugHandler *handler = (DebugHandler *) iter->data;

        if (handler->id == id) {
            handlers = g_slist_remove_link (handlers, iter);
            g_free (handler);
            return;
        }
    }

    debug (DEBUG_LEVEL_WARNING, "Could not remove debug handler %d", id);
}

void
debug (DebugLevel level, const char *format, ...)
{
    va_list args;
    GSList *iter;
    char *str;

    va_start (args, format);
    str = g_strdup_vprintf (format, args);
    va_end (args);

    for (iter = handlers; iter; iter = iter->next) {
        DebugHandler *handler = (DebugHandler *) iter->data;

        handler->fn (str, level, handler->user_data);
    }

    g_free (str);
}
