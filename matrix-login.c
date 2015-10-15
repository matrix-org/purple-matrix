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
#include "debug.h"

/* libmatrix */
#include "matrix-api.h"

/* TODO: make this configurable */
#define MATRIX_HOMESERVER "matrix.org"

static void matrixprpl_initialsync_complete(MatrixAccount *ma,
                                            gpointer user_data,
                                            const gchar *ret_data,
                                            gsize ret_len,
                                            int http_response_code,
                                            const gchar *http_response_msg,
                                            const gchar *body_start,
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
