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
    purple_debug_info("matrixprpl", "Error calling API: %s\n",
                error_message);
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

    purple_debug_info("matrixprpl", "API gave response %i\n",
            http_response_code);

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
    JsonParser *json_parser;
} MatrixApiResponseParserData;


/** create a MatrixApiResponseParserData */
static MatrixApiResponseParserData *_response_parser_data_new()
{
    MatrixApiResponseParserData *res = g_new(MatrixApiResponseParserData, 1);
    res->header_parsing_state = HEADER_PARSING_STATE_LAST_WAS_VALUE;
    res->current_header_name = g_string_new("");
    res->current_header_value = g_string_new("");
    res->content_type = NULL;
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
    return 0;
}


/**
 * callback from the http parser which handles the message body
 */
static int _handle_body(http_parser *http_parser, const char *at,
        size_t length)
{
    MatrixApiResponseParserData *response_data = http_parser->data;
    GError *err;

    if(purple_debug_is_verbose())
        purple_debug_info("matrixprpl", "Handling API response body %.*s\n",
                (int)length, at);

    if(strcmp(response_data->content_type, "application/json") == 0) {
        if(!json_parser_load_from_data(response_data -> json_parser, at, length,
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
                              "unable to parse HTTP response: %s\n",
                              http_errno_description(http_error));
            error_message = _("Error parsing response");
        } else {
            response_code = http_parser.status_code;
        }
    }

    if(!error_message) {
        root = json_parser_get_root(response_data -> json_parser);
    }

    if (error_message) {
        (data->error_callback)(data->conn, data->user_data, error_message);
    } else if(response_code >= 300) {
        (data->bad_response_callback)(data->conn, data->user_data,
                response_code, root);
    } else {
        (data->callback)(data->conn, data->user_data, root);
    }

    _response_parser_data_free(response_data);
    g_free(data);
}

/******************************************************************************
 *
 * API entry points
 */

/**
 * Start an HTTP call to the API
 *
 * @param request request headers and body to send, or NULL for default GET
 * @param max_len maximum number of bytes to return from the request. -1 for
 *                default (512K).
 */
static MatrixApiRequestData *matrix_api_start(const gchar *url,
        const gchar *request, MatrixConnectionData *conn,
        MatrixApiCallback callback, MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data, gssize max_len)
{
    MatrixApiRequestData *data;

    data = g_new0(MatrixApiRequestData, 1);
    data->conn = conn;
    data->callback = callback;
    data->error_callback = (error_callback == NULL ?
            matrix_api_error : error_callback);
    data->bad_response_callback = (bad_response_callback == NULL ?
            matrix_api_bad_response : bad_response_callback);
    data->user_data = user_data;

    data -> purple_data = purple_util_fetch_url_request_len_with_account(
            conn -> pc -> account,
            url, TRUE, NULL, TRUE, request, TRUE, max_len, matrix_api_complete,
            data);
    return data;
}

void matrix_api_cancel(MatrixApiRequestData *data)
{
    purple_util_fetch_url_cancel(data -> purple_data);
    data -> purple_data = NULL;
    (data->error_callback)(data->conn, data->user_data, "cancelled");

    g_free(data);
}


static void _add_proxy_auth_headers(GString *request_str, PurpleProxyInfo *gpi)
{
    PurpleProxyType type = purple_proxy_info_get_type(gpi);
    char *t1, *t2, *ntlm_type1;
    char hostname[256];
    int ret;

    if (purple_proxy_info_get_username(gpi) == NULL)
        return;

    if(type != PURPLE_PROXY_USE_ENVVAR && type != PURPLE_PROXY_HTTP)
        return;

    ret = gethostname(hostname, sizeof(hostname));
    hostname[sizeof(hostname) - 1] = '\0';
    if (ret < 0 || hostname[0] == '\0') {
          purple_debug_warning("util", "proxy - gethostname() failed -- is your hostname set?");
          strcpy(hostname, "localhost");
    }

    t1 = g_strdup_printf("%s:%s",
                purple_proxy_info_get_username(gpi),
                purple_proxy_info_get_password(gpi) ?
                        purple_proxy_info_get_password(gpi) : "");
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


static GString *_build_request(PurpleAccount *acct, const gchar *url,
        const gchar *method, const gchar *body)
{
    /* this is lifted from libpurple/util.c:url_fetch_send_cb. I wish libpurple
     * exposed it so that we didn't need to reinvent this wheel.
     */
    PurpleProxyInfo *gpi = purple_proxy_get_setup(acct);
    GString *request_str = g_string_new(NULL);
    gchar *url_host, *url_path, *url_user, *url_password;
    int url_port;

    purple_url_parse(url, &url_host, &url_port, &url_path, &url_user,
            &url_password);

    g_string_append_printf(request_str, "%s %s HTTP/1.1\r\n", method, url);
    g_string_append(request_str, "Connection: close\r\n");
    g_string_append_printf(request_str, "Host: %s\r\n", url_host);
    g_string_append_printf(request_str, "Content-Length: %zi\r\n",
            strlen(body));

    _add_proxy_auth_headers(request_str, gpi);
    g_string_append(request_str, "\r\n");
    g_string_append(request_str, body);

    return request_str;
}

gchar *_build_login_body(const gchar *username, const gchar *password)
{
    JsonObject *body;
    JsonNode *node;
    JsonGenerator *generator;
    gchar *result;

    body = json_object_new();
    json_object_set_string_member(body, "type", "m.login.password");
    json_object_set_string_member(body, "user", username);
    json_object_set_string_member(body, "password", password);
    node = json_node_alloc();
    json_node_init_object(node, body);
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
        MatrixApiCallback callback,
        gpointer user_data)
{
    gchar *url, *json;
    MatrixApiRequestData *fetch_data;
    GString *request;

    url = g_strconcat(conn->homeserver, "/_matrix/client/api/v1/login",
            NULL);

    json = _build_login_body(username, password);
    request = _build_request(conn->pc->account, url, "POST", json);
    g_free(json);

    if(purple_debug_is_unsafe())
        purple_debug_info("matrixprpl", "request %s\n", request->str);
    else
        purple_debug_info("matrixprpl", "logging in %s\n", username);

    fetch_data = matrix_api_start(url, request->str, conn, callback,
            NULL, NULL, user_data, 0);
    g_string_free(request, TRUE);
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
    
    url = g_string_new("");
    g_string_append_printf(url,
            "%s/_matrix/client/v2_alpha/sync?access_token=%s&timeout=%i",
            conn->homeserver, purple_url_encode(conn->access_token),
            timeout);

    if(since != NULL)
        g_string_append_printf(url, "&since=%s", purple_url_encode(since));

    if(full_state)
        g_string_append(url, "&full_state=true");

    purple_debug_info("matrixprpl", "syncing %s since %s (full_state=%i)\n",
                conn->pc->account->username, since, full_state);

    /* XXX: stream the response, so that we don't need to allocate so much
     * memory? But it's JSON
     */
    fetch_data = matrix_api_start(url->str, NULL, conn, callback,
            error_callback, bad_response_callback, user_data, 10*1024*1024);
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
    GString *request;
    JsonNode *body_node;
    JsonGenerator *generator;
    gchar *json;

    /* purple_url_encode uses a single static buffer, so we have to build up
     * the url gradually
     */
    url = g_string_new("");
    g_string_append_printf(url, "%s/_matrix/client/api/v1/rooms/",
            conn->homeserver);
    g_string_append(url, purple_url_encode(room_id));
    g_string_append(url, "/send/");
    g_string_append(url, purple_url_encode(event_type));
    g_string_append(url, "/");
    g_string_append(url, purple_url_encode(txn_id));
    g_string_append(url, "?access_token=");
    g_string_append(url, purple_url_encode(conn->access_token));

    body_node = json_node_alloc();
    json_node_init_object(body_node, content);

    generator = json_generator_new();
    json_generator_set_root(generator, body_node);
    json = json_generator_to_data(generator, NULL);
    g_object_unref(G_OBJECT(generator));
    json_node_free(body_node);

    request = _build_request(conn->pc->account, url->str, "PUT", json);
    g_free(json);

    purple_debug_info("matrixprpl", "sending %s on %s\n", event_type, room_id);

    fetch_data = matrix_api_start(url->str, request->str, conn, callback,
            error_callback, bad_response_callback,
            user_data, 0);
    g_string_free(request, TRUE);
    g_string_free(url, TRUE);

    return fetch_data;
}


MatrixApiRequestData *matrix_api_get_room_state(MatrixConnectionData *conn,
        const gchar *room_id,
        MatrixApiCallback callback,
        gpointer user_data)
{
    GString *url;
    MatrixApiRequestData *fetch_data;

    url = g_string_new(conn->homeserver);
    g_string_append(url, "/_matrix/client/api/v1/rooms/");
    g_string_append(url, purple_url_encode(room_id));
    g_string_append(url, "/state?access_token=");
    g_string_append(url, purple_url_encode(conn->access_token));

    purple_debug_info("matrixprpl", "getting state for %s\n", room_id);

    fetch_data = matrix_api_start(url->str, NULL, conn, callback,
            NULL, NULL, user_data, 10*1024*1024);
    g_string_free(url, TRUE);

    return fetch_data;
}
