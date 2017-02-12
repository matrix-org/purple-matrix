/**
 * Matrix end-to-end encryption support
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

#include <stdio.h>
#include <string.h>
#include "libmatrix.h"
#include "matrix-api.h"
#include "matrix-e2e.h"
#include "matrix-json.h"

/* json-glib */
#include <json-glib/json-glib.h>

#include "connection.h"
#ifndef MATRIX_NO_E2E
#include "olm/olm.h"

struct _MatrixE2EData {
    OlmAccount *oa;
    gchar *device_id;
};

/* Really clear an area of memory */
static void clear_mem(volatile char *data, size_t len)
{
#ifdef __STDC_LIB_EXT1__
    /* Untested! */
    memset_s(data, len, '\0', len);
#else
    size_t index;
    for(index = 0;index < len; index ++)
    {
        data[index] = '\0';
    }
#endif
}

/* Sign the JsonObject with olm_account_sign and add it to the object
 * as a 'signatures' member of the top level object.
 * 0 on success
 */
int matrix_sign_json(MatrixConnectionData *conn, JsonObject *tosign)
{
    int ret = -1;
    OlmAccount *account = conn->e2e->oa;
    const gchar *device_id = conn->e2e->device_id;
    PurpleConnection *pc = conn->pc;
    GString *can_json = matrix_canonical_json(tosign);
    gchar *can_json_c = g_string_free(can_json, FALSE);
    size_t sig_length = olm_account_signature_length(account);
    gchar *sig = g_malloc0(sig_length+1);
    if (olm_account_sign(account, can_json_c, strlen(can_json_c),
            sig, sig_length)==olm_error()) {
        purple_connection_error_reason(pc,
                PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                olm_account_last_error(account));
        goto out;
    }

    /* We need to add a "signatures" member which is an object, with
     * a "user_id" member that is itself an object which has an "ed25519:$DEVICEID" member
     * that is the signature.
     */
    GString *alg_dev = g_string_new(NULL);
    g_string_printf(alg_dev, "ed25519:%s", device_id);
    gchar *alg_dev_c = g_string_free(alg_dev, FALSE);
    JsonObject *sig_dev = json_object_new();
    json_object_set_string_member(sig_dev, alg_dev_c, sig);
    JsonObject *sig_obj = json_object_new();
    json_object_set_object_member(sig_obj, conn->user_id, sig_dev);
    json_object_set_object_member(tosign, "signatures", sig_obj);
    
    g_free(alg_dev_c);
    ret = 0;
out:
    g_free(can_json_c);
    g_free(sig);

    return ret;
}

/* Store the current Olm account data into the Purple account data
 */
static int matrix_store_e2e_account(MatrixConnectionData *conn)
{
    PurpleConnection *pc = conn->pc;

    size_t pickle_len = olm_pickle_account_length(conn->e2e->oa);
    char *pickled_account = g_malloc0(pickle_len+1);

    /* TODO: Wth to use as the key? We've not got anything in purple to protect
     * it with? We could do with stuffing something into the system key ring
     */
    if (olm_pickle_account(conn->e2e->oa, "!", 1, pickled_account, pickle_len) ==
        olm_error()) {
        purple_connection_error_reason(pc,
                PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                olm_account_last_error(conn->e2e->oa));
        g_free(pickled_account);
        return -1;
    }

    /* Create a JSON string to store in our account data, we include
     * our device and server as sanity checks.
     * TODO: Should we defer this until we've sent it to the server?
     */
    JsonObject *settings_body = json_object_new();
    json_object_set_string_member(settings_body, "device_id", conn->e2e->device_id);
    json_object_set_string_member(settings_body, "server", conn->homeserver);
    json_object_set_string_member(settings_body, "pickle", pickled_account);
    g_free(pickled_account);

    JsonNode *settings_node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(settings_node, settings_body);
    json_object_unref(settings_body);

    JsonGenerator *settings_generator = json_generator_new();
    json_generator_set_root(settings_generator, settings_node);
    gchar *settings_string = json_generator_to_data(settings_generator, NULL);
    g_object_unref(G_OBJECT(settings_generator));
    json_node_free(settings_node);
    purple_account_set_string(pc->account,
                    PRPL_ACCOUNT_OPT_OLM_ACCOUNT_KEYS, settings_string);
    g_free(settings_string);

    return 0;
}

/* Retrieve an Olm account from the Purple account data
 * Returns: 1 on success
 *          0 on no stored account
 *          -1 on error
 */
static int matrix_restore_e2e_account(MatrixConnectionData *conn)
{
    PurpleConnection *pc = conn->pc;
    gchar *pickled_account = NULL;
    const char *account_string =  purple_account_get_string(pc->account,
                    PRPL_ACCOUNT_OPT_OLM_ACCOUNT_KEYS, NULL);
    int ret = -1;
    if (!account_string || !*account_string) {
        return 0;
    }
    /* Deal with existing account string */
    JsonParser *json_parser = json_parser_new();
    const gchar *retrieved_device_id, *retrieved_hs, *retrieved_pickle;
    GError *err = NULL;
    if (!json_parser_load_from_data(json_parser,
            account_string, strlen(account_string),
            &err)) {
        purple_connection_error_reason(pc,
                PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                "Failed to parse stored account key");
        purple_debug_info("matrixprpl",
                "unable to parse account JSON: %s",
                err->message);
        g_error_free(err);
        g_object_unref(json_parser);
        ret = -1;
        goto out;

    }
    JsonNode *settings_node = json_parser_get_root(json_parser);
    JsonObject *settings_body = matrix_json_node_get_object(settings_node);
    retrieved_device_id = matrix_json_object_get_string_member(settings_body, "device_id");
    retrieved_hs = matrix_json_object_get_string_member(settings_body, "server");
    retrieved_pickle = matrix_json_object_get_string_member(settings_body, "pickle");
    if (!retrieved_device_id || !retrieved_hs || !retrieved_pickle) {
        purple_connection_error_reason(pc,
                PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                "Unable to retrieve part of the stored account key");
        g_object_unref(json_parser);

        ret = -1;
        goto out;
    }
    if (strcmp(retrieved_device_id, conn->e2e->device_id) ||
        strcmp(retrieved_hs, conn->homeserver)) {
        purple_connection_error_reason(pc,
            PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                "Device ID/HS doesn't matched for stored account key");
        g_object_unref(json_parser);

        ret = -1;
        goto out;
    }
    pickled_account = g_strdup(retrieved_pickle);
    if (olm_unpickle_account(conn->e2e->oa, "!", 1, pickled_account, strlen(retrieved_pickle)) ==
        olm_error()) {
        purple_connection_error_reason(pc,
                PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                olm_account_last_error(conn->e2e->oa));
        g_object_unref(json_parser);
        ret = -1;
        goto out;
    }
    g_object_unref(json_parser);
    purple_debug_info("matrixprpl", "Succesfully unpickled account\n");
    ret = 1;

out:
    g_free(pickled_account);
    return ret;
}

#else
/* ==== Stubs for when e2e is configured out of the build === */
#endif
