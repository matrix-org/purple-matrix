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
#include <eventloop.h>
#include <libpurple/request.h>
#include <libpurple/core.h>

/* libmatrix */
#include "libmatrix.h"
#include "matrix-api.h"
#include "matrix-json.h"
#include "matrix-sync.h"

static gboolean checkSyncRunning(gpointer user_data);

static void _start_next_sync(MatrixConnectionData *ma,
        const gchar *next_batch, gboolean full_state);


void matrix_connection_new(PurpleConnection *pc)
{
     MatrixConnectionData *conn;

     g_assert(purple_connection_get_protocol_data(pc) == NULL);
     conn = g_new0(MatrixConnectionData, 1);
     conn->pc = pc;
     conn->syncRun = FALSE;
     purple_connection_set_protocol_data(pc, conn);
}


void matrix_connection_free(PurpleConnection *pc)
{
    MatrixConnectionData *conn = purple_connection_get_protocol_data(pc);

    g_assert(conn != NULL);

    conn->syncRun = FALSE;

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
    ma->syncRun = TRUE;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ma->last_sync);

    if(body == NULL) {
        purple_connection_error_reason(pc, PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                "Couldn't parse sync response");
        return;
    }

    // Only update progress and set state if we're not already connected
    if (purple_connection_get_state(pc) != PURPLE_CONNECTED) {
        purple_connection_update_progress(pc, _("Connected"), 2, 3);
        purple_connection_set_state(pc, PURPLE_CONNECTED);
    }

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

static gboolean checkSyncRunning(gpointer user_data)
{
    MatrixConnectionData *ma = (MatrixConnectionData*) user_data;
    const gchar *next_batch;

    gboolean restart = FALSE;

    struct timespec start;
    struct timespec end;
    long startMillis, endMillis;
    long elapsedMillis = 0;

    if(ma == NULL || ma->syncRun == FALSE){
      restart = FALSE;
    }else{
      start = ma->last_sync;
      clock_gettime(CLOCK_MONOTONIC_RAW, &end);

      startMillis = (start.tv_sec*1000) + (start.tv_nsec / 1.0e6);
      endMillis = (end.tv_sec*1000) + (end.tv_nsec / 1.0e6);
      elapsedMillis = endMillis - startMillis;

      if(elapsedMillis > 60000){
        restart = TRUE;
      }
    }

    if(restart){
      matrix_connection_cancel_sync(ma->pc);
      next_batch = purple_account_get_string(ma->pc->account,
            PRPL_ACCOUNT_OPT_NEXT_BATCH, NULL);
      _start_next_sync(ma, next_batch, FALSE);
    }
    return TRUE;
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

    conn->syncRun = FALSE;

    purple_timeout_add(5000, checkSyncRunning, conn);

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

/*
 * Callback from _password_login when the user enters a password.
 */
static void
_password_received(PurpleConnection *gc, PurpleRequestFields *fields)
{
    PurpleAccount *acct;
    const char *entry;
    MatrixConnectionData *conn;
    gboolean remember;

    /* The password prompt dialog doesn't get disposed if the account disconnects */
    if (!PURPLE_CONNECTION_IS_VALID(gc))
      return;

    acct = purple_connection_get_account(gc);
    conn = purple_connection_get_protocol_data(gc);

    entry = purple_request_fields_get_string(fields, "password");
    remember = purple_request_fields_get_bool(fields, "remember");

    if (!entry || !*entry)
    {
        purple_notify_error(acct, NULL, _("Password is required to sign on."), NULL);
        return;
    }

    if (remember)
        purple_account_set_remember_password(acct, TRUE);

    purple_account_set_password(acct, entry);

    matrix_api_password_login(conn, acct->username,
            entry,
            purple_account_get_string(acct, "device_id", NULL),
            _login_completed, conn);
}


static void
_password_cancel(PurpleConnection *gc, PurpleRequestFields *fields)
{
    PurpleAccount *account;

    /* The password prompt dialog doesn't get disposed if the account disconnects */
    if (!PURPLE_CONNECTION_IS_VALID(gc))
        return;

    account = purple_connection_get_account(gc);

    /* Disable the account as the user has cancelled connecting */
    purple_account_set_enabled(account, purple_core_get_ui(), FALSE);
}

/*
 * Start a passworded login.
 */
static void _password_login(MatrixConnectionData *conn, PurpleAccount *acct)
{
    const char *password = purple_account_get_password(acct);

    if (password) {
        matrix_api_password_login(conn, acct->username,
                password,
                purple_account_get_string(acct, "device_id", NULL),
                _login_completed, conn);
    } else {
        purple_account_request_password(acct,G_CALLBACK( _password_received),
                G_CALLBACK(_password_cancel), conn->pc);
    }
}


/*
 * If we get an error during whoami just fall back to password
 * login.
 */
static void _whoami_error(MatrixConnectionData *conn,
        gpointer user_data, const gchar *error_message)
{
    PurpleAccount *acct = user_data;
    purple_debug_info("matrixprpl", "_whoami_error: %s\n", error_message);
    _password_login(conn, acct);
}

/*
 * If we get a bad response just fall back to password login
 */
static void _whoami_badresp(MatrixConnectionData *conn, gpointer user_data,
        int http_response_code, struct _JsonNode *json_root)
{
    purple_debug_info("matrixprpl", "_whoami_badresp\n");
    _whoami_error(conn, user_data, "Bad response");
}

/*
 * A response from the whoami we issued to validate our access token
 * If it's succesful then we can start the connection.
 */
static void _whoami_completed(MatrixConnectionData *conn,
        gpointer user_data,
        JsonNode *json_root,
        const char *raw_body, size_t raw_body_len, const char *content_type)
{
    JsonObject *root_obj = matrix_json_node_get_object(json_root);
    const gchar *user_id = matrix_json_object_get_string_member(root_obj,
            "user_id");

    purple_debug_info("matrixprpl", "_whoami_completed got %s\n", user_id);
    if (!user_id) {
        return _whoami_error(conn, user_data, "no user_id");
    }
    // TODO: That is out user_id - right?
    conn->user_id = g_strdup(user_id);
    _start_sync(conn);
}

void matrix_connection_start_login(PurpleConnection *pc)
{
    PurpleAccount *acct = pc->account;
    MatrixConnectionData *conn = purple_connection_get_protocol_data(pc);
    const gchar *homeserver = purple_account_get_string(pc->account,
            PRPL_ACCOUNT_OPT_HOME_SERVER, DEFAULT_HOME_SERVER);
    const gchar *access_token = purple_account_get_string(pc->account,
            PRPL_ACCOUNT_OPT_ACCESS_TOKEN, NULL);

    if(!g_str_has_suffix(homeserver, "/")) {
        conn->homeserver = g_strconcat(homeserver, "/", NULL);
    } else {
        conn->homeserver = g_strdup(homeserver);
    }

    purple_connection_set_state(pc, PURPLE_CONNECTING);
    purple_connection_update_progress(pc, _("Logging in"), 0, 3);

    if (access_token) {
        conn->access_token = g_strdup(access_token);
        matrix_api_whoami(conn, _whoami_completed, _whoami_error,
                _whoami_badresp, conn);
    } else {
        _password_login(conn, acct);
    }
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
