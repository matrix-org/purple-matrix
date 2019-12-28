/**
 * libmatrix.c
 *
 * This file exists to define the top-level PurplePluginInfo and
 * PurplePluginProtocolInfo structures which are used to integrate with
 * libpurple, and the callbacks which they refer to.
 *
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

#include "libmatrix.h"

#include <glib.h>

#include "account.h"
#include "accountopt.h"
#include "blist.h"
#include "connection.h"
#include "debug.h"
#include "prpl.h"
#include "version.h"

#include "matrix-connection.h"
#include "matrix-e2e.h"
#include "matrix-room.h"
#include "matrix-api.h"

/**
 * Called to get the icon name for the given buddy and account.
 *
 * If buddy is NULL and the account is non-NULL, it will return the
 * name to use for the account's icon. If both are NULL, it will
 * return the name to use for the protocol's icon.
 *
 * For now, everything just uses the 'matrix' icon.
 */
static const char *matrixprpl_list_icon(PurpleAccount *acct, PurpleBuddy *buddy)
{
    return "matrix";
}


/**
 * Called to get a list of the PurpleStatusType which are valid for this account
 *
 * (currently, we don't really implement any, but we have to return something
 * here)
 */
static GList *matrixprpl_status_types(PurpleAccount *acct)
{
    GList *types = NULL;
    PurpleStatusType *type;

    type = purple_status_type_new(PURPLE_STATUS_OFFLINE, "Offline", NULL,
            TRUE);
    types = g_list_prepend(types, type);

    type = purple_status_type_new(PURPLE_STATUS_AVAILABLE, "Online", NULL,
            TRUE);
    types = g_list_prepend(types, type);

    return types;
}

/**
 * handle sending typing notifications in a chat
 */
static guint matrixprpl_conv_send_typing(PurpleConversation *conv, 
        PurpleTypingState state, gpointer ignored)
{
    PurpleConnection *pc = purple_conversation_get_gc(conv);

    if (!PURPLE_CONNECTION_IS_CONNECTED(pc))
        return 0;

    if (g_strcmp0(purple_plugin_get_id(purple_connection_get_prpl(pc)), PRPL_ID))
        return 0;

    matrix_room_send_typing(conv, (state == PURPLE_TYPING));

    return 20;
}

/**
 * Start the connection to a matrix account
 */
void matrixprpl_login(PurpleAccount *acct)
{
    PurpleConnection *pc = purple_account_get_connection(acct);
    matrix_connection_new(pc);
    matrix_connection_start_login(pc);
    
    purple_signal_connect(purple_conversations_get_handle(), "chat-conversation-typing", 
        acct, PURPLE_CALLBACK(matrixprpl_conv_send_typing), pc);
    
    pc->flags |= PURPLE_CONNECTION_HTML;
}


/**
 * Called to handle closing the connection to an account
 */
static void matrixprpl_close(PurpleConnection *pc)
{
    matrix_connection_cancel_sync(pc);
    matrix_connection_free(pc);
}


/**
 * Get the list of information we need to add a chat to our buddy list.
 *
 * The first entry is special, and represents the unique "name" by which the
 * chat is identified in the buddy list with purple_blist_find_chat. In our case
 * that is room_id.
 */
static GList *matrixprpl_chat_info(PurpleConnection *gc)
{
    struct proto_chat_entry *pce; /* defined in prpl.h */

    pce = g_new0(struct proto_chat_entry, 1);
    pce->label = _("Room id");
    pce->identifier = PRPL_CHAT_INFO_ROOM_ID;
    pce->required = TRUE;

    return g_list_append(NULL, pce);
}

/* Get the defaults for the chat_info entries */
static GHashTable *matrixprpl_chat_info_defaults(PurpleConnection *gc,
                                          const char *room)
{
    GHashTable *defaults;

    defaults = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
    return defaults;
}

/* Get the name of a chat (as passed to serv_got_joined_chat) given the
 * chat_info entries. For us this is the room id so this is easy
 */
static char *matrixprpl_get_chat_name(GHashTable *components)
{
    const char *room = g_hash_table_lookup(components, PRPL_CHAT_INFO_ROOM_ID);
    return g_strdup(room);
}


/**
 * Handle a double-click on a chat in the buddy list, or acceptance of a chat
 * invite: it is expected that we join the chat.
 */
static void matrixprpl_join_chat(PurpleConnection *gc, GHashTable *components)
{
    const char *room = g_hash_table_lookup(components, PRPL_CHAT_INFO_ROOM_ID);
    int chat_id = g_str_hash(room);
    PurpleConversation *conv;
    PurpleConvChat *chat;

    conv = purple_find_chat(gc, chat_id);

    if(!conv) {
        matrix_connection_join_room(gc, room, components);
        return;
    }

    /* already in chat. This happens when the account was disconnected,
     * and has now been asked to reconnect.
     *
     * If we've got this far, chances are that we are correctly joined to
     * the room.
     */
    chat = PURPLE_CONV_CHAT(conv);
    chat->left = FALSE;

    if (!g_slist_find(gc->buddy_chats, conv))
            gc->buddy_chats = g_slist_append(gc->buddy_chats, conv);
    purple_conversation_update(conv, PURPLE_CONV_UPDATE_CHATLEFT);
}


/**
 * Handle refusing a chat invite.
 */
static void matrixprpl_reject_chat(PurpleConnection *gc, GHashTable *components)
{
    const char *room_id = g_hash_table_lookup(components,
            PRPL_CHAT_INFO_ROOM_ID);

    matrix_connection_reject_invite(gc, room_id);
}

static void matrixprpl_chat_invite(PurpleConnection *gc, int id,
        const char *message, const char *who)
{
    PurpleConversation *conv = purple_find_chat(gc, id);
    MatrixConnectionData *conn;
    conn = (MatrixConnectionData *)(conv->account->gc->proto_data);

    matrix_api_invite_user(conn, conv->name, who, NULL, NULL, NULL, NULL);
}

/**
 * handle leaving a chat: notify the server that we are leaving, and
 * (ultimately) free the memory structures associated with it
 */
static void matrixprpl_chat_leave(PurpleConnection *gc, int id) {
    PurpleConversation *conv = purple_find_chat(gc, id);
    purple_debug_info("matrixprpl", "%s is leaving chat room %s\n",
                      gc->account->username, conv->name);

    matrix_room_leave_chat(conv);
}


/**
 * handle sending messages in a chat
 */
static int matrixprpl_chat_send(PurpleConnection *gc, int id,
        const char *message, PurpleMessageFlags flags) {
    PurpleConversation *conv = purple_find_chat(gc, id);
    if(!conv) {
        purple_debug_info("matrixprpl",
                "tried to send message to chat room #%d but couldn't find "
                "chat room", id);
        return -1;
    }

    matrix_room_send_message(conv, message);
    return 0;
}


/**
 * Get the user_id of a user, given their displayname in a room
 *
 * @returns a string, which will be freed by the caller
 */
static char *matrixprpl_get_cb_real_name(PurpleConnection *gc, int id,
        const char *who)
{
    PurpleConversation *conv = purple_find_chat(gc, id);
    gchar *res;
    if(conv == NULL)
        return NULL;
    res = matrix_room_displayname_to_userid(conv, who);
    purple_debug_info("matrixprpl", "%s's real id in %s is %s\n", who,
            conv->name, res);
    return res;
}


/******************************************************************************
 *
 * prpl stuff. see prpl.h for more information.
 */

static PurplePluginProtocolInfo prpl_info =
{
    OPT_PROTO_UNIQUE_CHATNAME | OPT_PROTO_CHAT_TOPIC |
      OPT_PROTO_PASSWORD_OPTIONAL |
      OPT_PROTO_IM_IMAGE,    /* options */
    NULL,               /* user_splits, initialized in matrixprpl_init() */
    NULL,               /* protocol_options, initialized in matrixprpl_init() */
    {   /* icon_spec, a PurpleBuddyIconSpec */
        "png,jpg,gif",                   /* format */
        0,                               /* min_width */
        0,                               /* min_height */
        128,                             /* max_width */
        128,                             /* max_height */
        10000,                           /* max_filesize */
        PURPLE_ICON_SCALE_DISPLAY,       /* scale_rules */
    },
    matrixprpl_list_icon,                  /* list_icon */
    NULL,                                  /* list_emblem */
    NULL,                                  /* status_text */
    NULL,                                  /* tooltip_text */
    matrixprpl_status_types,               /* status_types */
    NULL,                                  /* blist_node_menu */
    matrixprpl_chat_info,                  /* chat_info */
    matrixprpl_chat_info_defaults,         /* chat_info_defaults */
    matrixprpl_login,                      /* login */
    matrixprpl_close,                      /* close */
    NULL,                                  /* send_im */
    NULL,                                  /* set_info */
    NULL,                                  /* send_typing */
    NULL,                                  /* get_info */
    NULL,                                  /* set_status */
    NULL,                                  /* set_idle */
    NULL,                                  /* change_passwd */
    NULL,                                  /* add_buddy */
    NULL,                                  /* add_buddies */
    NULL,                                  /* remove_buddy */
    NULL,                                  /* remove_buddies */
    NULL,                                  /* add_permit */
    NULL,                                  /* add_deny */
    NULL,                                  /* rem_permit */
    NULL,                                  /* rem_deny */
    NULL,                                  /* set_permit_deny */
    matrixprpl_join_chat,                  /* join_chat */
    matrixprpl_reject_chat,                /* reject_chat */
    matrixprpl_get_chat_name,              /* get_chat_name */
    matrixprpl_chat_invite,                /* chat_invite */
    matrixprpl_chat_leave,                 /* chat_leave */
    NULL,                                  /* chat_whisper */
    matrixprpl_chat_send,                  /* chat_send */
    NULL,                                  /* keepalive */
    NULL,                                  /* register_user */
    NULL,                                  /* get_cb_info */
    NULL,                                  /* get_cb_away */
    NULL,                                  /* alias_buddy */
    NULL,                                  /* group_buddy */
    NULL,                                  /* rename_group */
    NULL,                                  /* buddy_free */
    NULL,                                  /* convo_closed */
    NULL,                                  /* normalize */
    NULL,                                  /* set_buddy_icon */
    NULL,                                  /* remove_group */
    matrixprpl_get_cb_real_name,           /* get_cb_real_name */
    NULL,                                  /* set_chat_topic */
    NULL,                                  /* find_blist_chat */
    NULL,                                  /* roomlist_get_list */
    NULL,                                  /* roomlist_cancel */
    NULL,                                  /* roomlist_expand_category */
    NULL,                                  /* can_receive_file */
    NULL,                                  /* send_file */
    NULL,                                  /* new_xfer */
    NULL,                                  /* offline_message */
    NULL,                                  /* whiteboard_prpl_ops */
    NULL,                                  /* send_raw */
    NULL,                                  /* roomlist_room_serialize */
    NULL,                                  /* unregister_user */
    NULL,                                  /* send_attention */
    NULL,                                  /* get_attention_types */
    sizeof(PurplePluginProtocolInfo),      /* struct_size */
    NULL,                                  /* get_account_text_table */
    NULL,                                  /* initiate_media */
    NULL,                                  /* get_media_caps */
    NULL,                                  /* get_moods */
    NULL,                                  /* set_public_alias */
    NULL,                                  /* get_public_alias */
    NULL,                                  /* add_buddy_with_invite */
    NULL                                   /* add_buddies_with_invite */
};

static void matrixprpl_init(PurplePlugin *plugin)
{
    GList *protocol_options = NULL;

    purple_debug_info("matrixprpl", "starting up\n");

    protocol_options = g_list_append(protocol_options,
            purple_account_option_string_new(
                    _("Home server URL"), PRPL_ACCOUNT_OPT_HOME_SERVER,
                    DEFAULT_HOME_SERVER));
    protocol_options = g_list_append(protocol_options,
            purple_account_option_bool_new(
                    _("On reconnect, skip messages which were received in a "
                      "previous session"),
                    PRPL_ACCOUNT_OPT_SKIP_OLD_MESSAGES, FALSE));

    prpl_info.protocol_options = protocol_options;
}

static void matrixprpl_destroy(PurplePlugin *plugin) {
    purple_debug_info("matrixprpl", "shutting down\n");
}

static GList *matrixprpl_actions(PurplePlugin *plugin, gpointer context)
{
  GList *list = NULL;

  list = matrix_e2e_actions(list);

  return list;
}

static PurplePluginInfo info =
{
    PURPLE_PLUGIN_MAGIC,                                     /* magic */
    PURPLE_MAJOR_VERSION,                                    /* major_version */
    PURPLE_MINOR_VERSION,                                    /* minor_version */
    PURPLE_PLUGIN_PROTOCOL,                                  /* type */
    NULL,                                                    /* ui_requirement */
    0,                                                       /* flags */
    NULL,                                                    /* dependencies */
    PURPLE_PRIORITY_DEFAULT,                                 /* priority */
    PRPL_ID,                                                 /* id */
    "Matrix",                                                /* name */
    DISPLAY_VERSION,                                         /* version */
    N_("Matrix Protocol Plugin"),                            /* summary */
    N_("Matrix Protocol Plugin"),                            /* description */
    "Richard van der Hoff <richard@matrix.org>",             /* author */
    MATRIX_WEBSITE,                                          /* homepage */
    NULL,                                                    /* load */
    NULL,                                                    /* unload */
    matrixprpl_destroy,                                      /* destroy */
    NULL,                                                    /* ui_info */
    &prpl_info,                                              /* extra_info */
    NULL,                                                    /* prefs_info */
    matrixprpl_actions,                                      /* actions */
    NULL,                                                    /* padding... */
    NULL,
    NULL,
    NULL,
};

PURPLE_INIT_PLUGIN(matrix, matrixprpl_init, info);
