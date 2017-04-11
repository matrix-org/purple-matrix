/**
 * Handling of rooms within matrix
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

#include "matrix-room.h"

/* stdlib */
#include <inttypes.h>
#include <string.h>
#include <errno.h>

#include <glib/gstdio.h>

/* libpurple */
#include <libpurple/connection.h>
#include <libpurple/debug.h>

#include "libmatrix.h"
#include "matrix-api.h"
#include "matrix-e2e.h"
#include "matrix-event.h"
#include "matrix-json.h"
#include "matrix-roommembers.h"
#include "matrix-statetable.h"


static gchar *_get_room_name(MatrixConnectionData *conn,
        PurpleConversation *conv);
static const gchar *_get_my_display_name(PurpleConversation *conv);

static MatrixConnectionData *_get_connection_data_from_conversation(
        PurpleConversation *conv)
{
    return conv->account->gc->proto_data;
}

/******************************************************************************
 *
 * conversation data
 */

/*
 * identifiers for purple_conversation_get/set_data
 */

/* a MatrixRoomStateEventTable * - see below */
#define PURPLE_CONV_DATA_STATE "state"

/* a GList of MatrixRoomEvent * */
#define PURPLE_CONV_DATA_EVENT_QUEUE "queue"

/* PurpleUtilFetchUrlData * */
#define PURPLE_CONV_DATA_ACTIVE_SEND "active_send"

/* MatrixRoomMemberTable * - see below */
#define PURPLE_CONV_MEMBER_TABLE "member_table"

/* PURPLE_CONV_FLAG_* */
#define PURPLE_CONV_FLAGS "flags"
#define PURPLE_CONV_FLAG_NEEDS_NAME_UPDATE 0x1

/* Arbitrary limit on the size of an image to receive; should make
 * configurable. This is based on the worst-case assumption of a
 * 640x480 pixels, each with 3 bytes i.e. 900KiB. 640x480 is also the
 * server generated thumbnail size.
 */
static const size_t purple_max_media_size=640*480*3;

/**
 * Get the member table for a room
 */
static MatrixRoomMemberTable *matrix_room_get_member_table(
        PurpleConversation *conv)
{
    return purple_conversation_get_data(conv, PURPLE_CONV_MEMBER_TABLE);
}


/**
 * Get the state table for a room
 */
static MatrixRoomStateEventTable *matrix_room_get_state_table(
        PurpleConversation *conv)
{
    return purple_conversation_get_data(conv, PURPLE_CONV_DATA_STATE);
}


static guint _get_flags(PurpleConversation *conv)
{
    return GPOINTER_TO_UINT(purple_conversation_get_data(conv,
            PURPLE_CONV_FLAGS));
}


static void _set_flags(PurpleConversation *conv, guint flags)
{
    purple_conversation_set_data(conv, PURPLE_CONV_FLAGS,
            GUINT_TO_POINTER(flags));
}


/******************************************************************************
 *
 * room state handling
 */


/**
 * Update the name of the room in the buddy list and the chat window
 *
 * @param conv: conversation info
 */
static void _update_room_alias(PurpleConversation *conv)
{
    gchar *room_name;
    MatrixConnectionData *conn = _get_connection_data_from_conversation(conv);
    PurpleChat *chat;
    guint flags;

    room_name = _get_room_name(conn, conv);

    /* update the buddy list entry */
    chat = purple_blist_find_chat(conv->account, conv->name);
    /* we know there should be a buddy list entry for this room */
    g_assert(chat != NULL);
    purple_blist_alias_chat(chat, room_name);

    /* explicitly update the conversation title. This will tend to happen
     * anyway, but possibly not until the conversation tab is next activated.
     */
    if (strcmp(room_name, purple_conversation_get_title(conv)))
        purple_conversation_set_title(conv, room_name);

    g_free(room_name);

    flags = _get_flags(conv);
    flags &= ~PURPLE_CONV_FLAG_NEEDS_NAME_UPDATE;
    _set_flags(conv, flags);
}


static void _schedule_name_update(PurpleConversation *conv)
{
    guint flags = _get_flags(conv);
    flags |= PURPLE_CONV_FLAG_NEEDS_NAME_UPDATE;
    _set_flags(conv, flags);
    purple_debug_info("matrixprpl", "scheduled deferred room name update\n");
}

/**
 * Called when there is a change to the member list. Tells the MemberTable
 * about it.
 */
static void _on_member_change(PurpleConversation *conv,
        const gchar *member_user_id, MatrixRoomEvent *new_state)
{
    MatrixRoomMemberTable *member_table;

    member_table = matrix_room_get_member_table(conv);

    matrix_roommembers_update_member(member_table, member_user_id,
            new_state->content);
}

/**
 * Called when there is a change to the topic.
 */
static void _on_topic_change(PurpleConversation *conv,
        MatrixRoomEvent *new_state)
{
    PurpleConvChat *chat = PURPLE_CONV_CHAT(conv);
    
    purple_conv_chat_set_topic(chat, new_state->sender,
        matrix_json_object_get_string_member(new_state->content,
            "topic"));
}


/**
 * Called when there is a change list of typing userss.
 */
static void _on_typing(PurpleConversation *conv,
        MatrixRoomEvent *old_state, MatrixRoomEvent *new_state)
{
    PurpleConvChat *chat = PURPLE_CONV_CHAT(conv);
    JsonArray *old_user_ids, *new_user_ids;
    PurpleConvChatBuddyFlags cbflags;
    guint i, j;
    guint old_len, new_len;
    MatrixRoomMemberTable *member_table;

    member_table = matrix_room_get_member_table(conv);
    
    if (old_state != NULL) {
        old_user_ids = matrix_json_object_get_array_member(old_state->content, "user_ids");
        old_len = json_array_get_length(old_user_ids);
    } else {
        old_len = 0;
    }
    
    new_user_ids = matrix_json_object_get_array_member(new_state->content, "user_ids");
    new_len = json_array_get_length(new_user_ids);
    
    for (i = 0; i < old_len; i++) {
        const gchar *user_id = json_array_get_string_element(old_user_ids, i);
        MatrixRoomMember *roommember;
        const gchar *displayname;
        gboolean new_user_found = FALSE;
        
        for (j = 0; j < new_len; j++) {
            const gchar *new_user_id = json_array_get_string_element(new_user_ids, j);
            
            if (g_strcmp0(user_id, new_user_id)) {
                // no change, remove it from the new list
                json_array_remove_element(new_user_ids, j);
                new_len--;
                new_user_found = TRUE;
                break;
            }
        }
        
        if (new_user_found == FALSE) {
            roommember = matrix_roommembers_lookup_member(member_table, user_id);
            if (roommember) {
                displayname = matrix_roommember_get_displayname(roommember);
            
                // in old list, not in new, i.e. stopped typing
                cbflags = purple_conv_chat_user_get_flags(chat, displayname);
                cbflags &= ~PURPLE_CBFLAGS_TYPING;
                purple_conv_chat_user_set_flags(chat, displayname, cbflags);
            }
        }
    }
    
    // everything left in new_user_ids is new typing events
    for (i = 0; i < new_len; i++) {
        const gchar *user_id = json_array_get_string_element(new_user_ids, i);
        MatrixRoomMember *roommember;
        const gchar *displayname;
        
        roommember = matrix_roommembers_lookup_member(member_table, user_id);
        if (roommember) {
            displayname = matrix_roommember_get_displayname(roommember);
    
            cbflags = purple_conv_chat_user_get_flags(chat, displayname);
            cbflags |= PURPLE_CBFLAGS_TYPING;
            purple_conv_chat_user_set_flags(chat, displayname, cbflags);
        }
    }
    
}


/**
 * Called when there is a state update.
 *
 * old_state may be NULL to indicate addition of a state
 * key.
 */
static void _on_state_update(const gchar *event_type,
        const gchar *state_key, MatrixRoomEvent *old_state,
        MatrixRoomEvent *new_state, gpointer user_data)
{
    PurpleConversation *conv = user_data;
    g_assert(new_state != NULL);

    if(strcmp(event_type, "m.room.member") == 0) {
        _on_member_change(conv, state_key, new_state);
        /* we schedule a room name update here regardless of whether we end up
         * changing any members, because even changes to invited members can
         * affect the room name.
         */
        _schedule_name_update(conv);
    }
    else if(strcmp(event_type, "m.room.alias") == 0 ||
            strcmp(event_type, "m.room.canonical_alias") == 0 ||
            strcmp(event_type, "m.room.name") == 0) {
        _schedule_name_update(conv);
    } else if (strcmp(event_type, "m.room.encryption") == 0) {
        purple_debug_info("matrixprpl",
                          "Got m.room.encryption on_state_update\n");
    }
    else if(strcmp(event_type, "m.typing") == 0) {
        _on_typing(conv, old_state, new_state);
    }
    else if(strcmp(event_type, "m.room.topic") == 0) {
        _on_topic_change(conv, new_state);
    }
}

void matrix_room_handle_state_event(struct _PurpleConversation *conv,
        JsonObject *json_event_obj)
{
    MatrixRoomStateEventTable *state_table = matrix_room_get_state_table(conv);
    matrix_statetable_update(state_table, json_event_obj,
            _on_state_update, conv);
}


static gint _compare_member_user_id(const MatrixRoomMember *m,
        const gchar *user_id)
{
    return g_strcmp0(matrix_roommember_get_user_id(m), user_id);
}

/**
 * figure out the best name for a room based on its members list
 *
 * @returns a string which should be freed
 */
static gchar *_get_room_name_from_members(MatrixConnectionData *conn,
        PurpleConversation *conv)
{
    GList *tmp, *members;
    const gchar *member1;
    gchar *res;
    MatrixRoomMemberTable *member_table;

    member_table = matrix_room_get_member_table(conv);
    members = matrix_roommembers_get_active_members(member_table, TRUE);

    /* remove ourselves from the list */
    tmp = g_list_find_custom(members, conn->user_id,
            (GCompareFunc)_compare_member_user_id);
    if(tmp != NULL) {
        members = g_list_delete_link(members, tmp);
    }

    if(members == NULL) {
        /* nobody else here! */
        return NULL;
    }

    member1 = matrix_roommember_get_displayname(members->data);

    if(members->next == NULL) {
        /* one other person */
        res = g_strdup(member1);
    } else if(members->next->next == NULL) {
        /* two other people */
        const gchar *member2 = matrix_roommember_get_displayname(
                members->next->data);
        res = g_strdup_printf(_("%s and %s"), member1, member2);
    } else {
        int nmembers = g_list_length(members);
        res = g_strdup_printf(_("%s and %i others"), member1, nmembers);
    }

    g_list_free(members);
    return res;
}


/**
 * figure out the best name for a room
 *
 * @returns a string which should be freed
 */
static gchar *_get_room_name(MatrixConnectionData *conn,
        PurpleConversation *conv)
{
    MatrixRoomStateEventTable *state_table = matrix_room_get_state_table(conv);
    gchar *res;

    /* first try to pick a name based on the official name / alias */
    res = matrix_statetable_get_room_alias(state_table);
    if (res)
        return res;

    /* look for room members, and pick a name based on that */
    res = _get_room_name_from_members(conn, conv);
    if (res)
        return res;

    /* failing all else, just use the room id */
    return g_strdup(conv -> name);

}

/******************************************************************************
 *
 * event queue handling
 */
static void _send_queued_event(PurpleConversation *conv);

/**
 * Get the state table for a room
 */
static GList *_get_event_queue(PurpleConversation *conv)
{
    return purple_conversation_get_data(conv, PURPLE_CONV_DATA_EVENT_QUEUE);
}

static void _event_send_complete(MatrixConnectionData *account, gpointer user_data,
      JsonNode *json_root,
      const char *raw_body, size_t raw_body_len, const char *content_type)
{
    PurpleConversation *conv = user_data;
    JsonObject *response_object;
    const gchar *event_id;
    GList *event_queue;
    MatrixRoomEvent *event;

    response_object = matrix_json_node_get_object(json_root);
    event_id = matrix_json_object_get_string_member(response_object,
            "event_id");
    purple_debug_info("matrixprpl", "Successfully sent event id %s\n",
            event_id);

    event_queue = _get_event_queue(conv);
    event = event_queue -> data;
    matrix_event_free(event);
    event_queue = g_list_remove(event_queue, event);

    purple_conversation_set_data(conv, PURPLE_CONV_DATA_EVENT_QUEUE,
            event_queue);

    _send_queued_event(conv);
}


/**
 * Unable to send event to homeserver
 */
void _event_send_error(MatrixConnectionData *ma, gpointer user_data,
        const gchar *error_message)
{
    PurpleConversation *conv = user_data;
    matrix_api_error(ma, user_data, error_message);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND, NULL);

    /* for now, we leave the message queued. We should consider retrying. */
}

/**
 * homeserver gave non-200 on event send.
 */
void _event_send_bad_response(MatrixConnectionData *ma, gpointer user_data,
        int http_response_code, JsonNode *json_root)
{
    PurpleConversation *conv = user_data;
    matrix_api_bad_response(ma, user_data, http_response_code, json_root);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND, NULL);

    /* for now, we leave the message queued. We should consider retrying. */
}

/**************************** Image handling *********************************/
/* Data structure passed from the event hook to the upload completion */
struct SendImageEventData {
    PurpleConversation *conv;
    MatrixRoomEvent *event;
    int imgstore_id;
};

/**
 * Called back by matrix_api_upload_file after the image is uploaded.
 * We get a 'content_uri' identifying the uploaded file, and that's what
 * we put in the event.
 */
static void _image_upload_complete(MatrixConnectionData *ma,
      gpointer user_data, JsonNode *json_root,
      const char *raw_body, size_t raw_body_len, const char *content_type)
{
    MatrixApiRequestData *fetch_data = NULL;
    struct SendImageEventData *sied = user_data;
    JsonObject *response_object = matrix_json_node_get_object(json_root);
    const gchar *content_uri;
    PurpleStoredImage *image = purple_imgstore_find_by_id(sied->imgstore_id);

    content_uri = matrix_json_object_get_string_member(response_object,
            "content_uri");
    if (content_uri == NULL) {
        matrix_api_error(ma, sied->conv,
                "image_upload_complete: no content_uri");
        purple_imgstore_unref(image);
        g_free(sied);
        return;
    }

    json_object_set_string_member(sied->event->content, "url", content_uri);

    fetch_data = matrix_api_send(ma, sied->conv->name, sied->event->event_type,
             sied->event->txn_id, sied->event->content, _event_send_complete,
             _event_send_error, _event_send_bad_response, sied->conv);
    purple_conversation_set_data(sied->conv, PURPLE_CONV_DATA_ACTIVE_SEND,
                    fetch_data);
    purple_imgstore_unref(image);
    g_free(sied);
}

static void _image_upload_bad_response(MatrixConnectionData *ma, gpointer user_data,
            int http_response_code, JsonNode *json_root)
{
    struct SendImageEventData *sied = user_data;
    PurpleStoredImage *image = purple_imgstore_find_by_id(sied->imgstore_id);

    matrix_api_bad_response(ma, sied->conv, http_response_code, json_root);
    purple_imgstore_unref(image);
    purple_conversation_set_data(sied->conv, PURPLE_CONV_DATA_ACTIVE_SEND,
            NULL);
    g_free(sied);
    /* More clear up with the message? */
}

void _image_upload_error(MatrixConnectionData *ma, gpointer user_data,
            const gchar *error_message)
{
    struct SendImageEventData *sied = user_data;
    PurpleStoredImage *image = purple_imgstore_find_by_id(sied->imgstore_id);

    matrix_api_error(ma, sied->conv, error_message);
    purple_imgstore_unref(image);
    purple_conversation_set_data(sied->conv, PURPLE_CONV_DATA_ACTIVE_SEND,
            NULL);
    g_free(sied);
    /* More clear up with the message? */
}

/**
 * Return a mimetype based on some info; this should get replaced
 * with a glib/gio/gcontent_type_guess call if we can include it,
 * all other plugins do this manually.
 */
static const char *type_guess(PurpleStoredImage *image)
{
    /* Copied off the code in libpurple's jabber module */
    const char *ext = purple_imgstore_get_extension(image);

    if (strcmp(ext, "png") == 0) {
      return "image/png";
    } else if (strcmp(ext, "gif") == 0) {
      return "image/gif";
    } else if (strcmp(ext, "jpg") == 0) {
      return "image/jpeg";
    } else if (strcmp(ext, "tif") == 0) {
      return "image/tif";
    } else {
      return "image/x-icon"; /* or something... */
    }
}

/**
 * Check if the declared content-type is an image type we recognise.
 */
static gboolean is_known_image_type(const char *content_type)
{
    return !strcmp(content_type, "image/png") ||
           !strcmp(content_type, "image/jpeg") ||
           !strcmp(content_type, "image/gif") ||
           !strcmp(content_type, "image/tiff");
}

/* Structure hung off the event and used by _send_image_hook */
struct SendImageHookData {
    PurpleConversation *conv;
    int imgstore_id;
};
/**
 * Called back by _send_queued_event for an image.
 */
static void _send_image_hook(MatrixRoomEvent *event, gboolean just_free)
{
    MatrixApiRequestData *fetch_data;
    struct SendImageHookData *sihd = event->hook_data;
    /* Free'd by the callbacks from upload_file */
    struct SendImageEventData *sied = g_new0(struct SendImageEventData, 1);
    PurpleConnection *pc;
    MatrixConnectionData *acct;
    int imgstore_id;
    PurpleStoredImage *image;
    size_t imgsize;
    const char *filename;
    const char *ctype;
    gconstpointer imgdata;

    if (just_free) {
        g_free(event->hook_data);
        return;
    }

    pc = sihd->conv->account->gc;
    acct = purple_connection_get_protocol_data(pc);
    imgstore_id = sihd->imgstore_id;
    image = purple_imgstore_find_by_id(imgstore_id);
    if (!image)
        return;

    imgsize = purple_imgstore_get_size(image);
    filename = purple_imgstore_get_filename(image);
    imgdata = purple_imgstore_get_data(image);
    ctype = type_guess(image);

    purple_debug_info("matrixprpl", "%s: image id %d for %s (type: %s)\n",
            __func__,
            sihd->imgstore_id, filename, ctype);

    sied->conv = sihd->conv;
    sied->imgstore_id = sihd->imgstore_id;
    sied->event = event;
    json_object_set_string_member(event->content, "body", filename);

    fetch_data = matrix_api_upload_file(acct, ctype, imgdata, imgsize,
                           _image_upload_complete,
                           _image_upload_error,
                           _image_upload_bad_response, sied);
    if (fetch_data) {
        purple_conversation_set_data(sied->conv, PURPLE_CONV_DATA_ACTIVE_SEND,
                fetch_data);
    }
}

/* Passed through matrix_api_download_file all the way
 * downto _image_download_complete
 */
struct ReceiveImageData {
    PurpleConversation *conv;
    gint64 timestamp;
    const gchar *room_id;
    const gchar *sender_display_name;
    gchar *original_body;
};

static void _image_download_complete(MatrixConnectionData *ma,
          gpointer user_data, JsonNode *json_root,
          const char *raw_body, size_t raw_body_len, const char *content_type)
{
    struct ReceiveImageData *rid = user_data;
    if (is_known_image_type(content_type)) {
        /* Excellent - something to work with */
        int img_id = purple_imgstore_add_with_id(g_memdup(raw_body, raw_body_len),
                                                 raw_body_len, NULL);
        serv_got_chat_in(rid->conv->account->gc, g_str_hash(rid->room_id), rid->sender_display_name,
                PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_IMAGES,
                g_strdup_printf("<IMG ID=\"%d\">", img_id), rid->timestamp / 1000);
    } else {
        serv_got_chat_in(rid->conv->account->gc, g_str_hash(rid->room_id),
                rid->sender_display_name, PURPLE_MESSAGE_RECV,
                g_strdup_printf("%s (unknown type %s)",
                        rid->original_body, content_type), rid->timestamp / 1000);
    }
    purple_conversation_set_data(rid->conv, PURPLE_CONV_DATA_ACTIVE_SEND,
            NULL);
    g_free(rid->original_body);
    g_free(rid);
}

static void _image_download_bad_response(MatrixConnectionData *ma, gpointer user_data,
                int http_response_code, JsonNode *json_root)
{
    struct ReceiveImageData *rid = user_data;
    gchar *escaped_body = purple_markup_escape_text(rid->original_body, -1);
    serv_got_chat_in(rid->conv->account->gc, g_str_hash(rid->room_id),
            rid->sender_display_name, PURPLE_MESSAGE_RECV,
            g_strdup_printf("%s (bad response to download image %d)",
                    escaped_body, http_response_code),
                    rid->timestamp / 1000);
    purple_conversation_set_data(rid->conv, PURPLE_CONV_DATA_ACTIVE_SEND,
            NULL);
    g_free(escaped_body);
    g_free(rid->original_body);
    g_free(rid);
}

static void _image_download_error(MatrixConnectionData *ma, gpointer user_data,
                const gchar *error_message)
{
    struct ReceiveImageData *rid = user_data;
    gchar *escaped_body = purple_markup_escape_text(rid->original_body, -1);
    serv_got_chat_in(rid->conv->account->gc, g_str_hash(rid->room_id),
            rid->sender_display_name, PURPLE_MESSAGE_RECV,
            g_strdup_printf("%s (failed to download image %s)",
                    escaped_body, error_message), rid->timestamp / 1000);
    purple_conversation_set_data(rid->conv, PURPLE_CONV_DATA_ACTIVE_SEND,
            NULL);
    g_free(escaped_body);
    g_free(rid->original_body);
    g_free(rid);
}


/*
 * Called from matrix_room_handle_timeline_event when it finds an m.video
 * or m.audio or m.file or m.image; msg_body has the fallback text,
 * json_content_object has the json for the content sub object
 */
static gboolean _handle_incoming_media(PurpleConversation *conv,
        const gint64 timestamp, const gchar *room_id,
        const gchar *sender_display_name, const gchar *msg_body,
        JsonObject *json_content_object, const gchar *msg_type) {
    MatrixConnectionData *conn = _get_connection_data_from_conversation(conv);
    MatrixApiRequestData *fetch_data = NULL;

    const gchar *url;
    GString *download_url;
    guint64 size = 0;
    const gchar *mime_type = "unknown";
    JsonObject *json_info_object;

    url = matrix_json_object_get_string_member(json_content_object, "url");
    if (!url) {
        /* That seems odd, oh well, no point in getting upset */
        purple_debug_info("matrixprpl", "failed to get url for media\n");
        return FALSE;
    }
    download_url = get_download_url(conn->homeserver, url);
    if (!download_url) {
        purple_debug_error("matrixprpl", "failed to get download_url for media\n");
        return FALSE;
    }

    /* the 'info' member is optional */
    json_info_object = matrix_json_object_get_object_member(json_content_object,
            "info");
    if (json_info_object) {
        /* OK, we've got some (optional) info */
        size = matrix_json_object_get_int_member(json_info_object, "size");
        mime_type = matrix_json_object_get_string_member(json_info_object,
                        "mimetype");
        purple_debug_info("matrixprpl", "media info good: %s of %" PRId64 "\n",
                          mime_type, size);
    }

    serv_got_chat_in(conv->account->gc, g_str_hash(room_id),
            sender_display_name, PURPLE_MESSAGE_RECV,
            /* TODO convert size into a human readable format */
            g_strdup_printf("%s (type %s size %" PRId64 ") %s",
                    msg_body, mime_type, size, download_url->str), timestamp / 1000);

    /* m.audio is not supposed to have a thumbnail, handling completed
     */
    if (!strcmp("m.audio", msg_type)) {
        return TRUE;
    }

    /* If a thumbnail_url is available and the thumbnail size is small,
     * download that. Otherwise, only for m.image, ask for a server generated
     * thumbnail.
     */
    int is_image = !strcmp("m.image", msg_type);
    const gchar *thumb_url = "";
    JsonObject *json_thumb_info;
    guint64 thumb_size = 0;
    /* r0.2.0 -> r0.3.0
     * m.image content.thumb* -> content.info.thumb*
     * m.video -
     * m.file  content.thumb* -> content.info.thumb*
     */
    thumb_url = matrix_json_object_get_string_member(json_info_object, "thumbnail_url");
    json_thumb_info = matrix_json_object_get_object_member(json_info_object, "thumbnail_info");
    if (json_thumb_info) {
        thumb_size = matrix_json_object_get_int_member(json_thumb_info, "size");
    } else {
        /* m.image and m.file had thumbnail_* members directly in the content object prior to r0.3.0 */
        thumb_url = matrix_json_object_get_string_member(json_content_object, "thumbnail_url");
        json_thumb_info = matrix_json_object_get_object_member(json_content_object, "thumbnail_info");
        if (json_thumb_info) {
            thumb_size = matrix_json_object_get_int_member(json_thumb_info, "size");
        }
    }
    if (is_image && (size > 0) && (size < purple_max_media_size)) {
        /* if an m.image is small, get that instead of the thumbnail */
        thumb_url = url;
        thumb_size = size;
    }
    if (thumb_url || is_image) {
        struct ReceiveImageData *rid;
        rid = g_new0(struct ReceiveImageData, 1);
        rid->conv = conv;
        rid->timestamp = timestamp;
        rid->sender_display_name = sender_display_name;
        rid->room_id = room_id;
        rid->original_body = g_strdup(msg_body);

        if (thumb_url && (thumb_size > 0) && (thumb_size < purple_max_media_size)) {
            fetch_data = matrix_api_download_file(conn, thumb_url,
                    purple_max_media_size,
                    _image_download_complete,
                    _image_download_error,
                    _image_download_bad_response,
                    rid);
        } else if (thumb_url) {
            /* Ask the server to generate a thumbnail of the thumbnail.
             * Useful to improve the chance of showing something when the
             * original thumbnail is too big.
             */
            fetch_data = matrix_api_download_thumb(conn, thumb_url,
                    purple_max_media_size,
                    640, 480, TRUE, /* Scaled */
                    _image_download_complete,
                    _image_download_error,
                    _image_download_bad_response,
                    rid);
        } else {
            /* Ask the server to generate a thumbnail. Only for m.image.
             * TODO: Configure the size of thumbnails.
             * 640x480 is a good a width as any and reasonably likely to
             * fit in the byte size limit unless someone has a big long
             * tall png.
             */
            fetch_data = matrix_api_download_thumb(conn, url,
                    purple_max_media_size,
                    640, 480, TRUE, /* Scaled */
                    _image_download_complete,
                    _image_download_error,
                    _image_download_bad_response,
                    rid);
        }
        purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND,
                fetch_data);
        return fetch_data != NULL;
    }
    return TRUE;
}

/**
 * send the next queued event, provided the connection isn't shutting down.
 *
 * Updates PURPLE_CONV_DATA_ACTIVE_SEND either way.
 */
static void _send_queued_event(PurpleConversation *conv)
{
    MatrixApiRequestData *fetch_data = NULL;
    MatrixConnectionData *acct;
    MatrixRoomEvent *event;
    PurpleConnection *pc = conv->account->gc;
    GList *queue;

    acct = purple_connection_get_protocol_data(pc);
    queue = _get_event_queue(conv);

    if(queue == NULL) {
        /* nothing else to send */
    } else if(pc -> wants_to_die) {
        /* don't make any more requests if the connection is closing */
        purple_debug_info("matrixprpl", "Not sending new events on dying"
                " connection");
    } else {
        event = queue -> data;
        g_assert(event != NULL);
        if (event->hook)
            return event->hook(event, FALSE);

        purple_debug_info("matrixprpl", "Sending %s with txn id %s\n",
                event->event_type, event->txn_id);

        fetch_data = matrix_api_send(acct, conv->name, event->event_type,
                event->txn_id, event->content, _event_send_complete,
                _event_send_error, _event_send_bad_response, conv);
    }

    purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND,
            fetch_data);
}


static void _enqueue_event(PurpleConversation *conv, const gchar *event_type,
        JsonObject *event_content,
        EventSendHook hook, void *hook_data)
{
    MatrixRoomEvent *event;
    GList *event_queue;
    MatrixApiRequestData *active_send;

    event = matrix_event_new(event_type, event_content);
    event->txn_id = g_strdup_printf("%"G_GINT64_FORMAT"%"G_GUINT32_FORMAT,
            g_get_monotonic_time(), g_random_int());
    event->hook = hook;
    event->hook_data = hook_data;

    event_queue = _get_event_queue(conv);
    event_queue = g_list_append(event_queue, event);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_EVENT_QUEUE,
            event_queue);

    purple_debug_info("matrixprpl", "Enqueued %s with txn id %s\n",
            event_type, event->txn_id);

    active_send = purple_conversation_get_data(conv,
            PURPLE_CONV_DATA_ACTIVE_SEND);
    if(active_send != NULL) {
        purple_debug_info("matrixprpl", "Event send is already in progress\n");
    } else {
        _send_queued_event(conv);
    }
}


/**
 * If there is an event send in progress, cancel it
 */
static void _cancel_event_send(PurpleConversation *conv)
{
    MatrixApiRequestData *active_send = purple_conversation_get_data(conv,
            PURPLE_CONV_DATA_ACTIVE_SEND);

    if(active_send == NULL)
        return;

    purple_debug_info("matrixprpl", "Cancelling event send");
    matrix_api_cancel(active_send);

    g_assert(purple_conversation_get_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND)
            == NULL);
}

/*****************************************************************************/

void matrix_room_handle_timeline_event(PurpleConversation *conv,
       JsonObject *json_event_obj)
{
    const gchar *event_type, *sender_id, *transaction_id;
    gint64 timestamp;
    JsonObject *json_content_obj;
    JsonObject *json_unsigned_obj;
    const gchar *room_id, *msg_body, *msg_type;
    gchar *tmp_body = NULL;
    gchar *escaped_body = NULL;
    PurpleMessageFlags flags;

    const gchar *sender_display_name;
    MatrixRoomMember *sender = NULL;

    room_id = conv->name;

    event_type = matrix_json_object_get_string_member(
            json_event_obj, "type");
    sender_id = matrix_json_object_get_string_member(json_event_obj, "sender");
    timestamp = matrix_json_object_get_int_member(json_event_obj,
                "origin_server_ts");
    json_content_obj = matrix_json_object_get_object_member(
            json_event_obj, "content");

    if(event_type == NULL) {
        purple_debug_warning("matrixprpl", "event missing type field");
        return;
    }

    if(!strcmp(event_type, "m.room.encrypted")) {
        purple_debug_info("matrixprpl", "Got an m.room.encrypted!\n");
        matrix_e2e_decrypt_room(conv, json_event_obj);
        return;
    }

    if(strcmp(event_type, "m.room.message") != 0) {
        purple_debug_info("matrixprpl", "ignoring unknown room event %s\n",
                        event_type);
        return;
    }

    msg_body = matrix_json_object_get_string_member(json_content_obj, "body");
    if(msg_body == NULL) {
        purple_debug_warning("matrixprpl", "no body in message event\n");
        return;
    }

    msg_type = matrix_json_object_get_string_member(json_content_obj, "msgtype");
    if(msg_type == NULL) {
        purple_debug_warning("matrixprpl", "no msgtype in message event\n");
        return;
    }

    json_unsigned_obj = matrix_json_object_get_object_member(json_event_obj,
            "unsigned");
    transaction_id = matrix_json_object_get_string_member(json_unsigned_obj,
            "transaction_id");

    /* if it has a transaction id, it's an echo of a message we sent.
     * We shouldn't really just ignore it, but I'm not sure how to update a sent
     * message.
     */
    if(transaction_id != NULL) {
        purple_debug_info("matrixprpl", "got remote echo %s in %s\n", msg_body,
                room_id);
        return;
    }

    if(sender_id != NULL) {
        MatrixRoomMemberTable *table = matrix_room_get_member_table(conv);
        sender = matrix_roommembers_lookup_member(table, sender_id);
    }
    if (sender != NULL) {
        sender_display_name = matrix_roommember_get_displayname(sender);
    } else {
        sender_display_name = "<unknown>";
    }

    if (!strcmp(msg_type, "m.emote")) {
        tmp_body = g_strdup_printf("/me %s", msg_body);
    } else if ((!strcmp(msg_type, "m.video")) || (!strcmp(msg_type, "m.audio")) || \
            (!strcmp(msg_type, "m.file")) || (!strcmp(msg_type, "m.image"))) {
        if (_handle_incoming_media(conv, timestamp, room_id, sender_display_name,
                    msg_body, json_content_obj, msg_type)) {
            return;
        }
        /* Fall through - we couldn't handle the media, treat as text */
    }
    flags = PURPLE_MESSAGE_RECV;

    if (purple_strequal(matrix_json_object_get_string_member(json_content_obj, "format"), "org.matrix.custom.html")) {
        escaped_body = g_strdup(matrix_json_object_get_string_member(json_content_obj, "formatted_body"));
    } else {
        escaped_body = purple_markup_escape_text(tmp_body ? tmp_body : msg_body, -1);
    }
    g_free(tmp_body);
    purple_debug_info("matrixprpl", "got message from %s in %s\n", sender_id,
            room_id);
    serv_got_chat_in(conv->account->gc, g_str_hash(room_id),
            sender_display_name, flags, escaped_body, timestamp / 1000);
    g_free(escaped_body);
}


PurpleConversation *matrix_room_create_conversation(
        PurpleConnection *pc, const gchar *room_id)
{
    PurpleConversation *conv;
    MatrixRoomStateEventTable *state_table;
    MatrixRoomMemberTable *member_table;

    purple_debug_info("matrixprpl", "New room %s\n", room_id);

    /* tell purple we have joined this chat */
    conv = serv_got_joined_chat(pc, g_str_hash(room_id), room_id);

    /* set our data on it */
    state_table = matrix_statetable_new();
    member_table = matrix_roommembers_new_table();
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_EVENT_QUEUE, NULL);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_ACTIVE_SEND, NULL);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_STATE, state_table);
    purple_conversation_set_data(conv, PURPLE_CONV_MEMBER_TABLE,
            member_table);

    return conv;
}


/**
 * Leave a chat: notify the server that we are leaving, and (ultimately)
 * free the memory structures
 */
void matrix_room_leave_chat(PurpleConversation *conv)
{
    MatrixConnectionData *conn;
    MatrixRoomStateEventTable *state_table;
    GList *event_queue;
    MatrixRoomMemberTable *member_table;

    conn = _get_connection_data_from_conversation(conv);

    _cancel_event_send(conv);
    matrix_api_leave_room(conn, conv->name, NULL, NULL, NULL, NULL);

    /* At this point, we have no confirmation that the 'leave' request will
     * be successful (nor that it has even started), so it's questionable
     * whether we can/should actually free all of the room state.
     *
     * On the other hand, we don't have any mechanism for telling purple that
     * we haven't really left the room, and if the leave request does fail,
     * we'll set the error flag on the connection, which will eventually
     * result in pidgin flagging the connection as failed; things will
     * hopefully then get resynced when the user reconnects.
     */

    state_table = matrix_room_get_state_table(conv);
    matrix_statetable_destroy(state_table);
    purple_conversation_set_data(conv, PURPLE_CONV_DATA_STATE, NULL);

    member_table = matrix_room_get_member_table(conv);
    matrix_roommembers_free_table(member_table);
    purple_conversation_set_data(conv, PURPLE_CONV_MEMBER_TABLE, NULL);

    event_queue = _get_event_queue(conv);
    if(event_queue != NULL) {
        g_list_free_full(event_queue, (GDestroyNotify)matrix_event_free);
        purple_conversation_set_data(conv, PURPLE_CONV_DATA_EVENT_QUEUE, NULL);
    }
}


/* *****************************************************************************
 *
 * Tracking of member additions/removals.
 *
 * We don't tell libpurple about new arrivals immediately, because that is
 * inefficient and takes ages on a big room like Matrix HQ. Instead, the
 * MatrixRoomMemberTable builds up a list of changes, and we then go through
 * those changes after processing all of the state changes in a /sync.
 *
 * This introduces a complexity in that we need to track what we've told purple
 * the displayname of the user is (for instance, member1 leaves a channel,
 * meaning that there is no longer a clash of displaynames, so member2
 * can be renamed: we need to know what we previously told libpurple member2 was
 * called). We do this by setting the member's opaque data to the name we gave
 * to libpurple.
 */


static void _on_member_deleted(MatrixRoomMember *member)
{
    gchar *displayname = matrix_roommember_get_opaque_data(member);
    g_free(displayname);
    matrix_roommember_set_opaque_data(member, NULL, NULL);
}


/**
 * Tell libpurple about newly-arrived members
 */
static void _handle_new_members(PurpleConversation *conv,
        gboolean announce_arrivals)
{
    PurpleConvChat *chat = PURPLE_CONV_CHAT(conv);
    MatrixRoomMemberTable *table = matrix_room_get_member_table(conv);
    GList *names = NULL, *flags = NULL;
    GSList *members;

    members = matrix_roommembers_get_new_members(table);
    while(members != NULL) {
        MatrixRoomMember *member = members->data;
        const gchar *displayname;
        GSList *tmp;

        displayname = matrix_roommember_get_opaque_data(member);
        g_assert(displayname == NULL);

        displayname = matrix_roommember_get_displayname(member);
        matrix_roommember_set_opaque_data(member, g_strdup(displayname),
                _on_member_deleted);

        names = g_list_prepend(names, (gpointer)displayname);
        flags = g_list_prepend(flags, GINT_TO_POINTER(0));

        tmp = members;
        members = members->next;
        g_slist_free_1(tmp);
    }

    if(names) {
        purple_conv_chat_add_users(chat, names, NULL, flags, announce_arrivals);
        g_list_free(names);
        g_list_free(flags);
    }
}


/**
 * Tell libpurple about renamed members
 */
void _handle_renamed_members(PurpleConversation *conv)
{
    PurpleConvChat *chat = PURPLE_CONV_CHAT(conv);
    MatrixRoomMemberTable *table = matrix_room_get_member_table(conv);
    GSList *members;

    members = matrix_roommembers_get_renamed_members(table);
    while(members != NULL) {
        MatrixRoomMember *member = members->data;
        gchar *current_displayname;
        const gchar *new_displayname;
        GSList *tmp;

        current_displayname = matrix_roommember_get_opaque_data(member);
        g_assert(current_displayname != NULL);

        new_displayname = matrix_roommember_get_displayname(member);

        purple_conv_chat_rename_user(chat, current_displayname,
                new_displayname);

        matrix_roommember_set_opaque_data(member, g_strdup(new_displayname),
                _on_member_deleted);
        g_free(current_displayname);

        tmp = members;
        members = members->next;
        g_slist_free_1(tmp);
    }
}


/**
 * Tell libpurple about departed members
 */
void _handle_left_members(PurpleConversation *conv)
{
    PurpleConvChat *chat = PURPLE_CONV_CHAT(conv);
    MatrixRoomMemberTable *table = matrix_room_get_member_table(conv);
    GSList *members;

    members = matrix_roommembers_get_left_members(table);
    while(members != NULL) {
        MatrixRoomMember *member = members->data;
        gchar *current_displayname;
        GSList *tmp;

        current_displayname = matrix_roommember_get_opaque_data(member);
        g_assert(current_displayname != NULL);
        purple_conv_chat_remove_user(chat, current_displayname, NULL);

        g_free(current_displayname);
        matrix_roommember_set_opaque_data(member, NULL, NULL);

        tmp = members;
        members = members->next;
        g_slist_free_1(tmp);
    }
}


static void _update_user_list(PurpleConversation *conv,
        gboolean announce_arrivals)
{
    _handle_new_members(conv, announce_arrivals);
    _handle_renamed_members(conv);
    _handle_left_members(conv);
}



/**
 * Get the userid of a member of a room, given their displayname
 *
 * @returns a string, which will be freed by the caller, or null if not known
 */
gchar *matrix_room_displayname_to_userid(struct _PurpleConversation *conv,
        const gchar *who)
{
    /* TODO: make this more efficient */
    MatrixRoomMemberTable *table = matrix_room_get_member_table(conv);
    GList *members, *ptr;
    gchar *result = NULL;

    members = matrix_roommembers_get_active_members(table, TRUE);

    for(ptr = members; ptr != NULL; ptr = ptr->next) {
        MatrixRoomMember *member = ptr->data;
        const gchar *displayname = matrix_roommember_get_opaque_data(member);
        if(g_strcmp0(displayname, who) == 0) {
            result = g_strdup(matrix_roommember_get_user_id(member));
            break;
        }
    }

    g_list_free(members);
    return result;
}

/* ************************************************************************** */

void matrix_room_complete_state_update(PurpleConversation *conv,
        gboolean announce_arrivals)
{
    _update_user_list(conv, announce_arrivals);
    if(_get_flags(conv) & PURPLE_CONV_FLAG_NEEDS_NAME_UPDATE)
        _update_room_alias(conv);
}


static const gchar *_get_my_display_name(PurpleConversation *conv)
{
    MatrixConnectionData *conn = _get_connection_data_from_conversation(conv);
    MatrixRoomMemberTable *member_table =
            matrix_room_get_member_table(conv);
    MatrixRoomMember *me;

    me = matrix_roommembers_lookup_member(member_table, conn->user_id);
    if(me == NULL)
        return NULL;
    else
        return matrix_roommember_get_displayname(me);
}

/**
 * Send an image message in a room
 */
void matrix_room_send_image(PurpleConversation *conv, int imgstore_id,
        const gchar *message)
{
    JsonObject *content;
    struct SendImageHookData *sihd;

    if (!imgstore_id)
        return;
    /* This is the hook_data on the event, it gets free'd by the event
     * code when the event is free'd
     */
    sihd = g_new0(struct SendImageHookData, 1);

    /* We can't send this event until we've uploaded the image because
     * the event contents including the file ID that we get back from
     * the upload process.
     * Our hook gets called back when we're ready to send the event,
     * then we do the upload.
     */
    content = json_object_new();
    json_object_set_string_member(content, "msgtype", "m.image");

    sihd->imgstore_id = imgstore_id;
    sihd->conv = conv;
    purple_debug_info("matrixprpl", "%s: image id=%d\n", __func__, imgstore_id);
    _enqueue_event(conv, "m.room.message", content, _send_image_hook, sihd);
    json_object_unref(content);
    purple_conv_chat_write(PURPLE_CONV_CHAT(conv), _get_my_display_name(conv),
            message, PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_IMAGES,
            g_get_real_time()/1000/1000);
}

/**
 * Sends a typing notification in a room with a 25s timeout
 */
void matrix_room_send_typing(PurpleConversation *conv, gboolean typing)
{
    MatrixConnectionData *acct;
    PurpleConnection *pc = conv->account->gc;

    acct = purple_connection_get_protocol_data(pc);
    
    // Don't check callbacks as it's inconsequential whether typing notifications go through
    matrix_api_typing(acct, conv->name, typing, 25000,
            NULL, NULL, NULL, NULL);
    
}

/**
 * Send a message in a room
 */
void matrix_room_send_message(PurpleConversation *conv, const gchar *message)
{
    JsonObject *content;
    PurpleConvChat *chat = PURPLE_CONV_CHAT(conv);
    const char *type_string = "m.text";
    const char *image_start, *image_end;
    gchar *message_to_send, *message_dup;
    GData *image_attribs;

    /* Matrix doesn't have messages that have both images and text in, so
     * we have to split this message if it has an image.
     */
    if (purple_markup_find_tag("img", message,
                               &image_start,
                               &image_end,
                               &image_attribs)) {
        int imgstore_id = atoi(g_datalist_get_data(&image_attribs, "id"));
        gchar *image_message;
        purple_imgstore_ref_by_id(imgstore_id);

        if (image_start != message) {
            gchar *prefix = g_strndup(message, image_start - message);
            matrix_room_send_message(conv, prefix);
            g_free(prefix);
        }

        image_message = g_strndup(image_start, 1+(image_end-image_start));
        matrix_room_send_image(conv, imgstore_id, image_message);
        g_datalist_clear(&image_attribs);
        g_free(image_message);

        /* Anything after the image? */
        if (image_end[1]) {
            matrix_room_send_message(conv, image_end + 1);
        }
        return;
    }

    /* Matrix messages are JSON-encoded, so there's no need to HTML/XML-
     * escape the message body. Matrix clients don't unescape the bodies
     * either, so they end up seeing &quot; instead of "
     */
    message_dup = g_strdup(message);
    message_to_send = purple_markup_strip_html(message_dup);

    if (purple_message_meify(message_to_send, -1)) {
        type_string = "m.emote";
        purple_message_meify(message_dup, -1);
    }

    content = json_object_new();
    json_object_set_string_member(content, "msgtype", type_string);
    json_object_set_string_member(content, "body", message_to_send);
    json_object_set_string_member(content, "formatted_body", message_dup);
    json_object_set_string_member(content, "format", "org.matrix.custom.html");

    _enqueue_event(conv, "m.room.message", content, NULL, NULL);
    json_object_unref(content);

    purple_conv_chat_write(chat, _get_my_display_name(conv),
            message_dup, PURPLE_MESSAGE_SEND, g_get_real_time()/1000/1000);

    g_free(message_to_send);
    g_free(message_dup);
}
