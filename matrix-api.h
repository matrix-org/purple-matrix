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
 * @param json_root     NULL if there was no body, or it could not be
 *                          parsed as JSON; otherwise the root of the JSON
 *                          tree in the response
 */
typedef void (*MatrixApiCallback)(MatrixAccount *account,
                                  gpointer user_data,
                                  struct _JsonNode *json_root);


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
 * @param callback   Function to be called when the request completes
 * @param user_data  Opaque data to be passed to the callback
 */
PurpleUtilFetchUrlData *matrix_api_sync(MatrixAccount *account,
        const gchar *since,
        MatrixApiCallback callback,
        gpointer user_data);

#endif
