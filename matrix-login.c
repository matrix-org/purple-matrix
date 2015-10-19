/**
 * Implementation of the matrix login process
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

#include "matrix-login.h"

#include <string.h>

/* libpurple */
#include "connection.h"
#include "conversation.h"
#include "debug.h"

/* json-glib */
#include <json-glib/json-glib.h>

/* libmatrix */
#include "matrix-api.h"
#include "matrix-json.h"
#include "matrix-room.h"

/* TODO: make this configurable */
#define MATRIX_HOMESERVER "www.sw1v.org"

/* TODO: move this out */
typedef struct _RoomEventParserData {
	MatrixAccount *acct;
	const gchar *room_id;
	JsonObject *event_map;
} RoomEventParserData;

static void parse_timeline_event(JsonArray *timeline,
		guint state_idx, JsonNode *timeline_entry, gpointer user_data)
{
    RoomEventParserData *data = user_data;
    MatrixAccount *ma = data->acct;
    JsonObject *event_map = data->event_map;
    const gchar *room_id = data->room_id;
    const gchar *event_id, *event_type, *msg_body, *sender;
    JsonObject *json_event_obj, *json_content_obj;
    PurpleMessageFlags flags;
    gint64 timestamp;

    event_id = matrix_json_node_get_string(timeline_entry);
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
    json_content_obj = matrix_json_object_get_object_member(
    		json_event_obj, "content");
    if(event_type == NULL || json_content_obj == NULL)
    	return;

    if(strcmp(event_type, "m.room.message") != 0) {
    	purple_debug_info("prplmatrix", "ignoring unknown room event %s",
    			event_type);
    	return;
    }

    msg_body = matrix_json_object_get_string_member(json_content_obj, "body");
    if(msg_body == NULL) {
    	purple_debug_warning("prplmatrix", "no body in message event %s",
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

    purple_debug_info("prplmatrix", "got message %s in %s\n", msg_body, room_id);
    serv_got_chat_in(ma->pc, g_str_hash(room_id), sender, flags,
    		msg_body, timestamp / 1000);
}


static void parse_timeline_events(MatrixAccount *acct,
		const gchar *room_id,
		JsonArray *events, JsonObject* event_map)
{
	RoomEventParserData data = {acct, room_id, event_map};
    json_array_foreach_element(events, parse_timeline_event, &data);
}

/**
 * handle a room within the initial sync response
 */
static void matrixprpl_handle_initial_sync_room(
		const gchar *room_id, JsonObject *room_data, MatrixAccount *ma)
{
    JsonObject *state_object, *timeline_object, *event_map;
	JsonArray *state_array, *timeline_array;
    const gchar *room_name;
    PurpleConversation *conv;
    PurpleChat *chat;
    PurpleGroup *group;
    MatrixRoomStateEventTable *state_table;
    
    event_map = matrix_json_object_get_object_member(room_data, "event_map");
    
    /* parse the room state */
    state_table = g_hash_table_new(g_str_hash, g_str_equal); /* TODO: free */
    state_object = matrix_json_object_get_object_member(room_data, "state");
    state_array = matrix_json_object_get_array_member(state_object, "events");
    if(state_array != NULL)
    	matrix_room_parse_state_events(state_table, state_array, event_map);

    /* todo: distinguish between 1-1 chats and group chats? */

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

    /* ensure the alias in the buddy list is up-to-date */
    room_name = matrix_room_get_name(state_table);
    purple_blist_alias_chat(chat, room_name);

    /* tell purple we have joined this chat */
    conv = serv_got_joined_chat(ma->pc, g_str_hash(room_id), room_id);
    purple_conversation_set_data(conv, "room_id", g_strdup(room_id)); /* TODO: free */
    purple_conversation_set_data(conv, "state", state_table);

    timeline_object = matrix_json_object_get_object_member(
    		room_data, "timeline");
    timeline_array = matrix_json_object_get_array_member(
    		timeline_object, "events");
    parse_timeline_events(ma, room_id, timeline_array, event_map);
}


/**
 * handle the results of the intialSync request
 */
static void matrixprpl_handle_initial_sync(MatrixAccount *ma, JsonNode *body)
{
    JsonObject *rootObj;
    JsonObject *rooms;
    JsonObject *joined_rooms;
    GList *room_ids, *elem;

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
        purple_debug_info("matrixprpl", "got room %s\n", room_id);
    	matrixprpl_handle_initial_sync_room(room_id, room_data, ma);
    }
    g_list_free(room_ids);
    
#if 0
    /* tell purple about everyone on our buddy list who's connected */
    foreach_matrixprpl_gc(discover_status, gc, NULL);

    /* notify other matrixprpl accounts */
    foreach_matrixprpl_gc(report_status_change, gc, NULL);

    /* fetch stored offline messages */
    purple_debug_info("matrixprpl", "checking for offline messages for %s\n",
                      acct->username);
    offline_messages = g_hash_table_lookup(goffline_messages, acct->username);
    while (offline_messages) {
        GOfflineMessage *message = (GOfflineMessage *)offline_messages->data;
        purple_debug_info("matrixprpl", "delivering offline message to %s: %s\n",
                          acct->username, message->message);
        serv_got_im(gc, message->from, message->message, message->flags,
                    message->mtime);
        offline_messages = g_list_next(offline_messages);

        g_free(message->from);
        g_free(message->message);
        g_free(message);
    }

    g_list_free(offline_messages);
    g_hash_table_remove(goffline_messages, &acct->username);
#endif
}


static void matrixprpl_sync_complete(MatrixAccount *ma,
                                            gpointer user_data,
                                            int http_response_code,
                                            const gchar *http_response_msg,
                                            const gchar *body_start,
                                            JsonNode *body,
                                            const gchar *error_message)
{
    if (error_message) {
        purple_debug_info("matrixprpl", "initial sync gave error %s\n",
                          error_message);
        purple_connection_error_reason(ma->pc,
            PURPLE_CONNECTION_ERROR_NETWORK_ERROR, error_message);
        return;
    }

    if (http_response_code >= 400) {
        purple_debug_info("matrixprpl", "initial sync gave response %s: %s\n",
                          http_response_msg, body_start);
        purple_connection_error_reason(ma->pc,
                                       PURPLE_CONNECTION_ERROR_OTHER_ERROR, http_response_msg);
        return;
    }

    purple_connection_update_progress(ma->pc, _("Connected"), 2, 3);
    purple_connection_set_state(ma->pa->gc, PURPLE_CONNECTED);

    matrixprpl_handle_initial_sync(ma, body);
}
                
/*
 * do the initial sync after login, to get room state
 */
static void matrixprpl_start_sync(MatrixAccount *ma)
{
    purple_connection_set_state(ma->pc, PURPLE_CONNECTING);
    purple_connection_update_progress(ma->pc, _("Initial Sync"), 1, 3);
    matrix_sync(ma, matrixprpl_sync_complete, ma);
}

void matrixprpl_login(PurpleAccount *acct)
{
    PurpleConnection *pc = purple_account_get_connection(acct);
    MatrixAccount *ma = g_new0(MatrixAccount, 1); /* TODO: free */

    purple_connection_set_protocol_data(pc, ma);
    ma->pa = acct;
    ma->pc = pc;
  
    purple_debug_info("matrixprpl", "logging in %s\n", acct->username);

    /* TODO: make this configurable */
    ma->homeserver = g_strdup(MATRIX_HOMESERVER);
    
    /* TODO: use the login API to get our access token.
     *
     * For now, assume that the username *is* the access token.
     */
    ma->access_token = g_strdup(acct->username);

    matrixprpl_start_sync(ma);
}
