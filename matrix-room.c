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
#include "matrix-api.h"
#include "matrix-json.h"

/*
 * identifiers for purple_conversation_get/set_data
 */

/* a MatrixRoomStateEventTable * - see below */
#define PURPLE_CONV_DATA_STATE "state"

/* a GList of MatrixRoomEvents */
#define PURPLE_CONV_DATA_EVENT_QUEUE "queue"

/* PurpleUtilFetchUrlData * */
#define PURPLE_CONV_DATA_ACTIVE_SEND "active_send"

/******************************************************************************
 *
 * Events
 */

typedef struct _MatrixRoomEvent {
    /* for outgoing events, our made-up transaction id. NULL for incoming
     * events.
     */
    gchar *txn_id;
    gchar *event_type;
    JsonObject *content;
} MatrixRoomEvent;

/**
 * Allocate a new MatrixRoomEvent.
 *
 * @param event_type   the type of the event. this is copied into the event
 * @param content      the content of the event. This is used direct, but the
 *                     reference count is incremented.
 */
static MatrixRoomEvent *_alloc_room_event(const gchar *event_type,
        JsonObject *content)
{
    MatrixRoomEvent *event;
    event = g_new0(MatrixRoomEvent, 1);
    event->content = json_object_ref(content);
    event->event_type = g_strdup(event_type);
    return event;
}

static void _free_room_event(MatrixRoomEvent *event)
{
    if(event->content)
        json_object_unref(event->content);
    g_free(event->txn_id);
    g_free(event->event_type);
    g_free(event);
}

/******************************************************************************
 *
 * room state handling
 */

/* The state event table is a hashtable which maps from event type to
 * another hashtable, which maps from state key to content, which is itself a
 * MatrixRoomEvent.
 *
 */
typedef GHashTable MatrixRoomStateEventTable;

/**
 * create a new, empty, state table
 */
static MatrixRoomStateEventTable *_create_state_table()
{
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
            (GDestroyNotify) g_hash_table_destroy);
}


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
    MatrixRoomEvent *event;
    MatrixRoomStateEventTable *state_table;
    GHashTable *state_table_entry;

    event = _alloc_room_event(event_type, json_content_obj);

    state_table = matrix_room_get_state_table(conv);
    state_table_entry = g_hash_table_lookup(state_table, event_type);
    if(state_table_entry == NULL) {
        state_table_entry = g_hash_table_new_full(g_str_hash, g_str_equal,
                g_free, (GDestroyNotify)_free_room_event);
        g_hash_table_insert(state_table, g_strdup(event_type),
                state_table_entry);
    }

    g_hash_table_insert(state_table_entry, g_strdup(state_key), event);
}

/**
 * look up a particular bit of state
 *
 * @returns null if this key ies not known
 */
static MatrixRoomEvent *matrix_room_get_state_event(
        MatrixRoomStateEventTable *state_table, const gchar *event_type,
        const gchar *state_key)
{
    GHashTable *tmp;

    tmp = (GHashTable *) g_hash_table_lookup(state_table, event_type);
    if(tmp == NULL)
        return NULL;

    return (MatrixRoomEvent *)g_hash_table_lookup(tmp, state_key);
}

/**
 * figure out the best name for a room
 */
static const char *matrix_room_get_name(MatrixRoomStateEventTable *state_table)
{
    GHashTable *tmp;
    MatrixRoomEvent *event;

    /* start by looking for the official room name */
    event = matrix_room_get_state_event(state_table, "m.room.name", "");
    if(event != NULL) {
        const gchar *tmpname = matrix_json_object_get_string_member(
                event->content, "name");
        if(tmpname != NULL) {
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
            MatrixRoomEvent *event = value;
            JsonArray *array = matrix_json_object_get_array_member(
                    event->content, "aliases");
            if(array != NULL && json_array_get_length(array) > 0) {
                const gchar *tmpname = matrix_json_array_get_string_element(array, 0);
                if(tmpname != NULL) {
                    return tmpname;
                }
            }
        }
    }

    /* TODO: look for room members, and pick a name based on that */

    return "unknown";
}


/******************************************************************************
 *
 * event queue handling
 */
static void _send_queued_event(PurpleConversation *conv);

/**
 * Get the state table for a room
 */
static GList *_get_event_queue(PurpleConversation *conv)
{
    return purple_conversation_get_data(conv, PURPLE_CONV_DATA_EVENT_QUEUE);
}

static void _event_send_complete(MatrixAccount *account, gpointer user_data,
      JsonNode *json_root)
{
    PurpleConversation *conv = user_data;
    JsonObject *response_object;
    const gchar *event_id;
    GList *event_queue;
    MatrixRoomEvent *event;

    response_object = matrix_json_node_get_object(json_root);
    event_id = matrix_json_object_get_string_member(response_object,
            "event_id");
    purple_debug_info("matrixprpl", "Successfully sent event id %s\n",
            event_id);

    event_queue = _get_event_queue(conv);
    event = event_queue -> data;
    _free_room_event(event);
    event_queue = g_list_remove(event_queue, event);

    purple_conversation_set_data(conv, PURPLE_CONV_DATA_EVENT_QUEUE,
            event_queue);

    if(event_queue) {
        _send_queued_event(conv);
    } else {
        purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND,
                NULL);
    }
}


/**
 * Unable to send event to homeserver
 */
void _event_send_error(MatrixAccount *ma, gpointer user_data,
        const gchar *error_message)
{
    PurpleConversation *conv = user_data;
    matrix_api_error(ma, user_data, error_message);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND, NULL);

    /* for now, we leave the message queued. We should consider retrying. */
}

/**
 * homeserver gave non-200 on event send.
 */
void _event_send_bad_response(MatrixAccount *ma, gpointer user_data,
        int http_response_code, JsonNode *json_root)
{
    PurpleConversation *conv = user_data;
    matrix_api_bad_response(ma, user_data, http_response_code, json_root);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND, NULL);

    /* for now, we leave the message queued. We should consider retrying. */
}

static void _send_queued_event(PurpleConversation *conv)
{
    PurpleUtilFetchUrlData *fetch_data;
    MatrixAccount *acct;
    MatrixRoomEvent *event;

    acct = purple_connection_get_protocol_data(conv->account->gc);
    event = _get_event_queue(conv) -> data;
    g_assert(event != NULL);

    purple_debug_info("matrixprpl", "Sending %s with txn id %s\n",
            event->event_type, event->txn_id);

    fetch_data = matrix_api_send(acct, conv->name, event->event_type,
            event->txn_id, event->content, _event_send_complete,
            _event_send_error, _event_send_bad_response, conv);

    purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND,
            fetch_data);
}


static void _enqueue_event(PurpleConversation *conv, const gchar *event_type,
        JsonObject *event_content)
{
    MatrixRoomEvent *event;
    GList *event_queue;
    PurpleUtilFetchUrlData *active_send;

    event = _alloc_room_event(event_type, event_content);
    event->txn_id = g_strdup_printf("%"G_GINT64_FORMAT"%"G_GUINT32_FORMAT,
            g_get_monotonic_time(), g_random_int());

    event_queue = _get_event_queue(conv);
    event_queue = g_list_append(event_queue, event);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_EVENT_QUEUE,
            event_queue);

    purple_debug_info("matrixprpl", "Enqueued %s with txn id %s\n",
            event_type, event->txn_id);

    active_send = purple_conversation_get_data(conv,
            PURPLE_CONV_DATA_ACTIVE_SEND);
    if(active_send != NULL) {
        purple_debug_info("matrixprpl", "Event send is already in progress\n");
    } else {
        _send_queued_event(conv);
    }

}

/*****************************************************************************/


void matrix_room_handle_timeline_event(PurpleConversation *conv,
        const gchar *event_id, const gchar *event_type,
        const gchar *sender, gint64 timestamp, JsonObject *json_content_obj)
{
    const gchar *room_id, *msg_body;
    PurpleMessageFlags flags;

    room_id = conv->name;

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
    state_table = _create_state_table();
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_STATE, state_table);

    purple_conversation_set_data(conv, PURPLE_CONV_DATA_EVENT_QUEUE, NULL);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND, NULL);
    return conv;
}


/**
 * Leave a chat: notify the server that we are leaving, and (ultimately)
 * free the memory structures
 */
void matrix_room_leave_chat(PurpleConversation *conv)
{
    MatrixRoomStateEventTable *state_table;
    GList *event_queue;

    /* TODO: actually tell the server that we are leaving the chat, and only
     * destroy the memory structures once we get a response from that.
     *
     * For now, we just free the state table.
     */
    state_table = matrix_room_get_state_table(conv);
    g_hash_table_destroy(state_table);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_STATE, NULL);

    event_queue = _get_event_queue(conv);
    if(event_queue != NULL) {
        g_list_free_full(event_queue, (GDestroyNotify)_free_room_event);
        purple_conversation_set_data(conv, PURPLE_CONV_DATA_EVENT_QUEUE, NULL);
    }
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

    room_id = conv->name;

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
        comp = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
        g_hash_table_insert(comp, PRPL_CHAT_INFO_ROOM_ID, g_strdup(room_id));

        /* we set the alias to the room id initially, then change it to
         * something more user-friendly below.
         */
        chat = purple_chat_new(conv-> account, room_id, comp);
        purple_blist_add_chat(chat, group, NULL);
    }

    room_name = matrix_room_get_name(matrix_room_get_state_table(conv));
    purple_blist_alias_chat(chat, room_name);
}


/**
 * Send a message in a room
 */
void matrix_room_send_message(struct _PurpleConversation *conv,
        const gchar *message)
{
    JsonObject *content;

    content = json_object_new();
    json_object_set_string_member(content, "msgtype", "m.text");
    json_object_set_string_member(content, "body", message);

    _enqueue_event(conv, "m.room.message", content);
    json_object_unref(content);
}

