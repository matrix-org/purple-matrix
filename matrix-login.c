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

static void matrixprpl_sync_complete(MatrixAccount *ma,
                                            gpointer user_data,
                                            int http_response_code,
                                            const gchar *http_response_msg,
                                            const gchar *body_start,
                                            JsonNode *body,
                                            const gchar *error_message);

/**
 * handle the results of the sync request
 */
static void matrixprpl_handle_sync(MatrixAccount *ma, JsonNode *body)
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
        matrix_room_handle_sync(room_id, room_data, ma);
    }
    g_list_free(room_ids);

    /* Start the next sync */
    next_batch = matrix_json_object_get_string_member(rootObj, "next_batch");
    if(next_batch == NULL) {
        purple_connection_error_reason(ma->pc,
            PURPLE_CONNECTION_ERROR_OTHER_ERROR, "No next_batch field");
        return;
    }

    matrix_sync(ma, next_batch, matrixprpl_sync_complete, NULL);
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
        purple_debug_info("matrixprpl", "sync gave error %s\n",
                          error_message);
        purple_connection_error_reason(ma->pc,
            PURPLE_CONNECTION_ERROR_NETWORK_ERROR, error_message);
        return;
    }

    if (http_response_code >= 400) {
        purple_debug_info("matrixprpl", "sync gave response %s: %s\n",
                          http_response_msg, body_start);
        purple_connection_error_reason(ma->pc,
                                       PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                                       http_response_msg);
        return;
    }

    purple_debug_info("matrixprpl", "got sync result %s\n", body_start);

    purple_connection_update_progress(ma->pc, _("Connected"), 2, 3);
    purple_connection_set_state(ma->pa->gc, PURPLE_CONNECTED);

    matrixprpl_handle_sync(ma, body);
}

/*
 * do the initial sync after login, to get room state
 */
static void matrixprpl_start_sync(MatrixAccount *ma)
{
    purple_connection_update_progress(ma->pc, _("Initial Sync"), 1, 3);
    matrix_sync(ma, NULL, matrixprpl_sync_complete, NULL);
}


void matrixprpl_login(PurpleAccount *acct)
{
    PurpleConnection *pc = purple_account_get_connection(acct);
    MatrixAccount *ma = g_new0(MatrixAccount, 1); /* TODO: free */

    purple_connection_set_protocol_data(pc, ma);
    ma->pa = acct;
    ma->pc = pc;

    purple_connection_set_state(ma->pc, PURPLE_CONNECTING);
    purple_connection_update_progress(ma->pc, _("Logging in"), 0, 3);

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
