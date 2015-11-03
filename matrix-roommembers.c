/*
 * matrix-roommember.c
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


#include "matrix-roommembers.h"

#include <stdlib.h>
#include <string.h>

#include "debug.h"

#include "matrix-json.h"

/******************************************************************************
 *
 * Individual members
 */

typedef struct _MatrixRoomMember {
    gchar *user_id;

    /* the current room membership */
    int membership;

    /* the displayname from the state table (this is a pointer to the actual
     * string in the state table, so should not be freed here) */
    const gchar *state_displayname;

    /* data attached to this member (matrix-room.c uses it to track the
     * name we told libpurple this member had)
     */
    gpointer opaque_data;

    /* callback to delete the opaque_data. Called with a pointer to the member.
     */
    DestroyMemberNotify on_delete;
} MatrixRoomMember;


static int _parse_membership(const gchar *membership)
{
    if(membership == NULL)
        return MATRIX_ROOM_MEMBERSHIP_NONE;

    if(strcmp(membership, "join") == 0)
        return MATRIX_ROOM_MEMBERSHIP_JOIN;
    if(strcmp(membership, "leave") == 0)
        return MATRIX_ROOM_MEMBERSHIP_LEAVE;
    if(strcmp(membership, "invite") == 0)
        return MATRIX_ROOM_MEMBERSHIP_INVITE;
    return MATRIX_ROOM_MEMBERSHIP_NONE;
}

static MatrixRoomMember *_new_member(const gchar *userid)
{
    MatrixRoomMember *mem = g_new0(MatrixRoomMember, 1);
    mem->user_id = g_strdup(userid);
    return mem;
}

static void _free_member(MatrixRoomMember *member)
{
    g_assert(member != NULL);
    if(member->on_delete)
        member->on_delete(member);
    g_free(member->user_id);
    member->user_id = NULL;
    g_free(member);
}


/**
 * Get the user_id for the given member
 *
 * @returns a string, which should *not* be freed
 */
const gchar *matrix_roommember_get_user_id(const MatrixRoomMember *member)
{
    return member->user_id;
}

/**
 * Get the displayname for the given member
 *
 * @returns a string, which should *not* be freed
 */
const gchar *matrix_roommember_get_displayname(const MatrixRoomMember *member)
{
    if(member->state_displayname != NULL) {
        /* TODO: if there is more than one member with this displayname, we
         * should return a deduplicated name
         */
        return member->state_displayname;
    } else {
        return member->user_id;
    }
}


/**
 * Get the opaque data associated with the given member
 */
gpointer matrix_roommember_get_opaque_data(const MatrixRoomMember *member)
{
    return member->opaque_data;
}


/**
 * Set the opaque data associated with the given member
 */
void matrix_roommember_set_opaque_data(MatrixRoomMember *member,
        gpointer data, DestroyMemberNotify on_delete)
{
    member->opaque_data = data;
    member->on_delete = on_delete;
}


/******************************************************************************
 *
 * member table
 */

struct _MatrixRoomMemberTable {
    GHashTable *hash_table;
    GSList *new_members;
    GSList *left_members;
    GSList *renamed_members;
};

MatrixRoomMemberTable *matrix_roommembers_new_table()
{
    MatrixRoomMemberTable *table;
    table = g_new0(MatrixRoomMemberTable, 1);
    table -> hash_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                           (GDestroyNotify) _free_member);
    return table;
}


void matrix_roommembers_free_table(MatrixRoomMemberTable *table)
{
    g_hash_table_destroy(table->hash_table);
    table->hash_table = NULL;
    g_free(table);
}


MatrixRoomMember *matrix_roommembers_lookup_member(MatrixRoomMemberTable *table,
        const gchar *member_user_id)
{
    return g_hash_table_lookup(table->hash_table, member_user_id);
}


void matrix_roommembers_update_member(MatrixRoomMemberTable *table,
        const gchar *member_user_id, JsonObject *new_state)
{
    const gchar *old_displayname = NULL;
    MatrixRoomMember *member;
    int old_membership_val = MATRIX_ROOM_MEMBERSHIP_NONE,
            new_membership_val;
    const gchar *new_displayname, *new_membership;

    new_displayname = matrix_json_object_get_string_member(
             new_state, "displayname");
    new_membership = matrix_json_object_get_string_member(
                       new_state, "membership");

    new_membership_val = _parse_membership(new_membership);

    member = matrix_roommembers_lookup_member(table, member_user_id);

    if(member != NULL) {
        old_displayname = member -> state_displayname;
        old_membership_val = member -> membership;
    }

    if(!member) {
        member = _new_member(member_user_id);
        g_hash_table_insert(table->hash_table, g_strdup(member_user_id),
                member);
    }
    member->membership = new_membership_val;
    member->state_displayname = new_displayname;

    purple_debug_info("matrixprpl", "member %s change %i->%i, "
            "%s->%s\n", member_user_id,
            old_membership_val, new_membership_val,
            old_displayname, new_displayname);

    if(new_membership_val == MATRIX_ROOM_MEMBERSHIP_JOIN) {
        if(old_membership_val != MATRIX_ROOM_MEMBERSHIP_JOIN) {
            purple_debug_info("matrixprpl", "%s (%s) joins\n",
                    member_user_id, new_displayname);
            table->new_members = g_slist_append(
                    table->new_members, member);
        } else if(g_strcmp0(old_displayname, new_displayname) != 0) {
            purple_debug_info("matrixprpl", "%s (%s) changed name (was %s)\n",
                    member_user_id, new_displayname, old_displayname);
            table->renamed_members = g_slist_append(
                    table->renamed_members, member);
        }
    } else {
        if(old_membership_val == MATRIX_ROOM_MEMBERSHIP_JOIN) {
            purple_debug_info("matrixprpl", "%s (%s) leaves\n",
                    member_user_id, old_displayname);
            table->left_members = g_slist_append(
                    table->left_members, member);
        }
    }
}


/**
 * Returns a list of MatrixRoomMember *s. Free the list, but not the pointers.
 */
GList *matrix_roommembers_get_active_members(
        MatrixRoomMemberTable *member_table, gboolean include_invited)
{
    GHashTableIter iter;
    gpointer key, value;
    GList *members = NULL;

    g_hash_table_iter_init (&iter, member_table->hash_table);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
        MatrixRoomMember *member = value;

        if(member->membership == MATRIX_ROOM_MEMBERSHIP_JOIN ||
                (include_invited &&
                        member->membership == MATRIX_ROOM_MEMBERSHIP_INVITE)) {
            members = g_list_prepend(members, value);
        }
    }
    return members;
}


GSList *matrix_roommembers_get_new_members(MatrixRoomMemberTable *table)
{
    GSList *members = table->new_members;
    table->new_members = NULL;
    return members;
}


GSList *matrix_roommembers_get_renamed_members(MatrixRoomMemberTable *table)
{
    GSList *members = table->renamed_members;
    table->renamed_members = NULL;
    return members;

}


GSList *matrix_roommembers_get_left_members(MatrixRoomMemberTable *table)
{
    GSList *members = table->left_members;
    table->left_members = NULL;
    return members;

}

