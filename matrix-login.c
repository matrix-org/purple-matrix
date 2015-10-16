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
#define MATRIX_HOMESERVER "matrix.org"

/**
 * handle a room within the initialSync response
 */
static void matrixprpl_handle_initialsync_room(JsonArray *rooms,
                                               guint room_idx,
                                               JsonNode *room,
                                               gpointer user_data)
{
    MatrixAccount *ma = (MatrixAccount *)user_data;
    JsonObject *room_obj;
    JsonArray *state_array;
    const gchar *room_id;
    const gchar *room_name;
    PurpleConversation *conv;
    PurpleChat *chat;
    PurpleGroup *group;
    MatrixRoomStateEventTable *state_table;
    
    /* TODO: error handling */
    room_obj = matrix_json_node_get_object(room);
    room_id = matrix_json_object_get_string_member(room_obj, "room_id");
    state_array = matrix_json_object_get_array_member(room_obj, "state");
    
    purple_debug_info("matrixprpl", "got room %s\n", room_id);

    /* parse the room state */
    state_table = g_hash_table_new(g_str_hash, g_str_equal); /* TODO: free */
    if(state_array != NULL)
    	matrix_room_parse_state_events(state_table, state_array);

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
        g_hash_table_insert(comp, "room_id", g_strdup(room_id)); /* TODO: free? */
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
}


/**
 * handle the results of the intialSync request
 */
static void matrixprpl_handle_initialsync(MatrixAccount *ma, JsonNode *body)
{
    JsonObject *rootObj;
    JsonArray *rooms;
    
    /* TODO: error handling */
    rootObj = json_node_get_object(body);
    rooms = json_object_get_array_member(rootObj, "rooms");

    json_array_foreach_element(rooms, matrixprpl_handle_initialsync_room, ma);
    
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


static void matrixprpl_initialsync_complete(MatrixAccount *ma,
                                            gpointer user_data,
                                            int http_response_code,
                                            const gchar *http_response_msg,
                                            const gchar *body_start,
                                            JsonNode *body,
                                            const gchar *error_message)
{
    if (error_message) {
        purple_debug_info("matrixprpl", "initialsync gave error %s\n",
                          error_message);
        purple_connection_error_reason(ma->pc,
            PURPLE_CONNECTION_ERROR_NETWORK_ERROR, error_message);
        return;
    }

    if (http_response_code >= 400) {
        purple_debug_info("matrixprpl", "initialsync gave response %s: %s\n",
                          http_response_msg, body_start);
        purple_connection_error_reason(ma->pc,
                                       PURPLE_CONNECTION_ERROR_OTHER_ERROR, http_response_msg);
        return;
    }

    purple_connection_update_progress(ma->pc, _("Connected"), 2, 3);
    purple_connection_set_state(ma->pc, PURPLE_CONNECTED);

    matrixprpl_handle_initialsync(ma, body);
}
                
/*
 * do the initialsync after login, to get room state
 */
static void matrixprpl_start_initialsync(MatrixAccount *ma)
{
    purple_connection_set_state(ma->pc, PURPLE_CONNECTING);
    purple_connection_update_progress(ma->pc, _("Initial Sync"), 1, 3);
    matrix_initialsync(ma, matrixprpl_initialsync_complete, ma);
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

    matrixprpl_start_initialsync(ma);
}
