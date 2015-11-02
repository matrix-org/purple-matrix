/*
 * matrix-statetable.h
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

#ifndef MATRIX_STATETABLE_H_
#define MATRIX_STATETABLE_H_

#include <glib.h>

struct _MatrixRoomEvent;
struct _JsonObject;


/* The state event table is a hashtable which maps from event type to
 * another hashtable, which maps from state key to content, which is itself a
 * MatrixRoomEvent.
 *
 */
typedef GHashTable MatrixRoomStateEventTable;


/**
 * The type of a function which can be passed into matrix_statetable_update
 * to be called to handle an update
 */
typedef void (*MatrixStateUpdateCallback)(const gchar *event_type,
        const gchar *state_key, struct _MatrixRoomEvent *old_state,
        struct _MatrixRoomEvent *new_state, gpointer user_data);


/**
 * create a new, empty, state table
 */
MatrixRoomStateEventTable *matrix_statetable_new();


/**
 * free a state table
 */
void matrix_statetable_destroy(MatrixRoomStateEventTable *table);


/**
 * look up a particular bit of state
 *
 * @returns null if this key ies not known
 */
struct _MatrixRoomEvent *matrix_statetable_get_event(
        MatrixRoomStateEventTable *state_table, const gchar *event_type,
        const gchar *state_key);


/**
 * Update a state table with a new state event
 */
void matrix_statetable_update(MatrixRoomStateEventTable *state_table,
        struct _JsonObject *json_event_obj,
        MatrixStateUpdateCallback callback, gpointer user_data);


/**
 * If the room has an official name, or an alias, return it
 *
 * @returns a string which should be freed
 */
gchar *matrix_statetable_get_room_alias(MatrixRoomStateEventTable *state_table);

#endif /* MATRIX_STATETABLE_H_ */
