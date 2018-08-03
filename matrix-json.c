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

#include <stdio.h>
#include <string.h>
#include "matrix-json.h"

static GString *canonical_json_node(JsonNode *node, GString *result);
static GString *canonical_json_object(JsonObject *object, GString *result);

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

static gint canonical_json_sort(gconstpointer a, gconstpointer b)
{
    return strcmp((const gchar *)a, (const gchar *)b);
}

static GString *canonical_json_value(JsonNode *node, GString *result)
{
    GType vt = json_node_get_value_type(node);
    switch (vt) {
        case G_TYPE_STRING:
            /* TODO: I'm assuming our strings are nice UTF-8 strings already */
            result = g_string_append_c(result, '"');
            result = g_string_append(result, json_node_get_string(node));
            result = g_string_append_c(result, '"');
            break;

        default:
            fprintf(stderr, "%s: Unknown value type %zd\n", __func__,
                    (size_t)vt);
            /* TODO: Other value types */
            g_assert_not_reached();
    }

    return result;
}

static GString *canonical_json_array(JsonArray *arr, GString *result)
{
    guint nelems, i;
    result = g_string_append_c(result, '[');
    nelems = json_array_get_length(arr);
    for(i = 0; i < nelems; i++) {
        if (i) result=g_string_append_c(result, ',');
        result = canonical_json_node(json_array_get_element(arr, i), result);
    }
    result = g_string_append_c(result, ']');

    return result;
}

static GString *canonical_json_node(JsonNode *node, GString *result)
{
    switch (json_node_get_node_type(node)) {
        case JSON_NODE_OBJECT:
            result = canonical_json_object(json_node_get_object(node), result);
            break;

        case JSON_NODE_ARRAY:
            result = canonical_json_array(json_node_get_array(node), result);
            break;

        case JSON_NODE_VALUE:
            result = canonical_json_value(node, result);
            break;

        case JSON_NODE_NULL:
            result = g_string_append(result, "null");
            break;
    }
    return result;
}

static GString *canonical_json_object(JsonObject *object, GString *result)
{
    gboolean first = TRUE;
    result = result ? g_string_append_c(result, '{') : g_string_new("{");

    /* This gets an unsorted list of member names */
    GList *members = json_object_get_members(object);
    GList *cur;

    members = g_list_sort(members, canonical_json_sort);
    for(cur=g_list_first(members); cur; cur=g_list_next(cur)) {
        const gchar *cur_name = cur->data;
        JsonNode *cur_node  = json_object_get_member(object, cur_name);
        if (!first) result=g_string_append_c(result, ',');
        first = FALSE;
        result = g_string_append_c(result, '"');
        result = g_string_append(result, cur_name);
        result = g_string_append_c(result, '"');
        result = g_string_append_c(result, ':');
        result = canonical_json_node(cur_node, result);
    }

    g_list_free(members);

    result = g_string_append_c(result, '}');
    return result;
}

/* Produce a canonicalised string as defined in
 * http://matrix.org/docs/spec/appendices.html#canonical-json
 */
GString *matrix_canonical_json(JsonObject *object)
{
    return canonical_json_object(object, NULL);
}

/* Decode a json web signature (JWS) which is almost base64,
 * its needs _ -> / and - -> + and some = padding.
 * as https://tools.ietf.org/html/draft-ietf-jose-json-web-signature-41#appendix-C
 * The output buffer should be upto 3 bytes longer than the input
 * depending on the amount of = padding needed.
 */
void matrix_json_jws_tobase64(gchar *out, const gchar *in)
{
    unsigned int i;
    for (i=0;in[i];i++) {
        out[i] = in[i];
        switch (in[i]) {
            case '-':
                out[i] = '+';
                break;

            case '_':
                out[i] = '/';
                break;

            default:
                break;
        }
    }
    while (i & 3) {
        out[i] = '=';
        i++;
    }
    out[i] = '\0';
}

/* Just dump the Json with the string prefix for debugging */
void matrix_debug_jsonobject(const char *reason, JsonObject *object)
{
    JsonNode *tmp_top = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(tmp_top, object);
    JsonGenerator *generator = json_generator_new();
    json_generator_set_pretty(generator, TRUE);
    json_generator_set_root(generator, tmp_top);
    char *json = json_generator_to_data(generator, NULL);
    fprintf(stderr, "%s: %s\n", reason, json);
    g_free(json);
    g_object_unref(generator);
    json_node_free(tmp_top);
}
