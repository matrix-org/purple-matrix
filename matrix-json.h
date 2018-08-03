/*
 * matrix-json.h: Convenience wrappers for libjson-glib
 *
 * This file contains wrappers for the libjson-glib library, which sanity-check
 * their inputs and return NULL (as opposed to segfaulting and/or writing
 * assertion warnings) if objects do not exist or are of the wrong type.
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

#ifndef MATRIX_JSON_H_
#define MATRIX_JSON_H_

#include <json-glib/json-glib.h>

/*

 */

/* node - returns NULL if node == NULL or *node is of the wrong type */
const gchar *matrix_json_node_get_string(JsonNode *node);
gint64 matrix_json_node_get_int(JsonNode *node);
JsonObject *matrix_json_node_get_object(JsonNode *node);
JsonArray *matrix_json_node_get_array(JsonNode *node);


/* object - returns NULL if object == NULL, member_name does not exist,
 * or object[member_name] is of the wrong type
 */
JsonNode *matrix_json_object_get_member(JsonObject *object,
                                        const gchar *member_name);
const gchar *matrix_json_object_get_string_member(JsonObject *object,
                                                  const gchar *member_name);
gint64 matrix_json_object_get_int_member(JsonObject *object,
		const gchar *member_name);
JsonObject *matrix_json_object_get_object_member(JsonObject *object,
                                                 const gchar *member_name);
JsonArray *matrix_json_object_get_array_member(JsonObject *object,
                              	  	  	  	   const gchar *member_name);

/* array - returns NULL if array == NULL, len(array) <= index, or
 * array[index] is the wrong type
 */
JsonNode *matrix_json_array_get_element(JsonArray *array,
        guint index);
const gchar *matrix_json_array_get_string_element(JsonArray *array,
        guint index);


/* Produce a canonicalised string as defined in
 * https://matrix.org/speculator/spec/drafts%2Fe2e/appendices.html#canonical-json
 */
GString *matrix_canonical_json(JsonObject *object);

/* Decode a json web signature (JWS) which is almost base64,
 * its needs _ -> / and - -> + and some = padding.
 * as https://tools.ietf.org/html/draft-ietf-jose-json-web-signature-41#appendix-C
 * The output buffer should be upto 3 bytes longer than the input
 * depending on the amount of = padding needed.
 */
void matrix_json_jws_tobase64(gchar *out, const gchar *in);

/* Just dump the Json with the string prefix for debugging */
void matrix_debug_jsonobject(const char *reason, JsonObject *object);

#endif /* MATRIX_JSON_H_ */
