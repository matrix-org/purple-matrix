/**
 * matrix-api.h: Interface to the matrix client/server API.
 *
 * The intention is that this module provides an interface to the matrix API
 * without anything purple-specific.
 *
 * Each API method takes a 'MatrixConnectionData *'; this is used to determine
 * the URL of the homeserver, and the access_token which is used for
 * authorisation.
 *
 * The methods are asynchronous, and take a callback to be called when the
 * request completes.
 *
 * The methods may return NULL in the case of configuration errors, in which
 * case the 'error_callback' will have been called *before* the method returns
 * - so you should be careful not to access data structures which that callback
 * frees.
 *
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

#ifndef MATRIX_API_H
#define MATRIX_API_H

/* libpurple */
#include "util.h"

#include "matrix-connection.h"

struct _JsonNode;
struct _JsonObject;

typedef struct _MatrixApiRequestData MatrixApiRequestData;

/**
 * This is the signature used for functions that act as the callback
 * to the matrix api methods
 *
 * @param conn          The MatrixConnectionData passed into the api method
 * @param user_data     The user data that your code passed into the api
 *                      method.
 * @param json_root     NULL if there was no body, or it could not be
 *                          parsed as JSON; otherwise the root of the JSON
 *                          tree in the response
 * @param body          NULL if the body was parsable as JSON, else the raw
 *                          body.
 * @param body_len      The length of the body (valid when body is)
 *
 * @param content_type  The content type of the body
 */
typedef void (*MatrixApiCallback)(MatrixConnectionData *conn,
                                  gpointer user_data,
                                  struct _JsonNode *json_root,
                                  const char *body,
                                  size_t body_len, const char *content_type);

/**
 * Signature for functions which are called when there is an error calling the
 * API (such as a connection failure)
 *
 * @param conn             The MatrixConnectionData passed into the api method
 * @param user_data        The user data that your code passed into the api
 *                             method.
 * @param error_message    a descriptive error message
 *
 */
typedef void (*MatrixApiErrorCallback)(MatrixConnectionData *conn,
        gpointer user_data, const gchar *error_message);

/**
 * Default error callback. We just put the connection into the "error" state.
 */
void matrix_api_error(MatrixConnectionData *conn, gpointer user_data,
        const gchar *error_message);

/**
 * Signature for functions which are called when the API returns a non-200
 * response.
 *
 * @param conn            The MatrixConnectionData passed into the api method
 * @param user_data       The user data that your code passed into the api
 *                            method.
 * @param http_response   HTTP response code.
 * @param json_root       NULL if there was no body, or it could not be
 *                            parsed as JSON; otherwise the root of the JSON
 *                            tree in the response
 */
typedef void (*MatrixApiBadResponseCallback)(MatrixConnectionData *conn,
        gpointer user_data, int http_response_code,
        struct _JsonNode *json_root);

/**
 * Default bad-response callback. We just put the connection into the "error"
 * state.
 */
void matrix_api_bad_response(MatrixConnectionData *ma, gpointer user_data,
        int http_response_code, struct _JsonNode *json_root);





/**
 * Cancel a call to an API. This will also call the error_callback
 * with an error message of "cancelled".
 *
 * @param request   The result from an earlier matrix_api_* call
 */
void matrix_api_cancel(MatrixApiRequestData *request);


/**
 * call the /login API
 *
 * @param conn       The connection with which to make the request
 * @param username   user id to pass in request
 * @param password   password to pass in request
 * @param callback   Function to be called when the request completes
 * @param user_data  Opaque data to be passed to the callback
 */
MatrixApiRequestData *matrix_api_password_login(MatrixConnectionData *conn,
        const gchar *username,
        const gchar *password,
        const gchar *device_id,
        MatrixApiCallback callback,
        gpointer user_data);


/**
 * call the /sync API
 *
 * @param conn       The connection with which to make the request
 * @param since      If non-null, the batch token to start sync from
 * @param timeout    Number of milliseconds after which the API will time out if
 *                      no events
 * @param full_state       If true, will do a full state sync instead of an
 *                             incremental sync
 * @param callback         Function to be called when the request completes
 * @param error_callback   Function to be called if there is an error making
 *                             the request. If NULL, matrix_api_error will be
 *                             used.
 * @param bad_response_callback Function to be called if the API gives a non-200
 *                            response. If NULL, matrix_api_bad_response will be
 *                            used.
 * @param user_data  Opaque data to be passed to the callback
 */
MatrixApiRequestData *matrix_api_sync(MatrixConnectionData *conn,
        const gchar *since, int timeout, gboolean full_state,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data);


/**
 * Send an event to a room
 *
 * @param conn       The connection with which to make the request
 * @param room_id          The room to send the event to
 * @param event_type       The type of event (eg "m.room.message")
 * @param txn_id           Unique transaction id
 * @param content          The content of the event
 * @param callback         Function to be called when the request completes
 * @param error_callback   Function to be called if there is an error making
 *                             the request. If NULL, matrix_api_error will be
 *                             used.
 * @param bad_response_callback Function to be called if the API gives a non-200
 *                            response. If NULL, matrix_api_bad_response will be
 *                            used.
 * @param user_data        Opaque data to be passed to the callbacks
 */
MatrixApiRequestData *matrix_api_send(MatrixConnectionData *conn,
        const gchar *room_id, const gchar *event_type, const gchar *txn_id,
        struct _JsonObject *content,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data);

/**
 * Invite a user to a room
 *
 * @param conn             The connection with which to make the request
 * @param room_id          The room id to invite the user to
 *
 * @param who              The mxid of the person to invite
 *
 * @param callback         Function to be called when the request completes
 * @param error_callback   Function to be called if there is an error making
 *                             the request. If NULL, matrix_api_error will be
 *                             used.
 * @param bad_response_callback Function to be called if the API gives a non-200
 *                            response. If NULL, matrix_api_bad_response will be
 *                            used.
 * @param user_data        Opaque data to be passed to the callbacks
 */
void matrix_api_invite_user(MatrixConnectionData *conn,
        const gchar *room_id,
        const gchar *who,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data);

/**
 * Make a request to join a room
 *
 * @param conn             The connection with which to make the request
 * @param room             The room (id or alias) to join
 * @param callback         Function to be called when the request completes
 * @param error_callback   Function to be called if there is an error making
 *                             the request. If NULL, matrix_api_error will be
 *                             used.
 * @param bad_response_callback Function to be called if the API gives a non-200
 *                            response. If NULL, matrix_api_bad_response will be
 *                            used.
 * @param user_data        Opaque data to be passed to the callbacks
 */
MatrixApiRequestData *matrix_api_join_room(MatrixConnectionData *conn,
        const gchar *room,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data);

/**
 * Sends a typing notifiaction to a room
 *
 * @param conn             The connection with which to make the request
 * @param room_id          The room id to send the typing notification to
 * @param typing           Whether to mark the user as typing
 * @param typing_timeout   How long the server should mark the user as typing
 * @param callback         Function to be called when the request completes
 * @param error_callback   Function to be called if there is an error making
 *                             the request. If NULL, matrix_api_error will be
 *                             used.
 * @param bad_response_callback Function to be called if the API gives a non-200
 *                            response. If NULL, matrix_api_bad_response will be
 *                            used.
 * @param user_data        Opaque data to be passed to the callbacks
 */
MatrixApiRequestData *matrix_api_typing(MatrixConnectionData *conn,
        const gchar *room_id, gboolean typing,
        gint typing_timeout, MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data);


/**
 * Leave a room
 *
 * @param conn             The connection with which to make the request
 * @param room_id          The id of the room to leave
 * @param callback         Function to be called when the request completes
 * @param error_callback   Function to be called if there is an error making
 *                             the request. If NULL, matrix_api_error will be
 *                             used.
 * @param bad_response_callback Function to be called if the API gives a non-200
 *                            response. If NULL, matrix_api_bad_response will be
 *                            used.
 * @param user_data        Opaque data to be passed to the callbacks
 */
MatrixApiRequestData *matrix_api_leave_room(MatrixConnectionData *conn,
        const gchar *room_id,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data);


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
        const gchar *ctype,
        const gchar *data,
        gsize data_len,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data);


/* Get the complete download url for a given uri
 *
 * @param homeserver        The server hosting the file
 * @param uri               The file uri
 */
GString *get_download_url(const gchar *homeserver, const gchar *uri);


/**
 * Download a file
 *
 * @param conn             The connection with which to make the request
 * @param uri              The Matrix uri to fetch starting mxc://
 * @param max_size         A maximum size of file to receive.
 * @param callback         Function to be called when the request completes
 * @param error_callback   Function to be called if there is an error making
 *                             the request. If NULL, matrix_api_error will be
 *                             used.
 * @param bad_response_callback Function to be called if the API gives a non-200
 *                            response. If NULL, matrix_api_bad_response will be
 *                            used.
 * @param user_data        Opaque data to be passed to the callbacks
 *
 */
MatrixApiRequestData *matrix_api_download_file(MatrixConnectionData *conn,
        const gchar *uri,
        gsize max_size,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data);

/**
 * Download a thumbnail for a file
 *
 * @param conn             The connection with which to make the request
 * @param uri              The Matrix uri to fetch starting mxc://
 * @param max_size         A maximum size of file to receive.
 * @param width            Desired width; the server might not obey
 * @param height           Desired height; the server might not obey
 * @param scale            True to scale, false to crop
 * @param callback         Function to be called when the request completes
 * @param error_callback   Function to be called if there is an error making
 *                             the request. If NULL, matrix_api_error will be
 *                             used.
 * @param bad_response_callback Function to be called if the API gives a non-200
 *                            response. If NULL, matrix_api_bad_response will be
 *                            used.
 * @param user_data        Opaque data to be passed to the callbacks
 *
 */
MatrixApiRequestData *matrix_api_download_thumb(MatrixConnectionData *conn,
        const gchar *uri,
        gsize max_size,
        unsigned int width, unsigned int height, gboolean scale,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data);

/**
 * e2e: Upload keys; one or more of the device keys and the one time keys
 * @param conn             The connection with which to make the request
 * @param device_keys      (optional) Json Object with the signed device keys
 *                         device_keys gets unreferenced
 * @param one_time_keys    (optional) Json Object with one time key set
 *                         one_time_keys gets unreferenced
 * @param callback         Function to be called when the request completes
 * @param error_callback   Function to be called if there is an error making
 *                             the request. If NULL, matrix_api_error will be
 *                             used.
 * @param bad_response_callback Function to be called if the API gives a non-200
 *                            response. If NULL, matrix_api_bad_response will be
 *                            used.
 * @param user_data        Opaque data to be passed to the callbacks
 *
 */
MatrixApiRequestData *matrix_api_upload_keys(MatrixConnectionData *conn,
        struct _JsonObject *device_keys, struct _JsonObject *one_time_keys,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data);

#if 0
/**
 * Get the current state of a room
 *
 * @param conn             The connection with which to make the request
 * @param room_id          The room to get state for
 * @param callback         Function to be called when the request completes
 * @param user_data        Opaque data to be passed to the callbacks
 */
MatrixApiRequestData *matrix_api_get_room_state(MatrixConnectionData *conn,
        const gchar *room_id,
        MatrixApiCallback callback,
        gpointer user_data);
#endif

#endif
