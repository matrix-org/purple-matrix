/**
 * matrix-room.h: Handling of rooms within matrix
 *
 * This module is responsible for tracking the state of a matrix room, and
 * responding to events received from the sync api as well as the purple api.
 * (At some point it will probably make sense to split those concerns: the
 * implementation is already quite large).
 *
 *
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

#ifndef MATRIX_ROOM_H
#define MATRIX_ROOM_H

#include <glib.h>

#include <json-glib/json-glib.h>

#include "libmatrix.h"

struct _PurpleConversation;
struct _PurpleConnection;

/**
 * @param conn   connection data for the account
 * @param conv   conversation info
 */
void matrix_room_handle_initial_state(struct _PurpleConversation *conv);

/**
 * Create a new conversation for the given room
 */
struct _PurpleConversation *matrix_room_create_conversation(
        struct _PurpleConnection *pc, const gchar *room_id);

/**
 * Leave a chat: notify the server that we are leaving, and (ultimately)
 * free the memory structures
 */
void matrix_room_leave_chat(struct _PurpleConversation *conv);

/**
 * handle a single received timeline event for a room (such as a message)
 *
 * @param conv        info on the room
 * @param event_id    id of the event
 * @param json_event_obj  the event object.
 */
void matrix_room_handle_timeline_event(struct _PurpleConversation *conv,
        const gchar *event_id, JsonObject *json_event_obj);

/**
 * Send a message in a room
 */
void matrix_room_send_message(struct _PurpleConversation *conv,
        const gchar *message);

/*************************************************************************
 *
 * Room state handling
 */

/**
 * Update the state table on a room, based on a received state event
 *
 * @param conv        info on the room
 * @param event_id    id of the event
 * @param json_event_obj  the event object.
 */

void matrix_room_handle_state_event(struct _PurpleConversation *conv,
        const gchar *event_id, JsonObject *json_event_obj,
        gboolean suppress_state_update_notifications);


#endif
