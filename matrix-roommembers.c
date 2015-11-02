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
    gchar *userid;

    /* the displayname we gave to purple */
    gchar *current_displayname;

    /* the current room membership */
    int membership;

    /* the displayname from the state table */
    const gchar *state_displayname;
} MatrixRoomMember;


/**
 * calculate the displayname for the given member
 *
 * @returns a string, which should be freed
 */
static gchar *_calculate_displayname_for_member(const MatrixRoomMember *member)
{
    if(member->state_displayname != NULL) {
        return g_strdup(member->state_displayname);
    } else {
        return g_strdup(member->userid);
    }
}


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
    mem->userid = g_strdup(userid);
    return mem;
}

static void _free_member(MatrixRoomMember *member)
{
    g_assert(member != NULL);
    g_free(member->userid);
    member->userid = NULL;
    g_free(member->current_displayname);
    member->current_displayname = NULL;
    g_free(member);
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


static MatrixRoomMember *_lookup_member(MatrixRoomMemberTable *table,
        const gchar *userid)
{
    return g_hash_table_lookup(table->hash_table, userid);
}


#if 0
static void _on_member_changed_displayname(PurpleConversation *conv,
        const gchar *member_user_id, MatrixRoomMember *member)
{
    PurpleConvChat *chat = PURPLE_CONV_CHAT(conv);
    gchar *old_displayname, *new_displayname;

    old_displayname = member->current_displayname;
    g_assert(old_displayname != NULL);
    new_displayname = _calculate_displayname_for_member(member_user_id, member);

    purple_conv_chat_rename_user(chat, old_displayname, new_displayname);
    g_free(old_displayname);
    member->current_displayname = new_displayname;
}


static void _on_member_left(PurpleConversation *conv,
        const gchar *member_user_id, MatrixRoomMember *member)
{
    PurpleConvChat *chat = PURPLE_CONV_CHAT(conv);
    gchar *old_displayname;

    old_displayname = member->current_displayname;
    g_assert(old_displayname != NULL);
    purple_conv_chat_remove_user(chat, old_displayname, NULL);
    g_free(old_displayname);
    member->current_displayname = NULL;
}
#endif


const gchar *matrix_roommembers_get_displayname_for_member(
        MatrixRoomMemberTable *table, const gchar *user_id)
{
    MatrixRoomMember *member = _lookup_member(table, user_id);
    gchar *displayname;

    if(member == NULL)
        return user_id;

    displayname = member -> current_displayname;

    if (displayname != NULL)
        return displayname;

    member -> current_displayname = displayname;
    return displayname;
}


void matrix_roommembers_get_new_members(MatrixRoomMemberTable *table,
        GList **display_names, GList **flags)
{
    while(table->new_members != NULL) {
        MatrixRoomMember *member = table->new_members->data;
        gchar *displayname;
        GSList *tmp;

        g_assert(member->current_displayname == NULL);
        displayname = _calculate_displayname_for_member(member);
        member->current_displayname = displayname;
        *display_names = g_list_prepend(*display_names, displayname);
        *flags = g_list_prepend(*flags, GINT_TO_POINTER(0));

        tmp = table->new_members;
        table->new_members = tmp->next;
        g_slist_free_1(tmp);
    }
}

void matrix_roommembers_get_renamed_members(MatrixRoomMemberTable *table,
        GList **old_names, GList **new_names)
{
    while(table->renamed_members != NULL) {
        MatrixRoomMember *member = table->renamed_members->data;
        gchar *displayname;
        GSList *tmp;

        g_assert(member->current_displayname != NULL);
        displayname = _calculate_displayname_for_member(member);
        *old_names = g_list_prepend(*old_names, member->current_displayname);
        *new_names = g_list_prepend(*new_names, displayname);
        member->current_displayname = displayname;

        tmp = table->renamed_members;
        table->renamed_members = tmp->next;
        g_slist_free_1(tmp);
    }
}

void matrix_roommembers_get_left_members(MatrixRoomMemberTable *table,
        GList **names)
{
    while(table->left_members != NULL) {
        MatrixRoomMember *member = table->left_members->data;
        GSList *tmp;

        g_assert(member->current_displayname != NULL);
        *names = g_list_prepend(*names, member->current_displayname);
        member->current_displayname = NULL;

        tmp = table->left_members;
        table->left_members = tmp->next;
        g_slist_free_1(tmp);
    }
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

    member = _lookup_member(table, member_user_id);

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
            table->new_members = g_slist_append(
                    table->new_members, member);
        } else if(g_strcmp0(old_displayname, new_displayname) != 0) {
            table->renamed_members = g_slist_append(
                    table->renamed_members, member);
        }
    } else {
        if(old_membership_val == MATRIX_ROOM_MEMBERSHIP_JOIN) {
            table->left_members = g_slist_append(
                    table->left_members, member);
        }
    }
}


/**
 * Returns a list of user ids. Free the list, but not the string pointers.
 */
GList *matrix_roommembers_get_active_members(
        MatrixRoomMemberTable *member_table)
{
    GHashTableIter iter;
    gpointer key, value;
    GList *members = NULL;

    g_hash_table_iter_init (&iter, member_table->hash_table);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
        const gchar *user_id = key;
        MatrixRoomMember *member = value;

        if(member->membership == MATRIX_ROOM_MEMBERSHIP_JOIN)
            members = g_list_prepend(members, (gpointer)user_id);
    }
    return members;
}


/**
 * Get the userid of a member of a room, given their displayname
 *
 * @returns a string, which will be freed by the caller, or null if not known
 */
gchar *matrix_roommembers_displayname_to_userid(
        MatrixRoomMemberTable *table, const gchar *who)
{
    /* TODO: make this more efficient */
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init (&iter, table->hash_table);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
        const gchar *user_id = key;
        MatrixRoomMember *member = value;
        if(member->current_displayname != NULL
                && strcmp(who, member->current_displayname) == 0) {
            return g_strdup(user_id);
        }
    }
    return NULL;
}
