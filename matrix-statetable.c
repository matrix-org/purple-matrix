/*
 * matrix-statetable.c
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


#include "matrix-statetable.h"

#include "debug.h"

#include "matrix-event.h"
#include "matrix-json.h"


/**
 * create a new, empty, state table
 */
MatrixRoomStateEventTable *matrix_statetable_new()
{
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
            (GDestroyNotify) g_hash_table_destroy);
}

/**
 * look up a particular bit of state
 *
 * @returns null if this key ies not known
 */
MatrixRoomEvent *matrix_statetable_get_event(
        MatrixRoomStateEventTable *state_table, const gchar *event_type,
        const gchar *state_key)
{
    GHashTable *tmp;

    tmp = (GHashTable *) g_hash_table_lookup(state_table, event_type);
    if(tmp == NULL)
        return NULL;

    return (MatrixRoomEvent *)g_hash_table_lookup(tmp, state_key);
}


/**
 * Update the state table on a room
 */
void matrix_statetable_update(MatrixRoomStateEventTable *state_table,
        const gchar *event_id, JsonObject *json_event_obj,
        MatrixStateUpdateCallback callback, gpointer user_data)
{
    const gchar *event_type, *state_key;
    JsonObject *json_content_obj;
    MatrixRoomEvent *event, *old_event;
    GHashTable *state_table_entry;

    event_type = matrix_json_object_get_string_member(
            json_event_obj, "type");
    state_key = matrix_json_object_get_string_member(
            json_event_obj, "state_key");
    json_content_obj = matrix_json_object_get_object_member(
            json_event_obj, "content");

    if(event_type == NULL || state_key == NULL || json_content_obj == NULL) {
        purple_debug_warning("matrixprpl", "event missing fields");
        return;
    }

    event = matrix_event_new(event_type, json_content_obj);

    state_table_entry = g_hash_table_lookup(state_table, event_type);
    if(state_table_entry == NULL) {
        state_table_entry = g_hash_table_new_full(g_str_hash, g_str_equal,
                g_free, (GDestroyNotify)matrix_event_free);
        g_hash_table_insert(state_table, g_strdup(event_type),
                state_table_entry);
        old_event = NULL;
    } else {
        old_event = g_hash_table_lookup(state_table_entry,
                state_key);
    }

    if(callback) {
        callback(event_type, state_key, old_event, event, user_data);
    }

    g_hash_table_insert(state_table_entry, g_strdup(state_key), event);
}


/**
 * If the room has an official name, or an alias, return it
 *
 * @returns a string which should be freed
 */
gchar *matrix_statetable_get_room_alias(MatrixRoomStateEventTable *state_table)
{
    GHashTable *tmp;
    MatrixRoomEvent *event;
    const gchar *tmpname = NULL;

    /* start by looking for the official room name */
    event = matrix_statetable_get_event(state_table, "m.room.name", "");
    if(event != NULL) {
        tmpname = matrix_json_object_get_string_member(
                event->content, "name");
        if(tmpname != NULL) {
            return g_strdup(tmpname);
        }
    }

    /* look for an alias */
    tmp = (GHashTable *) g_hash_table_lookup(state_table, "m.room.aliases");
    if(tmp != NULL) {
        GHashTableIter iter;
        gpointer key, value;

        g_hash_table_iter_init(&iter, tmp);
        while(g_hash_table_iter_next(&iter, &key, &value)) {
            MatrixRoomEvent *event = value;
            JsonArray *array = matrix_json_object_get_array_member(
                    event->content, "aliases");
            if(array != NULL && json_array_get_length(array) > 0) {
                tmpname = matrix_json_array_get_string_element(array, 0);
                if(tmpname != NULL) {
                    return g_strdup(tmpname);
                }
            }
        }
    }

    return NULL;
}
