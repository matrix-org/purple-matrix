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

#include <string.h>

#include "debug.h"

#include "libmatrix.h"

typedef struct {
    MatrixAccount *account;
    MatrixApiCallback callback;
    gpointer user_data;
} MatrixApiRequestData;


/*
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
    int response_code = -1;
    gchar *response_message = NULL;
    const gchar *body_pointer = NULL;

    if (!error_message) {
        /* parse the response line */
        gchar *response_line;
        gchar **splits;
        gchar *ptr;
        
        ptr = strchr(ret_data, '\r');
        response_line = g_strndup(ret_data,
                                  ptr == NULL ? ret_len : ptr - ret_data);
        splits = g_strsplit(response_line, " ", 3);

        if(splits[0] == NULL || splits[1] == NULL || splits[2] == NULL) {
            /* invalid response line */
            purple_debug_info("matrixprpl",
                              "unable to parse response line %s\n",
                              response_line);
            error_message = "Error parsing response";
        } else {
            response_code = strtol(splits[1], NULL, 10);
            response_message = g_strdup(splits[2]);
        }
        g_free(response_line);
        g_strfreev(splits);
    }

    if (!error_message) {
        /* find the gap between header and body */
        body_pointer = strstr(ret_data, "\r\n\r\n");
        if(body_pointer != NULL) {
            body_pointer += 4;
        } else {
            body_pointer = ret_data + ret_len;
        }
    }

    (data->callback)(data->account, data->user_data,
                     ret_data, ret_len,
                     response_code, response_message,
                     body_pointer, error_message);

    g_free(data);
    g_free(response_message);
}

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

    purple_debug_info("matrixprpl", "sending HTTP request to %s", url);
    
    return purple_util_fetch_url_request_len(url, TRUE, NULL, TRUE, NULL,
                                             TRUE, max_len,
                                             matrix_api_complete, data);
}


PurpleUtilFetchUrlData *matrix_initialsync(MatrixAccount *account,
                                           MatrixApiCallback callback,
                                           gpointer user_data)
{
    gchar *url;
    PurpleUtilFetchUrlData *fetch_data;
    
    url = g_strdup_printf("https://%s/_matrix/client/api/v1/initialSync?"
                          "access_token=%s",
                          account->homeserver, account->access_token);

    /* XXX: stream the response, so that we don't need to allocate so much
     * memory? But it's JSON
     */
    fetch_data = matrix_api_start(url, account, callback, user_data,
                                  10*1024*1024);
    g_free(url);
    
    return fetch_data;
}
