/**
 * matrix-api.h: Interface to the matrix client/server API.
 *
 * The intention is that this module provides an interface to the matrix API
 * without anything purple-specific.
 *
 * Each API method takes a 'MatrixAccount *'; this is used to determine the
 * URL of the homeserver, and the access_token which is used for authorisation.
 *
 * The methods are asyncronous, and take a callback to be called when the method
 * completes.
 *
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

#include "libmatrix.h"

struct _JsonNode;
struct _JsonObject;

/**
 * This is the signature used for functions that act as the callback
 * to the matrix api methods
 *
 * @param account       The MatrixAccount passed into the api method
 * @param user_data     The user data that your code passed into the api
 *                      method.
 * @param json_root     NULL if there was no body, or it could not be
 *                          parsed as JSON; otherwise the root of the JSON
 *                          tree in the response
 */
typedef void (*MatrixApiCallback)(MatrixAccount *account,
                                  gpointer user_data,
                                  struct _JsonNode *json_root);

/**
 * Signature for functions which are called when there is an error calling the
 * API (such as a connection failure)
 *
 * @param account          The MatrixAccount passed into the api method
 * @param user_data        The user data that your code passed into the api
 *                             method.
 * @param error_message    a descriptive error message
 *
 */
typedef void (*MatrixApiErrorCallback)(MatrixAccount *ma, gpointer user_data,
        const gchar *error_message);

/**
 * Default error callback. We just put the connection into the "error" state.
 */
void matrix_api_error(MatrixAccount *ma, gpointer user_data,
        const gchar *error_message);

/**
 * Signature for functions which are called when the API returns a non-200
 * response.
 *
 * @param account             The MatrixAccount passed into the api method
 * @param user_data           The user data that your code passed into the api
 *                                method.
 * @param http_response       HTTP response code.
 * @param json_root           NULL if there was no body, or it could not be
 *                                parsed as JSON; otherwise the root of the JSON
 *                                tree in the response
 */
typedef void (*MatrixApiBadResponseCallback)(MatrixAccount *ma,
        gpointer user_data, int http_response_code,
        struct _JsonNode *json_root);

/**
 * Default bad-response callback. We just put the connection into the "error"
 * state.
 */
void matrix_api_bad_response(MatrixAccount *ma, gpointer user_data,
        int http_response_code, struct _JsonNode *json_root);


/**
 * call the /login API
 *
 * @param account    The MatrixAccount for which to make the request
 * @param username   user id to pass in request
 * @param password   password to pass in request
 * @param callback   Function to be called when the request completes
 * @param user_data  Opaque data to be passed to the callback
 */
PurpleUtilFetchUrlData *matrix_api_password_login(MatrixAccount *account,
        const gchar *username,
        const gchar *password,
        MatrixApiCallback callback,
        gpointer user_data);



/**
 * call the /sync API
 *
 * @param account    The MatrixAccount for which to make the request
 * @param since      If non-null, the batch token to start sync from
 * @param timeout    Number of milliseconds after which the API will time out if
 *                      no events
 * @param callback   Function to be called when the request completes
 * @param user_data  Opaque data to be passed to the callback
 */
PurpleUtilFetchUrlData *matrix_api_sync(MatrixAccount *account,
        const gchar *since, int timeout,
        MatrixApiCallback callback,
        gpointer user_data);


/**
 * Send an event to a room
 *
 * @param account          The MatrixAccount for which to make the request
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
PurpleUtilFetchUrlData *matrix_api_send(MatrixAccount *account,
        const gchar *room_id, const gchar *event_type, const gchar *txn_id,
        struct _JsonObject *content,
        MatrixApiCallback callback,
        MatrixApiErrorCallback error_callback,
        MatrixApiBadResponseCallback bad_response_callback,
        gpointer user_data);
#endif
