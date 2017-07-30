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
#include "debug.h"

/* json-glib */
#include <json-glib/json-glib.h>

#include "connection.h"
#ifndef MATRIX_NO_E2E
#include "olm/olm.h"

struct _MatrixE2EData {
    OlmAccount *oa;
    gchar *device_id;
    gchar *curve25519_pubkey;
    gchar *ed25519_pubkey;
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

/* Returns a pointer to a freshly allocated buffer of 'n' bytes of random data.
 * If it fails it returns NULL.
 * TODO: There must be some portable function we can call to do this.
 */
static void *get_random(size_t n)
{
    FILE *urandom = fopen("/dev/urandom", "rb");
    if (!urandom) {
        return NULL;
    }
    void *buffer = g_malloc(n);
    if (fread(buffer, 1, n, urandom) != n) {
        g_free(buffer);
        buffer = NULL;
    }
    fclose(urandom);
    return buffer;
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

/* Called from sync with an object of the form:
 *           "device_one_time_keys_count" : {
 *               "signed_curve25519" : 100
 *           },
 */
void matrix_e2e_handle_sync_key_counts(PurpleConnection *pc, JsonObject *count_object,
                                       gboolean force_send)
{
    gboolean need_to_send = force_send;
    gboolean valid_counts = FALSE;
    MatrixConnectionData *conn = purple_connection_get_protocol_data(pc);
    size_t max_keys = olm_account_max_number_of_one_time_keys(conn->e2e->oa);
    size_t to_create = max_keys;

    if (!force_send) {
        JsonObjectIter iter;
        const gchar *key_algo;
        JsonNode *key_count_node;
        json_object_iter_init(&iter, count_object);
        while (json_object_iter_next(&iter, &key_algo, &key_count_node)) {
            valid_counts = TRUE;
            gint64 count = matrix_json_node_get_int(key_count_node);
            if (count < max_keys / 2) {
                to_create = (max_keys / 2) - count;
                need_to_send = TRUE;
            }
            purple_debug_info("matrixprpl", "%s: %s: %ld\n",
                           __func__, key_algo, count);
        }
    }

    need_to_send |= !valid_counts;
    if (need_to_send) {
        purple_debug_info("matrixprpl", "%s: need to send\n",__func__);
        // TODO send_one_time_keys(conn, to_create);
    }
}

static void key_upload_callback(MatrixConnectionData *conn,
                                gpointer user_data,
                                struct _JsonNode *json_root,
                                const char *body,
                                size_t body_len, const char *content_type)
{
  // TODO
}

/*
 * Get a set of device keys for ourselves.  Either by retreiving it from our store
 * or by generating a new set.
 *
 * Returns: 0 on success
 */
int matrix_e2e_get_device_keys(MatrixConnectionData *conn, const gchar *device_id)
{
    PurpleConnection *pc = conn->pc;
    JsonObject * json_dev_keys = NULL;
    OlmAccount *account = olm_account(g_malloc0(olm_account_size()));
    char *pickled_account = NULL;
    void *random_pot = NULL;
    int ret = 0;

    if (!conn->e2e) {
        conn->e2e = g_new0(MatrixE2EData,1);
        conn->e2e->device_id = g_strdup(device_id);
    }
    conn->e2e->oa = account;

    /* Try and restore olm account from settings; may fail, may work
     * or may say there were no settings stored.
     */
    ret = matrix_restore_e2e_account(conn);
    purple_debug_info("matrixprpl",
                      "restore_e2e_account says %d\n", ret);
    if (ret < 0) {
        goto out;
    }

    if (ret == 0) {
        /* No stored account - create one */
        size_t needed_random = olm_create_account_random_length(account);
        random_pot = get_random(needed_random);
        if (!random_pot) {
            purple_connection_error_reason(pc,
                    PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                    "Unable to get randomness");
            ret = -1;
            goto out;
        };

        if (olm_create_account(account, random_pot, needed_random) ==
            olm_error()) {
            purple_connection_error_reason(pc,
                    PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                    olm_account_last_error(account));
            ret = -1;
            goto out;
        }
        ret = matrix_store_e2e_account(conn);
        if (ret) {
            goto out;
        }
    }

    /* Form a device keys object for an upload,
     * from https://matrix.org/speculator/spec/drafts%2Fe2e/client_server/unstable.html#post-matrix-client-unstable-keys-upload
     */
    json_dev_keys = json_object_new();
    json_object_set_string_member(json_dev_keys, "user_id", conn->user_id);
    json_object_set_string_member(json_dev_keys, "device_id", device_id);
    /* Add 'algorithms' array - is there a way to get libolm to tell us the list of what's supported */
    /* the output of olm_account_identity_keys isn't quite right for it */
    JsonArray *algorithms = json_array_new();
    json_array_add_string_element(algorithms, "m.olm.curve25519-aes-sha256");
    json_array_add_string_element(algorithms, "m.megolm.v1.aes-sha");
    json_object_set_array_member(json_dev_keys, "algorithms", algorithms);

    /* Add 'keys' entry */
    JsonObject *json_keys = json_object_new();
    gchar **algorithm_strings, **key_strings;
    int num_algorithms = get_id_keys(pc, account, &algorithm_strings,
                                     &key_strings);
    if (num_algorithms < 1) {
        json_object_unref(json_keys);
        goto out;
    }

    int alg;

    for(alg = 0; alg < num_algorithms; alg++) {
        GString *algdev = g_string_new(NULL);
        g_string_printf(algdev, "%s:%s", algorithm_strings[alg], device_id);
        gchar *alg_dev_char = g_string_free(algdev, FALSE);
        json_object_set_string_member(json_keys, alg_dev_char,
                                      key_strings[alg]);

        if (!strcmp(algorithm_strings[alg], "curve25519")) {
            conn->e2e->curve25519_pubkey = key_strings[alg];
        } else if (!strcmp(algorithm_strings[alg], "ed25519")) {
            conn->e2e->ed25519_pubkey = key_strings[alg];
        } else {
            g_free(key_strings[alg]);
        }
        g_free(algorithm_strings[alg]);
        g_free(alg_dev_char);
    }
    g_free(algorithm_strings);
    g_free(key_strings);
    json_object_set_object_member(json_dev_keys, "keys", json_keys);

    /* Sign */
    if (matrix_sign_json(conn, json_dev_keys)) {
        goto out;
    }

    /* Send the keys */
    matrix_api_upload_keys(conn, json_dev_keys, NULL /* TODO: one time keys */,
        key_upload_callback,
        matrix_api_error, matrix_api_bad_response, (void *)0);
    json_dev_keys = NULL; /* api_upload_keys frees it with it's whole json */

    ret = 0;

out:
    if (json_dev_keys)
         json_object_unref(json_dev_keys);
    g_free(pickled_account);
    g_free(random_pot);

    if (ret) {
        matrix_e2e_cleanup_connection(conn);
    }
    return ret;
}

void matrix_e2e_cleanup_connection(MatrixConnectionData *conn)
{
    if (conn->e2e) {
        g_free(conn->e2e->curve25519_pubkey);
        g_free(conn->e2e->oa);
        g_free(conn->e2e->device_id);
        g_free(conn->e2e);
        conn->e2e = NULL;
    }
}

#else
/* ==== Stubs for when e2e is configured out of the build === */
int matrix_e2e_get_device_keys(MatrixConnectionData *conn, const gchar *device_id)
{
    return -1;
}

void matrix_e2e_cleanup_connection(MatrixConnectionData *conn)
{
}

void matrix_e2e_handle_sync_key_counts(PurpleConnection *pc, JsonObject *count_object,
                                       gboolean force_send)
{
}

#endif
