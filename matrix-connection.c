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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA
 */

#include "matrix-connection.h"
#include "matrix-e2e.h"

#include <string.h>

/* json-glib */
#include <json-glib/json-glib.h>

/* libpurple */
#include <debug.h>

/* libmatrix */
#include "libmatrix.h"
#include "matrix-api.h"
#include "matrix-json.h"
#include "matrix-sync.h"

static void _start_next_sync(MatrixConnectionData *ma,
        const gchar *next_batch, gboolean full_state);


void matrix_connection_new(PurpleConnection *pc)
{
     MatrixConnectionData *conn;

     g_assert(purple_connection_get_protocol_data(pc) == NULL);
     conn = g_new0(MatrixConnectionData, 1);
     conn->pc = pc;
     purple_connection_set_protocol_data(pc, conn);
}


void matrix_connection_free(PurpleConnection *pc)
{
    MatrixConnectionData *conn = purple_connection_get_protocol_data(pc);

    g_assert(conn != NULL);

    matrix_e2e_cleanup_connection(conn);
    purple_connection_set_protocol_data(pc, NULL);

    g_free(conn->homeserver);
    conn->homeserver = NULL;

    g_free(conn->access_token);
    conn->access_token = NULL;

    g_free(conn->user_id);
    conn->user_id = NULL;

    conn->pc = NULL;

    g_free(conn);
}

void matrix_connection_cancel_sync(PurpleConnection *pc)
{
    MatrixConnectionData *conn = purple_connection_get_protocol_data(pc);
    g_assert(conn != NULL);
    if(conn->active_sync) {
        purple_debug_info("matrixprpl", "Cancelling active sync on %s\n",
                pc->account->username);
        matrix_api_cancel(conn->active_sync);
    }
    return;
}

/**
 * /sync failed
 */
void _sync_error(MatrixConnectionData *ma, gpointer user_data,
        const gchar *error_message)
{
    ma->active_sync = NULL;
    matrix_api_error(ma, user_data, error_message);
}

/**
 * /sync gave non-200 response
 */
void _sync_bad_response(MatrixConnectionData *ma, gpointer user_data,
        int http_response_code, JsonNode *json_root)
{
    ma->active_sync = NULL;
    matrix_api_bad_response(ma, user_data, http_response_code, json_root);
}


/* callback which is called when a /sync request completes */
static void _sync_complete(MatrixConnectionData *ma, gpointer user_data,
    JsonNode *body,
    const char *raw_body, size_t raw_body_len, const char *content_type)
{
    PurpleConnection *pc = ma->pc;
    const gchar *next_batch;

    ma->active_sync = NULL;

    if(body == NULL) {
        purple_connection_error_reason(pc, PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                "Couldn't parse sync response");
        return;
    }

    purple_connection_update_progress(pc, _("Connected"), 2, 3);
    purple_connection_set_state(pc, PURPLE_CONNECTED);

    matrix_sync_parse(pc, body, &next_batch);

    /* Start the next sync */
    if(next_batch == NULL) {
        purple_connection_error_reason(pc, PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                "No next_batch field");
        return;
    }
    purple_account_set_string(pc->account, PRPL_ACCOUNT_OPT_NEXT_BATCH,
            next_batch);

    _start_next_sync(ma, next_batch, FALSE);
}


static void _start_next_sync(MatrixConnectionData *ma,
        const gchar *next_batch, gboolean full_state)
{
    ma->active_sync = matrix_api_sync(ma, next_batch, 30000, full_state,
            _sync_complete, _sync_error, _sync_bad_response, NULL);
}


static gboolean _account_has_active_conversations(PurpleAccount *account)
{
    GList *ptr;

    for(ptr = purple_get_conversations(); ptr != NULL; ptr = g_list_next(ptr))
    {
        PurpleConversation *conv = ptr->data;
        if(conv -> account == account)
            return TRUE;
    }
    return FALSE;
}

static void _start_sync(MatrixConnectionData *conn)
{
    PurpleConnection *pc = conn->pc;
    gboolean needs_full_state_sync = TRUE;
    const gchar *next_batch;
    const gchar *device_id = purple_account_get_string(pc->account,
            "device_id", NULL);

    if (device_id) {
        matrix_e2e_get_device_keys(conn, device_id);
    }

    /* start the sync loop */
    next_batch = purple_account_get_string(pc->account,
            PRPL_ACCOUNT_OPT_NEXT_BATCH, NULL);

    if(next_batch != NULL) {
        /* if we have previously done a full_state sync on this account, there's
         * no need to do another. If there are already conversations associated
         * with this account, that is a pretty good indication that we have
         * previously done a full_state sync.
         */
        if(_account_has_active_conversations(pc->account)) {
            needs_full_state_sync = FALSE;
        } else {
            /* this appears to be the first time we have connected to this account
             * on this invocation of pidgin.
             */
            gboolean skip = purple_account_get_bool(pc->account,
                    PRPL_ACCOUNT_OPT_SKIP_OLD_MESSAGES, FALSE);
            if(!skip)
                next_batch = NULL;
        }
    }

    if(needs_full_state_sync) {
        purple_connection_update_progress(pc, _("Initial Sync"), 1, 3);
    } else {
        purple_connection_update_progress(pc, _("Connected"), 2, 3);
        purple_connection_set_state(pc, PURPLE_CONNECTED);
    }

    _start_next_sync(conn, next_batch, needs_full_state_sync);
}

static void _login_completed(MatrixConnectionData *conn,
        gpointer user_data,
        JsonNode *json_root,
        const char *raw_body, size_t raw_body_len, const char *content_type)
{
    PurpleConnection *pc = conn->pc;
    JsonObject *root_obj;
    const gchar *access_token;
    const gchar *device_id;

    root_obj = matrix_json_node_get_object(json_root);
    access_token = matrix_json_object_get_string_member(root_obj,
            "access_token");
    if(access_token == NULL) {
        purple_connection_error_reason(pc,
                PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                "No access_token in /login response");
        return;
    }
    conn->access_token = g_strdup(access_token);
    conn->user_id = g_strdup(matrix_json_object_get_string_member(root_obj,
            "user_id"));
    device_id = matrix_json_object_get_string_member(root_obj, "device_id");
    purple_account_set_string(pc->account, "device_id", device_id);
    purple_account_set_string(pc->account, PRPL_ACCOUNT_OPT_ACCESS_TOKEN,
            access_token);

    _start_sync(conn);
}


void matrix_connection_start_login(PurpleConnection *pc)
{
    PurpleAccount *acct = pc->account;
    MatrixConnectionData *conn = purple_connection_get_protocol_data(pc);
    const gchar *homeserver = purple_account_get_string(pc->account,
            PRPL_ACCOUNT_OPT_HOME_SERVER, DEFAULT_HOME_SERVER);

    if(!g_str_has_suffix(homeserver, "/")) {
        conn->homeserver = g_strconcat(homeserver, "/", NULL);
    } else {
        conn->homeserver = g_strdup(homeserver);
    }

    purple_connection_set_state(pc, PURPLE_CONNECTING);
    purple_connection_update_progress(pc, _("Logging in"), 0, 3);

    matrix_api_password_login(conn, acct->username,
            purple_account_get_password(acct),
            purple_account_get_string(acct, "device_id", NULL),
            _login_completed, conn);
}


static void _join_completed(MatrixConnectionData *conn,
        gpointer user_data,
        JsonNode *json_root,
        const char *raw_body, size_t raw_body_len, const char *content_type)
{
    GHashTable *components = user_data;
    JsonObject *root_obj;
    const gchar *room_id;

    root_obj = matrix_json_node_get_object(json_root);
    room_id = matrix_json_object_get_string_member(root_obj, "room_id");
    purple_debug_info("matrixprpl", "join %s completed", room_id);

    g_hash_table_destroy(components);
}


static void _join_error(MatrixConnectionData *conn,
        gpointer user_data, const gchar *error_message)
{
    GHashTable *components = user_data;
    g_hash_table_destroy(components);
    matrix_api_error(conn, user_data, error_message);
}


static void _join_failed(MatrixConnectionData *conn,
        gpointer user_data, int http_response_code,
        struct _JsonNode *json_root)
{
    GHashTable *components = user_data;
    JsonObject *json_obj;
    const gchar *error = NULL;
    const gchar *title = "Error joining chat";

    if (json_root != NULL) {
        json_obj = matrix_json_node_get_object(json_root);
        error = matrix_json_object_get_string_member(json_obj, "error");
    }

    purple_notify_error(conn->pc, title, title, error);
    purple_serv_got_join_chat_failed(conn->pc, components);
    g_hash_table_destroy(components);
}



void matrix_connection_join_room(struct _PurpleConnection *pc,
        const gchar *room, GHashTable *components)
{
    GHashTable *copy;
    GHashTableIter iter;
    gpointer key, value;

    MatrixConnectionData *conn = purple_connection_get_protocol_data(pc);

    /* we have to copy the components table, so that we can pass it back
     * later on :/
     */
    copy = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_iter_init (&iter, components);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
        g_hash_table_insert(copy, g_strdup(key), g_strdup(value));
    }

    matrix_api_join_room(conn, room, _join_completed, _join_error, _join_failed,
            copy);
}


void matrix_connection_reject_invite(struct _PurpleConnection *pc,
        const gchar *room_id)
{
    MatrixConnectionData *conn = purple_connection_get_protocol_data(pc);

    matrix_api_leave_room(conn, room_id, NULL, NULL, NULL, NULL);
}
