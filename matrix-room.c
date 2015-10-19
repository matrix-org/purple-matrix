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

/* identifiers for purple_conversation_get/set_data */
#define PURPLE_CONV_DATA_STATE "state"
#define PURPLE_CONV_DATA_ID "room_id"

/******************************************************************************
 *
 * room state handling
 */

/* The state event table is a hashtable which maps from event type to
 * another hashtable, which maps from state key to content, which is itself a
 * MatrixRoomStateEvent
 */
typedef GHashTable MatrixRoomStateEventTable;

typedef struct _MatrixRoomStateEvent {
    JsonObject *content;
} MatrixRoomStateEvent;


/**
 * Get the state table for a room
 */
MatrixRoomStateEventTable *matrix_room_get_state_table(PurpleConversation *conv)
{
    return purple_conversation_get_data(conv, PURPLE_CONV_DATA_STATE);
}


/**
 * Update the state table on a room
 */
void matrix_room_update_state_table(PurpleConversation *conv,
        const gchar *event_type, const gchar *state_key,
        JsonObject *json_content_obj)
{
    MatrixRoomStateEvent *event;
    MatrixRoomStateEventTable *state_table;
    GHashTable *state_table_entry;

    event = g_new0(MatrixRoomStateEvent, 1); /* TODO: free */
    event->content = json_content_obj;
    json_object_ref(event->content); /* TODO: free */

    state_table = matrix_room_get_state_table(conv);
    state_table_entry = g_hash_table_lookup(state_table, event_type);
    if(state_table_entry == NULL) {
        state_table_entry = g_hash_table_new(g_str_hash, g_str_equal); /* TODO: free */
        g_hash_table_insert(state_table, g_strdup(event_type), state_table_entry); /* TODO: free */
    }

    g_hash_table_insert(state_table_entry, g_strdup(state_key), event); /* TODO: free */
    /* TODO: free old event if it existed */
}

/**
 * look up a particular bit of state
 *
 * @returns null if this key ies not known
 */
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

/**
 * figure out the best name for a room
 */
static const char *matrix_room_get_name(MatrixRoomStateEventTable *state_table)
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



/*****************************************************************************/



void matrix_room_handle_timeline_event(PurpleConversation *conv,
        const gchar *event_id, const gchar *event_type,
        const gchar *sender, gint64 timestamp, JsonObject *json_content_obj)
{
    const gchar *room_id, *msg_body;
    PurpleMessageFlags flags;

    room_id = purple_conversation_get_data(conv, PURPLE_CONV_DATA_ID);

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

    if(sender == NULL) {
        sender = "<unknown>";
    }

    flags = PURPLE_MESSAGE_RECV;

    purple_debug_info("matrixprpl", "got message %s in %s\n", msg_body, room_id);
    serv_got_chat_in(conv->account->gc, g_str_hash(room_id), sender, flags,
                msg_body, timestamp / 1000);
}


PurpleConversation *matrix_room_get_or_create_conversation(
        MatrixAccount *ma, const gchar *room_id)
{
    PurpleConversation *conv = purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT, room_id, ma->pa);
    MatrixRoomStateEventTable *state_table;

    if(conv != NULL) {
        return conv;
    }

    purple_debug_info("matrixprpl", "New room %s\n", room_id);

    /* tell purple we have joined this chat */
    conv = serv_got_joined_chat(ma->pc, g_str_hash(room_id), room_id);

    /* set our data on it */
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_ID, g_strdup(room_id));
        /* TODO: free */

    state_table = g_hash_table_new(g_str_hash, g_str_equal); /* TODO: free */
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_STATE, state_table);
    return conv;
}

/**
 * Ensure the room is up to date in the buddy list (ie, it is present,
 * and the alias is correct)
 *
 * @param conv: conversation info
 */
void matrix_room_update_buddy_list(PurpleConversation *conv)
{
    const gchar *room_id, *room_name;
    PurpleChat *chat;

    room_id = purple_conversation_get_data(conv, PURPLE_CONV_DATA_ID);

    chat = purple_blist_find_chat(conv->account, room_id);
    if (!chat)
    {
        GHashTable *comp;
        PurpleGroup *group;

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
        chat = purple_chat_new(conv-> account, room_id, comp);
        purple_blist_add_chat(chat, group, NULL);
    }

    room_name = matrix_room_get_name(matrix_room_get_state_table(conv));
    purple_blist_alias_chat(chat, room_name);
}



