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

/* Arrays of name/node for an object */
struct CanonicalObjectTmp {
  guint total, count;
  const char **names;
  JsonNode **nodes;
};

/* Callback from matrix_canonical_json for each member of an object
 * user_data is a pointer to a CanoicalObjectTmp
 */
static void canonical_object_child(JsonObject *object,
        const gchar *member_name, JsonNode *member_node,
        gpointer user_data)
{
  struct CanonicalObjectTmp *cot = user_data;
  guint i;

  g_assert_cmpuint(cot->count, <, cot->total);

  /* The list needs to be with 'dictionary keys lexicographically sorted by unicode codepoint' */
  for(i = 0; i < cot->count; i++) {
    /* Is this the right comparison func? */
    if (g_utf8_collate(member_name, cot->names[i]) <= 0)
      break;
  }
  if (i < cot->count) {
    memmove(&(cot->names[i+1]), &(cot->names[i]), sizeof(cot->names[0]) * (cot->count - i));
    memmove(&(cot->nodes[i+1]), &(cot->nodes[i]), sizeof(cot->nodes[0]) * (cot->count - i));
  }
  cot->names[i] = member_name;
  cot->nodes[i] = member_node;

  cot->count++;
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
      fprintf(stderr, "%s: Unknown value type %zd\n", __func__, (size_t)vt);
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
    result = canonical_json_value(json_array_get_element(arr, i), result);
  }
  result = g_string_append_c(result, ']');

  return result;
}

/* Produce a canonicalised string as defined in
 * https://matrix.org/speculator/spec/drafts%2Fe2e/appendices.html#canonical-json
 */
GString *matrix_canonical_json(JsonObject *object, GString *result)
{
  guint i;

  result = result ? g_string_append_c(result, '{') : g_string_new("{");

  /* The canonical layout requires sorting by member names */
  struct CanonicalObjectTmp cot;
  cot.total = json_object_get_size(object);
  cot.count = 0;
  cot.names = g_new(const char *, cot.total);
  cot.nodes = g_new(JsonNode *, cot.total);

  json_object_foreach_member(object, canonical_object_child, &cot);
  for(i=0; i < cot.total; i++) {
    if (i) result=g_string_append_c(result, ',');
    result = g_string_append_c(result, '"');
    result = g_string_append(result, cot.names[i]);
    result = g_string_append_c(result, '"');
    result = g_string_append_c(result, ':');
    switch (json_node_get_node_type(cot.nodes[i])) {
      case JSON_NODE_OBJECT:
        result = matrix_canonical_json(json_object_get_object_member(object, cot.names[i]), result);
        break;

      case JSON_NODE_ARRAY:
        result = canonical_json_array(json_object_get_array_member(object, cot.names[i]), result);
        break;

      case JSON_NODE_VALUE:
        result = canonical_json_value(cot.nodes[i], result);
        break;

      case JSON_NODE_NULL:
        result = g_string_append(result, "null");
        break;
    }
  }

  g_free(cot.names);
  g_free(cot.nodes);

  result = g_string_append_c(result, '}');
  return result;
}
