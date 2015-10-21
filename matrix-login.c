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

#include "matrix-login.h"

#include <string.h>

/* json-glib */
#include <json-glib/json-glib.h>

/* libpurple */
#include <debug.h>

/* libmatrix */
#include "matrix-api.h"
#include "matrix-json.h"
#include "matrix-sync.h"

static void _login_completed(MatrixAccount *account,
        gpointer user_data,
        JsonNode *json_root)
{
    JsonObject *root_obj;
    const gchar *access_token;

    root_obj = matrix_json_node_get_object(json_root);
    access_token = matrix_json_object_get_string_member(root_obj, "access_token");
    if(access_token == NULL) {
        purple_connection_error_reason(account->pc,
                PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                "No access_token in /login response");
        return;
    }
    account->access_token = g_strdup(access_token); /* TODO: free */
    matrix_sync_start_loop(account);
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

    ma->homeserver = g_strdup(purple_account_get_string(
            acct, PRPL_ACCOUNT_OPT_HOME_SERVER, DEFAULT_HOME_SERVER));
    
    matrix_api_password_login(ma, acct->username,
            purple_account_get_password(acct), _login_completed, ma);
}
