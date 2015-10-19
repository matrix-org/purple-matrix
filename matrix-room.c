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

/* stdlib */
#include <string.h>

/* libpurple */
#include "connection.h"
#include "debug.h"

#include "libmatrix.h"
#include "matrix-json.h"

typedef struct _MatrixRoomStateEvent {
    JsonObject *content;
} MatrixRoomStateEvent;


typedef struct _RoomEventParserData {
    PurpleConversation *conv;
    const gchar *room_id;
    JsonObject *event_map;
} RoomEventParserData;


/**
 * handle a state event for a room
 */
static void _parse_state_event(JsonArray *state,
        guint state_idx, JsonNode *state_entry, gpointer user_data)
{
    RoomEventParserData *data = user_data;
    PurpleConversation *conv = data->conv;
    JsonObject *event_map = data->event_map;
    MatrixRoomStateEventTable *state_table;
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
        purple_debug_warning("prplmatrix", "unknown event_id %s\n", event_id);
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

    state_table = purple_conversation_get_data(conv, "state");
    state_table_entry = g_hash_table_lookup(state_table, event_type);
    if(state_table_entry == NULL) {
        state_table_entry = g_hash_table_new(g_str_hash, g_str_equal); /* TODO: free */
        g_hash_table_insert(state_table, g_strdup(event_type), state_table_entry); /* TODO: free */
    }

    g_hash_table_insert(state_table_entry, g_strdup(state_key), event); /* TODO: free */
    /* TODO: free old event if it existed */
}

/**
 * Parse a json list of room state into a MatrixRoomStateEventTable
 */
void _parse_state_events(PurpleConversation *conv, const gchar *room_id,
        JsonArray *state_array, JsonObject *event_map)
{
    RoomEventParserData data = {conv, room_id, event_map};
    json_array_foreach_element(state_array, _parse_state_event, &data);
}


static void _parse_timeline_event(JsonArray *timeline,
                guint state_idx, JsonNode *timeline_entry, gpointer user_data)
{
    RoomEventParserData *data = user_data;
    PurpleConversation *conv = data->conv;
    JsonObject *event_map = data->event_map;
    const gchar *room_id = data->room_id;
    const gchar *event_id, *event_type, *msg_body, *sender;
    JsonObject *json_event_obj, *json_content_obj;
    PurpleMessageFlags flags;
    gint64 timestamp;

    event_id = matrix_json_node_get_string(timeline_entry);
    if(event_id == NULL) {
        purple_debug_warning("matrixprpl", "non-string event_id");
        return;
    }

    json_event_obj = matrix_json_object_get_object_member(
                event_map, event_id);
    if(json_event_obj == NULL) {
        purple_debug_warning("matrixprpl", "unknown event_id %s\n", event_id);
                return;
    }

    event_type = matrix_json_object_get_string_member(
                json_event_obj, "type");
    json_content_obj = matrix_json_object_get_object_member(
                json_event_obj, "content");
    if(event_type == NULL || json_content_obj == NULL)
        return;

    if(strcmp(event_type, "m.room.message") != 0) {
        purple_debug_info("matrixprpl", "ignoring unknown room event %s\n",
                        event_type);
        return;
    }

    msg_body = matrix_json_object_get_string_member(json_content_obj, "body");
    if(msg_body == NULL) {
        purple_debug_warning("matrixprpl", "no body in message event %s\n",
                        event_id);
        return;
    }

    sender = matrix_json_object_get_string_member(json_event_obj, "sender");
    if(sender == NULL) {
        sender = "<unknown>";
    }

    timestamp = matrix_json_object_get_int_member(json_event_obj,
                "origin_server_ts");

    flags = PURPLE_MESSAGE_RECV;

    purple_debug_info("matrixprpl", "got message %s in %s\n", msg_body, room_id);
    serv_got_chat_in(conv->account->gc, g_str_hash(room_id), sender, flags,
                msg_body, timestamp / 1000);
}


static void _parse_timeline_events(PurpleConversation *conv,
        const gchar *room_id, JsonArray *events, JsonObject* event_map)
{
    RoomEventParserData data = {conv, room_id, event_map};
    json_array_foreach_element(events, _parse_timeline_event, &data);
}

/**
 * handle a room within the sync response
 */
void matrix_room_handle_sync(const gchar *room_id,
        JsonObject *room_data, MatrixAccount *ma)
{
    JsonObject *state_object, *timeline_object, *event_map;
    JsonArray *state_array, *timeline_array;
    const gchar *room_name;
    PurpleConversation *conv;
    PurpleChat *chat;
    PurpleGroup *group;
    MatrixRoomStateEventTable *state_table;

    event_map = matrix_json_object_get_object_member(room_data, "event_map");

    conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT,
            room_id, ma->pa);

    if(conv == NULL) {
        purple_debug_info("matrixprpl", "New room %s\n", room_id);
        /* tell purple we have joined this chat */
        conv = serv_got_joined_chat(ma->pc, g_str_hash(room_id), room_id);
        purple_conversation_set_data(conv, "room_id", g_strdup(room_id));
            /* TODO: free */
        state_table = g_hash_table_new(g_str_hash, g_str_equal); /* TODO: free */
        purple_conversation_set_data(conv, "state", state_table);
    } else {
        purple_debug_info("matrixprpl", "Updating room %s\n", room_id);
        state_table = purple_conversation_get_data(conv, "state");
    }

    /* add the room to the buddy list */
    chat = purple_blist_find_chat(ma->pa, room_id);
    if (!chat)
    {
        GHashTable *comp;
        group = purple_find_group("Matrix");
        if (!group)
        {
            group = purple_group_new("Matrix");
            purple_blist_add_group(group, NULL);
        }
        comp = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free); /* TODO:
                                                                              * free? */
        g_hash_table_insert(comp, PRPL_CHAT_INFO_ROOM_ID,
                g_strdup(room_id)); /* TODO: free? */

        /* we set the alias to the room id initially, then change it to
         * something more user-friendly below.
         */
        chat = purple_chat_new(ma->pa, room_id, comp);
        purple_blist_add_chat(chat, group, NULL);
    }

    /* parse the room state */
    state_object = matrix_json_object_get_object_member(room_data, "state");
    state_array = matrix_json_object_get_array_member(state_object, "events");
    if(state_array != NULL)
        _parse_state_events(conv, room_id, state_array, event_map);

    /* ensure the alias in the buddy list is up-to-date */
    room_name = matrix_room_get_name(state_table);
    purple_blist_alias_chat(chat, room_name);

    /* parse the timeline events */
    timeline_object = matrix_json_object_get_object_member(
                room_data, "timeline");
    timeline_array = matrix_json_object_get_array_member(
                timeline_object, "events");
    if(timeline_array != NULL)
        _parse_timeline_events(conv, room_id, timeline_array, event_map);
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

