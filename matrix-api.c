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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 */

#include "matrix-api.h"

/* std lib */
#include <string.h>

/* json-glib */
#include <json-glib/json-glib.h>
#include <http_parser.h>

/* libpurple */
#include <debug.h>

#include "libmatrix.h"
#include "matrix-json.h"

typedef struct {
    MatrixAccount *account;
    MatrixApiCallback callback;
    gpointer user_data;
} MatrixApiRequestData;


/**
 * Default callback if there was an error calling the API. We just put the
 * connection into the "error" state.
 *
 * @param account             The MatrixAccount passed into the api method
 * @param user_data           The user data that your code passed into the api
 *                                method.
 * @param error_message    a descriptive error message
 *
 */
void matrix_api_error(MatrixAccount *ma, gpointer user_data,
        const gchar *error_message)
{
    purple_debug_info("matrixprpl", "Error calling API: %s\n",
                error_message);
    purple_connection_error_reason(ma->pc,
                PURPLE_CONNECTION_ERROR_NETWORK_ERROR, error_message);
}

/**
 * Default callback if the API returns a non-200 response. We just put the
 * connection into the "error" state.
 *
 * @param account             The MatrixAccount passed into the api method
 * @param user_data           The user data that your code passed into the api
 *                                method.
 * @param http_response       HTTP response code.
 * @param json_root           NULL if there was no body, or it could not be
 *                                parsed as JSON; otherwise the root of the JSON
 *                                tree in the response
 */
void matrix_api_bad_response(MatrixAccount *ma, gpointer user_data,
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
        error_message = g_strdup(_("Error from home server"));
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
        matrix_api_error(data->account, data->user_data, error_message);
    } else if(response_code >= 300) {
        matrix_api_bad_response(data->account, data->user_data,
                response_code, root);
    } else {
        (data->callback)(data->account, data->user_data, root);
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
 * @param max_len maximum number of bytes to return from the request. -1 for
 *                default (512K).
 */
static PurpleUtilFetchUrlData *matrix_api_start(const gchar *url,
                                                MatrixAccount *account,
                                                MatrixApiCallback callback,
                                                gpointer user_data,
                                                gssize max_len)
{
    MatrixApiRequestData *data;

    data = g_new0(MatrixApiRequestData, 1);
    data->account = account;
    data->callback = callback;
    data->user_data = user_data;

    /* TODO: implement the per-account proxy settings */
    return purple_util_fetch_url_request_len(url, TRUE, NULL, TRUE, NULL,
                                             TRUE, max_len,
                                             matrix_api_complete, data);
}


PurpleUtilFetchUrlData *matrix_api_sync(MatrixAccount *account,
        const gchar *since,
        MatrixApiCallback callback,
        gpointer user_data)
{
    GString *url;
    PurpleUtilFetchUrlData *fetch_data;
    
    url = g_string_new("");
    g_string_append_printf(url,
            "https://%s/_matrix/client/v2_alpha/sync?access_token=%s",
            account->homeserver, account->access_token);

    if(since != NULL)
        g_string_append_printf(url, "&timeout=30000&since=%s", since);

    purple_debug_info("matrixprpl", "request %s\n", url->str);

    /* XXX: stream the response, so that we don't need to allocate so much
     * memory? But it's JSON
     */
    fetch_data = matrix_api_start(url->str, account, callback, user_data,
                                  10*1024*1024);
    g_string_free(url, TRUE);
    
    return fetch_data;
}
