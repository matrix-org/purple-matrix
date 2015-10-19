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
#include "debug.h"

/* libmatrix */
#include "matrix-sync.h"

/* TODO: make this configurable */
#define MATRIX_HOMESERVER "www.sw1v.org"



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

    matrix_sync_start_loop(ma);
}
