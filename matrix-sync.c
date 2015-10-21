/**
 * Handle the 'sync' loop
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

#include "matrix-sync.h"

/* json-glib */
#include <json-glib/json-glib.h>

/* libpurple */
#include "connection.h"
#include "conversation.h"
#include "debug.h"

/* libmatrix */
#include "matrix-api.h"
#include "matrix-json.h"
#include "matrix-room.h"


typedef struct _RoomEventParserData {
    PurpleConversation *conv;
    JsonObject *event_map;
    gboolean state_events;
} RoomEventParserData;


static void matrix_sync_complete(MatrixAccount *ma, gpointer user_data,
        JsonNode *body);

/**
 * handle an event for a room
 *
 * @param state        the complete state array (unused)
 * @param state_id     position within the array (unused)
 * @param state_entry  the event id to be handled
 * @param user_data    a RoomEventParserData
 */
static void _parse_room_event(JsonArray *event_array, guint event_idx,
        JsonNode *event, gpointer user_data)
{
    RoomEventParserData *data = user_data;
    PurpleConversation *conv = data->conv;
    JsonObject *event_map = data->event_map;
    JsonObject *json_event_obj;
    const gchar *event_id;

    event_id = matrix_json_node_get_string(event);
    if(event_id == NULL) {
        purple_debug_warning("prplmatrix", "non-string event_id");
        return;
    }

    json_event_obj = matrix_json_object_get_object_member(
            event_map, event_id);
    if(json_event_obj == NULL) {
        purple_debug_warning("prplmatrix", "unknown event_id %s\n", event_id);
        return;
    }

    if(data->state_events)
        matrix_room_update_state_table(conv, event_id, json_event_obj);
    else
        matrix_room_handle_timeline_event(conv, event_id, json_event_obj);


}

/**
 * parse the list of events in a sync response
 */
static void _parse_room_event_array(PurpleConversation *conv, JsonArray *events,
        JsonObject* event_map, gboolean state_events)
{
    RoomEventParserData data = {conv, event_map, state_events};
    json_array_foreach_element(events, _parse_room_event, &data);
}


/**
 * handle a room within the sync response
 */
static void matrix_sync_room(const gchar *room_id,
        JsonObject *room_data, MatrixAccount *ma)
{
    JsonObject *state_object, *timeline_object, *event_map;
    JsonArray *state_array, *timeline_array;
    PurpleConversation *conv;

    event_map = matrix_json_object_get_object_member(room_data, "event_map");
    conv = matrix_room_get_or_create_conversation(ma, room_id);

    /* parse the room state */
    state_object = matrix_json_object_get_object_member(room_data, "state");
    state_array = matrix_json_object_get_array_member(state_object, "events");
    if(state_array != NULL)
        _parse_room_event_array(conv, state_array, event_map, TRUE);

    /* parse the timeline events */
    timeline_object = matrix_json_object_get_object_member(
                room_data, "timeline");
    timeline_array = matrix_json_object_get_array_member(
                timeline_object, "events");
    if(timeline_array != NULL)
        _parse_room_event_array(conv, timeline_array, event_map, FALSE);

    /* ensure the buddy list is up to date*/
    matrix_room_update_buddy_list(conv);
}


/**
 * handle the results of the sync request
 */
static void matrix_handle_sync(MatrixAccount *ma, JsonNode *body)
{
    JsonObject *rootObj;
    JsonObject *rooms;
    JsonObject *joined_rooms;
    GList *room_ids, *elem;
    const gchar *next_batch;

    rootObj = matrix_json_node_get_object(body);
    rooms = matrix_json_object_get_object_member(rootObj, "rooms");
    joined_rooms = matrix_json_object_get_object_member(rooms, "joined");

    if(joined_rooms == NULL) {
        purple_debug_warning("matrixprpl", "didn't find joined rooms list\n");
        return;
    }

    room_ids = json_object_get_members(joined_rooms);
    for(elem = room_ids; elem; elem = elem->next) {
        const gchar *room_id = elem->data;
        JsonObject *room_data = matrix_json_object_get_object_member(
                joined_rooms, room_id);
        matrix_sync_room(room_id, room_data, ma);
    }
    g_list_free(room_ids);

    /* Start the next sync */
    next_batch = matrix_json_object_get_string_member(rootObj, "next_batch");
    if(next_batch == NULL) {
        purple_connection_error_reason(ma->pc,
            PURPLE_CONNECTION_ERROR_OTHER_ERROR, "No next_batch field");
        return;
    }
    matrix_api_sync(ma, next_batch, matrix_sync_complete, NULL);
}

/* callback which is called when a /sync request completes */
static void matrix_sync_complete(MatrixAccount *ma, gpointer user_data,
    JsonNode *body)
{
    purple_connection_update_progress(ma->pc, _("Connected"), 2, 3);
    purple_connection_set_state(ma->pa->gc, PURPLE_CONNECTED);

    matrix_handle_sync(ma, body);
}


void matrix_sync_start_loop(MatrixAccount *ma)
{
    purple_connection_update_progress(ma->pc, _("Initial Sync"), 1, 3);
    matrix_api_sync(ma, NULL, matrix_sync_complete, NULL);
}
