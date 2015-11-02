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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA
 */

#include "matrix-room.h"

/* stdlib */
#include <string.h>

/* libpurple */
#include "connection.h"
#include "debug.h"

#include "libmatrix.h"
#include "matrix-api.h"
#include "matrix-event.h"
#include "matrix-json.h"
#include "matrix-roommembers.h"
#include "matrix-statetable.h"


static gchar *_get_room_name(MatrixConnectionData *conn,
        PurpleConversation *conv);

static MatrixConnectionData *_get_connection_data_from_conversation(
        PurpleConversation *conv)
{
    return conv->account->gc->proto_data;
}

/******************************************************************************
 *
 * conversation data
 */

/*
 * identifiers for purple_conversation_get/set_data
 */

/* a MatrixRoomStateEventTable * - see below */
#define PURPLE_CONV_DATA_STATE "state"

/* a GList of MatrixRoomEvent * */
#define PURPLE_CONV_DATA_EVENT_QUEUE "queue"

/* PurpleUtilFetchUrlData * */
#define PURPLE_CONV_DATA_ACTIVE_SEND "active_send"

/* MatrixRoomMemberTable * - see below */
#define PURPLE_CONV_MEMBER_TABLE "member_table"


/**
 * Get the member table for a room
 */
static MatrixRoomMemberTable *matrix_room_get_member_table(
        PurpleConversation *conv)
{
    return purple_conversation_get_data(conv, PURPLE_CONV_MEMBER_TABLE);
}


/**
 * Get the state table for a room
 */
static MatrixRoomStateEventTable *matrix_room_get_state_table(
        PurpleConversation *conv)
{
    return purple_conversation_get_data(conv, PURPLE_CONV_DATA_STATE);
}


/******************************************************************************
 *
 * room state handling
 */


/**
 * Update the name of the room in the buddy list (which in turn will update it
 * in the chat window)
 *
 * @param conv: conversation info
 */
static void _update_room_alias(PurpleConversation *conv)
{
    gchar *room_name;
    MatrixConnectionData *conn = _get_connection_data_from_conversation(conv);
    PurpleChat *chat = purple_blist_find_chat(conv->account, conv->name);

    /* we know there should be a buddy list entry for this room */
    g_assert(chat != NULL);

    room_name = _get_room_name(conn, conv);
    purple_blist_alias_chat(chat, room_name);
    g_free(room_name);
}


/**
 * Called when there is a change to the member list. Tells the MemberTable
 * about it.
 */
static void _on_member_change(PurpleConversation *conv,
        const gchar *member_user_id, MatrixRoomEvent *new_state)
{
    MatrixRoomMemberTable *member_table;

    member_table = matrix_room_get_member_table(conv);

    matrix_roommembers_update_member(member_table, member_user_id,
            new_state->content);
}


/**
 * Called when there is a state update.
 *
 * old_state may be NULL to indicate addition of a state
 * key.
 */
static void _on_state_update(const gchar *event_type,
        const gchar *state_key, MatrixRoomEvent *old_state,
        MatrixRoomEvent *new_state, gpointer user_data)
{
    PurpleConversation *conv = user_data;
    g_assert(new_state != NULL);

    if(strcmp(event_type, "m.room.member") == 0) {
        _on_member_change(conv, state_key, new_state);
    }
    else if(strcmp(event_type, "m.room.alias") == 0 ||
            strcmp(event_type, "m.room.room_name") == 0) {
        _update_room_alias(conv);
    }
}

void matrix_room_handle_state_event(struct _PurpleConversation *conv,
        const gchar *event_id, JsonObject *json_event_obj)
{
    MatrixRoomStateEventTable *state_table = matrix_room_get_state_table(conv);
    matrix_statetable_update(state_table, json_event_obj,
            _on_state_update, conv);
}


/**
 * figure out the best name for a room based on its members list
 *
 * @returns a string which should be freed
 */
static gchar *_get_room_name_from_members(MatrixConnectionData *conn,
        PurpleConversation *conv)
{
    GList *tmp, *members;
    const gchar *member1;
    gchar *res;
    MatrixRoomMemberTable *member_table;

    member_table = matrix_room_get_member_table(conv);
    members = matrix_roommembers_get_active_members(member_table);

    /* remove ourselves from the list */
    tmp = g_list_find_custom(members, conn->user_id, (GCompareFunc)strcmp);
    if(tmp != NULL) {
        members = g_list_delete_link(members, tmp);
    }

    if(members == NULL) {
        /* nobody else here! */
        return NULL;
    }

    member1 = matrix_roommembers_get_displayname_for_member(
            member_table, members->data);

    if(members->next == NULL) {
        /* one other person */
        res = g_strdup(member1);
    } else if(members->next->next == NULL) {
        /* two other people */
        const gchar *member2 = matrix_roommembers_get_displayname_for_member(
                member_table, members->next->data);
        res = g_strdup_printf(_("%s and %s"), member1, member2);
    } else {
        int nmembers = g_list_length(members);
        res = g_strdup_printf(_("%s and %i others"), member1, nmembers);
    }

    g_list_free(members);
    return res;
}


/**
 * figure out the best name for a room
 *
 * @returns a string which should be freed
 */
static gchar *_get_room_name(MatrixConnectionData *conn,
        PurpleConversation *conv)
{
    MatrixRoomStateEventTable *state_table = matrix_room_get_state_table(conv);
    gchar *res;

    /* first try to pick a name based on the official name / alias */
    res = matrix_statetable_get_room_alias(state_table);
    if (res)
        return res;

    /* look for room members, and pick a name based on that */
    res = _get_room_name_from_members(conn, conv);
    if (res)
        return res;

    /* failing all else, just use the room id */
    return g_strdup(conv -> name);

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

static void _event_send_complete(MatrixConnectionData *account, gpointer user_data,
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
    matrix_event_free(event);
    event_queue = g_list_remove(event_queue, event);

    purple_conversation_set_data(conv, PURPLE_CONV_DATA_EVENT_QUEUE,
            event_queue);

    _send_queued_event(conv);
}


/**
 * Unable to send event to homeserver
 */
void _event_send_error(MatrixConnectionData *ma, gpointer user_data,
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
void _event_send_bad_response(MatrixConnectionData *ma, gpointer user_data,
        int http_response_code, JsonNode *json_root)
{
    PurpleConversation *conv = user_data;
    matrix_api_bad_response(ma, user_data, http_response_code, json_root);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND, NULL);

    /* for now, we leave the message queued. We should consider retrying. */
}

/**
 * send the next queued event, provided the connection isn't shutting down.
 *
 * Updates PURPLE_CONV_DATA_ACTIVE_SEND either way.
 */
static void _send_queued_event(PurpleConversation *conv)
{
    MatrixApiRequestData *fetch_data = NULL;
    MatrixConnectionData *acct;
    MatrixRoomEvent *event;
    PurpleConnection *pc = conv->account->gc;
    GList *queue;

    acct = purple_connection_get_protocol_data(pc);
    queue = _get_event_queue(conv);

    if(queue == NULL) {
        /* nothing else to send */
    } else if(pc -> wants_to_die) {
        /* don't make any more requests if the connection is closing */
        purple_debug_info("matrixprpl", "Not sending new events on dying"
                " connection");
    } else {
        event = queue -> data;
        g_assert(event != NULL);
        purple_debug_info("matrixprpl", "Sending %s with txn id %s\n",
                event->event_type, event->txn_id);

        fetch_data = matrix_api_send(acct, conv->name, event->event_type,
                event->txn_id, event->content, _event_send_complete,
                _event_send_error, _event_send_bad_response, conv);
    }

    purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND,
            fetch_data);
}


static void _enqueue_event(PurpleConversation *conv, const gchar *event_type,
        JsonObject *event_content)
{
    MatrixRoomEvent *event;
    GList *event_queue;
    MatrixApiRequestData *active_send;

    event = matrix_event_new(event_type, event_content);
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


/**
 * If there is an event send in progress, cancel it
 */
static void _cancel_event_send(PurpleConversation *conv)
{
    MatrixApiRequestData *active_send = purple_conversation_get_data(conv,
            PURPLE_CONV_DATA_ACTIVE_SEND);

    if(active_send == NULL)
        return;

    purple_debug_info("matrixprpl", "Cancelling event send");
    matrix_api_cancel(active_send);

    g_assert(purple_conversation_get_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND)
            == NULL);
}

/*****************************************************************************/

void matrix_room_handle_timeline_event(PurpleConversation *conv,
        const gchar *event_id, JsonObject *json_event_obj)
{
    const gchar *event_type, *sender_id, *transaction_id;
    gint64 timestamp;
    JsonObject *json_content_obj;
    JsonObject *json_unsigned_obj;
    const gchar *room_id, *msg_body;
    PurpleMessageFlags flags;
    const gchar *sender_display_name;

    room_id = conv->name;

    event_type = matrix_json_object_get_string_member(
            json_event_obj, "type");
    sender_id = matrix_json_object_get_string_member(json_event_obj, "sender");
    timestamp = matrix_json_object_get_int_member(json_event_obj,
                "origin_server_ts");
    json_content_obj = matrix_json_object_get_object_member(
            json_event_obj, "content");

    if(event_type == NULL) {
        purple_debug_warning("matrixprpl", "event missing type field");
        return;
    }

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

    json_unsigned_obj = matrix_json_object_get_object_member(json_event_obj,
            "unsigned");
    transaction_id = matrix_json_object_get_string_member(json_unsigned_obj,
            "transaction_id");

    /* if it has a transaction id, it's an echo of a message we sent.
     * We shouldn't really just ignore it, but I'm not sure how to update a sent
     * message.
     */
    if(transaction_id != NULL) {
        purple_debug_info("matrixprpl", "got remote echo %s in %s\n", msg_body,
                room_id);
        return;
    }

    if(sender_id == NULL) {
        sender_display_name = "<unknown>";
    } else {
        MatrixRoomMemberTable *member_table =
                matrix_room_get_member_table(conv);

        sender_display_name = matrix_roommembers_get_displayname_for_member(
                member_table, sender_id);
    }

    flags = PURPLE_MESSAGE_RECV;

    purple_debug_info("matrixprpl", "got message from %s in %s\n", sender_id,
            room_id);
    serv_got_chat_in(conv->account->gc, g_str_hash(room_id),
            sender_display_name, flags, msg_body, timestamp / 1000);
}


PurpleConversation *matrix_room_create_conversation(
        PurpleConnection *pc, const gchar *room_id)
{
    PurpleConversation *conv;
    MatrixRoomStateEventTable *state_table;
    MatrixRoomMemberTable *member_table;

    purple_debug_info("matrixprpl", "New room %s\n", room_id);

    /* tell purple we have joined this chat */
    conv = serv_got_joined_chat(pc, g_str_hash(room_id), room_id);

    /* set our data on it */
    state_table = matrix_statetable_new();
    member_table = matrix_roommembers_new_table();
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_EVENT_QUEUE, NULL);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND, NULL);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_STATE, state_table);
    purple_conversation_set_data(conv, PURPLE_CONV_MEMBER_TABLE,
            member_table);

    return conv;
}


/**
 * Leave a chat: notify the server that we are leaving, and (ultimately)
 * free the memory structures
 */
void matrix_room_leave_chat(PurpleConversation *conv)
{
    MatrixConnectionData *conn;
    MatrixRoomStateEventTable *state_table;
    GList *event_queue;
    MatrixRoomMemberTable *member_table;

    conn = _get_connection_data_from_conversation(conv);

    _cancel_event_send(conv);
    matrix_api_leave_room(conn, conv->name, NULL, NULL, NULL, NULL);

    /* At this point, we have no confirmation that the 'leave' request will
     * be successful (nor that it has even started), so it's questionable
     * whether we can/should actually free all of the room state.
     *
     * On the other hand, we don't have any mechanism for telling purple that
     * we haven't really left the room, and if the leave request does fail,
     * we'll set the error flag on the connection, which will eventually
     * result in pidgin flagging the connection as failed; things will
     * hopefully then get resynced when the user reconnects.
     */

    state_table = matrix_room_get_state_table(conv);
    matrix_statetable_destroy(state_table);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_STATE, NULL);

    member_table = matrix_room_get_member_table(conv);
    matrix_roommembers_free_table(member_table);
    purple_conversation_set_data(conv, PURPLE_CONV_MEMBER_TABLE, NULL);

    event_queue = _get_event_queue(conv);
    if(event_queue != NULL) {
        g_list_free_full(event_queue, (GDestroyNotify)matrix_event_free);
        purple_conversation_set_data(conv, PURPLE_CONV_DATA_EVENT_QUEUE, NULL);
    }
}


static void _update_user_list(PurpleConversation *conv,
        gboolean announce_arrivals)
{
    PurpleConvChat *chat = PURPLE_CONV_CHAT(conv);
    MatrixRoomMemberTable *table = matrix_room_get_member_table(conv);
    GList *names = NULL, *flags = NULL, *oldnames = NULL;
    gboolean updated = FALSE;

    matrix_roommembers_get_new_members(table, &names, &flags);
    if(names) {
        purple_conv_chat_add_users(chat, names, NULL, flags, announce_arrivals);
        g_list_free(names);
        g_list_free(flags);
        names = NULL;
        flags = NULL;
        updated = TRUE;
    }

    matrix_roommembers_get_renamed_members(table, &oldnames, &names);
    if(names) {
        GList *name1 = names, *oldname1 = oldnames;
        while(name1 && oldname1) {
            purple_conv_chat_rename_user(chat, oldname1->data, name1->data);
            name1 = g_list_next(name1);
            oldname1 = g_list_next(oldname1);
        }
        g_list_free_full(oldnames, (GDestroyNotify)g_free);
        g_list_free(names);
        names = NULL;
        oldnames = NULL;
        updated = TRUE;
    }

    matrix_roommembers_get_left_members(table, &names);
    if(names) {
        purple_conv_chat_remove_users(chat, names, NULL);
        g_list_free_full(names, (GDestroyNotify)g_free);
        names = NULL;
        updated = TRUE;
    }

    if(updated)
        _update_room_alias(conv);
}


void matrix_room_complete_state_update(PurpleConversation *conv,
        gboolean announce_arrivals)
{
    _update_user_list(conv, announce_arrivals);
}


static const gchar *_get_my_display_name(PurpleConversation *conv)
{
    MatrixConnectionData *conn = _get_connection_data_from_conversation(conv);
    MatrixRoomMemberTable *member_table =
            matrix_room_get_member_table(conv);

    return matrix_roommembers_get_displayname_for_member(
            member_table, conn->user_id);
}

/**
 * Send a message in a room
 */
void matrix_room_send_message(PurpleConversation *conv, const gchar *message)
{
    JsonObject *content;
    PurpleConvChat *chat = PURPLE_CONV_CHAT(conv);

    content = json_object_new();
    json_object_set_string_member(content, "msgtype", "m.text");
    json_object_set_string_member(content, "body", message);

    _enqueue_event(conv, "m.room.message", content);
    json_object_unref(content);

    purple_conv_chat_write(chat, _get_my_display_name(conv),
            message, PURPLE_MESSAGE_SEND, g_get_real_time()/1000/1000);
}


gchar *matrix_room_displayname_to_userid(struct _PurpleConversation *conv,
        const gchar *who)
{
    MatrixRoomMemberTable *member_table =
            matrix_room_get_member_table(conv);
    return matrix_roommembers_displayname_to_userid(member_table, who);
}
