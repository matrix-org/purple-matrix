/*
 * matrix-roommembers.h
 *
 *
 * Copyright (c) Openmarket UK Ltd 2015
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

#ifndef MATRIX_ROOMMEMBERS_H_
#define MATRIX_ROOMMEMBERS_H_

#include <glib.h>

/* the potential states of a user's membership of a room */
#define MATRIX_ROOM_MEMBERSHIP_NONE 0
#define MATRIX_ROOM_MEMBERSHIP_JOIN 1
#define MATRIX_ROOM_MEMBERSHIP_INVITE 2
#define MATRIX_ROOM_MEMBERSHIP_LEAVE 3

typedef struct _MatrixRoomMemberTable MatrixRoomMemberTable;
struct _JsonObject;

/**
 * Allocate a new MatrixRoomMemberTable
 */
MatrixRoomMemberTable *matrix_roommembers_new_table();

/**
 * Free a MatrixRoomMemberTable
 */
void matrix_roommembers_free_table(MatrixRoomMemberTable *table);


/**
 * Handle the update of a room member.
 *
 * For efficiency, we do not immediately notify purple of the changes. Instead,
 * you should call matrix_roommembers_get_(new,renamed,left)_members once
 * the whole state table has been handled.
 */
void matrix_roommembers_update_member(MatrixRoomMemberTable *table,
        const gchar *member_user_id, struct _JsonObject *new_state);


/**
 * Get the displayname for the given userid
 *
 * @returns a string, which should *not* be freed
 */
const gchar *matrix_roommembers_get_displayname_for_member(
        MatrixRoomMemberTable *table, const gchar *user_id);


/**
 * Get a list of the members who have joined this room.
 *
 * Returns a list of user ids. Free the list, but not the string pointers.
 */
GList *matrix_roommembers_get_active_members(
        MatrixRoomMemberTable *member_table);


/**
 * Get a list of the new members since the last time this function was called.
 *
 * @param display_names     returns the list of display names. Do not free the
 *                          pointers.
 * @param flags             returns a corresponding list of zeros
 */
void matrix_roommembers_get_new_members(MatrixRoomMemberTable *table,
        GList **display_names, GList **flags);


/**
 * Get a list of the members who have been renamed since the last time this
 * function was called.
 *
 * @param old_names  returns the list of old display names. These are no
 *                   longer required, so should be freed
 * @param new_names  returns the list of new display names. Do *not* free these
 *                   pointers.
 */
void matrix_roommembers_get_renamed_members(MatrixRoomMemberTable *table,
        GList **old_names, GList **new_names);


/**
 * Get a list of the members who have left the channel since the last time this
 * function was called.
 *
 * @param new_names  returns the list of display names. These are no
 *                   longer required, so should be freed
 */
void matrix_roommembers_get_left_members(MatrixRoomMemberTable *table,
        GList **names);


/**
 * Get the userid of a member of a room, given their displayname
 *
 * @returns a string, which will be freed by the caller, or null if not known
 */
gchar *matrix_roommembers_displayname_to_userid(
        MatrixRoomMemberTable *table, const gchar *who);


#endif /* MATRIX_ROOMMEMBERS_H_ */
