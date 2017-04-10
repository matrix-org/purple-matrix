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

#include <string.h>
#include "matrix-sync.h"

/* json-glib */
#include <json-glib/json-glib.h>

/* libpurple */
#include "connection.h"
#include "conversation.h"
#include "debug.h"

/* libmatrix */
#include "matrix-connection.h"
#include "matrix-e2e.h"
#include "matrix-event.h"
#include "matrix-json.h"
#include "matrix-room.h"
#include "matrix-statetable.h"


typedef struct _RoomEventParserData {
    PurpleConversation *conv;
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
    JsonObject *json_event_obj;

    json_event_obj = matrix_json_node_get_object(event);
    if(json_event_obj == NULL) {
        purple_debug_warning("prplmatrix", "non-object event\n");
        return;
    }

    if(data->state_events) {
        matrix_room_handle_state_event(conv, json_event_obj);
    } else {
        if(json_object_has_member(json_event_obj, "state_key")) {
            matrix_room_handle_state_event(conv, json_event_obj);
            matrix_room_complete_state_update(conv, TRUE);
        } else {
            matrix_room_handle_timeline_event(conv, json_event_obj);
        }
    }
}

/**
 * parse the list of events in a sync response
 */
static void _parse_room_event_array(PurpleConversation *conv, JsonArray *events,
        gboolean state_events)
{
    RoomEventParserData data = {conv, state_events};
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
    purple_debug_info("matrixprpl", "added buddy list entry for room %s\n",
            room_id);

    return chat;
}


/**
 * handle a joined room within the sync response
 */
static void matrix_sync_room(const gchar *room_id,
        JsonObject *room_data, PurpleConnection *pc,
        gboolean handle_timeline)
{
    JsonObject *state_object, *timeline_object, *ephemeral_object;
    JsonArray *state_array, *timeline_array, *ephemeral_array;
    PurpleConversation *conv;
    gboolean initial_sync = FALSE;

    /* ensure we have an entry in the buddy list for this room. */
    _ensure_blist_entry(pc->account, room_id);

    conv = purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT, room_id, pc->account);

    if(conv == NULL) {
        conv = matrix_room_create_conversation(pc, room_id);
        initial_sync = TRUE;
    }

    /* parse the room state */
    state_object = matrix_json_object_get_object_member(room_data, "state");
    state_array = matrix_json_object_get_array_member(state_object, "events");
    if(state_array != NULL)
        _parse_room_event_array(conv, state_array, TRUE);

    matrix_room_complete_state_update(conv, !initial_sync);

    /* parse the ephemeral events */
    /* (uses the state table to track the state of who is typing and who isn't) */
    ephemeral_object = matrix_json_object_get_object_member(room_data, "ephemeral");
    ephemeral_array = matrix_json_object_get_array_member(ephemeral_object, "events");
    if(ephemeral_array != NULL)
        _parse_room_event_array(conv, ephemeral_array, TRUE);

    if (handle_timeline) {
        /* parse the timeline events */
        timeline_object = matrix_json_object_get_object_member(
                    room_data, "timeline");
        timeline_array = matrix_json_object_get_array_member(
                    timeline_object, "events");
        if(timeline_array != NULL)
            _parse_room_event_array(conv, timeline_array, FALSE);
    }
}


static void _parse_invite_state_event(JsonArray *event_array, guint event_idx,
        JsonNode *event, gpointer user_data)
{
    MatrixRoomStateEventTable *state_table = user_data;
    JsonObject *event_obj;

    event_obj = matrix_json_node_get_object(event);
    if(event_obj == NULL) {
        purple_debug_warning("prplmatrix", "non-object event");
        return;
    }

    matrix_statetable_update(state_table, event_obj, NULL, NULL);
}


/**
 * tell purple about our incoming invitation
 */
static void _raise_invite_request(PurpleConnection *pc,
        const gchar *room_id, const gchar *sender, const gchar *room_name)
{
    GHashTable *components;

    /* we could share this code with _ensure_blist_entry
     *
     * libpurple destroys the hashtable when the invite is dealt with.
     */
    components = g_hash_table_new_full(g_str_hash, g_str_equal,
            NULL, g_free);
    g_hash_table_insert(components, PRPL_CHAT_INFO_ROOM_ID, g_strdup(room_id));

    serv_got_chat_invite(pc, room_name, sender, NULL, components);
}


/**
 * handle an invitation within the sync response
 */
static void _handle_invite(const gchar *room_id,
        JsonObject *invite_data, PurpleConnection *pc)
{
    JsonObject *invite_state_object;
    JsonArray *events;
    MatrixRoomStateEventTable *state_table;
    MatrixConnectionData *conn;
    MatrixRoomEvent *event;
    const gchar *sender;
    gchar *room_name = NULL;

    conn = purple_connection_get_protocol_data(pc);

    invite_state_object = matrix_json_object_get_object_member(invite_data,
            "invite_state");
    events = matrix_json_object_get_array_member(invite_state_object,
            "events");

    if(events == NULL) {
        purple_debug_warning("prplmatrix", "no events array in invite event\n");
        return;
    }

    state_table = matrix_statetable_new();
    json_array_foreach_element(events, _parse_invite_state_event,
                state_table);

    /* look for our own m.room.member event, so we can see who invited us */
    event = matrix_statetable_get_event(state_table, "m.room.member",
            conn -> user_id);
    if(event != NULL)
        sender = event->sender;
    else
        sender = "?";


    /* try and figure out the room name */
    room_name = matrix_statetable_get_room_alias(state_table);
    if(room_name == NULL) {
        /* just name the room after the sender of the invite, for now */
        room_name = g_strdup(sender);
    }

    _raise_invite_request(pc, room_id, sender, room_name);

    matrix_statetable_destroy(state_table);
    g_free(room_name);
}


/**
 * handle the results of the sync request
 */
void matrix_sync_parse(PurpleConnection *pc, JsonNode *body,
        const gchar **next_batch)
{
    JsonObject *rootObj;
    JsonObject *rooms;
    JsonObject *joined_rooms, *invited_rooms;
    GList *room_ids, *elem;

    rootObj = matrix_json_node_get_object(body);
    *next_batch = matrix_json_object_get_string_member(rootObj, "next_batch");
    rooms = matrix_json_object_get_object_member(rootObj, "rooms");

    joined_rooms = matrix_json_object_get_object_member(rooms, "join");
    if(joined_rooms != NULL) {
        room_ids = json_object_get_members(joined_rooms);
        for(elem = room_ids; elem; elem = elem->next) {
            const gchar *room_id = elem->data;
            JsonObject *room_data = matrix_json_object_get_object_member(
                    joined_rooms, room_id);
            purple_debug_info("matrixprpl", "Syncing room (1)%s\n", room_id);
            matrix_sync_room(room_id, room_data, pc, FALSE);
        }
        g_list_free(room_ids);
    }


    invited_rooms = matrix_json_object_get_object_member(rooms, "invite");
    if(invited_rooms != NULL) {
        room_ids = json_object_get_members(invited_rooms);
        for(elem = room_ids; elem; elem = elem->next) {
            const gchar *room_id = elem->data;
            JsonObject *room_data = matrix_json_object_get_object_member(
                    invited_rooms, room_id);
            purple_debug_info("matrixprpl", "Invite to room %s\n", room_id);
            _handle_invite(room_id, room_data, pc);
        }
        g_list_free(room_ids);
    }

    /* Handle d2d messages so we can create any e2e sessions needed
     * We need to do this after we created rooms/conversations, but before
     * we handle timeline events that we might need to decrypt.
     */
    JsonObject *to_device = matrix_json_object_get_object_member(rootObj,
                               "to_device");
    if (to_device) {
        JsonArray *events = matrix_json_object_get_array_member(to_device,
                                                               "events");
        guint i = 0;
        JsonNode *device_event;
        while (device_event = matrix_json_array_get_element(events, i++),
               device_event) {
            JsonObject *event_obj = matrix_json_node_get_object(device_event);
            const gchar *event_type;
            event_type = matrix_json_object_get_string_member(event_obj,
                                                               "type");
            purple_debug_info("matrixprpl",  "to_device: Got %s from %s\n",
                    event_type,
                    matrix_json_object_get_string_member(event_obj, "sender"));
            if (!strcmp(event_type, "m.room.encrypted")) {
                matrix_e2e_decrypt_d2d(pc, event_obj);
            } else {
            }

        }
    }

    JsonObject *dev_key_counts = matrix_json_object_get_object_member(rootObj,
                                    "device_one_time_keys_count");
    if (dev_key_counts) {
	      matrix_e2e_handle_sync_key_counts(pc, dev_key_counts, FALSE);
    }

    /* Now go round the rooms again getting the timeline events */
    if (joined_rooms != NULL) {
        room_ids = json_object_get_members(joined_rooms);
        for(elem = room_ids; elem; elem = elem->next) {
            const gchar *room_id = elem->data;
            JsonObject *room_data = matrix_json_object_get_object_member(
                    joined_rooms, room_id);
            purple_debug_info("matrixprpl", "Syncing room (2) %s\n", room_id);
            matrix_sync_room(room_id, room_data, pc, TRUE);
        }
        g_list_free(room_ids);
    }
}

