/**
 * matrix-sync.c: Handle the 'sync' loop
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

#include "matrix-sync.h"

/* json-glib */
#include <json-glib/json-glib.h>

/* libpurple */
#include "connection.h"
#include "conversation.h"
#include "debug.h"

/* libmatrix */
#include "matrix-json.h"
#include "matrix-room.h"


typedef struct _RoomEventParserData {
    PurpleConversation *conv;
    JsonObject *event_map;
    gboolean state_events;
} RoomEventParserData;


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
        matrix_room_handle_state_event(conv, event_id, json_event_obj);
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


static PurpleChat *_ensure_blist_entry(PurpleAccount *acct,
        const gchar *room_id)
{
    GHashTable *comp;
    PurpleGroup *group;
    PurpleChat *chat = purple_blist_find_chat(acct, room_id);

    if (chat)
        return chat;

    group = purple_find_group("Matrix");
    if (!group) {
        group = purple_group_new("Matrix");
        purple_blist_add_group(group, NULL);
    }

    comp = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
    g_hash_table_insert(comp, PRPL_CHAT_INFO_ROOM_ID, g_strdup(room_id));

    /* we set the alias to the room id initially, then change it to
     * something more user-friendly later.
     */
    chat = purple_chat_new(acct, room_id, comp);

    /* encourage matrix chats to be persistent by default. This is clearly a
     * hack :/ */
    purple_blist_node_set_bool(&chat->node, "gtk-persistent", TRUE);

    purple_blist_add_chat(chat, group, NULL);

    return chat;
}


/**
 * handle a room within the sync response
 */
static void matrix_sync_room(const gchar *room_id,
        JsonObject *room_data, PurpleConnection *pc)
{
    JsonObject *state_object, *timeline_object, *event_map;
    JsonArray *state_array, *timeline_array;
    PurpleConversation *conv;
    gboolean initial_sync = FALSE;

    /* ensure we have an entry in the buddy list for this room.
     * TODO: We should only do this if the user is actually *in* the room. */
    _ensure_blist_entry(pc->account, room_id);

    conv = purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT, room_id, pc->account);

    if(conv == NULL) {
        conv = matrix_room_create_conversation(pc, room_id);
        initial_sync = TRUE;
    }

    event_map = matrix_json_object_get_object_member(room_data, "event_map");

    /* parse the room state */
    state_object = matrix_json_object_get_object_member(room_data, "state");
    state_array = matrix_json_object_get_array_member(state_object, "events");
    if(state_array != NULL)
        _parse_room_event_array(conv, state_array, event_map, TRUE);

    matrix_room_complete_state_update(conv, !initial_sync);

    /* parse the timeline events */
    timeline_object = matrix_json_object_get_object_member(
                room_data, "timeline");
    timeline_array = matrix_json_object_get_array_member(
                timeline_object, "events");
    if(timeline_array != NULL)
        _parse_room_event_array(conv, timeline_array, event_map, FALSE);
}


/**
 * handle the results of the sync request
 */
void matrix_sync_parse(PurpleConnection *pc, JsonNode *body,
        const gchar **next_batch)
{
    JsonObject *rootObj;
    JsonObject *rooms;
    JsonObject *joined_rooms;
    GList *room_ids, *elem;

    rootObj = matrix_json_node_get_object(body);
    rooms = matrix_json_object_get_object_member(rootObj, "rooms");
    joined_rooms = matrix_json_object_get_object_member(rooms, "joined");

    *next_batch = matrix_json_object_get_string_member(rootObj, "next_batch");

    if(joined_rooms == NULL) {
        purple_debug_warning("matrixprpl", "didn't find joined rooms list\n");
        return;
    }

    room_ids = json_object_get_members(joined_rooms);
    for(elem = room_ids; elem; elem = elem->next) {
        const gchar *room_id = elem->data;
        JsonObject *room_data = matrix_json_object_get_object_member(
                joined_rooms, room_id);
        purple_debug_info("matrixprpl", "Syncing room %s", room_id);
        matrix_sync_room(room_id, room_data, pc);
    }
    g_list_free(room_ids);
}

