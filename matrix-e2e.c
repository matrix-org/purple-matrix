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
#include "libmatrix.h"
#include "matrix-e2e.h"

#include "connection.h"
#include "olm/olm.h"

struct _MatrixE2EData {
    OlmAccount *oa;
    gchar *device_id;
};

/* Returns the list of algorithms and our keys for those algorithms on the current account */
static int get_id_keys(PurpleConnection *pc, OlmAccount *account, gchar ***algorithms, gchar ***keys)
{
    /* There has to be an easier way than this.... */
    size_t id_key_len = olm_account_identity_keys_length(account);
    gchar *id_keys = g_malloc0(id_key_len+1);
    if (olm_account_identity_keys(account, id_keys, id_key_len) ==
        olm_error()) {
        purple_connection_error_reason(pc,
                PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                olm_account_last_error(account));
        g_free(id_keys);
        return -1;
    }

    /* We get back a json string, something like:
     * {"curve25519":"encodedkey...","ed25519":"encodedkey...."}'
     */
    JsonParser *json_parser = json_parser_new();
    GError *err = NULL;
    if (!json_parser_load_from_data(json_parser,
            id_keys, strlen(id_keys), &err)) {
        purple_connection_error_reason(pc,
                PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                "Failed to parse olm account ID keys");
        purple_debug_info("matrixprpl",
                "unable to parse olm account ID keys: %s",
                err->message);
        g_error_free(err);
        g_free(id_keys);
        g_object_unref(json_parser);
        return -1;
    }
    /* We have one object with a series of string members where
     * each member is named after the algorithm.
     */
    JsonNode *id_node = json_parser_get_root(json_parser);
    JsonObject *id_body = matrix_json_node_get_object(id_node);
    guint n_keys = json_object_get_size(id_body);
    *algorithms = g_new(gchar *, n_keys);
    *keys = g_new(gchar *, n_keys);
    JsonObjectIter iter;
    const gchar *key_algo;
    JsonNode *key_node;
    guint i = 0;
    json_object_iter_init(&iter, id_body);
    while (json_object_iter_next(&iter, &key_algo, &key_node)) {
        (*algorithms)[i] = g_strdup(key_algo);
        (*keys)[i] = g_strdup(matrix_json_object_get_string_member(id_body, key_algo));
        i++;
    }

    g_free(id_keys);
    g_object_unref(json_parser);

    return n_keys;
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
    GString *can_json = matrix_canonical_json(tosign, NULL);
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

