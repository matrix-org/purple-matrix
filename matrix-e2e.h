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

#ifndef MATRIX_E2E_H
#define MATRIX_E2E_H

#include <json-glib/json-glib.h>
#include "matrix-connection.h"

typedef struct _MatrixE2EData MatrixE2EData;
typedef struct _PurpleConversation PurpleConversation;
typedef struct _MatrixMediaCryptInfo MatrixMediaCryptInfo;

GList *matrix_e2e_actions(GList *list);
int matrix_e2e_get_device_keys(MatrixConnectionData *conn, const gchar *device_id);
void matrix_e2e_cleanup_connection(MatrixConnectionData *conn);
void matrix_e2e_cleanup_conversation(PurpleConversation *conv);
void matrix_e2e_decrypt_d2d(struct _PurpleConnection *pc, struct _JsonObject *event);
JsonParser *matrix_e2e_decrypt_room(struct _PurpleConversation *conv, struct _JsonObject *event);
gboolean matrix_e2e_parse_media_decrypt_info(MatrixMediaCryptInfo **crypt,
                                             JsonObject *file_obj);
void matrix_e2e_handle_sync_key_counts(struct _PurpleConnection *pc, struct _JsonObject *count_object, gboolean force_send);

#endif
