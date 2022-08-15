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
#include "roomlist.h"

struct _PurpleConversation;
struct _PurpleConnection;

/**
 * @param conv   conversation info
 */
void matrix_room_complete_state_update(struct _PurpleConversation *conv,
        gboolean announce_arrivals);


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
 * Update the state table on a room, based on a received state event
 *
 * @param conv        info on the room
 * @param json_event_obj  the event object.
 */

void matrix_room_handle_state_event(struct _PurpleConversation *conv,
        JsonObject *json_event_obj);

/**
 * handle a single received timeline event for a room (such as a message)
 *
 * @param conv        info on the room
 * @param json_event_obj  the event object.
 */
void matrix_room_handle_timeline_event(struct _PurpleConversation *conv,
        JsonObject *json_event_obj);

/**
 * Sends a typing notification in a room with a 25s timeout
 */
void matrix_room_send_typing(struct _PurpleConversation *conv, gboolean typing);
        
/**
 * Send a message in a room
 */
void matrix_room_send_message(struct _PurpleConversation *conv,
        const gchar *message);


/**
 * Get the userid of a member of a room, given their displayname
 *
 * @returns a string, which will be freed by the caller
 */
gchar *matrix_room_displayname_to_userid(struct _PurpleConversation *conv,
        const gchar *who);

/**
 * Download the list of publically available rooms
 *
 * @returns a PurpleRoomlist struct
 */
PurpleRoomlist *matrixprpl_roomlist_get_list(PurpleConnection *pc);

void matrixprpl_roomlist_cancel(PurpleRoomlist * list);

#endif
