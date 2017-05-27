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
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <sqlite3.h>
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

struct _MatrixOlmSession;

struct _MatrixE2EData {
    OlmAccount *oa;
    gchar *device_id;
    gchar *curve25519_pubkey;
    gchar *ed25519_pubkey;
    sqlite3 *db;
    /* Mapping from MatrixHashKeyOlm to MatrixOlmSession */
    GHashTable *olm_session_hash;
};

#define PURPLE_CONV_E2E_STATE "e2e"

/* Hung off the Purple conversation with the PURPLE_CONV_E2E_STATE */
typedef struct _MatrixE2ERoomData {
    /* Mapping from _MatrixHashKeyInBoundMegOlm to OlmInboundGroupSession */
    GHashTable *megolm_sessions_inbound;
} MatrixE2ERoomData;

typedef struct _MatrixHashKeyOlm {
    gchar *sender_key;
    gchar *sender_id;
} MatrixHashKeyOlm;

typedef struct _MatrixOlmSession {
    gchar *sender_key;
    gchar *sender_id;
    OlmSession *session;
    sqlite3_int64 unique;
    struct _MatrixOlmSession *next;
} MatrixOlmSession;

typedef struct _MatrixHashKeyInBoundMegOlm {
    gchar *sender_key;
    gchar *sender_id;
    gchar *session_id;
    gchar *device_id;
} MatrixHashKeyInBoundMegOlm;

/* GHashFunc for two MatrixHasKeyOlm */
static gboolean olm_inbound_equality(gconstpointer a, gconstpointer b)
{
    const MatrixHashKeyOlm *hk_a;
    const MatrixHashKeyOlm *hk_b;
    hk_a = (const MatrixHashKeyOlm *)a;
    hk_b = (const MatrixHashKeyOlm *)b;

    return !strcmp(hk_a->sender_key, hk_b->sender_key) &&
           !strcmp(hk_a->sender_id, hk_b->sender_id);
}

/* GHashFunc for a _MatrixHashKeyOlm */
static guint olm_inbound_hash(gconstpointer a)
{
    const MatrixHashKeyOlm *hk;
    hk = (const MatrixHashKeyOlm *)a;
    return g_str_hash(hk->sender_key) +
           g_str_hash(hk->sender_id);
}

static void olm_hash_key_destroy(gpointer k)
{
    MatrixHashKeyOlm *ok = k;

    g_free(ok->sender_key);
    g_free(ok->sender_id);
    g_free(ok);
}

static void free_matrix_olm_session(MatrixOlmSession *msession,
                                    gboolean free_session)
{
    g_free(msession->sender_id);
    g_free(msession->sender_key);
    if (free_session) {
        olm_clear_session(msession->session);
        g_free(msession->session);
    }
}

static void olm_hash_value_destroy(gpointer v)
{
    MatrixOlmSession *os = v;

    while (os) {
        MatrixOlmSession *next = os->next;
        free_matrix_olm_session(os, TRUE);
        os = next;
    }
}

/* GEqualFunc for two MatrixHashKeyInBoundMegOlm */
static gboolean megolm_inbound_equality(gconstpointer a, gconstpointer b)
{
    const MatrixHashKeyInBoundMegOlm *hk_a;
    const MatrixHashKeyInBoundMegOlm *hk_b;
    hk_a = (const MatrixHashKeyInBoundMegOlm *)a;
    hk_b = (const MatrixHashKeyInBoundMegOlm *)b;

    return !strcmp(hk_a->sender_key, hk_b->sender_key) &&
           !strcmp(hk_a->sender_id, hk_b->sender_id) &&
           !strcmp(hk_a->session_id, hk_b->session_id) &&
           !strcmp(hk_a->device_id, hk_b->device_id);
}

/* GHashFunc for a _MatrixHashKeyInBoundMegOlm */
static guint megolm_inbound_hash(gconstpointer a)
{
    const MatrixHashKeyInBoundMegOlm *hk;
    hk = (const MatrixHashKeyInBoundMegOlm *)a;

    return g_str_hash(hk->sender_key) +
           g_str_hash(hk->session_id) +
           g_str_hash(hk->sender_id) +
           g_str_hash(hk->device_id);
}

static void megolm_inbound_key_destroy(gpointer v)
{
    MatrixHashKeyInBoundMegOlm *key = v;
    g_free(key->sender_key);
    g_free(key->session_id);
    g_free(key->sender_id);
    g_free(key->device_id);
}

static void megolm_inbound_value_destroy(gpointer v)
{
    OlmInboundGroupSession *oigs = v;

    olm_clear_inbound_group_session(oigs);
    g_free(oigs);
}

static MatrixE2ERoomData *get_e2e_room_data(PurpleConversation *conv)
{
    MatrixE2ERoomData *result;

    result = purple_conversation_get_data(conv, PURPLE_CONV_E2E_STATE);
    if (!result) {
        result = g_new0(MatrixE2ERoomData, 1);
        purple_conversation_set_data(conv, PURPLE_CONV_E2E_STATE, result);
    }

    return result;
}

static GHashTable *get_e2e_inbound_megolm_hash(PurpleConversation *conv)
{
    MatrixE2ERoomData *rd = get_e2e_room_data(conv);

    if (!rd->megolm_sessions_inbound) {
        rd->megolm_sessions_inbound = g_hash_table_new_full(megolm_inbound_hash,
                                                   megolm_inbound_equality,
                                                   megolm_inbound_key_destroy,
                                                   megolm_inbound_value_destroy);
    }

    return rd->megolm_sessions_inbound;
}

static OlmInboundGroupSession *get_inbound_megolm_session(
       PurpleConversation *conv,
        const gchar *sender_key, const gchar *sender_id,
        const gchar *session_id, const gchar *device_id)
{
    MatrixHashKeyInBoundMegOlm match;
    match.sender_key = (gchar *)sender_key;
    match.sender_id = (gchar *)sender_id;
    match.session_id = (gchar *)session_id;
    match.device_id = (gchar *)device_id;

    OlmInboundGroupSession *result =
       (OlmInboundGroupSession *)g_hash_table_lookup(
               get_e2e_inbound_megolm_hash(conv), &match);
    purple_debug_info("matrixprpl",  "%s: %s/%s/%s/%s: %p\n",
                      __func__, device_id, sender_id, sender_key, session_id,
                      result);
    return result;
}

static void store_inbound_megolm_session(PurpleConversation *conv,
        const gchar *sender_key, const gchar *sender_id,
        const gchar *session_id, const gchar *device_id,
        OlmInboundGroupSession *igs) {
    MatrixHashKeyInBoundMegOlm *key = g_new0(MatrixHashKeyInBoundMegOlm, 1);
    key->sender_key = g_strdup(sender_key);
    key->sender_id = g_strdup(sender_id);
    key->session_id = g_strdup(session_id);
    key->device_id = g_strdup(device_id);
    purple_debug_info("matrixprpl", "%s: %s/%s/%s/%s\n",
               __func__, device_id, sender_id, sender_key, session_id);
    g_hash_table_insert(get_e2e_inbound_megolm_hash(conv), key, igs);
}

/* Find if we already have an OlmSession for this sender/sender_key somewhere
 * that this body matches.
 */
static MatrixOlmSession *find_olm_session(MatrixConnectionData *conn,
                                    const char *sender_id, const char *sender_key,
                                    const char *body)
{
    gboolean have_sender = FALSE;
    MatrixOlmSession *cur_entry, *list_head, *hash_result;
    MatrixOlmSession *result = NULL;
    MatrixHashKeyOlm match;

    purple_debug_info("matrixprpl", "find_olm_session for %s/%s\n",
                       sender_id, sender_key);
    match.sender_key = (gchar *)sender_key;
    match.sender_id = (gchar *)sender_id;
    hash_result = (MatrixOlmSession *)g_hash_table_lookup(
                       conn->e2e->olm_session_hash, &match);
    cur_entry = hash_result;
    list_head = hash_result;

    while (cur_entry) {
        if (!strcmp(sender_id, cur_entry->sender_id) &&
            !strcmp(sender_key, cur_entry->sender_key)) {
            size_t ret;
            char *body_double = g_strdup(body);
            have_sender = TRUE;
            ret = olm_matches_inbound_session(cur_entry->session, body_double,
                                              strlen(body));
            g_free(body_double);
            if (ret == 1) {
                purple_debug_info("matrixprpl",
                                  "%s: Found matching session for %s/%s\n",
                                  __func__, sender_id, sender_key);
                return cur_entry;
            }
            if (ret == olm_error()) {
                purple_debug_warning("matrixprpl",
                        "%s: Error while checking session %p for "
                        "match with %s/%s: %s\n", __func__, cur_entry->session,
                        sender_id, sender_key,
                        olm_session_last_error(cur_entry->session));
            }
        }
        cur_entry = cur_entry->next;
    }

    if (!have_sender) {
        MatrixOlmSession **chain = &list_head;
        int ret;
        /* We have no entries at all for the sender+key, lets load all the
         * data from the db
         */
        const char *query = "SELECT session_pickle, rowid "
                            "FROM olmsessions "
                            "WHERE sender_name = ? AND "
                            "sender_key = ?";

        sqlite3_stmt *dbstmt = NULL;
        ret = sqlite3_prepare_v2(conn->e2e->db, query, -1, &dbstmt, NULL);
        if (ret != SQLITE_OK || !dbstmt) {
             purple_debug_warning("matrixprpl",
                       "%s: Failed to prep select %d '%s'\n",
                       __func__, ret, query);
            goto bad_sql;
        }
        ret = sqlite3_bind_text(dbstmt, 1, sender_id, -1, NULL);
        if (ret == SQLITE_OK) {
            ret = sqlite3_bind_text(dbstmt, 2, sender_key, -1, NULL);
        }
        if (ret != SQLITE_OK) {
            purple_debug_warning("matrixprpl", "%s: Failed to bind %d\n",
                               __func__, ret);
            goto bad_sql;
        }

        /* Find the end of the chain to get the pointer to update */
        for (cur_entry = list_head; cur_entry; cur_entry=cur_entry->next) {
            chain = &(cur_entry->next);
        }

        while (ret = sqlite3_step(dbstmt), ret == SQLITE_ROW) {
            const gchar *pickle = (gchar *)sqlite3_column_text(dbstmt, 0);
            gchar *dupe_pickle;
            if (!pickle) {
                purple_debug_warning("matrixprpl",
                        "%s: Empty pickle for %s/%s\n", __func__,
                        sender_id, sender_key);
                continue;
            };
            dupe_pickle = g_strdup(pickle);
            OlmSession *session = olm_session(g_malloc0(olm_session_size()));
            if (olm_unpickle_session(session, "!", 1, dupe_pickle,
                                     strlen(dupe_pickle)) == olm_error()) {
                purple_debug_warning("matrixprpl",
                                     "%s: Failed to unpickle %s for %s/%s\n",
                                     __func__, pickle, sender_id, sender_key);
                g_free(dupe_pickle);
                g_free(session);
                continue;
            }
            g_free(dupe_pickle);
            cur_entry = g_new0(MatrixOlmSession, 1);
            cur_entry->sender_id = g_strdup(sender_id);
            cur_entry->sender_key = g_strdup(sender_key);
            cur_entry->session = session;
            cur_entry->unique = sqlite3_column_int64(dbstmt, 1);
            *chain = cur_entry;

            if (!result) {
                char *body_double = g_strdup(body);
                /* But is this the session we're after ? */
                ret = olm_matches_inbound_session(session,
                                                  body_double, strlen(body));
                g_free(body_double);
                if (ret == 1) {
                    purple_debug_info("matrixprpl",
                               "%s: Found (loaded) session for %s/%s\n",
                               __func__, sender_id, sender_key);
                    result = cur_entry;
                    /* Carry on loading any other sessions */
                }
                if (ret == olm_error()) {
                    purple_debug_warning("matrixprpl",
                        "%s: Error while checking loaded session %p for "
                        "match with %s/%s: %s\n", __func__, session,
                        sender_id, sender_key,
                        olm_session_last_error(session));
                }
                if (!ret) {
                    purple_debug_warning("matrixprpl",
                            "%s: Loaded session (%" PRIx64
                            ") is not a match for %s/%s\n",
                            __func__, (int64_t)cur_entry->unique, sender_id,
                            sender_key);
                }
            }
        }
        if (ret != SQLITE_DONE) {
            purple_debug_warning("matrixprpl", "%s: db step failed %d\n",
                                 __func__, ret);
            goto bad_sql;
        }
bad_sql:
        sqlite3_finalize(dbstmt);
    }
    if (list_head && !hash_result) {
        MatrixHashKeyOlm *key = g_new0(MatrixHashKeyOlm, 1);
        key->sender_key = g_strdup(sender_key);
        key->sender_id = g_strdup(sender_id);
        /* We loaded entries where there were none before, set the hash */
        g_hash_table_insert(conn->e2e->olm_session_hash, key, list_head);
    }
    return result;
}

/* Save a new olm session into the database */
static MatrixOlmSession *store_olm_session(MatrixConnectionData *conn,
                                           OlmSession *session,
                                           const char *sender_id,
                                           const char *sender_key)
{
    MatrixOlmSession *cur_entry = g_new0(MatrixOlmSession, 1);
    size_t pickle_len = olm_pickle_session_length(session);
    gchar *pickle = g_malloc(pickle_len+1);
    sqlite3_stmt *dbstmt = NULL;

    pickle_len = olm_pickle_session(session, "!", 1, pickle, pickle_len);
    if (pickle_len == olm_error()) {
        purple_debug_warning("matrixprpl",
                             "%s: Failed to pickle session for %s/%s: %s\n",
                             __func__, sender_id, sender_key,
                             olm_session_last_error(session));
        goto err;
    }
    pickle[pickle_len] = '\0';

    cur_entry->sender_id = g_strdup(sender_id);
    cur_entry->sender_key = g_strdup(sender_key);
    cur_entry->session = session;

    const char *query = "INSERT into olmsessions "
                        "(sender_name, sender_key, session_pickle) "
                        "VALUES (?, ?, ?)";

    int ret = sqlite3_prepare_v2(conn->e2e->db, query, -1, &dbstmt, NULL);
    if (ret != SQLITE_OK || !dbstmt) {
        purple_debug_warning("matrixprpl",
                             "%s: Failed to prep insert %d '%s'\n",
                             __func__, ret, query);
        goto err;
    }
    ret = sqlite3_bind_text(dbstmt, 1, sender_id, -1, NULL);
    if (ret == SQLITE_OK) {
        ret = sqlite3_bind_text(dbstmt, 2, sender_key, -1, NULL);
    }
    if (ret == SQLITE_OK) {
        ret = sqlite3_bind_text(dbstmt, 3, pickle, -1, NULL);
    }
    if (ret != SQLITE_OK) {
        purple_debug_warning("matrixprpl",
                             "%s: Failed to bind %d\n", __func__, ret);
        goto err;
    }

    ret = sqlite3_step(dbstmt);
    if (ret != SQLITE_DONE) {
        purple_debug_warning("matrixprpl",
                             "%s: Insert failed %d (%s)\n", __func__,
                             ret, query);
        goto err;
    }
    sqlite3_finalize(dbstmt);
    cur_entry->unique = sqlite3_last_insert_rowid(conn->e2e->db);

    MatrixHashKeyOlm match;
    match.sender_key = (gchar *)sender_key;
    match.sender_id = (gchar *)sender_id;
    MatrixOlmSession *hash_result = (MatrixOlmSession *)g_hash_table_lookup(
                       conn->e2e->olm_session_hash, &match);
    if (hash_result) {
        /* If there's already an entry, insert it after the head */
        cur_entry->next = hash_result->next;
        hash_result->next = cur_entry;
    } else {
        /* No entry, we need to stuff it into the hash table */
        MatrixHashKeyOlm *key = g_new0(MatrixHashKeyOlm, 1);
        key->sender_key = g_strdup(sender_key);
        key->sender_id = g_strdup(sender_id);
        /* We loaded entries where there were none before, set the hash */
        g_hash_table_insert(conn->e2e->olm_session_hash, key, cur_entry);
    }

    return cur_entry;

err:
    g_free(pickle);
    free_matrix_olm_session(cur_entry, FALSE);
    return NULL;
}

/* Update an existing OLM session in the database */
static int update_olm_session(MatrixConnectionData *conn,
                              MatrixOlmSession *mos)
{
    size_t pickle_len = olm_pickle_session_length(mos->session);
    gchar *pickle = g_malloc(pickle_len+1);
    sqlite3_stmt *dbstmt = NULL;
    int ret = -1;

    pickle_len = olm_pickle_session(mos->session, "!", 1, pickle, pickle_len);
    if (pickle_len == olm_error()) {
        purple_debug_warning("matrixprpl",
                "%s: Failed to pickle session for %s/%s: %s\n",
                __func__, mos->sender_id, mos->sender_key,
                olm_session_last_error(mos->session));
        goto err;
    }
    pickle[pickle_len] = '\0';

    const char *query ="UPDATE olmsessions SET session_pickle=? "
                       "WHERE sender_name = ? AND sender_key = ? AND "
                       "ROWID = ?";
    ret = sqlite3_prepare_v2(conn->e2e->db, query, -1, &dbstmt, NULL);
    if (ret != SQLITE_OK || !dbstmt) {
        purple_debug_warning("matrixprpl",
                "%s: Failed to prep update %d '%s'\n",
                __func__, ret, query);
        ret = -1;
        goto err;
    }
    ret = sqlite3_bind_text(dbstmt, 1, pickle, -1, NULL);
    if (ret == SQLITE_OK) {
        ret = sqlite3_bind_text(dbstmt, 2, mos->sender_id, -1, NULL);
    }
    if (ret == SQLITE_OK) {
        ret = sqlite3_bind_text(dbstmt, 3, mos->sender_key, -1, NULL);
    }
    if (ret == SQLITE_OK) {
        ret = sqlite3_bind_int64(dbstmt, 4, mos->unique);
    }

    if (ret != SQLITE_OK) {
        purple_debug_warning("matrixprpl",
                             "%s: Failed to bind %d\n", __func__, ret);
        ret = -1;
        goto err;
    }

    ret = sqlite3_step(dbstmt);
    if (ret != SQLITE_DONE) {
        purple_debug_warning("matrixprpl",
                             "%s: Update failed %d (%s)\n", __func__,
                ret, query);
        ret = -1;
        goto err;

    }
    ret = 0;

err:
    sqlite3_finalize(dbstmt);
    g_free(pickle);

    return ret;
}

static void key_upload_callback(MatrixConnectionData *conn,
                                gpointer user_data,
                                struct _JsonNode *json_root,
                                const char *body,
                                size_t body_len, const char *content_type);

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

/* See: https://matrix.org/docs/guides/e2e_implementation.html#creating-and-registering-one-time-keys */
static int send_one_time_keys(MatrixConnectionData *conn, size_t n_keys)
{
    PurpleConnection *pc = conn->pc;
    int ret;
    size_t random_needed;
    void *random_buffer;
    void *olm_1t_keys_json = NULL;
    JsonParser *json_parser = NULL;
    size_t olm_keys_buffer_size;
    JsonObject *otk_json = NULL;
    random_needed = olm_account_generate_one_time_keys_random_length(
                       conn->e2e->oa, n_keys);
    random_buffer = get_random(random_needed);
    if (!random_buffer) {
        return -1;
    }

    if (olm_account_generate_one_time_keys(conn->e2e->oa, n_keys, random_buffer,
                                               random_needed) == olm_error()) {
        purple_connection_error_reason(pc,
                PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                olm_account_last_error(conn->e2e->oa));
        ret = -1;
        goto out;
    }

    olm_keys_buffer_size = olm_account_one_time_keys_length(conn->e2e->oa);
    olm_1t_keys_json = g_malloc0(olm_keys_buffer_size+1);
    if (olm_account_one_time_keys(conn->e2e->oa, olm_1t_keys_json,
                                       olm_keys_buffer_size) == olm_error()) {
        purple_connection_error_reason(pc,
                PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                olm_account_last_error(conn->e2e->oa));
        ret = -1;
        goto out;
    }

    /* olm_1t_keys_json has json like:
     *   {
     *     curve25519: {
     *       "keyid1": "base64encodedcurve25519key1",
     *       "keyid2": "base64encodedcurve25519key2"
     *     }
     *   }
     *   I think in practice this is just curve25519 but I'll avoid hard coding
     *   We need to produce an object with a set of signed objects each having
     *   one key
     */
    json_parser = json_parser_new();
    GError *err = NULL;
    if (!json_parser_load_from_data(json_parser,
          olm_1t_keys_json, strlen(olm_1t_keys_json), &err)) {
        purple_connection_error_reason(pc,
            PURPLE_CONNECTION_ERROR_OTHER_ERROR,
            "Failed to parse generated 1-time json");
        g_error_free(err);
        ret = -1;
        goto out;
    }

    /* The output JSON we're generating */
    otk_json = json_object_new();

    JsonNode *olm_1tk_root = json_parser_get_root(json_parser);
    JsonObject *olm_1tk_obj = matrix_json_node_get_object(olm_1tk_root);
    JsonObjectIter algo_iter;
    json_object_iter_init(&algo_iter, olm_1tk_obj);
    const gchar *keys_algo;
    JsonNode *keys_node;
    while (json_object_iter_next(&algo_iter, &keys_algo, &keys_node)) {
        /* We're expecting keys_algo to be "curve25519" and keys_node to be an
         * object with a set of keys.
         */
        JsonObjectIter keys_iter;
        JsonObject *keys_obj = matrix_json_node_get_object(keys_node);
        json_object_iter_init(&keys_iter, keys_obj);
        const gchar *key_id;
        JsonNode *key_node;
        while (json_object_iter_next(&keys_iter, &key_id, &key_node)) {
            const gchar *key_string = matrix_json_node_get_string(key_node);

            JsonObject *signed_key = json_object_new();
            json_object_set_string_member(signed_key, "key", key_string);
            ret = matrix_sign_json(conn, signed_key);
            if (ret) {
                g_object_unref(signed_key);
                goto out;
            }
            gchar *signed_key_name = g_strdup_printf("signed_%s:%s", keys_algo,
                                                       key_id);
            json_object_set_object_member(otk_json,
                                               signed_key_name, signed_key);
            g_free(signed_key_name);
        }
    }

    matrix_api_upload_keys(conn, NULL, otk_json,
        key_upload_callback,
        matrix_api_error, matrix_api_bad_response, (void *)1);
    otk_json = NULL; /* matrix_api_upload_keys frees with its json */
    ret = 0;
out:
    g_object_unref(json_parser);
    if (otk_json)
        g_object_unref(otk_json);
    g_free(random_buffer);
    g_free(olm_1t_keys_json);

    return ret;
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
        send_one_time_keys(conn, to_create);
    }
}

/* Called back when we've successfully uploaded the device keys
 * we use 'user_data' = 1 to indicate we did an upload of one time
 * keys.
 */
static void key_upload_callback(MatrixConnectionData *conn,
                                gpointer user_data,
                                struct _JsonNode *json_root,
                                const char *body,
                                size_t body_len, const char *content_type)
{
    /* The server responds with a count of the one time keys on the server */
    JsonObject *top_object = matrix_json_node_get_object(json_root);
    JsonObject *key_counts = matrix_json_object_get_object_member(top_object,
                                       "one_time_key_counts");

    purple_debug_info("matrixprpl",
                      "%s: json_root=%p top_object=%p key_counts=%p\n",
                      __func__, json_root, top_object, key_counts);
    /* True if it's a response to a key upload */
    if (user_data) {
        /* Tell Olm that these one time keys are uploaded */
        olm_account_mark_keys_as_published(conn->e2e->oa);
        matrix_store_e2e_account(conn);
    }

    matrix_e2e_handle_sync_key_counts(conn->pc, key_counts, !key_counts);
}

static void close_e2e_db(MatrixConnectionData *conn)
{
    sqlite3_close(conn->e2e->db);
    conn->e2e->db = NULL;
}

/* 'check' and 'create' are SQL statements; call check, if it returns no result
 * then run 'create'.
 * typically for checking for the existence of a table and creating it if it didn't
 * exist.
 */
static int ensure_table(MatrixConnectionData *conn, const char *check, const char *create)
{
    PurpleConnection *pc = conn->pc;
    int ret;
    sqlite3_stmt *dbstmt;
    ret = sqlite3_prepare_v2(conn->e2e->db, check, -1, &dbstmt, NULL);
    if (ret != SQLITE_OK || !dbstmt) {
        purple_connection_error_reason(pc,
            PURPLE_CONNECTION_ERROR_OTHER_ERROR,
            "Failed to check e2e db table list (prep)");
        return -1;
    }
    ret = sqlite3_step(dbstmt);
    sqlite3_finalize(dbstmt);
    purple_debug_info("matrixprpl", "%s:db table query %d\n", __func__, ret);
    if (ret == SQLITE_ROW) {
        /* Already exists */
        return 0;
    }
    ret = sqlite3_prepare_v2(conn->e2e->db, create, -1, &dbstmt, NULL);
    if (ret != SQLITE_OK || !dbstmt) {
        purple_connection_error_reason(pc,
            PURPLE_CONNECTION_ERROR_OTHER_ERROR,
            "Failed to create e2e db table (prep)");
        return -1;
    }
    ret = sqlite3_step(dbstmt);
    sqlite3_finalize(dbstmt);
    if (ret != SQLITE_DONE) {
        purple_connection_error_reason(pc,
            PURPLE_CONNECTION_ERROR_OTHER_ERROR,
            "Failed to create e2e db table (step)");
        return -1;
    }

    return 0;
}
static int open_e2e_db(MatrixConnectionData *conn)
{
    PurpleConnection *pc = conn->pc;
    int ret;
    const char *purple_username = 
               purple_account_get_username(purple_connection_get_account(pc));
    char *cfilename = g_strdup_printf("matrix-%s-%s.db", conn->user_id,
                                       purple_username);
    const char *escaped_filename = purple_escape_filename(cfilename);
    g_free(cfilename);
    char *full_path = g_strdup_printf("%s/%s", purple_user_dir(),
                                               escaped_filename);
    ret = sqlite3_open(full_path, &conn->e2e->db);
    purple_debug_info("matrixprpl", "Opened e2e db at %s %d\n", full_path, ret);
    g_free(full_path);
    if (ret) {
        purple_connection_error_reason(pc,
            PURPLE_CONNECTION_ERROR_OTHER_ERROR,
            "Failed to open e2e db");
        return ret;
    }

    ret = ensure_table(conn,
                 "SELECT name FROM sqlite_master WHERE type='table' AND name='olmsessions'",
                 "CREATE TABLE olmsessions (sender_name text, sender_key text,"
                 "                          session_pickle text,"
                 "                          PRIMARY KEY (sender_name, sender_key))");

    if (ret) {
        close_e2e_db(conn);
        return ret;
    }

    return 0;
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
        conn->e2e->olm_session_hash = g_hash_table_new_full(olm_inbound_hash,
                                                       olm_inbound_equality,
                                                       olm_hash_key_destroy,
                                                       olm_hash_value_destroy);
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

    /* Open the e2e db - an sqlite db held for the account */
    ret = open_e2e_db(conn);
    if (ret) {
        goto out;
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

void matrix_e2e_cleanup_conversation(PurpleConversation *conv)
{
    MatrixE2ERoomData *result = purple_conversation_get_data(conv,
                                                     PURPLE_CONV_E2E_STATE);
    if (result) {
        g_hash_table_destroy(result->megolm_sessions_inbound);
        g_free(result);
        purple_conversation_set_data(conv, PURPLE_CONV_E2E_STATE, NULL);
    }
}

void matrix_e2e_cleanup_connection(MatrixConnectionData *conn)
{
    GList *ptr;
    for(ptr = purple_get_conversations(); ptr != NULL; ptr = g_list_next(ptr))
    {
        PurpleConversation *conv = ptr->data;
        matrix_e2e_cleanup_conversation(conv);
    }
    if (conn->e2e) {
        close_e2e_db(conn);
        g_hash_table_destroy(conn->e2e->olm_session_hash);
        g_free(conn->e2e->curve25519_pubkey);
        g_free(conn->e2e->oa);
        g_free(conn->e2e->device_id);
        g_free(conn->e2e);
        conn->e2e = NULL;
    }
}

/* See: https://matrix.org/docs/guides/e2e_implementation.html#handling-an-m-room-key-event
 */
static int handle_m_room_key(PurpleConnection *pc, MatrixConnectionData *conn,
                             const gchar *sender, const gchar *sender_key, const gchar *sender_device,
                             JsonObject *mrk)
{
    int ret = 0;
    purple_debug_info("matrixprpl", "%s\n", __func__);
    JsonObject *mrk_content;
    const gchar *mrk_algo;
    PurpleConversation *conv;
    mrk_content = matrix_json_object_get_object_member(mrk, "content");
    mrk_algo = matrix_json_object_get_string_member(mrk_content, "algorithm");
    OlmInboundGroupSession *in_mo_session = NULL;

    if (!mrk_algo || strcmp(mrk_algo, "m.megolm.v1.aes-sha2")) {
        purple_debug_info("matrixprpl", "%s: Not megolm (%s)\n",
                               __func__, mrk_algo);
        ret = -1;
        goto out;
    }

    const gchar *mrk_room_id, *mrk_session_id, *mrk_session_key;
    mrk_room_id = matrix_json_object_get_string_member(mrk_content, "room_id");
    mrk_session_id = matrix_json_object_get_string_member(mrk_content,
                                                               "session_id");
    mrk_session_key = matrix_json_object_get_string_member(mrk_content,
                                                               "session_key");

    conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT,
                                                 mrk_room_id, pc->account);
    if (!conv) {
        purple_debug_info("matrixprpl", "%s: Unknown room %s\n",
                          __func__, mrk_room_id);
        ret = -1;
        goto out;
    }

    /* Search for an existing session with the matching room_id,
     * sender_key, session_id */
    in_mo_session = get_inbound_megolm_session(conv, sender_key,
                                               sender, mrk_session_id,
                                               sender_device);
      
    if (!in_mo_session) {
        /* Bah, no match, lets make one */
        in_mo_session =  olm_inbound_group_session(g_malloc(
                               olm_inbound_group_session_size()));
        if (olm_init_inbound_group_session(in_mo_session,
                                       (const uint8_t *)mrk_session_key,
                                       strlen(mrk_session_key)) ==
                olm_error()) {
            purple_debug_info("matrixprpl",
                       "%s: megolm inbound session creation failed: %s\n",
                       __func__,
                       olm_inbound_group_session_last_error(in_mo_session));
            ret = -1;
            goto out;
        }

        store_inbound_megolm_session(conv, sender_key, sender,
                                     mrk_session_id, sender_device,
                                     in_mo_session);
    }

out:
    if (ret) {
        if (in_mo_session) {
            olm_clear_inbound_group_session(in_mo_session);
        }
        g_free(in_mo_session);
    }
    return ret;
}

/* Called from decypt_olm after we've decrypted an olm message.
 */
static int handle_decrypted_olm(PurpleConnection *pc,
                                MatrixConnectionData *conn,
                                const gchar *sender,
                                const gchar *sender_key, gchar *plaintext)
{
    JsonParser *json_parser = json_parser_new();
    GError *err = NULL;
    int ret = 0;

    purple_debug_info("matrixprpl", "%s: %s\n", __func__, plaintext);
    if (!json_parser_load_from_data(json_parser, plaintext, strlen(plaintext),
                                       &err)) {
        purple_connection_error_reason(pc,
                PURPLE_CONNECTION_ERROR_OTHER_ERROR,
                "Failed to parse decrypted olm JSON");
        purple_debug_info("matrixprpl",
                "unable to parse decrypted olm JSON: %s",
                err->message);
        g_error_free(err);
        ret = -1;
        goto out;

    }
    JsonNode *pt_node = json_parser_get_root(json_parser);
    JsonObject *pt_body = matrix_json_node_get_object(pt_node);

    /* The spec says we need to check these actually match */
    const gchar *pt_sender, *pt_sender_device, *pt_recipient, *pt_recipient_ed;
    const gchar *pt_type;
    pt_sender = matrix_json_object_get_string_member(pt_body, "sender");
    pt_sender_device = matrix_json_object_get_string_member(pt_body,
                                                            "sender_device");
    pt_recipient = matrix_json_object_get_string_member(pt_body, "recipient");
    JsonObject *pt_recipient_keys =
               matrix_json_object_get_object_member(pt_body, "recipient_keys");
    pt_recipient_ed = matrix_json_object_get_string_member(pt_recipient_keys,
                                                           "ed25519");
    pt_type = matrix_json_object_get_string_member(pt_body, "type");

    if (!pt_sender || !pt_sender_device || !pt_recipient ||
        !pt_recipient_ed || !pt_type) {
        purple_debug_info("matrixprpl",
                          "%s: Missing field\n", __func__);
        ret = -1;
        goto out;
    }
    if (strcmp(sender, pt_sender)) {
        purple_debug_info("matrixprpl",
           "%s: Mismatch on sender '%s' vs '%s'\n",
           __func__, sender, pt_sender);
        ret = -1;
        goto out;
    }
    if (strcmp(conn->user_id, pt_recipient)) {
        purple_debug_info("matrixprpl",
                          "%s: Mismatch on recipient '%s' vs '%s'\n",
                          __func__, conn->user_id, pt_recipient);
        ret = -1;
        goto out;
    }
    if (strcmp(conn->e2e->ed25519_pubkey, pt_recipient_ed)) {
        purple_debug_info("matrixprpl",
           "%s: Mismatch on recipient key '%s' vs '%s' pt_recipient_keys=%p\n",
           __func__, conn->e2e->ed25519_pubkey, pt_recipient_ed,
           pt_recipient_keys);
        ret = -1;
        goto out;
    }

    /* TODO: check the device against the keys in use, stash somewhere? */
    if (!strcmp(pt_type, "m.room_key")) {
        ret = handle_m_room_key(pc, conn, pt_sender, sender_key,
                                pt_sender_device, pt_body);
    } else {
        purple_debug_info("matrixprpl",
                          "%s: Got '%s' from '%s'/'%s'\n",
                          __func__, pt_type, pt_sender_device, pt_sender);
    }
out:
    g_object_unref(json_parser);
    return ret;
}

/*
 * See:
 * https://matrix.org/docs/guides/e2e_implementation.html#m-olm-v1-curve25519-aes-sha2
 * TODO: All the error paths in this function need to clean up!
 */
static void decrypt_olm(PurpleConnection *pc, MatrixConnectionData *conn, JsonObject *cevent,
                                   JsonObject *cevent_content)
{
    const gchar *cevent_sender;
    const gchar *sender_key;
    JsonObject *cevent_ciphertext;
    gchar *cevent_body_copy = NULL;
    gchar *plaintext = NULL;
    size_t max_plaintext_len = 0;
    cevent_sender = matrix_json_object_get_string_member(cevent, "sender");
    sender_key = matrix_json_object_get_string_member(cevent_content,
                                                       "sender_key");
    cevent_ciphertext = matrix_json_object_get_object_member(cevent_content,
                                                               "ciphertext");
    /* TODO: Look up sender_key - I think we need to check this against device
     * list from user? */
    MatrixOlmSession *mos = NULL;
    OlmSession *session = NULL;

    if (!cevent_ciphertext || !sender_key) {
        purple_debug_info("matrixprpl",
                               "%s: no ciphertext or sender_key in olm event\n",
                               __func__);
        goto err;
    }
    JsonObject *our_ciphertext;
    our_ciphertext = matrix_json_object_get_object_member(cevent_ciphertext,
                                               conn->e2e->curve25519_pubkey);
    if (!our_ciphertext) {
        purple_debug_info("matrixprpl",
                          "%s: No ciphertext with our curve25519 pubkey\n",
                          __func__);
        goto err;
    }
    JsonNode *type_node = matrix_json_object_get_member(our_ciphertext, "type");
    if (!type_node) {
        purple_debug_info("matrixprpl", "%s: No type node\n", __func__);
        goto err;
    }

    gint64 type = matrix_json_node_get_int(type_node);
    purple_debug_info("matrixprpl",
                      "%s: Type %zd olm encrypted message from %s\n",
                      __func__, (size_t)type, cevent_sender);
    if (!type) {
        /* A 'prekey' message to establish an Olm session */
        const gchar *cevent_body;
        cevent_body = matrix_json_object_get_string_member(our_ciphertext,
                                                           "body");
        mos = find_olm_session(conn, cevent_sender, sender_key, cevent_body);
        if (!mos) {
            cevent_body_copy = g_strdup(cevent_body);
            /* OK, no existing session, lets create one */
            session = olm_session(g_malloc0(olm_session_size()));

            if (olm_create_inbound_session_from(session, conn->e2e->oa,
                                               sender_key, strlen(sender_key),
                                               cevent_body_copy,
                                               strlen(cevent_body)) ==
                olm_error()) {
                purple_debug_info("matrixprpl",
                    "%s: olm prekey inbound_session_from failed with %s\n",
                    __func__, olm_session_last_error(session));
                g_free(session);
                goto err;
            }
            if (olm_remove_one_time_keys(conn->e2e->oa, session) ==
                olm_error()) {
                purple_debug_info("matrixprpl",
                    "%s: Failed to remove 1tk from inbound session "
                    " creation: %s\n",
                    __func__, olm_account_last_error(conn->e2e->oa));
                g_free(session);
                goto err;
            }
            mos = store_olm_session(conn, session, cevent_sender, sender_key);
            if (!mos) {
                g_free(session);
                goto err;
            }
            if (matrix_store_e2e_account(conn)) {
                g_free(session);
                goto err;
            }
        }
        session = mos->session;
        cevent_body_copy = g_strdup(cevent_body);
        max_plaintext_len = olm_decrypt_max_plaintext_length(session,
                                       0 /* Prekey */,
                                       cevent_body_copy,
                                       strlen(cevent_body_copy));
        if (max_plaintext_len == olm_error()) {
            purple_debug_info("matrixprpl",
                              "%s: Failed to get plaintext length %s\n",
                              __func__, olm_session_last_error(session));
            goto err;
        }
        plaintext = g_malloc0(max_plaintext_len + 1);
        cevent_body_copy = g_strdup(cevent_body);

        size_t pt_len = olm_decrypt(session, 0 /* Prekey */, cevent_body_copy,
                                       strlen(cevent_body),
                                       plaintext, max_plaintext_len);
        if (pt_len == olm_error() || pt_len >= max_plaintext_len) {
            purple_debug_info("matrixprpl",
                              "%s: Failed to decrypt inbound session creation"
                              " event: %s\n",
                              __func__, olm_session_last_error(session));
            goto err;
        }
        update_olm_session(conn, mos);
        plaintext[pt_len] = '\0';
        handle_decrypted_olm(pc, conn, cevent_sender, sender_key, plaintext);
    } else {
        purple_debug_info("matrixprpl", "%s: Type %zd olm\n", __func__, type);
    }
    if (plaintext) {
        clear_mem(plaintext, max_plaintext_len);
    }
    g_free(plaintext);
    g_free(cevent_body_copy);

    matrix_store_e2e_account(conn);
    return;

err:
    if (plaintext) {
        clear_mem(plaintext, max_plaintext_len);
    }
    g_free(plaintext);
    g_free(cevent_body_copy);
}

/*
 * See:
 * https://matrix.org/docs/guides/e2e_implementation.html#handling-an-m-room-encrypted-event
 * For decrypting d2d messages
 * TODO: We really need to build a queue of stuff to decrypt, especially since they take multiple
 * messages to deal with when we have to fetch stuff/validate a device id
 */
void matrix_e2e_decrypt_d2d(PurpleConnection *pc, JsonObject *cevent)
{
    MatrixConnectionData *conn = purple_connection_get_protocol_data(pc);
    const gchar *cevent_type;
    const gchar *cevent_sender;
    cevent_type = matrix_json_object_get_string_member(cevent, "type");
    cevent_sender = matrix_json_object_get_string_member(cevent, "sender");
    purple_debug_info("matrixprpl", "%s: %s from %s\n", __func__, cevent_type,
                       cevent_sender);

    if (strcmp(cevent_type, "m.room.encrypted")) {
        purple_debug_info("matrixprpl", "%s: %s unexpected type\n",
                               __func__, cevent_type);
        goto out;
    }

    JsonObject *cevent_content = matrix_json_object_get_object_member(cevent,
                                                               "content");
    const gchar *cevent_algo;
    cevent_algo = matrix_json_object_get_string_member(cevent_content,
                                                       "algorithm");
    if (!cevent_algo) {
        purple_debug_info("matrixprpl",
           "%s: Encrypted event doesn't have algorithm entry\n", __func__);
        goto out;
    }

    if (!strcmp(cevent_algo, "m.olm.v1.curve25519-aes-sha2")) {
        decrypt_olm(pc, conn, cevent, cevent_content);
    } else if (!strcmp(cevent_algo, "m.megolm.v1.aes-sha2")) {
        purple_debug_info("matrixprpl",
           "%s: It's megolm - unexpected for d2d!\n", __func__);
    } else {
        purple_debug_info("matrixprpl",
           "%s: Unknown crypto algorithm %s\n", __func__, cevent_algo);
    }

out:
    return;
}

/*
 * If succesful returns a JsonParser on the decrypted event.
 */
JsonParser *matrix_e2e_decrypt_room(PurpleConversation *conv,
                                    struct _JsonObject *cevent)
{
    JsonObject *cevent_content;
    const gchar *cevent_sender, *cevent_sender_key, *cevent_session_id;
    const gchar *algorithm, *cevent_ciphertext, *cevent_device_id;
    gchar *dupe_ciphertext = NULL;
    gchar *plaintext = NULL;
    size_t maxlen = 0;
    JsonParser *plaintext_parser = NULL;

    cevent_sender = matrix_json_object_get_string_member(cevent, "sender");
    cevent_content = matrix_json_object_get_object_member(cevent, "content");
    cevent_sender_key = matrix_json_object_get_string_member(cevent_content,
                                                               "sender_key");
    cevent_session_id = matrix_json_object_get_string_member(cevent_content,
                                                               "session_id");
    cevent_device_id = matrix_json_object_get_string_member(cevent_content,
                                                               "device_id");
    algorithm = matrix_json_object_get_string_member(cevent_content,
                                                       "algorithm");
    cevent_ciphertext = matrix_json_object_get_string_member(cevent_content,
                                                               "ciphertext");

    if (!algorithm || strcmp(algorithm, "m.megolm.v1.aes-sha2")) {
        purple_debug_info("matrixprpl", "%s: Bad algorithm %s\n",
                               __func__, algorithm);
        goto out;
    }

    if (!cevent_sender || !cevent_content || !cevent_sender_key ||
        !cevent_session_id || !cevent_device_id || !cevent_ciphertext) {
        purple_debug_info("matrixprpl",
                          "%s: Missing field sender: %s content: %p "
                          "sender_key: %s session_id: %s device_id: %s "
                          "ciphertext: %s\n",
                               __func__, cevent_sender, cevent_content,
                               cevent_sender_key, cevent_session_id,
                               cevent_device_id, cevent_ciphertext);
        goto out;
    }

    OlmInboundGroupSession *oigs = get_inbound_megolm_session(conv,
                                               cevent_sender_key,
                                               cevent_sender, cevent_session_id,
                                               cevent_device_id);
    if (!oigs) {
        // TODO: Queue this message and decrypt it when we get the session?
        // TODO: Check device verification state?
        purple_debug_info("matrixprpl",
                          "%s: No Megolm session for %s/%s/%s/%s\n", __func__,
                          cevent_device_id, cevent_sender, cevent_sender_key,
                          cevent_session_id);
        goto out;
    }
    purple_debug_info("matrixprpl",
                      "%s: have Megolm session %p for %s/%s/%s/%s\n",
                      __func__, oigs, cevent_device_id, cevent_sender,
                      cevent_sender_key, cevent_session_id);
    dupe_ciphertext = g_strdup(cevent_ciphertext);
    maxlen = olm_group_decrypt_max_plaintext_length(oigs,
                       (uint8_t *)dupe_ciphertext,
                       strlen(dupe_ciphertext));
    if (maxlen == olm_error()) {
        purple_debug_info("matrixprpl",
                          "%s: olm_group_decrypt_max_plaintext_length says "
                          "%s for %s/%s/%s/%s\n",
                __func__, olm_inbound_group_session_last_error(oigs),
                cevent_device_id, cevent_sender, cevent_sender_key,
                cevent_session_id);
        goto out;
    }
    dupe_ciphertext = g_strdup(cevent_ciphertext);
    plaintext = g_malloc0(maxlen+1);
    uint32_t index;
    size_t decrypt_len = olm_group_decrypt(oigs, (uint8_t *)dupe_ciphertext,
                                           strlen(dupe_ciphertext),
                                           (uint8_t *)plaintext, maxlen,
                                           &index);
    if (decrypt_len == olm_error()) {
        purple_debug_info("matrixprpl",
                "%s: olm_group_decrypt says %s for %s/%s/%s/%s\n",
                __func__, olm_inbound_group_session_last_error(oigs),
                cevent_device_id, cevent_sender, cevent_sender_key,
                cevent_session_id);
        goto out;
    }

    if (decrypt_len > maxlen) {
        purple_debug_info("matrixprpl",
                "%s: olm_group_decrypt len=%zd max was supposed to be %zd\n",
                __func__, decrypt_len, maxlen);
        goto out;
    }
    // TODO: Stash index somewhere - supposed to check it for validity
    plaintext[decrypt_len] = '\0';
    purple_debug_info("matrixprpl",
                      "%s: Decrypted megolm event as '%s' index=%zd\n",
                      __func__, plaintext, (size_t)index);

    plaintext_parser = json_parser_new();
    GError *err = NULL;
    if (!json_parser_load_from_data(plaintext_parser,
                                    plaintext, strlen(plaintext), &err)) {
        purple_debug_info("matrixprpl",
                          "%s: Failed to json parse decrypted plain text: %s\n",
                          __func__, plaintext);
        g_object_unref(plaintext_parser);
        goto out;
    }

out:
    g_free(dupe_ciphertext);
    if (plaintext) {
        clear_mem(plaintext, maxlen);
    }
    g_free(plaintext);

    return plaintext_parser;
}

static void action_device_info(PurplePluginAction *action)
{
    PurpleConnection *pc = (PurpleConnection *) action->context;

    if (!pc) return;
    MatrixConnectionData *conn = purple_connection_get_protocol_data(pc);
    if (!conn || !conn->e2e) return;
    char *title = g_strdup_printf("Device info for %s", conn->user_id);
    char *body = g_strdup_printf("Device ID: %s"
                                 "<br>Device Key: %s",
                                 conn->e2e->device_id,
                                 conn->e2e->ed25519_pubkey);
    purple_notify_formatted(pc, title, title, NULL, body, NULL, NULL);
    g_free(title);
    g_free(body);
}

/* Hook for adding purple 'action' menu items */
GList *matrix_e2e_actions(GList *list)
{
    list = g_list_append(list,
                         purple_plugin_action_new(_("Device info"),
                                                 action_device_info));
    return list;
}

#else
/* ==== Stubs for when e2e is configured out of the build === */
void matrix_e2e_decrypt_d2d(PurpleConnection *pc, JsonObject *cevent)
{
}

JsonParser *matrix_e2e_decrypt_room(PurpleConversation *conv,
                                    struct _JsonObject *cevent)
{
    return NULL;
}

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

void matrix_e2e_cleanup_conversation(PurpleConversation *conv)
{
}

GList *matrix_e2e_actions(GList *list)
{
    return list;
}

#endif
