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
#include "conversation.h"
#include "matrix-event.h"
#include "matrix-room.h"
#include "matrix-roommembers.h"
#include "matrix-statetable.h"

#define PURPLE_CONV_DATA_ACTIVE_SEND "active_send"
#define MEGOLM_ROTATE_AFTER 100
#define MEGOLM_ALGO "m.megolm.v1.aes-sha2"
#define OLM_ALGO "m.olm.v1.curve25519-aes-sha2"

gboolean matrix_e2e_conversation_is_encrypted(PurpleConversation *conv)
{
    MatrixRoomStateEventTable *state_table;
    MatrixRoomEvent *event;
    const gchar *algorithm;

    state_table = matrix_room_get_state_table(conv);
    if (!state_table) {
        return FALSE;
    }
    event = matrix_statetable_get_event(state_table, "m.room.encryption", "");
    if (!event) {
        return FALSE;
    }
    algorithm = matrix_json_object_get_string_member(event->content,
            "algorithm");
    return algorithm != NULL && algorithm[0] != '\0';
}

#ifndef MATRIX_NO_E2E
#include "olm/olm.h"
#include <gcrypt.h>

struct _MatrixOlmSession;

struct _MatrixE2EData {
    OlmAccount *oa;
    gchar *device_id;
    gchar *curve25519_pubkey;
    gchar *ed25519_pubkey;
    sqlite3 *db;
    /* Mapping from MatrixHashKeyOlm to MatrixOlmSession */
    GHashTable *olm_session_hash;
    /* Mapping from user_id to GHashTable(device_id -> MatrixE2EDevice) */
    GHashTable *devices;
};

typedef struct _MatrixE2EDevice {
    gchar *user_id;
    gchar *device_id;
    gchar *curve25519;
    gchar *ed25519;
} MatrixE2EDevice;

typedef struct _MatrixEncryptSend {
    MatrixConnectionData *conn;
    PurpleConversation *conv;
    gchar *event_type;
    gchar *txn_id;
    JsonObject *content;
    MatrixApiCallback callback;
    MatrixApiErrorCallback error_callback;
    MatrixApiBadResponseCallback bad_response_callback;
    gpointer user_data;
    /* Have we already queried the device keys during this send? Without this
     * we would loop forever querying devices for a user that has no e2e
     * capable devices at all.
     */
    gboolean queried_devices;
    /* Have we already claimed one-time keys during this send? */
    gboolean claimed_otks;
} MatrixEncryptSend;

#define PURPLE_CONV_E2E_STATE "e2e"

/* Hung off the Purple conversation with the PURPLE_CONV_E2E_STATE */
typedef struct _MatrixE2ERoomData {
    /* Mapping from _MatrixHashKeyInBoundMegOlm to OlmInboundGroupSession */
    GHashTable *megolm_sessions_inbound;
    OlmOutboundGroupSession *megolm_outbound;
    gchar *megolm_session_id;
    /* Keys of the form "user_id\\ndevice_id" that have the current outbound key */
    GHashTable *megolm_shared;
    guint megolm_messages_sent;
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

struct _MatrixMediaCryptInfo {
    guchar sha256[32];
    guchar aes_k[32];
    guchar aes_iv[16];
};

static void key_upload_callback(MatrixConnectionData *conn,
                                gpointer user_data,
                                struct _JsonNode *json_root,
                                const char *body,
                                size_t body_len, const char *content_type);
static MatrixApiRequestData *encrypt_send_continue(MatrixEncryptSend *ctx);
static void encrypt_send_free(MatrixEncryptSend *ctx);
static void encrypt_send_continue_cb(MatrixConnectionData *conn,
                                     gpointer user_data,
                                     struct _JsonNode *json_root,
                                     const char *body,
                                     size_t body_len,
                                     const char *content_type);
static void encrypt_keyshare_complete(MatrixConnectionData *conn,
                                      gpointer user_data,
                                      struct _JsonNode *json_root,
                                      const char *body,
                                      size_t body_len,
                                      const char *content_type);
static MatrixOlmSession *find_olm_session(MatrixConnectionData *conn,
                                          const char *sender_id,
                                          const char *sender_key,
                                          const char *body);
static MatrixOlmSession *store_olm_session(MatrixConnectionData *conn,
                                           OlmSession *session,
                                           const char *sender_id,
                                           const char *sender_key);
static int update_olm_session(MatrixConnectionData *conn,
                              MatrixOlmSession *mos);

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

    clear_mem(ok->sender_key, strlen(ok->sender_key));
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

static gchar *json_object_to_cstring(JsonObject *obj)
{
    JsonNode *node;
    JsonGenerator *generator;
    gchar *json;

    node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, obj);
    generator = json_generator_new();
    json_generator_set_root(generator, node);
    json = json_generator_to_data(generator, NULL);
    g_object_unref(G_OBJECT(generator));
    json_node_free(node);
    return json;
}

static void free_e2e_device(gpointer p)
{
    MatrixE2EDevice *dev = p;
    g_free(dev->user_id);
    g_free(dev->device_id);
    g_free(dev->curve25519);
    g_free(dev->ed25519);
    g_free(dev);
}

static gchar *device_share_key(const gchar *user_id, const gchar *device_id)
{
    return g_strdup_printf("%s\n%s", user_id, device_id);
}

static MatrixOlmSession *get_any_olm_session(MatrixConnectionData *conn,
        const char *sender_id, const char *sender_key)
{
    MatrixHashKeyOlm match;
    MatrixOlmSession *head;

    match.sender_key = (gchar *)sender_key;
    match.sender_id = (gchar *)sender_id;
    head = g_hash_table_lookup(conn->e2e->olm_session_hash, &match);
    if (head) {
        return head;
    }
    /* Load any pickled sessions for this device from the db */
    find_olm_session(conn, sender_id, sender_key, "");
    return g_hash_table_lookup(conn->e2e->olm_session_hash, &match);
}

static gboolean verify_signed_json(const gchar *ed25519_key,
        const gchar *user_id, const gchar *key_id, JsonObject *obj)
{
    gchar *raw;
    JsonParser *parser;
    JsonObject *dup;
    JsonObject *sigs;
    JsonObject *user_sigs;
    gchar *sig_name;
    const gchar *sig;
    gchar *sig_copy;
    GString *canonical;
    OlmUtility *utility;
    size_t ret;
    gboolean ok = FALSE;

    raw = json_object_to_cstring(obj);
    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, raw, -1, NULL)) {
        g_free(raw);
        g_object_unref(parser);
        return FALSE;
    }
    g_free(raw);
    dup = matrix_json_node_get_object(json_parser_get_root(parser));
    sigs = matrix_json_object_get_object_member(dup, "signatures");
    user_sigs = matrix_json_object_get_object_member(sigs, user_id);
    sig_name = g_strdup_printf("ed25519:%s", key_id);
    sig = matrix_json_object_get_string_member(user_sigs, sig_name);
    g_free(sig_name);
    if (!sig || !ed25519_key) {
        g_object_unref(parser);
        return FALSE;
    }
    sig_copy = g_strdup(sig);
    json_object_remove_member(dup, "signatures");
    json_object_remove_member(dup, "unsigned");
    canonical = matrix_canonical_json(dup);
    utility = olm_utility(g_malloc0(olm_utility_size()));
    ret = olm_ed25519_verify(utility, ed25519_key, strlen(ed25519_key),
            canonical->str, canonical->len, sig_copy, strlen(sig_copy));
    ok = (ret != olm_error());
    if (!ok) {
        purple_debug_info("matrixprpl", "%s: signature check failed: %s\n",
                __func__, olm_utility_last_error(utility));
    }
    olm_clear_utility(utility);
    g_free(utility);
    g_string_free(canonical, TRUE);
    g_free(sig_copy);
    g_object_unref(parser);
    return ok;
}

static gboolean device_supports_megolm(JsonObject *device_keys)
{
    JsonArray *algorithms;
    guint i, n;

    algorithms = matrix_json_object_get_array_member(device_keys, "algorithms");
    if (!algorithms) {
        return TRUE;
    }
    n = json_array_get_length(algorithms);
    for (i = 0; i < n; i++) {
        const gchar *algo = matrix_json_array_get_string_element(algorithms, i);
        if (algo && !strcmp(algo, MEGOLM_ALGO)) {
            return TRUE;
        }
    }
    return FALSE;
}

static void store_queried_device(MatrixConnectionData *conn, const gchar *user_id,
        const gchar *device_id, JsonObject *device_keys)
{
    JsonObject *keys;
    gchar *curve_name, *ed_name;
    const gchar *curve, *ed;
    GHashTable *user_devs;
    MatrixE2EDevice *dev;

    if (!device_supports_megolm(device_keys)) {
        purple_debug_info("matrixprpl",
                "%s: skipping %s/%s (no megolm)\n",
                __func__, user_id, device_id);
        return;
    }

    keys = matrix_json_object_get_object_member(device_keys, "keys");
    curve_name = g_strdup_printf("curve25519:%s", device_id);
    ed_name = g_strdup_printf("ed25519:%s", device_id);
    curve = matrix_json_object_get_string_member(keys, curve_name);
    ed = matrix_json_object_get_string_member(keys, ed_name);
    g_free(curve_name);
    g_free(ed_name);
    if (!curve || !ed) {
        purple_debug_info("matrixprpl", "%s: missing keys for %s/%s\n",
                __func__, user_id, device_id);
        return;
    }
    if (!verify_signed_json(ed, user_id, device_id, device_keys)) {
        purple_debug_info("matrixprpl",
                "%s: invalid device signature for %s/%s\n",
                __func__, user_id, device_id);
        return;
    }

    if (!conn->e2e->devices) {
        conn->e2e->devices = g_hash_table_new_full(g_str_hash, g_str_equal,
                g_free, (GDestroyNotify)g_hash_table_destroy);
    }
    user_devs = g_hash_table_lookup(conn->e2e->devices, user_id);
    if (!user_devs) {
        user_devs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                free_e2e_device);
        g_hash_table_insert(conn->e2e->devices, g_strdup(user_id), user_devs);
    }

    dev = g_new0(MatrixE2EDevice, 1);
    dev->user_id = g_strdup(user_id);
    dev->device_id = g_strdup(device_id);
    dev->curve25519 = g_strdup(curve);
    dev->ed25519 = g_strdup(ed);
    g_hash_table_replace(user_devs, g_strdup(device_id), dev);
}

static void handle_device_key_query(MatrixConnectionData *conn, JsonObject *root)
{
    JsonObject *device_keys;
    JsonObjectIter user_iter;
    const gchar *user_id;
    JsonNode *user_node;

    device_keys = matrix_json_object_get_object_member(root, "device_keys");
    if (!device_keys) {
        return;
    }
    json_object_iter_init(&user_iter, device_keys);
    while (json_object_iter_next(&user_iter, &user_id, &user_node)) {
        JsonObject *devices = matrix_json_node_get_object(user_node);
        JsonObjectIter dev_iter;
        const gchar *device_id;
        JsonNode *dev_node;

        if (!devices) {
            continue;
        }
        json_object_iter_init(&dev_iter, devices);
        while (json_object_iter_next(&dev_iter, &device_id, &dev_node)) {
            JsonObject *dev_obj = matrix_json_node_get_object(dev_node);
            if (dev_obj) {
                store_queried_device(conn, user_id, device_id, dev_obj);
            }
        }
    }
}

static JsonObject *encrypt_olm_plaintext(MatrixConnectionData *conn,
        MatrixOlmSession *mos, const gchar *their_curve, const gchar *plaintext)
{
    size_t message_type;
    size_t random_len;
    void *random_buf = NULL;
    size_t message_len;
    gchar *message;
    size_t ret;
    JsonObject *body;
    JsonObject *ciphertext;
    JsonObject *content;

    message_type = olm_encrypt_message_type(mos->session);
    if (message_type == olm_error()) {
        purple_debug_warning("matrixprpl", "%s: message type: %s\n",
                __func__, olm_session_last_error(mos->session));
        return NULL;
    }
    random_len = olm_encrypt_random_length(mos->session);
    if (random_len) {
        random_buf = get_random(random_len);
        if (!random_buf) {
            return NULL;
        }
    }
    message_len = olm_encrypt_message_length(mos->session, strlen(plaintext));
    message = g_malloc0(message_len + 1);
    ret = olm_encrypt(mos->session, plaintext, strlen(plaintext),
            random_buf, random_len, message, message_len);
    g_free(random_buf);
    if (ret == olm_error()) {
        purple_debug_warning("matrixprpl", "%s: encrypt failed: %s\n",
                __func__, olm_session_last_error(mos->session));
        g_free(message);
        return NULL;
    }
    message[ret] = '\0';
    update_olm_session(conn, mos);

    body = json_object_new();
    json_object_set_int_member(body, "type", (gint64)message_type);
    json_object_set_string_member(body, "body", message);
    g_free(message);

    ciphertext = json_object_new();
    json_object_set_object_member(ciphertext, their_curve, body);

    content = json_object_new();
    json_object_set_string_member(content, "algorithm", OLM_ALGO);
    json_object_set_string_member(content, "sender_key",
            conn->e2e->curve25519_pubkey);
    json_object_set_object_member(content, "ciphertext", ciphertext);
    return content;
}

static MatrixOlmSession *create_outbound_olm_session(MatrixConnectionData *conn,
        MatrixE2EDevice *dev, const gchar *otk)
{
    OlmSession *session;
    void *random_buf;
    size_t random_len;
    MatrixOlmSession *mos;

    session = olm_session(g_malloc0(olm_session_size()));
    random_len = olm_create_outbound_session_random_length(session);
    random_buf = get_random(random_len);
    if (!random_buf) {
        g_free(session);
        return NULL;
    }
    if (olm_create_outbound_session(session, conn->e2e->oa,
            dev->curve25519, strlen(dev->curve25519),
            otk, strlen(otk), random_buf, random_len) == olm_error()) {
        purple_debug_warning("matrixprpl",
                "%s: outbound olm session for %s/%s: %s\n",
                __func__, dev->user_id, dev->device_id,
                olm_session_last_error(session));
        g_free(random_buf);
        g_free(session);
        return NULL;
    }
    g_free(random_buf);
    mos = store_olm_session(conn, session, dev->user_id, dev->curve25519);
    if (!mos) {
        olm_clear_session(session);
        g_free(session);
        return NULL;
    }
    return mos;
}

static int ensure_outbound_megolm(PurpleConversation *conv,
        MatrixConnectionData *conn)
{
    MatrixE2ERoomData *rd = get_e2e_room_data(conv);
    OlmOutboundGroupSession *ogs;
    void *random_buf;
    size_t random_len;
    size_t id_len, key_len;
    gchar *session_id;
    gchar *session_key;
    OlmInboundGroupSession *igs;

    if (rd->megolm_outbound &&
            rd->megolm_messages_sent < MEGOLM_ROTATE_AFTER) {
        return 0;
    }

    if (rd->megolm_outbound) {
        olm_clear_outbound_group_session(rd->megolm_outbound);
        g_free(rd->megolm_outbound);
        rd->megolm_outbound = NULL;
    }
    g_free(rd->megolm_session_id);
    rd->megolm_session_id = NULL;
    if (rd->megolm_shared) {
        g_hash_table_remove_all(rd->megolm_shared);
    } else {
        rd->megolm_shared = g_hash_table_new_full(g_str_hash, g_str_equal,
                g_free, NULL);
    }
    rd->megolm_messages_sent = 0;

    ogs = olm_outbound_group_session(g_malloc0(olm_outbound_group_session_size()));
    random_len = olm_init_outbound_group_session_random_length(ogs);
    random_buf = get_random(random_len);
    if (!random_buf) {
        g_free(ogs);
        return -1;
    }
    if (olm_init_outbound_group_session(ogs, random_buf, random_len) ==
            olm_error()) {
        purple_debug_warning("matrixprpl", "%s: %s\n", __func__,
                olm_outbound_group_session_last_error(ogs));
        g_free(random_buf);
        g_free(ogs);
        return -1;
    }
    g_free(random_buf);

    id_len = olm_outbound_group_session_id_length(ogs);
    session_id = g_malloc0(id_len + 1);
    if (olm_outbound_group_session_id(ogs, (uint8_t *)session_id, id_len) ==
            olm_error()) {
        purple_debug_warning("matrixprpl", "%s: session id: %s\n", __func__,
                olm_outbound_group_session_last_error(ogs));
        olm_clear_outbound_group_session(ogs);
        g_free(ogs);
        g_free(session_id);
        return -1;
    }

    key_len = olm_outbound_group_session_key_length(ogs);
    session_key = g_malloc0(key_len + 1);
    if (olm_outbound_group_session_key(ogs, (uint8_t *)session_key, key_len) ==
            olm_error()) {
        purple_debug_warning("matrixprpl", "%s: session key: %s\n", __func__,
                olm_outbound_group_session_last_error(ogs));
        olm_clear_outbound_group_session(ogs);
        g_free(ogs);
        g_free(session_id);
        g_free(session_key);
        return -1;
    }

    igs = olm_inbound_group_session(g_malloc0(olm_inbound_group_session_size()));
    if (olm_init_inbound_group_session(igs, (uint8_t *)session_key,
            strlen(session_key)) == olm_error()) {
        purple_debug_warning("matrixprpl",
                "%s: inbound from outbound failed: %s\n", __func__,
                olm_inbound_group_session_last_error(igs));
        olm_clear_inbound_group_session(igs);
        g_free(igs);
        olm_clear_outbound_group_session(ogs);
        g_free(ogs);
        g_free(session_id);
        g_free(session_key);
        return -1;
    }
    store_inbound_megolm_session(conv, conn->e2e->curve25519_pubkey,
            conn->user_id, session_id, conn->e2e->device_id, igs);
    clear_mem(session_key, strlen(session_key));
    g_free(session_key);

    rd->megolm_outbound = ogs;
    rd->megolm_session_id = session_id;
    purple_debug_info("matrixprpl", "%s: created megolm session %s for %s\n",
            __func__, session_id, conv->name);
    return 0;
}

static JsonObject *build_room_key_plaintext(MatrixConnectionData *conn,
        PurpleConversation *conv, MatrixE2EDevice *dev)
{
    MatrixE2ERoomData *rd = get_e2e_room_data(conv);
    JsonObject *pt;
    JsonObject *content;
    JsonObject *keys;
    JsonObject *recipient_keys;
    size_t key_len;
    gchar *session_key;

    key_len = olm_outbound_group_session_key_length(rd->megolm_outbound);
    session_key = g_malloc0(key_len + 1);
    if (olm_outbound_group_session_key(rd->megolm_outbound,
            (uint8_t *)session_key, key_len) == olm_error()) {
        g_free(session_key);
        return NULL;
    }

    content = json_object_new();
    json_object_set_string_member(content, "algorithm", MEGOLM_ALGO);
    json_object_set_string_member(content, "room_id", conv->name);
    json_object_set_string_member(content, "session_id", rd->megolm_session_id);
    json_object_set_string_member(content, "session_key", session_key);
    clear_mem(session_key, strlen(session_key));
    g_free(session_key);

    keys = json_object_new();
    json_object_set_string_member(keys, "ed25519", conn->e2e->ed25519_pubkey);
    recipient_keys = json_object_new();
    json_object_set_string_member(recipient_keys, "ed25519", dev->ed25519);

    pt = json_object_new();
    json_object_set_string_member(pt, "type", "m.room_key");
    json_object_set_object_member(pt, "content", content);
    json_object_set_string_member(pt, "sender", conn->user_id);
    json_object_set_string_member(pt, "sender_device", conn->e2e->device_id);
    json_object_set_object_member(pt, "keys", keys);
    json_object_set_string_member(pt, "recipient", dev->user_id);
    json_object_set_object_member(pt, "recipient_keys", recipient_keys);
    return pt;
}

static void encrypt_send_free(MatrixEncryptSend *ctx)
{
    g_free(ctx->event_type);
    g_free(ctx->txn_id);
    if (ctx->content) {
        json_object_unref(ctx->content);
    }
    g_free(ctx);
}

static void encrypt_send_error(MatrixConnectionData *conn, gpointer user_data,
        const gchar *error_message)
{
    MatrixEncryptSend *ctx = user_data;
    ctx->error_callback(conn, ctx->user_data, error_message);
    encrypt_send_free(ctx);
}

static void encrypt_send_bad_response(MatrixConnectionData *conn,
        gpointer user_data, int http_response_code, JsonNode *json_root)
{
    MatrixEncryptSend *ctx = user_data;
    ctx->bad_response_callback(conn, ctx->user_data, http_response_code,
            json_root);
    encrypt_send_free(ctx);
}

static MatrixApiRequestData *finish_encrypted_room_send(MatrixEncryptSend *ctx)
{
    MatrixE2ERoomData *rd = get_e2e_room_data(ctx->conv);
    JsonObject *plaintext_obj;
    gchar *plaintext;
    size_t message_len;
    gchar *message;
    size_t ret;
    JsonObject *enc_content;
    MatrixApiRequestData *fetch_data;

    if (ensure_outbound_megolm(ctx->conv, ctx->conn)) {
        encrypt_send_error(ctx->conn, ctx, "Failed to create Megolm session");
        return NULL;
    }

    plaintext_obj = json_object_new();
    json_object_set_string_member(plaintext_obj, "type", ctx->event_type);
    json_object_set_object_member(plaintext_obj, "content",
            json_object_ref(ctx->content));
    json_object_set_string_member(plaintext_obj, "room_id", ctx->conv->name);
    plaintext = json_object_to_cstring(plaintext_obj);
    json_object_unref(plaintext_obj);

    message_len = olm_group_encrypt_message_length(rd->megolm_outbound,
            strlen(plaintext));
    message = g_malloc0(message_len + 1);
    ret = olm_group_encrypt(rd->megolm_outbound, (uint8_t *)plaintext,
            strlen(plaintext), (uint8_t *)message, message_len);
    clear_mem(plaintext, strlen(plaintext));
    g_free(plaintext);
    if (ret == olm_error()) {
        g_free(message);
        encrypt_send_error(ctx->conn, ctx, "Failed to encrypt room event");
        return NULL;
    }
    message[ret] = '\0';
    rd->megolm_messages_sent++;

    enc_content = json_object_new();
    json_object_set_string_member(enc_content, "algorithm", MEGOLM_ALGO);
    json_object_set_string_member(enc_content, "sender_key",
            ctx->conn->e2e->curve25519_pubkey);
    json_object_set_string_member(enc_content, "device_id",
            ctx->conn->e2e->device_id);
    json_object_set_string_member(enc_content, "session_id",
            rd->megolm_session_id);
    json_object_set_string_member(enc_content, "ciphertext", message);
    g_free(message);

    fetch_data = matrix_api_send(ctx->conn, ctx->conv->name, "m.room.encrypted",
            ctx->txn_id, enc_content, ctx->callback, ctx->error_callback,
            ctx->bad_response_callback, ctx->user_data);
    json_object_unref(enc_content);
    encrypt_send_free(ctx);
    return fetch_data;
}

static MatrixApiRequestData *share_megolm_and_send(MatrixEncryptSend *ctx)
{
    MatrixE2ERoomData *rd;
    MatrixRoomMemberTable *members;
    GList *member_list, *mp;
    JsonObject *messages;
    guint n_messages = 0;
    gchar *txn;

    if (ensure_outbound_megolm(ctx->conv, ctx->conn)) {
        encrypt_send_error(ctx->conn, ctx, "Failed to create Megolm session");
        return NULL;
    }
    rd = get_e2e_room_data(ctx->conv);
    members = matrix_room_get_member_table(ctx->conv);
    member_list = matrix_roommembers_get_active_members(members, FALSE);
    messages = json_object_new();

    for (mp = member_list; mp; mp = g_list_next(mp)) {
        MatrixRoomMember *member = mp->data;
        const gchar *user_id = matrix_roommember_get_user_id(member);
        GHashTable *user_devs;
        GHashTableIter dev_iter;
        gpointer key, value;
        JsonObject *user_messages = NULL;

        if (!ctx->conn->e2e->devices) {
            continue;
        }
        user_devs = g_hash_table_lookup(ctx->conn->e2e->devices, user_id);
        if (!user_devs) {
            continue;
        }
        g_hash_table_iter_init(&dev_iter, user_devs);
        while (g_hash_table_iter_next(&dev_iter, &key, &value)) {
            MatrixE2EDevice *dev = value;
            gchar *share_key;
            MatrixOlmSession *mos;
            JsonObject *pt;
            gchar *pt_str;
            JsonObject *olm_content;

            if (!strcmp(user_id, ctx->conn->user_id) &&
                    !strcmp(dev->device_id, ctx->conn->e2e->device_id)) {
                continue;
            }
            share_key = device_share_key(user_id, dev->device_id);
            if (g_hash_table_lookup(rd->megolm_shared, share_key)) {
                g_free(share_key);
                continue;
            }
            mos = get_any_olm_session(ctx->conn, user_id, dev->curve25519);
            if (!mos) {
                purple_debug_info("matrixprpl",
                        "%s: no olm session for %s/%s\n",
                        __func__, user_id, dev->device_id);
                g_free(share_key);
                continue;
            }
            pt = build_room_key_plaintext(ctx->conn, ctx->conv, dev);
            if (!pt) {
                g_free(share_key);
                continue;
            }
            pt_str = json_object_to_cstring(pt);
            json_object_unref(pt);
            olm_content = encrypt_olm_plaintext(ctx->conn, mos, dev->curve25519,
                    pt_str);
            clear_mem(pt_str, strlen(pt_str));
            g_free(pt_str);
            if (!olm_content) {
                g_free(share_key);
                continue;
            }
            if (!user_messages) {
                user_messages = json_object_new();
                json_object_set_object_member(messages, user_id, user_messages);
            }
            json_object_set_object_member(user_messages, dev->device_id,
                    olm_content);
            g_hash_table_insert(rd->megolm_shared, share_key,
                    GINT_TO_POINTER(1));
            n_messages++;
        }
    }
    g_list_free(member_list);

    if (!n_messages) {
        json_object_unref(messages);
        return finish_encrypted_room_send(ctx);
    }

    txn = g_strdup_printf("%s-keyshare", ctx->txn_id);
    {
        MatrixApiRequestData *fetch_data;
        fetch_data = matrix_api_send_to_device(ctx->conn, "m.room.encrypted",
                txn, messages, encrypt_keyshare_complete,
                encrypt_send_error, encrypt_send_bad_response, ctx);
        json_object_unref(messages);
        g_free(txn);
        return fetch_data;
    }
}

static MatrixApiRequestData *claim_missing_otks_and_send(MatrixEncryptSend *ctx)
{
    MatrixRoomMemberTable *members;
    GList *member_list, *mp;
    JsonObject *otk_claim;
    guint n_claims = 0;

    members = matrix_room_get_member_table(ctx->conv);
    member_list = matrix_roommembers_get_active_members(members, FALSE);
    otk_claim = json_object_new();

    for (mp = member_list; mp; mp = g_list_next(mp)) {
        MatrixRoomMember *member = mp->data;
        const gchar *user_id = matrix_roommember_get_user_id(member);
        GHashTable *user_devs;
        GHashTableIter dev_iter;
        gpointer key, value;
        JsonObject *user_claim = NULL;

        if (!ctx->conn->e2e->devices) {
            continue;
        }
        user_devs = g_hash_table_lookup(ctx->conn->e2e->devices, user_id);
        if (!user_devs) {
            continue;
        }
        g_hash_table_iter_init(&dev_iter, user_devs);
        while (g_hash_table_iter_next(&dev_iter, &key, &value)) {
            MatrixE2EDevice *dev = value;
            if (!strcmp(user_id, ctx->conn->user_id) &&
                    !strcmp(dev->device_id, ctx->conn->e2e->device_id)) {
                continue;
            }
            if (get_any_olm_session(ctx->conn, user_id, dev->curve25519)) {
                continue;
            }
            if (!user_claim) {
                user_claim = json_object_new();
                json_object_set_object_member(otk_claim, user_id, user_claim);
            }
            json_object_set_string_member(user_claim, dev->device_id,
                    "signed_curve25519");
            n_claims++;
        }
    }
    g_list_free(member_list);

    if (!n_claims || ctx->claimed_otks) {
        json_object_unref(otk_claim);
        return share_megolm_and_send(ctx);
    }
    ctx->claimed_otks = TRUE;

    return matrix_api_claim_keys(ctx->conn, otk_claim,
            encrypt_send_continue_cb,
            encrypt_send_error, encrypt_send_bad_response, ctx);
}

static void handle_claimed_otks(MatrixConnectionData *conn, JsonObject *root)
{
    JsonObject *one_time_keys;
    JsonObjectIter user_iter;
    const gchar *user_id;
    JsonNode *user_node;

    one_time_keys = matrix_json_object_get_object_member(root, "one_time_keys");
    if (!one_time_keys) {
        return;
    }
    json_object_iter_init(&user_iter, one_time_keys);
    while (json_object_iter_next(&user_iter, &user_id, &user_node)) {
        JsonObject *devices = matrix_json_node_get_object(user_node);
        JsonObjectIter dev_iter;
        const gchar *device_id;
        JsonNode *dev_node;
        GHashTable *user_devs;
        MatrixE2EDevice *dev;

        if (!devices || !conn->e2e->devices) {
            continue;
        }
        user_devs = g_hash_table_lookup(conn->e2e->devices, user_id);
        json_object_iter_init(&dev_iter, devices);
        while (json_object_iter_next(&dev_iter, &device_id, &dev_node)) {
            JsonObject *keys_obj = matrix_json_node_get_object(dev_node);
            JsonObjectIter key_iter;
            const gchar *key_name;
            JsonNode *key_node;

            if (!user_devs || !keys_obj) {
                continue;
            }
            dev = g_hash_table_lookup(user_devs, device_id);
            if (!dev) {
                continue;
            }
            json_object_iter_init(&key_iter, keys_obj);
            while (json_object_iter_next(&key_iter, &key_name, &key_node)) {
                JsonObject *otk = matrix_json_node_get_object(key_node);
                const gchar *key;
                if (!g_str_has_prefix(key_name, "signed_curve25519:")) {
                    continue;
                }
                key = matrix_json_object_get_string_member(otk, "key");
                if (!key) {
                    continue;
                }
                if (!verify_signed_json(dev->ed25519, user_id, device_id, otk)) {
                    purple_debug_info("matrixprpl",
                            "%s: bad OTK signature %s/%s\n",
                            __func__, user_id, device_id);
                    continue;
                }
                create_outbound_olm_session(conn, dev, key);
                break;
            }
        }
    }
}

static JsonObject *build_device_query_for_room(PurpleConversation *conv)
{
    MatrixRoomMemberTable *members;
    GList *member_list, *mp;
    JsonObject *device_keys;

    members = matrix_room_get_member_table(conv);
    member_list = matrix_roommembers_get_active_members(members, FALSE);
    device_keys = json_object_new();
    for (mp = member_list; mp; mp = g_list_next(mp)) {
        MatrixRoomMember *member = mp->data;
        const gchar *user_id = matrix_roommember_get_user_id(member);
        json_object_set_array_member(device_keys, user_id, json_array_new());
    }
    g_list_free(member_list);
    return device_keys;
}

static void encrypt_send_continue_cb(MatrixConnectionData *conn,
        gpointer user_data, JsonNode *json_root, const char *body,
        size_t body_len, const char *content_type)
{
    MatrixEncryptSend *ctx = user_data;
    PurpleConversation *conv = ctx->conv;
    MatrixApiRequestData *next;
    JsonObject *root = matrix_json_node_get_object(json_root);

    (void)body;
    (void)body_len;
    (void)content_type;

    if (root && matrix_json_object_get_object_member(root, "device_keys")) {
        handle_device_key_query(conn, root);
    }
    if (root && matrix_json_object_get_object_member(root, "one_time_keys")) {
        handle_claimed_otks(conn, root);
    }

    /* encrypt_send_continue may synchronously finish the send, in which case
     * ctx (and hence ctx->conv) has been freed by the time it returns; we
     * saved conv above so that we can still update the active send.
     */
    next = encrypt_send_continue(ctx);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND, next);
}

/* After each async step, decide whether we still need keys or can encrypt. */
static MatrixApiRequestData *encrypt_send_continue(MatrixEncryptSend *ctx)
{
    MatrixRoomMemberTable *members;
    GList *member_list, *mp;
    gboolean need_query = FALSE;

    if (!ctx->conn->e2e || !ctx->conn->e2e->oa) {
        encrypt_send_error(ctx->conn, ctx, "E2E is not initialised");
        return NULL;
    }

    members = matrix_room_get_member_table(ctx->conv);
    member_list = matrix_roommembers_get_active_members(members, FALSE);
    /* Only query once per send; otherwise a member with no e2e capable
     * devices would cause us to keep querying forever.
     */
    if (!ctx->queried_devices) {
        for (mp = member_list; mp; mp = g_list_next(mp)) {
            MatrixRoomMember *member = mp->data;
            const gchar *user_id = matrix_roommember_get_user_id(member);
            if (!ctx->conn->e2e->devices ||
                    !g_hash_table_lookup(ctx->conn->e2e->devices, user_id)) {
                need_query = TRUE;
                break;
            }
        }
    }
    g_list_free(member_list);

    if (need_query) {
        JsonObject *query = build_device_query_for_room(ctx->conv);
        MatrixApiRequestData *fetch_data;
        ctx->queried_devices = TRUE;
        fetch_data = matrix_api_query_keys(ctx->conn, query,
                encrypt_send_continue_cb, encrypt_send_error,
                encrypt_send_bad_response, ctx);
        json_object_unref(query);
        return fetch_data;
    }

    return claim_missing_otks_and_send(ctx);
}

/* Called when the m.room_key to-device messages have been sent. Once the
 * keys are on their way we can send the actual encrypted room event.
 */
static void encrypt_keyshare_complete(MatrixConnectionData *conn,
        gpointer user_data, JsonNode *json_root, const char *body,
        size_t body_len, const char *content_type)
{
    MatrixEncryptSend *ctx = user_data;
    PurpleConversation *conv = ctx->conv;
    MatrixApiRequestData *next;

    (void)conn;
    (void)json_root;
    (void)body;
    (void)body_len;
    (void)content_type;

    /* ctx is freed by finish_encrypted_room_send, so use the saved conv. */
    next = finish_encrypted_room_send(ctx);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND, next);
}

/* Public entry point for sending a room event. If the room is not encrypted
 * this simply forwards to matrix_api_send; otherwise it walks through the
 * steps needed to encrypt an event (query device keys, claim one-time keys,
 * share the megolm session, then encrypt and send).
 */
MatrixApiRequestData *matrix_e2e_send(MatrixConnectionData *conn,
        PurpleConversation *conv, const gchar *event_type,
        const gchar *txn_id, struct _JsonObject *content,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data)
{
    MatrixEncryptSend *ctx;

    if (!matrix_e2e_conversation_is_encrypted(conv)) {
        return matrix_api_send(conn, conv->name, event_type, txn_id, content,
                callback, error_callback, bad_response_callback, user_data);
    }

    ctx = g_new0(MatrixEncryptSend, 1);
    ctx->conn = conn;
    ctx->conv = conv;
    ctx->event_type = g_strdup(event_type);
    ctx->txn_id = g_strdup(txn_id);
    ctx->content = json_object_ref(content);
    ctx->callback = callback;
    ctx->error_callback = error_callback;
    ctx->bad_response_callback = bad_response_callback;
    ctx->user_data = user_data;

    purple_debug_info("matrixprpl", "%s: encrypting %s in %s\n",
            __func__, event_type, conv->name);
    return encrypt_send_continue(ctx);
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
            have_sender = TRUE;
            /* An empty body means "just make sure everything is loaded"; it
             * is used when the caller doesn't know which session a normal
             * olm message belongs to (those don't identify their session).
             */
            if (body && body[0]) {
                size_t ret;
                char *body_double = g_strdup(body);
                ret = olm_matches_inbound_session(cur_entry->session,
                                                  body_double, strlen(body));
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
                            "match with %s/%s: %s\n", __func__,
                            cur_entry->session, sender_id, sender_key,
                            olm_session_last_error(cur_entry->session));
                }
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
            /* Advance the pointer so the next loaded session is appended
             * rather than replacing this one.
             */
            chain = &(cur_entry->next);

            if (!result && body && body[0]) {
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
    size_t max_keys;
    size_t to_create;

    if (!conn->e2e || !conn->e2e->oa) {
        /* E2E never got initialised (e.g. no device id stored) */
        return;
    }

    max_keys = olm_account_max_number_of_one_time_keys(conn->e2e->oa);
    to_create = max_keys;

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

/* Older versions of the plugin created the olmsessions table with a primary
 * key on (sender_name, sender_key), which only permits a single olm session
 * per device. Rebuild the table (preserving existing rows) if we find that
 * old schema so that multiple sessions can be stored.
 * Returns 0 on success (including "nothing to do"), -1 on error.
 */
static int migrate_e2e_db(MatrixConnectionData *conn)
{
    PurpleConnection *pc = conn->pc;
    sqlite3_stmt *dbstmt = NULL;
    gboolean has_id_column = FALSE;
    const char *inspect = "PRAGMA table_info(olmsessions)";
    const char *migrate =
        "BEGIN;"
        "CREATE TABLE olmsessions_new ("
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "    sender_name text, sender_key text, session_pickle text);"
        "INSERT INTO olmsessions_new (sender_name, sender_key, session_pickle)"
        "    SELECT sender_name, sender_key, session_pickle FROM olmsessions;"
        "DROP TABLE olmsessions;"
        "ALTER TABLE olmsessions_new RENAME TO olmsessions;"
        "COMMIT;";
    gchar *errmsg = NULL;
    int ret;

    ret = sqlite3_prepare_v2(conn->e2e->db, inspect, -1, &dbstmt, NULL);
    if (ret != SQLITE_OK || !dbstmt) {
        purple_connection_error_reason(pc,
            PURPLE_CONNECTION_ERROR_OTHER_ERROR,
            "Failed to inspect e2e db sessions table");
        return -1;
    }
    while (sqlite3_step(dbstmt) == SQLITE_ROW) {
        /* Column 1 of PRAGMA table_info is the column name */
        const gchar *name = (const gchar *)sqlite3_column_text(dbstmt, 1);
        if (name && !strcmp(name, "id")) {
            has_id_column = TRUE;
            break;
        }
    }
    sqlite3_finalize(dbstmt);

    if (has_id_column) {
        /* Already using the new schema */
        return 0;
    }

    purple_debug_info("matrixprpl",
            "%s: migrating olmsessions to multi-session schema\n", __func__);

    ret = sqlite3_exec(conn->e2e->db, migrate, NULL, NULL, &errmsg);
    if (ret != SQLITE_OK) {
        purple_debug_warning("matrixprpl", "%s: migration failed: %s\n",
                __func__, errmsg ? errmsg : "(unknown)");
        sqlite3_exec(conn->e2e->db, "ROLLBACK;", NULL, NULL, NULL);
        sqlite3_free(errmsg);
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
                 /* Use an auto-increment rowid rather than a primary key on
                  * (sender_name, sender_key): a peer may create a new olm
                  * session for us at any time (e.g. when it rotates) and we
                  * need to be able to remember more than one session per
                  * device.
                  */
                 "CREATE TABLE olmsessions ("
                 "                          id INTEGER PRIMARY KEY AUTOINCREMENT,"
                 "                          sender_name text, sender_key text,"
                 "                          session_pickle text)");

    if (ret) {
        close_e2e_db(conn);
        return ret;
    }

    ret = migrate_e2e_db(conn);
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
    json_array_add_string_element(algorithms, OLM_ALGO);
    json_array_add_string_element(algorithms, MEGOLM_ALGO);
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
        if (result->megolm_sessions_inbound) {
            g_hash_table_destroy(result->megolm_sessions_inbound);
        }
        if (result->megolm_outbound) {
            olm_clear_outbound_group_session(result->megolm_outbound);
            g_free(result->megolm_outbound);
        }
        g_free(result->megolm_session_id);
        if (result->megolm_shared) {
            g_hash_table_destroy(result->megolm_shared);
        }
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
        if (conn->e2e->olm_session_hash) {
            g_hash_table_destroy(conn->e2e->olm_session_hash);
        }
        if (conn->e2e->devices) {
            g_hash_table_destroy(conn->e2e->devices);
        }
        g_free(conn->e2e->curve25519_pubkey);
        g_free(conn->e2e->ed25519_pubkey);
        if (conn->e2e->oa) {
            olm_clear_account(conn->e2e->oa);
            g_free(conn->e2e->oa);
        }
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

/* Restore a session to a previously taken pickle, undoing any state change
 * that a failed decrypt attempt might have made. `pickle` must have come from
 * olm_pickle_session() on this session.
 */
static void olm_restore_session(OlmSession *session, const gchar *pickle,
        size_t pickle_len)
{
    gchar *restore = g_strdup(pickle);
    olm_clear_session(session);
    if (olm_unpickle_session(session, "!", 1, restore, pickle_len) ==
            olm_error()) {
        purple_debug_warning("matrixprpl",
                "%s: Failed to restore session: %s\n", __func__,
                olm_session_last_error(session));
    }
    clear_mem(restore, pickle_len);
    g_free(restore);
}

/* Try to decrypt a normal (type 1) olm message with the given session.
 *
 * We cannot tell up-front whether a normal message belongs to a particular
 * session (unlike prekey messages, they don't identify it), so the caller may
 * have to try several. To make that safe we take a pickle of the session
 * first and restore it if the attempt fails, so a session that doesn't own
 * the message is left exactly as it was.
 *
 * Returns a newly allocated NUL-terminated plaintext (and advances the
 * session) on success, or NULL on failure (leaving the session unchanged).
 */
static gchar *olm_try_decrypt_normal(OlmSession *session, const gchar *body,
        size_t *plaintext_length)
{
    size_t pickle_alloc_len;
    size_t pickle_len;
    gchar *pickle;
    gchar *body_copy;
    gchar *plaintext;
    size_t max_len;
    size_t pt_len;

    pickle_alloc_len = olm_pickle_session_length(session);
    pickle = g_malloc(pickle_alloc_len + 1);
    pickle_len = olm_pickle_session(session, "!", 1, pickle,
            pickle_alloc_len);
    if (pickle_len == olm_error()) {
        purple_debug_warning("matrixprpl",
                "%s: Failed to pickle session: %s\n", __func__,
                olm_session_last_error(session));
        g_free(pickle);
        return NULL;
    }
    pickle[pickle_len] = '\0';

    body_copy = g_strdup(body);
    max_len = olm_decrypt_max_plaintext_length(session, 1, body_copy,
            strlen(body_copy));
    g_free(body_copy);
    if (max_len == olm_error()) {
        olm_restore_session(session, pickle, pickle_len);
        clear_mem(pickle, pickle_len);
        g_free(pickle);
        return NULL;
    }

    plaintext = g_malloc0(max_len + 1);
    body_copy = g_strdup(body);
    pt_len = olm_decrypt(session, 1, body_copy, strlen(body_copy),
            plaintext, max_len);
    g_free(body_copy);
    if (pt_len == olm_error() || pt_len >= max_len) {
        purple_debug_info("matrixprpl",
                "%s: Session %p didn't decrypt message: %s\n", __func__,
                session, olm_session_last_error(session));
        olm_restore_session(session, pickle, pickle_len);
        clear_mem(plaintext, max_len);
        g_free(plaintext);
        clear_mem(pickle, pickle_len);
        g_free(pickle);
        return NULL;
    }

    clear_mem(pickle, pickle_len);
    g_free(pickle);
    plaintext[pt_len] = '\0';
    *plaintext_length = pt_len;
    return plaintext;
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
    } else if (type == 1) {
        /* A normal olm message sent over a session we already have. Unlike
         * prekey messages, normal messages don't identify which session they
         * were sent with, so try each session we hold for this sender until
         * one decrypts it. olm_try_decrypt_normal() pickles and, on failure,
         * restores the session around each attempt, so a session which
         * doesn't own the message is left exactly as it was.
         */
        const gchar *cevent_body;
        MatrixOlmSession *candidate;

        cevent_body = matrix_json_object_get_string_member(our_ciphertext,
                                                           "body");
        if (!cevent_body) {
            purple_debug_info("matrixprpl", "%s: No body in olm message\n",
                              __func__);
            goto err;
        }

        candidate = get_any_olm_session(conn, cevent_sender, sender_key);
        for (; candidate; candidate = candidate->next) {
            size_t candidate_len = 0;

            plaintext = olm_try_decrypt_normal(candidate->session,
                                               cevent_body, &candidate_len);
            if (!plaintext) {
                /* Not the session this message belongs to - try the next */
                continue;
            }

            /* Found the session this message belongs to */
            max_plaintext_len = candidate_len;
            update_olm_session(conn, candidate);
            purple_debug_info("matrixprpl",
                    "%s: Decrypted normal olm message from %s\n",
                    __func__, cevent_sender);
            handle_decrypted_olm(pc, conn, cevent_sender, sender_key,
                                 plaintext);
            break;
        }
        if (!plaintext) {
            purple_debug_info("matrixprpl",
                    "%s: No session matched normal olm message from %s\n",
                    __func__, cevent_sender);
        }
    } else {
        purple_debug_info("matrixprpl", "%s: Unknown olm message type %zd\n",
                          __func__, type);
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

    if (!conn->e2e || !conn->e2e->oa) {
        purple_debug_info("matrixprpl",
                "%s: No e2e data, can't decrypt d2d event\n", __func__);
        return;
    }

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

/* Parse the 'file' object on media to extract keys, returns
 * FALSE if there's a problem, returns TRUE whether or not there's
 * any crypto information, returns TRUE and allocates *crypt
 * with the keys if the crypto info is there.
 * The *crypt should be g_free'd by someone later.
 */
gboolean matrix_e2e_parse_media_decrypt_info(MatrixMediaCryptInfo **crypt,
                                             JsonObject *file_obj)
{
    const gchar *v_str = matrix_json_object_get_string_member(file_obj, "v");
    const gchar *iv_str = matrix_json_object_get_string_member(file_obj, "iv");
    /* RFC7517 JSON Web Key */
    JsonObject *key_obj = matrix_json_object_get_object_member(file_obj,
                                                               "key");
    JsonObject *hashes_obj = matrix_json_object_get_object_member(file_obj,
                                                               "hashes");
    const gchar *alg_str = matrix_json_object_get_string_member(key_obj, "alg");
    const gchar *kty_str = matrix_json_object_get_string_member(key_obj, "kty");
    const gchar *k_str = matrix_json_object_get_string_member(key_obj, "k");
    const gchar *sha256_str = matrix_json_object_get_string_member(hashes_obj,
                                                                   "sha256");
    if (!v_str || strcmp(v_str, "v2")) {
        purple_debug_info("matrixprpl", "bad media decrypt version\n");
        return FALSE;
    }
    if (!iv_str || !alg_str || !kty_str || !k_str || !sha256_str) {
        purple_debug_info("matrixprpl", "missing media decrypt field\n");
        /* Potentially this is OK, just not crypted */
        return TRUE;
    }
    if (strcmp(alg_str, "A256CTR") || strcmp(kty_str, "oct")) {
        purple_debug_info("matrixprpl", "media decrypt: bad alg/kty :%s/%s\n",
                alg_str, kty_str);
        return FALSE;
    }
    if (strlen(iv_str) != 22 || strlen(sha256_str) != 43 ||
        strlen(k_str) != 43) {
        purple_debug_info("matrixprpl", "media decrypt: Bad key/sha len:"
                " iv: %s sha256: %s k: %s\n", iv_str, sha256_str, k_str);
        return FALSE;
    }

    /* The k_str isn't a simple base64, it's a JSON web signature that needs
     * to be decoded first.
     */
    gchar tmp_str[48];
    gsize k_len, iv_len, sha256_len;
    matrix_json_jws_tobase64(tmp_str, k_str);
    guchar *decoded_k = g_base64_decode(tmp_str, &k_len);
    matrix_json_jws_tobase64(tmp_str, iv_str);
    guchar *decoded_iv = g_base64_decode(tmp_str, &iv_len);
    matrix_json_jws_tobase64(tmp_str, sha256_str);
    guchar *decoded_sha256 = g_base64_decode(tmp_str, &sha256_len);

    if (k_len != 32 || iv_len != 16 || sha256_len != 32) {
        purple_debug_info("matrixprpl", "media decrypt: Bad base64 decode:"
                " iv: %s=%zd sha256: %s=%zd k: %s=%zd\n",
                iv_str, iv_len, sha256_str, sha256_len,  k_str, k_len);
        g_free(decoded_k);
        g_free(decoded_iv);
        g_free(decoded_sha256);
        return FALSE;
    }
    purple_debug_info("matrixprpl", "media decrypt: got decrypt keys\n");

    *crypt = g_new0(MatrixMediaCryptInfo, 1);
    memcpy((*crypt)->sha256, decoded_sha256, 32);
    memcpy((*crypt)->aes_k, decoded_k, 32);
    memcpy((*crypt)->aes_iv, decoded_iv, 16);

    clear_mem((char *)decoded_k, sizeof(decoded_k));
    g_free(decoded_k);
    g_free(decoded_iv);
    g_free(decoded_sha256);

    return TRUE;
}

/* Decrypt media (in or inlen) into a buffer it allocates (*out)
 * returns NULL or an error string.
 */
const char *matrix_e2e_decrypt_media(MatrixMediaCryptInfo *crypt,
                                     size_t inlen, const void *in, void **out)
{
    char *fail_str = NULL;
    gcry_error_t gcry_err;
    gcry_cipher_hd_t cipher_hd;
    gboolean copen = FALSE;

    *out = NULL;
    gcry_err = gcry_cipher_open(&cipher_hd, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_CTR, 0);
    if (gcry_err) {
        fail_str = "failed to open cipher";
        goto err;
    }
    copen = TRUE;
    gcry_err = gcry_cipher_setkey(cipher_hd, crypt->aes_k, 32);
    if (gcry_err) {
        fail_str = "failed to set key";
        goto err;
    }
    /* Note: this is only working if we use setctr not setiv */
    gcry_err = gcry_cipher_setctr(cipher_hd, crypt->aes_iv, 16);
    if (gcry_err) {
        fail_str = "failed to set iv";
        goto err;
    }
    *out = g_malloc(inlen); /* Do I need to round this to block len? */
    gcry_cipher_final(cipher_hd);
    gcry_err = gcry_cipher_decrypt(cipher_hd, *out, inlen, in, inlen);
    if (gcry_err) {
        g_free(*out);
        fail_str = "failed to decrypt";
        goto err;
    }

    gcry_cipher_close(cipher_hd);

    return NULL;

err:
    g_free(*out);
    *out = NULL;
    if (copen) gcry_cipher_close(cipher_hd);

    return fail_str;
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

MatrixApiRequestData *matrix_e2e_send(MatrixConnectionData *conn,
        PurpleConversation *conv, const gchar *event_type,
        const gchar *txn_id, struct _JsonObject *content,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data)
{
    /* Without crypto support we can only send the event in the clear. */
    return matrix_api_send(conn, conv->name, event_type, txn_id, content,
            callback, error_callback, bad_response_callback, user_data);
}

void matrix_e2e_cleanup_conversation(PurpleConversation *conv)
{
}

gboolean matrix_e2e_parse_media_decrypt_info(MatrixMediaCryptInfo **crypt,
                                             JsonObject *file_obj)
{
    JsonObject *key_obj = matrix_json_object_get_object_member(file_obj,
                                                               "key");
    if (key_obj) {
        /* This has a key but with crypto turned off we'll have to fail */
        return FALSE;
    }

    return TRUE;
}

const char *matrix_e2e_decrypt_media(MatrixMediaCryptInfo *crypt,
                                     size_t inlen, const void *in, void **out)
{
    return "Crypto not available";
}


GList *matrix_e2e_actions(GList *list)
{
    return list;
}

#endif
