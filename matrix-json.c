/*
 * matrix-json.c
 *
 * Convenience wrappers for libjson-glib
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

#include "matrix-json.h"

/* node */

const gchar *matrix_json_node_get_string(JsonNode *node)
{
    if(node == NULL)
        return NULL;
    if(JSON_NODE_TYPE(node) != JSON_NODE_VALUE)
        return NULL;
    return json_node_get_string(node);
}

gint64 matrix_json_node_get_int(JsonNode *node)
{
    if(node == NULL)
        return 0;
    if(JSON_NODE_TYPE(node) != JSON_NODE_VALUE)
        return 0;
    return json_node_get_int(node);
}


JsonObject *matrix_json_node_get_object (JsonNode *node)
{
    if(node == NULL)
        return NULL;
    if(JSON_NODE_TYPE(node) != JSON_NODE_OBJECT)
        return NULL;
    return json_node_get_object(node);
}

JsonArray *matrix_json_node_get_array(JsonNode *node)
{
    if(node == NULL)
        return NULL;
    if(JSON_NODE_TYPE(node) != JSON_NODE_ARRAY)
        return NULL;
    return json_node_get_array(node);
}



/* object */

JsonNode *matrix_json_object_get_member (JsonObject  *object,
        const gchar *member_name)
{
    g_assert(member_name != NULL);

    if(object == NULL)
        return NULL;

    return json_object_get_member(object, member_name);
}

const gchar *matrix_json_object_get_string_member(JsonObject  *object,
        const gchar *member_name)
{
    JsonNode *member;
    member = matrix_json_object_get_member(object, member_name);
    return matrix_json_node_get_string(member);
}

gint64 matrix_json_object_get_int_member(JsonObject  *object,
        const gchar *member_name)
{
    JsonNode *member;
    member = matrix_json_object_get_member(object, member_name);
    return matrix_json_node_get_int(member);
}


JsonObject *matrix_json_object_get_object_member(JsonObject  *object,
        const gchar *member_name)
{
    JsonNode *member;
    member = matrix_json_object_get_member(object, member_name);
    return matrix_json_node_get_object(member);
}


JsonArray *matrix_json_object_get_array_member(JsonObject  *object,
        const gchar *member_name)
{
    JsonNode *member;
    member = matrix_json_object_get_member(object, member_name);
    return matrix_json_node_get_array(member);
}



/* array */
JsonNode *matrix_json_array_get_element(JsonArray *array,
        guint index)
{
    if(array == NULL)
        return NULL;
    if(json_array_get_length(array) <= index)
        return NULL;
    return json_array_get_element(array, index);
}

const gchar *matrix_json_array_get_string_element(JsonArray *array,
        guint index)
{
    JsonNode *element;
    element = matrix_json_array_get_element(array, index);
    return matrix_json_node_get_string(element);
}
