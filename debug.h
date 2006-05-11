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

#ifndef __YUM_DEBUG_H__
#define __YUM_DEBUG_H__

#include <glib.h>

typedef enum {
    DEBUG_LEVEL_ERROR = -1,
    DEBUG_LEVEL_WARNING = 0,
    DEBUG_LEVEL_INFO = 1,
    DEBUG_LEVEL_DEBUG = 2
} DebugLevel;

typedef void (*DebugFn) (const char *message,
                         DebugLevel level,
                         gpointer user_data);

guint debug_add_handler (DebugFn fn, gpointer user_data);
void  debug_remove_handler (guint id);

void debug (DebugLevel level, const char *format, ...);

#endif /* __YUM_DEBUG_H__ */
