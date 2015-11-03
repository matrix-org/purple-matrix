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



/* ****************************************************************************
 *
 * Handling of individual members
 */

typedef struct _MatrixRoomMember MatrixRoomMember;

/**
 * Get the user_id for the given member
 *
 * @returns a string, which should *not* be freed
 */
const gchar *matrix_roommember_get_user_id(const MatrixRoomMember *member);

/**
 * Get the displayname for the given member
 *
 * @returns a string, which should *not* be freed
 */
const gchar *matrix_roommember_get_displayname(const MatrixRoomMember *member);


/**
 * Get the opaque data associated with the given member
 */
gpointer matrix_roommember_get_opaque_data(const MatrixRoomMember *member);


typedef void (*DestroyMemberNotify)(MatrixRoomMember *member);
/**
 * Set the opaque data associated with the given member
 *
 * @param on_delete: a callback which will be called when the RoomMember is
 * deleted (usually when its parent MatrixRoomMemberTable is deleted). It is
 * passed a pointer to the MatrixRoomMember.
 */
void matrix_roommember_set_opaque_data(MatrixRoomMember *member,
        gpointer data, DestroyMemberNotify on_delete);



/* ****************************************************************************
 *
 * Member table
 */

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
 * Look up a room member given the userid
 *
 * @returns MatrixRoomMember *, or NULL if unknown
 */
MatrixRoomMember *matrix_roommembers_lookup_member(MatrixRoomMemberTable *table,
        const gchar *member_user_id);

/**
 * Get a list of the members who have joined this room.
 *
 * Returns a list of MatrixRoomMember *s. Free the list, but not the pointers.
 */
GList *matrix_roommembers_get_active_members(
        MatrixRoomMemberTable *member_table, gboolean include_invited);


/**
 * Get a list of the new members since the last time this function was called.
 *
 * @returns a list of MatrixRoomMember *s. Free the list when you are done with
 * it.
 */
GSList *matrix_roommembers_get_new_members(MatrixRoomMemberTable *table);


/**
 * Get a list of the members who have been renamed since the last time this
 * function was called.
 *
 * @returns a list of MatrixRoomMember *s. Free the list when you are done with
 * it.
 */
GSList *matrix_roommembers_get_renamed_members(MatrixRoomMemberTable *table);


/**
 * Get a list of the members who have left the channel since the last time this
 * function was called.
 *
 * @returns a list of MatrixRoomMember *s. Free the list when you are done with
 * it.
 */
GSList *matrix_roommembers_get_left_members(MatrixRoomMemberTable *table);


#endif /* MATRIX_ROOMMEMBERS_H_ */
