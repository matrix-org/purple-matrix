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
        const gchar *next_batch, int timeout);


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

    purple_connection_set_protocol_data(pc, NULL);

    g_free(conn->homeserver);
    conn->homeserver = NULL;

    g_free(conn->access_token);
    conn->access_token = NULL;

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
    JsonNode *body)
{
    PurpleConnection *pc = ma->pc;
    const gchar *next_batch;

    purple_connection_update_progress(pc, _("Connected"), 2, 3);
    purple_connection_set_state(pc, PURPLE_CONNECTED);

    matrix_sync_parse(pc, body, &next_batch);

    /* Start the next sync */
    if(next_batch == NULL) {
        purple_connection_error_reason(pc,
            PURPLE_CONNECTION_ERROR_OTHER_ERROR, "No next_batch field");
        return;
    }
    purple_account_set_string(pc->account, PRPL_ACCOUNT_OPT_NEXT_BATCH,
            next_batch);

    _start_next_sync(ma, next_batch, 30000);
}


static void _start_next_sync(MatrixConnectionData *ma,
        const gchar *next_batch, int timeout)
{
    ma->active_sync = matrix_api_sync(ma, next_batch, timeout,
            _sync_complete, _sync_error, _sync_bad_response, NULL);
}

static void _login_completed(MatrixConnectionData *conn,
        gpointer user_data,
        JsonNode *json_root)
{
    PurpleConnection *pc = conn->pc;
    JsonObject *root_obj;
    const gchar *access_token;
    const gchar *next_batch;

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

    /* start the sync loop */
    next_batch = purple_account_get_string(pc->account,
            PRPL_ACCOUNT_OPT_NEXT_BATCH, NULL);

    purple_connection_update_progress(pc, _("Initial Sync"), 1, 3);
    _start_next_sync(conn, next_batch, 0);
}


void matrix_connection_start_login(PurpleConnection *pc)
{
    PurpleAccount *acct = pc->account;
    MatrixConnectionData *conn = purple_connection_get_protocol_data(pc);

    conn->homeserver = g_strdup(purple_account_get_string(pc->account,
            PRPL_ACCOUNT_OPT_HOME_SERVER, DEFAULT_HOME_SERVER));

    purple_connection_set_state(pc, PURPLE_CONNECTING);
    purple_connection_update_progress(pc, _("Logging in"), 0, 3);

    matrix_api_password_login(conn, acct->username,
            purple_account_get_password(acct), _login_completed, conn);
}

