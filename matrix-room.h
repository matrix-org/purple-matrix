/**
 * Handling of rooms within matrix
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 */

#ifndef MATRIX_ROOM_H
#define MATRIX_ROOM_H

#include <glib.h>

#include <json-glib/json-glib.h>

#include "libmatrix.h"

struct _PurpleConversation;

/**
 * Ensure the room is up to date in the buddy list (ie, it is present,
 * and the alias is correct)
 *
 * @param conv   conversation info
 */
void matrix_room_update_buddy_list(struct _PurpleConversation *conv);

/**
 * If this is an active conversation, return it; otherwise, create it anew.
 *
 * @param ma       account associated with the chat
 */
struct _PurpleConversation *matrix_room_get_or_create_conversation(
        MatrixAccount *ma, const gchar *room_id);

/**
 * Leave a chat: notify the server that we are leaving, and (ultimately)
 * free the memory structures
 */
void matrix_room_leave_chat(struct _PurpleConversation *conv);

/**
 * handle a single timeline event for a room (such as a message)
 *
 * @param conv        info on the room
 * @param event_id    id of the event
 * @param event_type  type of the event (eg m.room.message)
 * @param sender      sender of the event
 * @param timestamp   timestamp at the origin server
 * @param json_content_obj  the 'content' of the event.
 */
void matrix_room_handle_timeline_event(struct _PurpleConversation *conv,
        const gchar *event_id, const gchar *event_type,
        const gchar *sender, gint64 timestamp, JsonObject *json_content_obj);



/*************************************************************************
 *
 * Room state handling
 */

/**
 * Update the state table on a room
 *
 * @param conv        info on the room
 * @param event_type  type of the event (eg m.room.name)
 * @param state_key
 */
void matrix_room_update_state_table(struct _PurpleConversation *conv,
        const gchar *event_type, const gchar *state_key,
        JsonObject *json_content_obj);


#endif
