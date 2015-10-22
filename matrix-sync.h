/**
 * matrix-sync.h
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

#ifndef MATRIX_SYNC_H_
#define MATRIX_SYNC_H_

#include <glib.h>

struct _PurpleConnection;
struct _JsonNode;

/**
 * Parse and dispatch the results of a /sync call.
 *
 * @param pc          Connection to which these results relate
 * @param body        Body of /sync response
 * @param next_batch  Returns a pointer to the next_batch setting, for the next
 *                    sync (or NULL if none was found)
 */
void matrix_sync_parse(struct _PurpleConnection *pc, struct _JsonNode *body,
        const gchar **next_batch);


#endif /* MATRIX_SYNC_H_ */
