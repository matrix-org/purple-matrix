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

#include <stdarg.h>
#include <string.h>
#include <time.h>

#include <glib.h>

#include "account.h"
#include "accountopt.h"
#include "blist.h"
#include "cmds.h"
#include "conversation.h"
#include "connection.h"
#include "debug.h"
#include "notify.h"
#include "privacy.h"
#include "prpl.h"
#include "roomlist.h"
#include "status.h"
#include "util.h"
#include "version.h"

#include "matrix-connection.h"
#include "matrix-room.h"

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
 * Start the connection to a matrix account
 */
void matrixprpl_login(PurpleAccount *acct)
{
    PurpleConnection *pc = purple_account_get_connection(acct);
    matrix_connection_new(pc);
    matrix_connection_start_login(pc);
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



/******************************************************************************
 * The following comes from the 'nullprpl' dummy protocol. TODO: clear this out
 * and keep only what we need.
 */

static PurplePlugin *_matrix_protocol = NULL;

#define MATRIX_STATUS_ONLINE   "online"
#define MATRIX_STATUS_AWAY     "away"
#define MATRIX_STATUS_OFFLINE  "offline"

typedef void (*GcFunc)(PurpleConnection *from,
                       PurpleConnection *to,
                       gpointer userdata);

typedef struct {
    GcFunc fn;
    PurpleConnection *from;
    gpointer userdata;
} GcFuncData;

/*
 * helpers
 */
static PurpleConnection *get_matrixprpl_gc(const char *username) {
    PurpleAccount *acct = purple_accounts_find(username, PRPL_ID);
    if (acct && purple_account_is_connected(acct))
        return acct->gc;
    else
        return NULL;
}

static void call_if_matrixprpl(gpointer data, gpointer userdata) {
    PurpleConnection *gc = (PurpleConnection *)(data);
    GcFuncData *gcfdata = (GcFuncData *)userdata;

    if (!strcmp(gc->account->protocol_id, PRPL_ID))
        gcfdata->fn(gcfdata->from, gc, gcfdata->userdata);
}

static void foreach_matrixprpl_gc(GcFunc fn, PurpleConnection *from,
                                  gpointer userdata) {
    GcFuncData gcfdata = { fn, from, userdata };
    g_list_foreach(purple_connections_get_all(), call_if_matrixprpl,
                   &gcfdata);
}


typedef void(*ChatFunc)(PurpleConvChat *from, PurpleConvChat *to,
                        int id, const char *room, gpointer userdata);

typedef struct {
    ChatFunc fn;
    PurpleConvChat *from_chat;
    gpointer userdata;
} ChatFuncData;

static void call_chat_func(gpointer data, gpointer userdata) {
    PurpleConnection *to = (PurpleConnection *)data;
    ChatFuncData *cfdata = (ChatFuncData *)userdata;

    int id = cfdata->from_chat->id;
    PurpleConversation *conv = purple_find_chat(to, id);
    if (conv) {
        PurpleConvChat *chat = purple_conversation_get_chat_data(conv);
        cfdata->fn(cfdata->from_chat, chat, id, conv->name, cfdata->userdata);
    }
}

static void foreach_gc_in_chat(ChatFunc fn, PurpleConnection *from,
                               int id, gpointer userdata) {
    PurpleConversation *conv = purple_find_chat(from, id);
    ChatFuncData cfdata = { fn,
                            purple_conversation_get_chat_data(conv),
                            userdata };

    g_list_foreach(purple_connections_get_all(), call_chat_func,
                   &cfdata);
}


static void discover_status(PurpleConnection *from, PurpleConnection *to,
                            gpointer userdata) {
    const char *from_username = from->account->username;
    const char *to_username = to->account->username;

    if (purple_find_buddy(from->account, to_username)) {
        PurpleStatus *status = purple_account_get_active_status(to->account);
        const char *status_id = purple_status_get_id(status);
        const char *message = purple_status_get_attr_string(status, "message");

        if (!strcmp(status_id, MATRIX_STATUS_ONLINE) ||
            !strcmp(status_id, MATRIX_STATUS_AWAY) ||
            !strcmp(status_id, MATRIX_STATUS_OFFLINE)) {
            purple_debug_info("matrixprpl", "%s sees that %s is %s: %s\n",
                              from_username, to_username, status_id, message);
            purple_prpl_got_user_status(from->account, to_username, status_id,
                                        (message) ? "message" : NULL, message, NULL);
        } else {
            purple_debug_error("matrixprpl",
                               "%s's buddy %s has an unknown status: %s, %s",
                               from_username, to_username, status_id, message);
        }
    }
}

static void report_status_change(PurpleConnection *from, PurpleConnection *to,
                                 gpointer userdata) {
    purple_debug_info("matrixprpl", "notifying %s that %s changed status\n",
                      to->account->username, from->account->username);
    discover_status(to, from, NULL);
}


/*
 * UI callbacks
 */
static void matrixprpl_input_user_info(PurplePluginAction *action)
{
    PurpleConnection *gc = (PurpleConnection *)action->context;
    PurpleAccount *acct = purple_connection_get_account(gc);
    purple_debug_info("matrixprpl", "showing 'Set User Info' dialog for %s\n",
                      acct->username);

    purple_account_request_change_user_info(acct);
}

/* this is set to the actions member of the PurplePluginInfo struct at the
 * bottom.
 */
static GList *matrixprpl_actions(PurplePlugin *plugin, gpointer context)
{
    PurplePluginAction *action = purple_plugin_action_new(
        _("Set User Info..."), matrixprpl_input_user_info);
    return g_list_append(NULL, action);
}


/*
 * prpl functions
 */

static char *matrixprpl_status_text(PurpleBuddy *buddy) {
    purple_debug_info("matrixprpl", "getting %s's status text for %s\n",
                      buddy->name, buddy->account->username);

    if (purple_find_buddy(buddy->account, buddy->name)) {
        PurplePresence *presence = purple_buddy_get_presence(buddy);
        PurpleStatus *status = purple_presence_get_active_status(presence);
        const char *name = purple_status_get_name(status);
        const char *message = purple_status_get_attr_string(status, "message");

        char *text;
        if (message && strlen(message) > 0)
            text = g_strdup_printf("%s: %s", name, message);
        else
            text = g_strdup(name);

        purple_debug_info("matrixprpl", "%s's status text is %s\n", buddy->name, text);
        return text;

    } else {
        purple_debug_info("matrixprpl", "...but %s is not logged in\n", buddy->name);
        return g_strdup("Not logged in");
    }
}

static void matrixprpl_tooltip_text(PurpleBuddy *buddy,
                                    PurpleNotifyUserInfo *info,
                                    gboolean full) {
    PurpleConnection *gc = get_matrixprpl_gc(buddy->name);

    if (gc) {
        /* they're logged in */
        PurplePresence *presence = purple_buddy_get_presence(buddy);
        PurpleStatus *status = purple_presence_get_active_status(presence);
        char *msg = matrixprpl_status_text(buddy);
        purple_notify_user_info_add_pair(info, purple_status_get_name(status),
                                         msg);
        g_free(msg);

        if (full) {
            const char *user_info = purple_account_get_user_info(gc->account);
            if (user_info)
                purple_notify_user_info_add_pair(info, _("User info"), user_info);
        }

    } else {
        /* they're not logged in */
        purple_notify_user_info_add_pair(info, _("User info"), _("not logged in"));
    }

    purple_debug_info("matrixprpl", "showing %s tooltip for %s\n",
                      (full) ? "full" : "short", buddy->name);
}

static GList *matrixprpl_status_types(PurpleAccount *acct)
{
    GList *types = NULL;
    PurpleStatusType *type;

    purple_debug_info("matrixprpl", "returning status types for %s: %s, %s, %s\n",
                      acct->username,
                      MATRIX_STATUS_ONLINE, MATRIX_STATUS_AWAY, MATRIX_STATUS_OFFLINE);

    type = purple_status_type_new_with_attrs(PURPLE_STATUS_AVAILABLE,
                                             MATRIX_STATUS_ONLINE, NULL, TRUE, TRUE, FALSE,
                                             "message", _("Message"), purple_value_new(PURPLE_TYPE_STRING),
                                             NULL);
    types = g_list_prepend(types, type);

    type = purple_status_type_new_with_attrs(PURPLE_STATUS_AWAY,
                                             MATRIX_STATUS_AWAY, NULL, TRUE, TRUE, FALSE,
                                             "message", _("Message"), purple_value_new(PURPLE_TYPE_STRING),
                                             NULL);
    types = g_list_prepend(types, type);

    type = purple_status_type_new_with_attrs(PURPLE_STATUS_OFFLINE,
                                             MATRIX_STATUS_OFFLINE, NULL, TRUE, TRUE, FALSE,
                                             "message", _("Message"), purple_value_new(PURPLE_TYPE_STRING),
                                             NULL);
    types = g_list_prepend(types, type);

    return g_list_reverse(types);
}

static void blist_example_menu_item(PurpleBlistNode *node, gpointer userdata) {
    purple_debug_info("matrixprpl", "example menu item clicked on user %s\n",
                      ((PurpleBuddy *)node)->name);

    purple_notify_info(NULL,  /* plugin handle or PurpleConnection */
                       _("Primary title"),
                       _("Secondary title"),
                       _("This is the callback for the matrixprpl menu item."));
}

static GList *matrixprpl_blist_node_menu(PurpleBlistNode *node) {
    purple_debug_info("matrixprpl", "providing buddy list context menu item\n");

    if (PURPLE_BLIST_NODE_IS_BUDDY(node)) {
        PurpleMenuAction *action = purple_menu_action_new(
            _("Matrixprpl example menu item"),
            PURPLE_CALLBACK(blist_example_menu_item),
            NULL,   /* userdata passed to the callback */
            NULL);  /* child menu items */
        return g_list_append(NULL, action);
    } else {
        return NULL;
    }
}

static int matrixprpl_send_im(PurpleConnection *gc, const char *who,
                              const char *message, PurpleMessageFlags flags)
{
    const char *from_username = gc->account->username;
    PurpleAccount *to_acct = purple_accounts_find(who, PRPL_ID);

    purple_debug_info("matrixprpl", "sending message from %s to %s: %s\n",
                      from_username, who, message);

    /* is the sender blocked by the recipient's privacy settings? */
    if (to_acct && !purple_privacy_check(to_acct, gc->account->username)) {
        char *msg = g_strdup_printf(
            _("Your message was blocked by %s's privacy settings."), who);
        purple_debug_info("matrixprpl",
                          "discarding; %s is blocked by %s's privacy settings\n",
                          from_username, who);
        purple_conv_present_error(who, gc->account, msg);
        g_free(msg);
        return 0;
    }

    return 1;
}

static void matrixprpl_set_info(PurpleConnection *gc, const char *info) {
    purple_debug_info("matrixprpl", "setting %s's user info to %s\n",
                      gc->account->username, info);
}

static const char *typing_state_to_string(PurpleTypingState typing) {
    switch (typing) {
        case PURPLE_NOT_TYPING:  return "is not typing";
        case PURPLE_TYPING:      return "is typing";
        case PURPLE_TYPED:       return "stopped typing momentarily";
        default:               return "unknown typing state";
    }
}

static void notify_typing(PurpleConnection *from, PurpleConnection *to,
                          gpointer typing) {
    const char *from_username = from->account->username;
    const char *action = typing_state_to_string((PurpleTypingState)typing);
    purple_debug_info("matrixprpl", "notifying %s that %s %s\n",
                      to->account->username, from_username, action);

    serv_got_typing(to,
                    from_username,
                    0, /* if non-zero, a timeout in seconds after which to
                        * reset the typing status to PURPLE_NOT_TYPING */
                    (PurpleTypingState)typing);
}

static unsigned int matrixprpl_send_typing(PurpleConnection *gc, const char *name,
                                           PurpleTypingState typing) {
    purple_debug_info("matrixprpl", "%s %s\n", gc->account->username,
                      typing_state_to_string(typing));
    foreach_matrixprpl_gc(notify_typing, gc, (gpointer)typing);
    return 0;
}

static void matrixprpl_get_info(PurpleConnection *gc, const char *username) {
    const char *body;
    PurpleNotifyUserInfo *info = purple_notify_user_info_new();
    PurpleAccount *acct;

    purple_debug_info("matrixprpl", "Fetching %s's user info for %s\n", username,
                      gc->account->username);

    if (!get_matrixprpl_gc(username)) {
        char *msg = g_strdup_printf(_("%s is not logged in."), username);
        purple_notify_error(gc, _("User Info"), _("User info not available. "), msg);
        g_free(msg);
    }

    acct = purple_accounts_find(username, PRPL_ID);
    if (acct)
        body = purple_account_get_user_info(acct);
    else
        body = _("No user info.");
    purple_notify_user_info_add_pair(info, "Info", body);

    /* show a buddy's user info in a nice dialog box */
    purple_notify_userinfo(gc,        /* connection the buddy info came through */
                           username,  /* buddy's username */
                           info,      /* body */
                           NULL,      /* callback called when dialog closed */
                           NULL);     /* userdata for callback */
}

static void matrixprpl_set_status(PurpleAccount *acct, PurpleStatus *status) {
    const char *msg = purple_status_get_attr_string(status, "message");
    purple_debug_info("matrixprpl", "setting %s's status to %s: %s\n",
                      acct->username, purple_status_get_name(status), msg);

    foreach_matrixprpl_gc(report_status_change, get_matrixprpl_gc(acct->username),
                          NULL);
}

static void matrixprpl_set_idle(PurpleConnection *gc, int idletime) {
    purple_debug_info("matrixprpl",
                      "purple reports that %s has been idle for %d seconds\n",
                      gc->account->username, idletime);
}

static void matrixprpl_change_passwd(PurpleConnection *gc, const char *old_pass,
                                     const char *new_pass) {
    purple_debug_info("matrixprpl", "%s wants to change their password\n",
                      gc->account->username);
}

static void matrixprpl_add_buddy(PurpleConnection *gc, PurpleBuddy *buddy,
                                 PurpleGroup *group)
{
    const char *username = gc->account->username;
    PurpleConnection *buddy_gc = get_matrixprpl_gc(buddy->name);

    purple_debug_info("matrixprpl", "adding %s to %s's buddy list\n", buddy->name,
                      username);

    if (buddy_gc) {
        PurpleAccount *buddy_acct = buddy_gc->account;

        discover_status(gc, buddy_gc, NULL);

        if (purple_find_buddy(buddy_acct, username)) {
            purple_debug_info("matrixprpl", "%s is already on %s's buddy list\n",
                              username, buddy->name);
        } else {
            purple_debug_info("matrixprpl", "asking %s if they want to add %s\n",
                              buddy->name, username);
            purple_account_request_add(buddy_acct,
                                       username,
                                       NULL,   /* local account id (rarely used) */
                                       NULL,   /* alias */
                                       NULL);  /* message */
        }
    }
}

static void matrixprpl_add_buddies(PurpleConnection *gc, GList *buddies,
                                   GList *groups) {
    GList *buddy = buddies;
    GList *group = groups;

    purple_debug_info("matrixprpl", "adding multiple buddies\n");

    while (buddy && group) {
        matrixprpl_add_buddy(gc, (PurpleBuddy *)buddy->data, (PurpleGroup *)group->data);
        buddy = g_list_next(buddy);
        group = g_list_next(group);
    }
}

static void matrixprpl_remove_buddy(PurpleConnection *gc, PurpleBuddy *buddy,
                                    PurpleGroup *group)
{
    purple_debug_info("matrixprpl", "removing %s from %s's buddy list\n",
                      buddy->name, gc->account->username);
}

static void matrixprpl_remove_buddies(PurpleConnection *gc, GList *buddies,
                                      GList *groups) {
    GList *buddy = buddies;
    GList *group = groups;

    purple_debug_info("matrixprpl", "removing multiple buddies\n");

    while (buddy && group) {
        matrixprpl_remove_buddy(gc, (PurpleBuddy *)buddy->data,
                                (PurpleGroup *)group->data);
        buddy = g_list_next(buddy);
        group = g_list_next(group);
    }
}

/*
 * matrixprpl uses purple's local whitelist and blacklist, stored in blist.xml, as
 * its authoritative privacy settings, and uses purple's logic (specifically
 * purple_privacy_check(), from privacy.h), to determine whether messages are
 * allowed or blocked.
 */
static void matrixprpl_add_permit(PurpleConnection *gc, const char *name) {
    purple_debug_info("matrixprpl", "%s adds %s to their allowed list\n",
                      gc->account->username, name);
}

static void matrixprpl_add_deny(PurpleConnection *gc, const char *name) {
    purple_debug_info("matrixprpl", "%s adds %s to their blocked list\n",
                      gc->account->username, name);
}

static void matrixprpl_rem_permit(PurpleConnection *gc, const char *name) {
    purple_debug_info("matrixprpl", "%s removes %s from their allowed list\n",
                      gc->account->username, name);
}

static void matrixprpl_rem_deny(PurpleConnection *gc, const char *name) {
    purple_debug_info("matrixprpl", "%s removes %s from their blocked list\n",
                      gc->account->username, name);
}

static void matrixprpl_set_permit_deny(PurpleConnection *gc) {
}

static void matrixprpl_chat_invite(PurpleConnection *gc, int id,
                                   const char *message, const char *who) {
    const char *username = gc->account->username;
    PurpleConversation *conv = purple_find_chat(gc, id);
    const char *room = conv->name;
    PurpleAccount *to_acct = purple_accounts_find(who, PRPL_ID);

    purple_debug_info("matrixprpl", "%s is inviting %s to join chat room %s\n",
                      username, who, room);

    if (to_acct) {
        PurpleConversation *to_conv = purple_find_chat(to_acct->gc, id);
        if (to_conv) {
            char *tmp = g_strdup_printf("%s is already in chat room %s.", who, room);
            purple_debug_info("matrixprpl",
                              "%s is already in chat room %s; "
                              "ignoring invitation from %s\n",
                              who, room, username);
            purple_notify_info(gc, _("Chat invitation"), _("Chat invitation"), tmp);
            g_free(tmp);
        } else {
            GHashTable *components;
            components = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
            g_hash_table_replace(components, "room", g_strdup(room));
            g_hash_table_replace(components, "invited_by", g_strdup(username));
            serv_got_chat_invite(to_acct->gc, room, username, message, components);
        }
    }
}

#if 0
static PurpleCmdRet send_whisper(PurpleConversation *conv, const gchar *cmd,
                                 gchar **args, gchar **error, void *userdata) {
    const char *to_username;
    const char *message;
    const char *from_username;
    PurpleConvChat *chat;
    PurpleConvChatBuddy *chat_buddy;
    PurpleConnection *to;

    /* parse args */
    to_username = args[0];
    message = args[1];

    if (!to_username || strlen(to_username) == 0) {
        *error = g_strdup(_("Whisper is missing recipient."));
        return PURPLE_CMD_RET_FAILED;
    } else if (!message || strlen(message) == 0) {
        *error = g_strdup(_("Whisper is missing message."));
        return PURPLE_CMD_RET_FAILED;
    }

    from_username = conv->account->username;
    purple_debug_info("matrixprpl", "%s whispers to %s in chat room %s: %s\n",
                      from_username, to_username, conv->name, message);

    chat = purple_conversation_get_chat_data(conv);
    chat_buddy = purple_conv_chat_cb_find(chat, to_username);
    to = get_matrixprpl_gc(to_username);

    if (!chat_buddy) {
        /* this will be freed by the caller */
        *error = g_strdup_printf(_("%s is not logged in."), to_username);
        return PURPLE_CMD_RET_FAILED;
    } else if (!to) {
        *error = g_strdup_printf(_("%s is not in this chat room."), to_username);
        return PURPLE_CMD_RET_FAILED;
    } else {
        /* write the whisper in the sender's chat window  */
        char *message_to = g_strdup_printf("%s (to %s)", message, to_username);
        purple_conv_chat_write(chat, from_username, message_to,
                               PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_WHISPER,
                               time(NULL));
        g_free(message_to);

        /* send the whisper */
        serv_chat_whisper(to, chat->id, from_username, message);

        return PURPLE_CMD_RET_OK;
    }
}
#endif

static void matrixprpl_chat_whisper(PurpleConnection *gc, int id, const char *who,
                                    const char *message) {
    const char *username = gc->account->username;
    PurpleConversation *conv = purple_find_chat(gc, id);
    purple_debug_info("matrixprpl",
                      "%s receives whisper from %s in chat room %s: %s\n",
                      username, who, conv->name, message);

    /* receive whisper on recipient's account */
    serv_got_chat_in(gc, id, who, PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_WHISPER,
                     message, time(NULL));
}


static void matrixprpl_register_user(PurpleAccount *acct) {
    purple_debug_info("matrixprpl", "registering account for %s\n",
                      acct->username);
}

static void matrixprpl_get_cb_info(PurpleConnection *gc, int id, const char *who) {
    PurpleConversation *conv = purple_find_chat(gc, id);
    purple_debug_info("matrixprpl",
                      "retrieving %s's info for %s in chat room %s\n", who,
                      gc->account->username, conv->name);

    matrixprpl_get_info(gc, who);
}

static void matrixprpl_alias_buddy(PurpleConnection *gc, const char *who,
                                   const char *alias) {
    purple_debug_info("matrixprpl", "%s sets %s's alias to %s\n",
                      gc->account->username, who, alias);
}

static void matrixprpl_group_buddy(PurpleConnection *gc, const char *who,
                                   const char *old_group,
                                   const char *new_group) {
    purple_debug_info("matrixprpl", "%s has moved %s from group %s to group %s\n",
                      gc->account->username, who, old_group, new_group);
}

static void matrixprpl_rename_group(PurpleConnection *gc, const char *old_name,
                                    PurpleGroup *group, GList *moved_buddies) {
    purple_debug_info("matrixprpl", "%s has renamed group %s to %s\n",
                      gc->account->username, old_name, group->name);
}

static void matrixprpl_convo_closed(PurpleConnection *gc, const char *who) {
    purple_debug_info("matrixprpl", "%s's conversation with %s was closed\n",
                      gc->account->username, who);
}

/* normalize a username (e.g. remove whitespace, add default domain, etc.)
 * for matrixprpl, this is a noop.
 */
static const char *matrixprpl_normalize(const PurpleAccount *acct,
                                        const char *input) {
    return NULL;
}

static void matrixprpl_set_buddy_icon(PurpleConnection *gc,
                                      PurpleStoredImage *img) {
    purple_debug_info("matrixprpl", "setting %s's buddy icon to %s\n",
                      gc->account->username,
                      img ? purple_imgstore_get_filename(img) : "(matrix)");
}

static void matrixprpl_remove_group(PurpleConnection *gc, PurpleGroup *group) {
    purple_debug_info("matrixprpl", "%s has removed group %s\n",
                      gc->account->username, group->name);
}


static void set_chat_topic_fn(PurpleConvChat *from, PurpleConvChat *to,
                              int id, const char *room, gpointer userdata) {
    const char *topic = (const char *)userdata;
    const char *username = from->conv->account->username;
    char *msg;

    purple_conv_chat_set_topic(to, username, topic);

    if (topic && strlen(topic) > 0)
        msg = g_strdup_printf(_("%s sets topic to: %s"), username, topic);
    else
        msg = g_strdup_printf(_("%s clears topic"), username);

    purple_conv_chat_write(to, username, msg,
                           PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_NO_LOG,
                           time(NULL));
    g_free(msg);
}

static void matrixprpl_set_chat_topic(PurpleConnection *gc, int id,
                                      const char *topic) {
    PurpleConversation *conv = purple_find_chat(gc, id);
    PurpleConvChat *chat = purple_conversation_get_chat_data(conv);
    const char *last_topic;

    if (!chat)
        return;

    purple_debug_info("matrixprpl", "%s sets topic of chat room '%s' to '%s'\n",
                      gc->account->username, conv->name, topic);

    last_topic = purple_conv_chat_get_topic(chat);
    if ((!topic && !last_topic) ||
        (topic && last_topic && !strcmp(topic, last_topic)))
        return;  /* topic is unchanged, this is a noop */

    foreach_gc_in_chat(set_chat_topic_fn, gc, id, (gpointer)topic);
}

static gboolean matrixprpl_finish_get_roomlist(gpointer roomlist) {
    purple_roomlist_set_in_progress((PurpleRoomlist *)roomlist, FALSE);
    return FALSE;
}

static PurpleRoomlist *matrixprpl_roomlist_get_list(PurpleConnection *gc) {
    const char *username = gc->account->username;
    PurpleRoomlist *roomlist = purple_roomlist_new(gc->account);
    GList *fields = NULL;
    PurpleRoomlistField *field;
    GList *chats;
    GList *seen_ids = NULL;

    purple_debug_info("matrixprpl", "%s asks for room list; returning:\n", username);

    /* set up the room list */
    field = purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_STRING, "room",
                                      "room", TRUE /* hidden */);
    fields = g_list_append(fields, field);

    field = purple_roomlist_field_new(PURPLE_ROOMLIST_FIELD_INT, "Id", "Id", FALSE);
    fields = g_list_append(fields, field);

    purple_roomlist_set_fields(roomlist, fields);

    /* add each chat room. the chat ids are cached in seen_ids so that each room
     * is only returned once, even if multiple users are in it. */
    for (chats  = purple_get_chats(); chats; chats = g_list_next(chats)) {
        PurpleConversation *conv = (PurpleConversation *)chats->data;
        PurpleRoomlistRoom *room;
        const char *name = conv->name;
        int id = purple_conversation_get_chat_data(conv)->id;

        /* have we already added this room? */
        if (g_list_find_custom(seen_ids, name, (GCompareFunc)strcmp))
            continue;                                /* yes! try the next one. */

        /* This cast is OK because this list is only staying around for the life
         * of this function and none of the conversations are being deleted
         * in that timespan. */
        seen_ids = g_list_prepend(seen_ids, (char *)name); /* no, it's new. */
        purple_debug_info("matrixprpl", "%s (%d), ", name, id);

        room = purple_roomlist_room_new(PURPLE_ROOMLIST_ROOMTYPE_ROOM, name, NULL);
        purple_roomlist_room_add_field(roomlist, room, name);
        purple_roomlist_room_add_field(roomlist, room, &id);
        purple_roomlist_room_add(roomlist, room);
    }

    g_list_free(seen_ids);
    purple_timeout_add(1 /* ms */, matrixprpl_finish_get_roomlist, roomlist);
    return roomlist;
}

static void matrixprpl_roomlist_cancel(PurpleRoomlist *list) {
    purple_debug_info("matrixprpl", "%s asked to cancel room list request\n",
                      list->account->username);
}

static void matrixprpl_roomlist_expand_category(PurpleRoomlist *list,
                                                PurpleRoomlistRoom *category) {
    purple_debug_info("matrixprpl", "%s asked to expand room list category %s\n",
                      list->account->username, category->name);
}

/* matrixprpl doesn't support file transfer...yet... */
static gboolean matrixprpl_can_receive_file(PurpleConnection *gc,
                                            const char *who) {
    return FALSE;
}

static gboolean matrixprpl_offline_message(const PurpleBuddy *buddy) {
    purple_debug_info("matrixprpl",
                      "reporting that offline messages are supported for %s\n",
                      buddy->name);
    return TRUE;
}


/******************************************************************************
 *
 * prpl stuff. see prpl.h for more information.
 */

static PurplePluginProtocolInfo prpl_info =
{
    OPT_PROTO_CHAT_TOPIC,                /* options */
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
    NULL,                                /* list_emblem */
    matrixprpl_status_text,                /* status_text */
    matrixprpl_tooltip_text,               /* tooltip_text */
    matrixprpl_status_types,               /* status_types */
    matrixprpl_blist_node_menu,            /* blist_node_menu */
    matrixprpl_chat_info,                  /* chat_info */
    matrixprpl_chat_info_defaults,         /* chat_info_defaults */
    matrixprpl_login,                      /* login */
    matrixprpl_close,                      /* close */
    matrixprpl_send_im,                    /* send_im */
    matrixprpl_set_info,                   /* set_info */
    matrixprpl_send_typing,                /* send_typing */
    matrixprpl_get_info,                   /* get_info */
    matrixprpl_set_status,                 /* set_status */
    matrixprpl_set_idle,                   /* set_idle */
    matrixprpl_change_passwd,              /* change_passwd */
    matrixprpl_add_buddy,                  /* add_buddy */
    matrixprpl_add_buddies,                /* add_buddies */
    matrixprpl_remove_buddy,               /* remove_buddy */
    matrixprpl_remove_buddies,             /* remove_buddies */
    matrixprpl_add_permit,                 /* add_permit */
    matrixprpl_add_deny,                   /* add_deny */
    matrixprpl_rem_permit,                 /* rem_permit */
    matrixprpl_rem_deny,                   /* rem_deny */
    matrixprpl_set_permit_deny,            /* set_permit_deny */
    matrixprpl_join_chat,                  /* join_chat */
    matrixprpl_reject_chat,                /* reject_chat */
    matrixprpl_get_chat_name,              /* get_chat_name */
    matrixprpl_chat_invite,                /* chat_invite */
    matrixprpl_chat_leave,                 /* chat_leave */
    matrixprpl_chat_whisper,               /* chat_whisper */
    matrixprpl_chat_send,                  /* chat_send */
    NULL,                                /* keepalive */
    matrixprpl_register_user,              /* register_user */
    matrixprpl_get_cb_info,                /* get_cb_info */
    NULL,                                /* get_cb_away */
    matrixprpl_alias_buddy,                /* alias_buddy */
    matrixprpl_group_buddy,                /* group_buddy */
    matrixprpl_rename_group,               /* rename_group */
    NULL,                                /* buddy_free */
    matrixprpl_convo_closed,               /* convo_closed */
    matrixprpl_normalize,                  /* normalize */
    matrixprpl_set_buddy_icon,             /* set_buddy_icon */
    matrixprpl_remove_group,               /* remove_group */
    NULL,                                /* get_cb_real_name */
    matrixprpl_set_chat_topic,             /* set_chat_topic */
    NULL,                                  /* find_blist_chat */
    matrixprpl_roomlist_get_list,          /* roomlist_get_list */
    matrixprpl_roomlist_cancel,            /* roomlist_cancel */
    matrixprpl_roomlist_expand_category,   /* roomlist_expand_category */
    matrixprpl_can_receive_file,           /* can_receive_file */
    NULL,                                /* send_file */
    NULL,                                /* new_xfer */
    matrixprpl_offline_message,            /* offline_message */
    NULL,                                /* whiteboard_prpl_ops */
    NULL,                                /* send_raw */
    NULL,                                /* roomlist_room_serialize */
    NULL,                                /* unregister_user */
    NULL,                                /* send_attention */
    NULL,                                /* get_attention_types */
    sizeof(PurplePluginProtocolInfo),    /* struct_size */
    NULL,                                /* get_account_text_table */
    NULL,                                /* initiate_media */
    NULL,                                /* get_media_caps */
    NULL,                                /* get_moods */
    NULL,                                /* set_public_alias */
    NULL,                                /* get_public_alias */
    NULL,                                /* add_buddy_with_invite */
    NULL                                 /* add_buddies_with_invite */
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
                    _("On reconnect, replay recent messages from joined rooms"),
                    PRPL_ACCOUNT_OPT_REPLAY_OLD_MESSAGES, TRUE));

    prpl_info.protocol_options = protocol_options;


#if 0
    /* register whisper chat command, /msg */
    purple_cmd_register("msg",
                        "ws",                  /* args: recipient and message */
                        PURPLE_CMD_P_DEFAULT,  /* priority */
                        PURPLE_CMD_FLAG_CHAT,
                        "prpl-matrix",
                        send_whisper,
                        "msg &lt;username&gt; &lt;message&gt;: send a private message, aka a whisper",
                        NULL);                 /* userdata */
#endif

    _matrix_protocol = plugin;
}

static void matrixprpl_destroy(PurplePlugin *plugin) {
    purple_debug_info("matrixprpl", "shutting down\n");
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
