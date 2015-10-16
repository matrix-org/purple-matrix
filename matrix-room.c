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

#include "matrix-room.h"

/* libpurple */
#include "connection.h"
#include "debug.h"

#include "libmatrix.h"
#include "matrix-json.h"

typedef struct _MatrixRoomStateEvent {
    JsonObject *content;
} MatrixRoomStateEvent;

typedef struct _RoomStateParserData {
	JsonObject *event_map;
	MatrixRoomStateEventTable *state_table;
} RoomStateParserData;


/**
 * handle a state event for a room
 */
static void _matrix_room_handle_roomstate(JsonArray *state,
		guint state_idx, JsonNode *state_entry, gpointer user_data)
{
	RoomStateParserData *data = user_data;
    MatrixRoomStateEventTable *state_table = data->state_table;
    JsonObject *event_map = data->event_map;
    GHashTable *state_table_entry;
    JsonObject *json_event_obj, *json_content_obj;
    MatrixRoomStateEvent *event;
    const gchar *event_id, *event_type, *state_key;

    event_id = matrix_json_node_get_string(state_entry);
    if(event_id == NULL) {
    	purple_debug_warning("prplmatrix", "non-string event_id");
    	return;
    }

    json_event_obj = matrix_json_object_get_object_member(
    		event_map, event_id);
    if(json_event_obj == NULL) {
    	purple_debug_warning("prplmatrix", "unknown event_id %s", event_id);
		return;
    }

    event_type = matrix_json_object_get_string_member(
    		json_event_obj, "type");
    state_key = matrix_json_object_get_string_member(
    		json_event_obj, "state_key");
    json_content_obj = matrix_json_object_get_object_member(
    		json_event_obj, "content");
    if(event_type == NULL || state_key == NULL || json_content_obj == NULL)
    	return;

    event = g_new0(MatrixRoomStateEvent, 1); /* TODO: free */
    event->content = json_content_obj;
    json_object_ref(event->content); /* TODO: free */

    state_table_entry = g_hash_table_lookup(state_table, event_type);
    if(state_table_entry == NULL) {
        state_table_entry = g_hash_table_new(g_str_hash, g_str_equal); /* TODO: free */
        g_hash_table_insert(state_table, g_strdup(event_type), state_table_entry); /* TODO: free */
    }

    g_hash_table_insert(state_table_entry, g_strdup(state_key), event); /* TODO: free */
    /* TODO: free old event if it existed */
}


void matrix_room_parse_state_events(MatrixRoomStateEventTable *state_table,
		JsonArray *state_array, JsonObject *event_map)
{
	RoomStateParserData data = {event_map, state_table};
    json_array_foreach_element(state_array, _matrix_room_handle_roomstate,
    		&data);
}

static MatrixRoomStateEvent *matrix_room_get_state_event(
		MatrixRoomStateEventTable *state_table, const gchar *event_type,
		const gchar *state_key)
{
	GHashTable *tmp;

	tmp = (GHashTable *) g_hash_table_lookup(state_table, event_type);
	if(tmp == NULL)
		return NULL;

	return (MatrixRoomStateEvent *)g_hash_table_lookup(tmp, state_key);
}


const char *matrix_room_get_name(MatrixRoomStateEventTable *state_table)
{
	GHashTable *tmp;
	MatrixRoomStateEvent *event;

	/* start by looking for the official room name */
	event = matrix_room_get_state_event(state_table, "m.room.name", "");
	if(event != NULL) {
		const gchar *tmpname = matrix_json_object_get_string_member(
				event->content, "name");
		if(tmpname != NULL) {
		    purple_debug_info("matrixprpl", "got room name %s\n", tmpname);
			return tmpname;
		}
    }

	/* look for an alias */
	tmp = (GHashTable *) g_hash_table_lookup(state_table, "m.room.aliases");
	if(tmp != NULL) {
		GHashTableIter iter;
		g_hash_table_iter_init(&iter, tmp);
		gpointer key, value;
		while(g_hash_table_iter_next(&iter, &key, &value)) {
			MatrixRoomStateEvent *event = value;
			JsonArray *array = matrix_json_object_get_array_member(
					event->content, "aliases");
			if(array != NULL && json_array_get_length(array) > 0) {
				const gchar *tmpname = matrix_json_array_get_string_element(array, 0);
				if(tmpname != NULL) {
				    purple_debug_info("matrixprpl", "got room alias %s\n",
				    		tmpname);
					return tmpname;
				}
			}
		}
	}

	/* TODO: look for room members, and pick a name based on that */

	return "unknown";
}


GList *matrixprpl_chat_info(PurpleConnection *gc)
{
    struct proto_chat_entry *pce; /* defined in prpl.h */

    purple_debug_info("matrixprpl", "returning chat setting 'room_id'\n");

    pce = g_new0(struct proto_chat_entry, 1);
    pce->label = _("Chat _room");
    pce->identifier = "room_id";
    pce->required = TRUE;

    return g_list_append(NULL, pce);
}

GHashTable *matrixprpl_chat_info_defaults(PurpleConnection *gc,
                                          const char *room)
{
    GHashTable *defaults;

    purple_debug_info("matrixprpl", "returning chat default setting "
                      "'room_id' = 'default'\n");

    defaults = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
    g_hash_table_insert(defaults, "room_id", g_strdup("default"));
    return defaults;
}

char *matrixprpl_get_chat_name(GHashTable *components)
{
    const char *room = "room_name"; /* TODO fix */
    purple_debug_info("matrixprpl", "reporting chat room name '%s'\n", room);
    return g_strdup(room);
}

