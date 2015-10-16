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

#ifndef MATRIX_API_H
#define MATRIX_API_H

/* libpurple */
#include "util.h"

#include "libmatrix.h"

struct _JsonNode;

/**
 * This is the signature used for functions that act as the callback
 * to the matrix api methods
 *
 * @param account       The MatrixAccount passed into the api method
 * @param user_data     The user data that your code passed into the api
 *                      method.
 * @param http_response -1 on error, otherwise this is the HTTP response code.
 * @param http_response_msg NULL on error, otherwise this is the message from
 *                      the HTTP response line.
 * @param body_start    NULL on error, or if there was no body in the response;
 *                      otherwise a pointer to the start of the body in the
 *                      response
 * @param json_root     NULL on error, or if there was no body in the response;
 *                      otherwise the root of the JSON tree in the response
 * @param error_message If something went wrong then this will contain
 *                      a descriptive error message, and ret_text will be
 *                      NULL and ret_len will be 0.
 */
typedef void (*MatrixApiCallback)(MatrixAccount *account,
                                  gpointer user_data,
                                  int http_response_code,
                                  const gchar *http_response_msg,
                                  const gchar *body_start,
                                  struct _JsonNode *json_root,
                                  const gchar *error_message);


/**
 * call the /sync API
 *
 * @param account    The MatrixAccount for which to make the request
 * @param callback   Function to be called when the request completes
 * @param user_data  Opaque data to be passed to the callback
 */
PurpleUtilFetchUrlData *matrix_sync(MatrixAccount *account,
                                    MatrixApiCallback callback,
                                    gpointer user_data);

#endif
