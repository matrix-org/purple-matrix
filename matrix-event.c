/*
 * matrix-event.c
 *
 *
 * Copyright (c) Openmarket UK Ltd 2015
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA
 */


#include "matrix-event.h"

#include <glib.h>

#include <json-glib/json-glib.h>

/**
 * Allocate a new MatrixRoomEvent.
 *
 * @param event_type   the type of the event. this is copied into the event
 * @param content      the content of the event. This is used direct, but the
 *                     reference count is incremented.
 */
MatrixRoomEvent *matrix_event_new(const gchar *event_type, JsonObject *content)
{
    MatrixRoomEvent *event;
    event = g_new0(MatrixRoomEvent, 1);
    event->content = json_object_ref(content);
    event->event_type = g_strdup(event_type);
    return event;
}


void matrix_event_free(MatrixRoomEvent *event)
{
    if(event->content)
        json_object_unref(event->content);
    g_free(event->txn_id);
    g_free(event->sender);
    g_free(event->event_type);
    g_free(event);
}
