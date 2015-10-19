/**
 * Handling of rooms within matrix
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

#ifndef MATRIX_ROOM_H
#define MATRIX_ROOM_H

#include <glib.h>

#include "libmatrix.h"

struct _PurpleConnection;

/* The state event table is a hashtable which maps from event type to
 * another hashtable, which maps from state key to content, which is itself a
 * MatrixRoomStateEvent
 */
typedef GHashTable MatrixRoomStateEventTable;

struct _JsonObject;

/**
 * handle a room within the sync response
 */
void matrix_room_handle_sync(const gchar *room_id,
        struct _JsonObject *room_data, MatrixAccount *ma);

/**
 * Figure out the best name for a room, from its state table
 */
const char *matrix_room_get_name(MatrixRoomStateEventTable *state_table);

#endif
