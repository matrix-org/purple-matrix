/*
 * matrix-event.h
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

#ifndef MATRIX_EVENT_H_
#define MATRIX_EVENT_H_

#include <glib.h>

struct _JsonObject;

typedef struct _MatrixRoomEvent {
    /* for outgoing events, our made-up transaction id. NULL for incoming
     * events.
     */
    gchar *txn_id;
    gchar *event_type;
    struct _JsonObject *content;
} MatrixRoomEvent;


/**
 * Allocate a new MatrixRoomEvent.
 *
 * @param event_type   the type of the event. this is copied into the event
 * @param content      the content of the event. This is used direct, but the
 *                     reference count is incremented.
 */
MatrixRoomEvent *matrix_event_new(const gchar *event_type,
        struct _JsonObject *content);

void matrix_event_free(MatrixRoomEvent *event);


#endif /* MATRIX_EVENT_H_ */
