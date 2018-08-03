/**
 * Interface to the matrix client/server API
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

#include "matrix-api.h"

/* std lib */
#include <string.h>

/* json-glib */
#include <json-glib/json-glib.h>
#include <http_parser.h>

/* libpurple */
#include <debug.h>
#include <ntlm.h>
#include <libpurple/version.h>

#include "libmatrix.h"
#include "matrix-json.h"

struct _MatrixApiRequestData {
    PurpleUtilFetchUrlData *purple_data;
    MatrixConnectionData *conn;
    MatrixApiCallback callback;
    MatrixApiErrorCallback error_callback;
    MatrixApiBadResponseCallback bad_response_callback;
    gpointer user_data;
};


/**
 * Default callback if there was an error calling the API. We just put the
 * connection into the "error" state.
 *
 */
void matrix_api_error(MatrixConnectionData *conn, gpointer user_data,
        const gchar *error_message)
{
    if(strcmp(error_message, "cancelled") != 0)
        purple_connection_error_reason(conn->pc,
            PURPLE_CONNECTION_ERROR_NETWORK_ERROR, error_message);
}

/**
 * Default callback if the API returns a non-200 response. We just put the
 * connection into the "error" state.
 */
void matrix_api_bad_response(MatrixConnectionData *ma, gpointer user_data,
        int http_response_code, JsonNode *json_root)
{
    JsonObject *json_obj;
    const gchar *errcode = NULL, *error = NULL;
    gchar *error_message;

    if(json_root != NULL) {
        json_obj = matrix_json_node_get_object(json_root);
        errcode = matrix_json_object_get_string_member(json_obj, "errcode");
        error = matrix_json_object_get_string_member(json_obj, "error");
    }

    if(errcode != NULL && error != NULL) {
        error_message = g_strdup_printf("%s: %s: %s",
                _("Error from home server"), errcode, error);
    } else {
        error_message = g_strdup_printf("%s: %i",
                _("Error from home server"), http_response_code);
    }

    purple_connection_error_reason(ma->pc,
            PURPLE_CONNECTION_ERROR_OTHER_ERROR,
            error_message);

    g_free(error_message);
}


/******************************************************************************
 *
 * HTTP response parsing
 */


#define HEADER_PARSING_STATE_LAST_WAS_VALUE 0
#define HEADER_PARSING_STATE_LAST_WAS_FIELD 1

typedef struct {
    int header_parsing_state;
    GString *current_header_name;
    GString *current_header_value;
    gchar *content_type;
    gboolean got_headers;
    JsonParser *json_parser;
    char *body;
    size_t body_len;
} MatrixApiResponseParserData;


/** create a MatrixApiResponseParserData */
static MatrixApiResponseParserData *_response_parser_data_new()
{
    MatrixApiResponseParserData *res = g_new0(MatrixApiResponseParserData, 1);
    res->header_parsing_state = HEADER_PARSING_STATE_LAST_WAS_VALUE;
    res->current_header_name = g_string_new("");
    res->current_header_value = g_string_new("");
    res->json_parser = json_parser_new();
    return res;
}

/** free a MatrixApiResponseParserData */
static void _response_parser_data_free(MatrixApiResponseParserData *data)
{
    if(data == NULL)
        return;

    g_string_free(data->current_header_name, TRUE);
    g_string_free(data->current_header_value, TRUE);
    g_free(data->content_type);

    /* free the JSON parser, and all of the node structures */
    if(data -> json_parser)
        g_object_unref(data -> json_parser);
    g_free(data->body);
    data->body = NULL;

    g_free(data);
}

static void _handle_header_completed(MatrixApiResponseParserData *response_data)
{
    const gchar *name = response_data->current_header_name->str,
            *value = response_data->current_header_value->str;

    if(*name == '\0') {
        /* nothing to do here */
        return;
    }

    if(purple_debug_is_verbose())
        purple_debug_info("matrixprpl", "Handling API response header %s: %s\n",
                name, value);

    if(strcmp(name, "Content-Type") == 0) {
        g_free(response_data->content_type);
        response_data->content_type = g_strdup(value);
    }
}

/**
 * callback from the http parser which handles a header name
 */
static int _handle_header_field(http_parser *http_parser, const char *at,
        size_t length)
{
    MatrixApiResponseParserData *response_data = http_parser->data;

    if (response_data->header_parsing_state ==
            HEADER_PARSING_STATE_LAST_WAS_VALUE) {
        /* starting a new header */
        _handle_header_completed(response_data);

        g_string_truncate(response_data -> current_header_name, 0);
        g_string_truncate(response_data -> current_header_value, 0);
    }

    g_string_append_len(response_data -> current_header_name, at, length);
    response_data->header_parsing_state = HEADER_PARSING_STATE_LAST_WAS_FIELD;
    return 0;
}

/**
 * callback from the http parser which handles a header value
 */
static int _handle_header_value(http_parser *http_parser, const char *at,
        size_t length)
{
    MatrixApiResponseParserData *response_data = http_parser->data;
    g_string_append_len(response_data -> current_header_value, at, length);
    response_data->header_parsing_state = HEADER_PARSING_STATE_LAST_WAS_VALUE;
    return 0;
}


static int _handle_headers_complete(http_parser *http_parser)
{
    MatrixApiResponseParserData *response_data = http_parser->data;
    _handle_header_completed(response_data);
    response_data->got_headers = TRUE;
    return 0;
}


/**
 * callback from the http parser which handles the message body
 * Can be called multiple times as we accumulate chunks.
 */
static int _handle_body(http_parser *http_parser, const char *at,
        size_t length)
{
    MatrixApiResponseParserData *response_data = http_parser->data;
    if(purple_debug_is_verbose())
        purple_debug_info("matrixprpl", "Handling API response body %.*s\n",
                (int)length, at);

    response_data->body = g_realloc(response_data->body,
                                    response_data->body_len + length);
    memcpy(response_data->body + response_data->body_len, at, length);
    response_data->body_len += length;

    return 0;
}

/**
 * callback from the http parser after all chunks have arrived.
 */
static int _handle_message_complete(http_parser *http_parser)
{
    MatrixApiResponseParserData *response_data = http_parser->data;
    GError *err = NULL;

    if(strcmp(response_data->content_type, "application/json") == 0) {
        if(!json_parser_load_from_data(response_data -> json_parser,
                                       response_data->body,
                                       response_data->body_len,
                &err)) {
            purple_debug_info("matrixprpl", "unable to parse JSON: %s\n",
                    err->message);
            g_error_free(err);
            return 1;
        }
    }
    return 0;
}


/**
 * The callback we give to purple_util_fetch_url_request - does some
 * initial processing of the response
 */
static void matrix_api_complete(PurpleUtilFetchUrlData *url_data,
                                gpointer user_data,
                                const gchar *ret_data,
                                gsize ret_len,
                                const gchar *error_message)
{
    MatrixApiRequestData *data = (MatrixApiRequestData *)user_data;
    MatrixApiResponseParserData *response_data = NULL;
    int response_code = -1;
    JsonNode *root = NULL;
    
    if(error_message) {
        purple_debug_warning("matrixprpl", "Error from http request: %s\n",
                error_message);
    } else if (purple_debug_is_verbose()) {
        purple_debug_info("matrixprpl", "Got response: %s\n", ret_data);
    }

    if(!error_message) {
        http_parser http_parser;
        http_parser_settings http_parser_settings;
        enum http_errno http_error;

        memset(&http_parser, 0, sizeof(http_parser));
        memset(&http_parser_settings, 0, sizeof(http_parser_settings));

        response_data = _response_parser_data_new();

        http_parser_settings.on_header_field = _handle_header_field;
        http_parser_settings.on_header_value = _handle_header_value;
        http_parser_settings.on_headers_complete = _handle_headers_complete;
        http_parser_settings.on_body = _handle_body;
        http_parser_settings.on_message_complete = _handle_message_complete;

        http_parser_init(&http_parser, HTTP_RESPONSE);
        http_parser.data = response_data;
        http_parser_execute(&http_parser, &http_parser_settings,
                ret_data, ret_len);

        /* we have to do a separate call to tell the parser that we've got to
         * EOF.
         */
        http_parser_execute(&http_parser, &http_parser_settings, ret_data, 0);

        http_error = HTTP_PARSER_ERRNO(&http_parser);
        if(http_error != HPE_OK) {
            /* invalid response line */
            purple_debug_info("matrixprpl",
                              "Error (%s) parsing HTTP response %s\n",
                              http_errno_description(http_error), ret_data);
            error_message = _("Invalid response from homeserver");
        } else if (!response_data->got_headers) {
            /* this will happen if we hit EOF before the end of the headers */
            purple_debug_info("matrixprpl",
                              "EOF before end of HTTP headers in response %s\n",
                              ret_data);
            error_message = _("Invalid response from homeserver");

        } else {
            response_code = http_parser.status_code;
        }
    }

    if(!error_message) {
        root = json_parser_get_root(response_data -> json_parser);
    }

    if (error_message) {
        purple_debug_info("matrixprpl", "Handling error: %s\n", error_message);
        (data->error_callback)(data->conn, data->user_data, error_message);
    } else if(response_code >= 300) {
        purple_debug_info("matrixprpl", "API gave response %i\n",
                response_code);
        (data->bad_response_callback)(data->conn, data->user_data,
                response_code, root);
    } else if (data->callback) {
        (data->callback)(data->conn, data->user_data, root,
                         response_data->body, response_data->body_len,
                         response_data->content_type );
    }

    _response_parser_data_free(response_data);
    g_free(data);
}

/******************************************************************************
 *
 * API entry points
 */

/*
 * Add proxy authentication headers to a request
 */
static void _add_proxy_auth_headers(GString *request_str, PurpleProxyInfo *gpi)
{
    const char *username, *password;
    char *t1, *t2, *ntlm_type1;
    const gchar *hostname;

    username = purple_proxy_info_get_username(gpi);
    password = purple_proxy_info_get_password(gpi);
    if (username == NULL)
        return;

    hostname = g_get_host_name();

    t1 = g_strdup_printf("%s:%s", username, password ? password : "");
    t2 = purple_base64_encode((const guchar *)t1, strlen(t1));
    g_free(t1);

    ntlm_type1 = purple_ntlm_gen_type1(hostname, "");
    g_string_append_printf(request_str,
            "Proxy-Authorization: Basic %s\r\n"
            "Proxy-Authorization: NTLM %s\r\n"
            "Proxy-Connection: Keep-Alive\r\n",
            t2, ntlm_type1);
    g_free(ntlm_type1);
    g_free(t2);
}


/**
 * parse a URL as much as we need
 *
 * @param url    url to be parsed
 * @param host   returns a pointer to the start of the hostname, or NULL if none
 * @param path   returns a pointer to the start of the path
 */
static void _parse_url(const gchar *url, const gchar **host, const gchar **path)
{
    const gchar *ptr;

    /* find the separator between the host and the path */
    /* first find the end of the scheme */
    ptr = url;
    while(*ptr != ':' && *ptr != '/' && *ptr != '\0')
        ptr++;

    if(*ptr != ':') {
        /* no scheme, so presumably no hostname - it's a relative path */
        *host = NULL;
        *path = ptr;
        return;
    }

    /* the url has a scheme, which implies it also has a hostname */
    ptr++;
    while(*ptr == '/')
        ptr++;
    *host = ptr;
    /* skip the rest of the hostname. The path starts at the next /. */
    while(*ptr != '/' && *ptr != '\0')
        ptr++;
    *path = ptr;
}


/**
 * We have to build our own HTTP requests because:
 *   - libpurple only supports GET
 *   - libpurple's purple_url_parse assumes that the path + querystring is
 *     shorter than 256 bytes.
 *
 *  @returns a GString* which should be freed
 */
static GString *_build_request(PurpleAccount *acct, const gchar *url,
        const gchar *method, const gchar *extra_headers,
        const gchar *body,
        const gchar *extra_data, gsize extra_len)
{
    PurpleProxyInfo *gpi = purple_proxy_get_setup(acct);
    GString *request_str = g_string_new(NULL);
    const gchar *url_host, *url_path;
    gboolean using_http_proxy = FALSE;

    if(gpi != NULL) {
        PurpleProxyType type = purple_proxy_info_get_type(gpi);
        using_http_proxy = (type == PURPLE_PROXY_USE_ENVVAR
                || type == PURPLE_PROXY_HTTP);
    }

    _parse_url(url, &url_host, &url_path);

    /* we only support absolute URLs (with schemes) */
    g_assert(url_host != NULL);

    /* If we are connecting via a proxy, we should put the whole url
     * in the request line. (But synapse chokes if we do that on a direct
     * connection.)
     */
    g_string_append_printf(request_str, "%s %s HTTP/1.1\r\n",
            method, using_http_proxy ? url : url_path);
    g_string_append_printf(request_str, "Host: %.*s\r\n",
            (int)(url_path-url_host), url_host);

    if (extra_headers != NULL)
        g_string_append(request_str, extra_headers);
    g_string_append(request_str, "Connection: close\r\n");
    g_string_append_printf(request_str, "Content-Length: %" G_GSIZE_FORMAT "\r\n",
            extra_len + (body == NULL ? 0 : strlen(body)));

    if(using_http_proxy)
        _add_proxy_auth_headers(request_str, gpi);

    g_string_append(request_str, "\r\n");
    if(body != NULL)
        g_string_append(request_str, body);

    if(extra_data != NULL)
        g_string_append_len(request_str, extra_data, extra_len);

    return request_str;
}


/**
 * Start an HTTP call to the API
 *
 * @param method      HTTP method (eg "GET")
 * @param extra_headers  Extra HTTP headers to add
 * @param body        body of request, or NULL if none
 * @param extra_data  raw binary data to be sent after the body
 * @param extra_len   The length of the raw binary data
 * @param max_len     maximum number of bytes to return from the request. -1 for
 *                    default (512K).
 *
 * @returns handle for the request, or NULL if the request couldn't be started
 *   (eg, invalid hostname). In this case, the error_callback will have
 *   been called already.
 * Note: extra_data/extra_len is only available on libpurple >=2.11.0
 */
static MatrixApiRequestData *matrix_api_start_full(const gchar *url,
        const gchar *method, const gchar *extra_headers,
        const gchar *body,
        const gchar *extra_data, gsize extra_len,
        MatrixConnectionData *conn,
        MatrixApiCallback callback, MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data, gssize max_len)
{
    MatrixApiRequestData *data;
    GString *request;
    PurpleUtilFetchUrlData *purple_data;

    if (error_callback == NULL)
        error_callback = matrix_api_error;
    if (bad_response_callback == NULL)
        bad_response_callback = matrix_api_bad_response;

    /* _build_request assumes the url is absolute, so enforce that here */
    if(!g_str_has_prefix(url, "http://") &&
            !g_str_has_prefix(url, "https://")) {
        gchar *error_msg;
        error_msg = g_strdup_printf(_("Invalid homeserver URL %s"), url);
        error_callback(conn, user_data, error_msg);
        g_free(error_msg);
        return NULL;
    }

#if !PURPLE_VERSION_CHECK(2,11,0)
    if (extra_len) {
        gchar *error_msg;
        error_msg = g_strdup_printf(_("Feature not available on old purple version"));
        error_callback(conn, user_data, error_msg);
        g_free(error_msg);
        return NULL;
    }
#endif

    request = _build_request(conn->pc->account, url, method, extra_headers,
                             body, extra_data, extra_len);

    if(purple_debug_is_unsafe())
        purple_debug_info("matrixprpl", "request %s\n", request->str);


    data = g_new0(MatrixApiRequestData, 1);
    data->conn = conn;
    data->callback = callback;
    data->error_callback = error_callback;
    data->bad_response_callback = bad_response_callback;
    data->user_data = user_data;

#if PURPLE_VERSION_CHECK(2,11,0)
    purple_data = purple_util_fetch_url_request_data_len_with_account(
            conn -> pc -> account,
            url, FALSE, NULL, TRUE, request->str, request->len,
            TRUE, max_len, matrix_api_complete,
            data);
#else
    purple_data = purple_util_fetch_url_request_len_with_account(
            conn -> pc -> account,
            url, FALSE, NULL, TRUE, request->str, TRUE,
            max_len, matrix_api_complete,
            data);
#endif

    if(purple_data == NULL) {
        /* we couldn't start the request. In this case, our callback will
         * already have been called, which will have freed data.
         */
        data = NULL;
    } else {
        data->purple_data = purple_data;
    }

    g_string_free(request, TRUE);
    return data;
}


/**
 * Start an HTTP call to the API; lighter version of matrix_api_start_full
 * since most callers don't need the extras.
 *
 * @param method      HTTP method (eg "GET")
 * @param body        body of request, or NULL if none
 * @param max_len     maximum number of bytes to return from the request. -1 for
 *                    default (512K).
 *
 * @returns handle for the request, or NULL if the request couldn't be started
 *   (eg, invalid hostname). In this case, the error_callback will have
 *   been called already.
 */
static MatrixApiRequestData *matrix_api_start(const gchar *url,
        const gchar *method, const gchar *body,
        MatrixConnectionData *conn,
        MatrixApiCallback callback, MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data, gssize max_len)
{
    return matrix_api_start_full(url, method, NULL, body, NULL, 0, conn,
            callback, error_callback, bad_response_callback,
            user_data, max_len);
}


void matrix_api_cancel(MatrixApiRequestData *data)
{
    if(data -> purple_data != NULL)
        purple_util_fetch_url_cancel(data -> purple_data);
    data -> purple_data = NULL;
    (data->error_callback)(data->conn, data->user_data, "cancelled");

    g_free(data);
}


gchar *_build_login_body(const gchar *username, const gchar *password, const gchar *device_id)
{
    JsonObject *body;
    JsonNode *node;
    JsonGenerator *generator;
    gchar *result;

    body = json_object_new();
    json_object_set_string_member(body, "type", "m.login.password");
    json_object_set_string_member(body, "user", username);
    json_object_set_string_member(body, "password", password);
    json_object_set_string_member(body, "initial_device_display_name", "purple-matrix");
    if (device_id != NULL)
        json_object_set_string_member(body, "device_id", device_id);
    
    node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(node, body);
    json_object_unref(body);

    generator = json_generator_new();
    json_generator_set_root(generator, node);
    result = json_generator_to_data(generator, NULL);
    g_object_unref(G_OBJECT(generator));
    json_node_free(node);
    return result;
}

MatrixApiRequestData *matrix_api_password_login(MatrixConnectionData *conn,
        const gchar *username,
        const gchar *password,
        const gchar *device_id,
        MatrixApiCallback callback,
        gpointer user_data)
{
    gchar *url, *json;
    MatrixApiRequestData *fetch_data;

    purple_debug_info("matrixprpl", "logging in %s\n", username);

    // As per https://github.com/matrix-org/synapse/pull/459, synapse
    // didn't expose login at 'r0'.
    url = g_strconcat(conn->homeserver, "_matrix/client/api/v1/login",
            NULL);

    json = _build_login_body(username, password, device_id);

    fetch_data = matrix_api_start(url, "POST", json, conn, callback,
            NULL, NULL, user_data, 0);
    g_free(json);
    g_free(url);

    return fetch_data;
}


MatrixApiRequestData *matrix_api_sync(MatrixConnectionData *conn,
        const gchar *since, int timeout, gboolean full_state,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data)
{
    GString *url;
    MatrixApiRequestData *fetch_data;

    url = g_string_new(conn->homeserver);
    g_string_append_printf(url,
            "_matrix/client/r0/sync?access_token=%s&timeout=%i",
            purple_url_encode(conn->access_token), timeout);

    if(since != NULL)
        g_string_append_printf(url, "&since=%s", purple_url_encode(since));

    if(full_state)
        g_string_append(url, "&full_state=true");

    purple_debug_info("matrixprpl", "syncing %s since %s (full_state=%i)\n",
                conn->pc->account->username, since, full_state);

    /* XXX: stream the response, so that we don't need to allocate so much
     * memory? But it's JSON
     */
    fetch_data = matrix_api_start(url->str, "GET", NULL, conn, callback,
            error_callback, bad_response_callback, user_data, 40*1024*1024);
    g_string_free(url, TRUE);
    
    return fetch_data;
}


MatrixApiRequestData *matrix_api_send(MatrixConnectionData *conn,
        const gchar *room_id, const gchar *event_type, const gchar *txn_id,
        JsonObject *content, MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data)
{
    GString *url;
    MatrixApiRequestData *fetch_data;
    JsonNode *body_node;
    JsonGenerator *generator;
    gchar *json;

    /* purple_url_encode uses a single static buffer, so we have to build up
     * the url gradually
     */
    url = g_string_new(conn->homeserver);
    g_string_append(url, "_matrix/client/r0/rooms/");
    g_string_append(url, purple_url_encode(room_id));
    g_string_append(url, "/send/");
    g_string_append(url, purple_url_encode(event_type));
    g_string_append(url, "/");
    g_string_append(url, purple_url_encode(txn_id));
    g_string_append(url, "?access_token=");
    g_string_append(url, purple_url_encode(conn->access_token));

    body_node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(body_node, content);

    generator = json_generator_new();
    json_generator_set_root(generator, body_node);
    json = json_generator_to_data(generator, NULL);
    g_object_unref(G_OBJECT(generator));
    json_node_free(body_node);

    purple_debug_info("matrixprpl", "sending %s on %s\n", event_type, room_id);

    fetch_data = matrix_api_start(url->str, "PUT", json, conn, callback,
            error_callback, bad_response_callback,
            user_data, 0);
    g_free(json);
    g_string_free(url, TRUE);

    return fetch_data;
}

void matrix_api_invite_user(MatrixConnectionData *conn,
        const gchar *room_id,
        const gchar *who,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data)
{
    GString *url;
    JsonNode *body_node;
    JsonGenerator *generator;
    gchar *json;

    JsonObject *invitee;
    invitee = json_object_new();
    json_object_set_string_member(invitee, "user_id", who);

    url = g_string_new(conn->homeserver);
    g_string_append(url, "_matrix/client/r0/rooms/");
    g_string_append(url, purple_url_encode(room_id));
    g_string_append(url, "/invite?access_token=");
    g_string_append(url, purple_url_encode(conn->access_token));

    body_node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(body_node, invitee);

    generator = json_generator_new();
    json_generator_set_root(generator, body_node);
    json = json_generator_to_data(generator, NULL);
    g_object_unref(G_OBJECT(generator));
    json_node_free(body_node);

    purple_debug_info("matrixprpl", "sending an invite on %s\n", room_id);

    matrix_api_start(url->str, "POST", json, conn, callback,
            error_callback, bad_response_callback,
            user_data, 0);
    g_free(json);
    g_string_free(url, TRUE);
    json_object_unref(invitee);
}

MatrixApiRequestData *matrix_api_join_room(MatrixConnectionData *conn,
        const gchar *room,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data)
{
    GString *url;
    MatrixApiRequestData *fetch_data;

    url = g_string_new(conn->homeserver);
    g_string_append(url, "_matrix/client/r0/join/");
    g_string_append(url, purple_url_encode(room));
    g_string_append(url, "?access_token=");
    g_string_append(url, purple_url_encode(conn->access_token));

    purple_debug_info("matrixprpl", "joining %s\n", room);

    fetch_data = matrix_api_start(url->str, "POST", "{}", conn, callback,
            error_callback, bad_response_callback,
            user_data, 0);
    g_string_free(url, TRUE);

    return fetch_data;
}

MatrixApiRequestData *matrix_api_typing(MatrixConnectionData *conn,
        const gchar *room_id, gboolean typing,
        gint typing_timeout, MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data)
{
    GString *url;
    MatrixApiRequestData *fetch_data;
    JsonNode *body_node;
    JsonGenerator *generator;
    gchar *json;
    JsonObject *content;

    /* purple_url_encode uses a single static buffer, so we have to build up
     * the url gradually
     */
    url = g_string_new(conn->homeserver);
    g_string_append(url, "_matrix/client/r0/rooms/");
    g_string_append(url, purple_url_encode(room_id));
    g_string_append(url, "/typing/");
    g_string_append(url, purple_url_encode(conn->user_id));
    g_string_append(url, "?access_token=");
    g_string_append(url, purple_url_encode(conn->access_token));

    body_node = json_node_new(JSON_NODE_OBJECT);
    content = json_object_new();
    json_object_set_boolean_member(content, "typing", typing);
    if (typing == TRUE) {
        json_object_set_int_member(content, "timeout", typing_timeout);
    }
    json_node_set_object(body_node, content);

    generator = json_generator_new();
    json_generator_set_root(generator, body_node);
    json = json_generator_to_data(generator, NULL);
    g_object_unref(G_OBJECT(generator));
    json_node_free(body_node);

    purple_debug_info("matrixprpl", "typing in %s\n", room_id);

    fetch_data = matrix_api_start(url->str, "PUT", json, conn, callback,
            error_callback, bad_response_callback,
            user_data, 0);
    g_free(json);
    g_string_free(url, TRUE);
    json_object_unref(content);

    return fetch_data;
}


MatrixApiRequestData *matrix_api_leave_room(MatrixConnectionData *conn,
        const gchar *room_id,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data)
{
    GString *url;
    MatrixApiRequestData *fetch_data;

    url = g_string_new(conn->homeserver);
    g_string_append(url, "_matrix/client/r0/rooms/");
    g_string_append(url, purple_url_encode(room_id));
    g_string_append(url, "/leave?access_token=");
    g_string_append(url, purple_url_encode(conn->access_token));

    purple_debug_info("matrixprpl", "leaving %s\n", room_id);

    fetch_data = matrix_api_start(url->str, "POST", "{}", conn, callback,
            error_callback, bad_response_callback,
            user_data, 0);
    g_string_free(url, TRUE);

    return fetch_data;
}

/**
 * Upload a file
 *
 * @param conn             The connection with which to make the request
 * @param ctype            Content type of file
 * @param data             Raw data content of file
 * @param data_len         Length of the data
 * @param callback         Function to be called when the request completes
 * @param user_data        Opaque data to be passed to the callback
 */
MatrixApiRequestData *matrix_api_upload_file(MatrixConnectionData *conn,
        const gchar *ctype, const gchar *data, gsize data_len,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data)
{
    GString *url, *extra_header;
    MatrixApiRequestData *fetch_data;

    url = g_string_new(conn->homeserver);
    g_string_append(url, "_matrix/media/r0/upload");
    g_string_append(url, "?access_token=");
    g_string_append(url, purple_url_encode(conn->access_token));

    extra_header = g_string_new("Content-Type: ");
    g_string_append(extra_header, ctype);
    g_string_append(extra_header, "\r\n");

    fetch_data = matrix_api_start_full(url->str, "POST", extra_header->str, "",
            data, data_len, conn,
            callback, error_callback, bad_response_callback, user_data, 0);
    g_string_free(url, TRUE);
    g_string_free(extra_header, TRUE);

    return fetch_data;
}

GString *get_download_url(const gchar *homeserver, const gchar *uri)
{
    GString *url;

    /* Sanity check the uri - TODO: Add more sanity */
    if (strncmp(uri, "mxc://", 6)) {
        return NULL;
    }
    url = g_string_new(homeserver);
    g_string_append(url, "_matrix/media/r0/download/");
    g_string_append(url, uri + 6); /* i.e. after the mxc:// */
    return url;
}

/**
 * Download a file
 * @param uri       URI string in the form mxc://example.com/unique
 */
MatrixApiRequestData *matrix_api_download_file(MatrixConnectionData *conn,
        const gchar *uri,
        gsize max_size,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data)
{
    GString *url;
    MatrixApiRequestData *fetch_data;

    url = get_download_url(conn->homeserver, uri);
    if (!url) {
        error_callback(conn, user_data, "bad media uri");
        return NULL;
    }

    /* I'd like to validate the headers etc a bit before downloading the
     * data (maybe using _handle_header_completed), also I'm not convinced
     * purple always does sane things on over-size.
     */
    fetch_data = matrix_api_start(url->str, "GET", NULL, conn, callback,
            error_callback, bad_response_callback, user_data, max_size);
    g_string_free(url, TRUE);

    return fetch_data;
}

/**
 * Download a thumbnail for a file
 * @param uri       URI string in the form mxc://example.com/unique
 */
MatrixApiRequestData *matrix_api_download_thumb(MatrixConnectionData *conn,
        const gchar *uri,
        gsize max_size,
        unsigned int width, unsigned int height, gboolean scale,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data)
{
    GString *url;
    MatrixApiRequestData *fetch_data;
    char tmp[64];

    /* Sanity check the uri - TODO: Add more sanity */
    if (strncmp(uri, "mxc://", 6)) {
        error_callback(conn, user_data, "bad media uri");
        return NULL;
    }
    url = g_string_new(conn->homeserver);
    g_string_append(url, "_matrix/media/r0/thumbnail/");
    g_string_append(url, uri + 6); /* i.e. after the mxc:// */
    sprintf(tmp, "?width=%u", width);
    g_string_append(url, tmp);
    sprintf(tmp, "&height=%u", height);
    g_string_append(url, tmp);
    g_string_append(url, scale ? "&method=scale": "&method=crop");

    /* I'd like to validate the headers etc a bit before downloading the
     * data (maybe using _handle_header_completed), also I'm not convinced
     * purple always does sane things on over-size.
     */
    fetch_data = matrix_api_start(url->str, "GET", NULL, conn, callback,
            error_callback, bad_response_callback, user_data, max_size);
    g_string_free(url, TRUE);

    return fetch_data;
}

MatrixApiRequestData *matrix_api_upload_keys(MatrixConnectionData *conn,
        JsonObject *device_keys, JsonObject *one_time_keys,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data)
{
    GString *url;
    MatrixApiRequestData *fetch_data;
    JsonNode *body_node;
    JsonObject *top_obj;
    JsonGenerator *generator;
    gchar *json;

    url = g_string_new(conn->homeserver);
    g_string_append(url, "_matrix/client/r0/keys/upload?access_token=");
    g_string_append(url, purple_url_encode(conn->access_token));

    top_obj = json_object_new();
    if (device_keys) {
        json_object_set_object_member(top_obj, "device_keys", device_keys);
    }
    if (one_time_keys) {
        json_object_set_object_member(top_obj, "one_time_keys", one_time_keys);
    }
    body_node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(body_node, top_obj);
    json_object_unref(top_obj);

    generator = json_generator_new();
    json_generator_set_root(generator, body_node);
    json = json_generator_to_data(generator, NULL);
    g_object_unref(G_OBJECT(generator));
    json_node_free(body_node);

    fetch_data = matrix_api_start_full(url->str, "POST",
            "Content-Type: application/json", json, NULL, 0,
            conn, callback, error_callback, bad_response_callback,
            user_data, 1024);
    g_free(json);
    g_string_free(url, TRUE);

    return fetch_data;
}


#if 0
MatrixApiRequestData *matrix_api_get_room_state(MatrixConnectionData *conn,
        const gchar *room_id,
        MatrixApiCallback callback,
        gpointer user_data)
{
    GString *url;
    MatrixApiRequestData *fetch_data;

    url = g_string_new(conn->homeserver);
    g_string_append(url, "/_matrix/client/r0/rooms/");
    g_string_append(url, purple_url_encode(room_id));
    g_string_append(url, "/state?access_token=");
    g_string_append(url, purple_url_encode(conn->access_token));

    purple_debug_info("matrixprpl", "getting state for %s\n", room_id);

    fetch_data = matrix_api_start(url->str, NULL, conn, callback,
            NULL, NULL, user_data, 10*1024*1024);
    g_string_free(url, TRUE);

    return fetch_data;
}
#endif
