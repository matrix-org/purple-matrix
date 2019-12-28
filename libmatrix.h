/**
 * libmatrix.h
 *
 * Defines some common macros and structures which are used in a number of
 * places throughout the code.
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

/* Some notes on libpurple's object model.
 *
 * +---------------+
 * | PurpleAccount | <-.
 * +---------------+   |
 * | gc            | --+--.
 * .               .   |  |
 * +---------------+   |  |
 *                     |  |
 *        .------------'  |
 *        |               V
 *        |     +------------------+             +----------------------+
 *        |     | PurpleConnection |<--.     ,-->| MatrixConnectionData |
 *        |     +------------------+   |     |   +----------------------+
 *        +-----| account          |   `-----+---| pc                   |
 *        |     | protocol_data    |---------'   .                      .
 *        |     .                  .             +----------------------+
 *        |     +------------------+
 *        |
 *        |     +--------------------+            +--------------------+
 *        |     | PurpleConversation |<--.   ,--->| PurpleConvChat     |
 *        |     +--------------------+   |   |    +--------------------+
 *        +-----| account            |   `---+----| conv               |
 *        |     | name               |       |    .                    .
 *        |     | title              |       |    +--------------------+
 *        |     | u.chat             |-------'
 *        |     | data               |
 *        |     .                    .
 *        |     +--------------------+
 *        |
 *        |     +--------------------+
 *        |     | PurpleBlistNode    |
 *        |     +--------------------+
 *        |     .                    .
 *        |     +--------------------+
 *        |               ^
 *        |               | (inherits)
 *        |     +--------------------+
 *        |     | PurpleChat         |
 *        |     +--------------------+
 *        '-----| account            |
 *              | components         |
 *              .                    .
 *              +--------------------+
 *
 *
 *
 * There is one PurpleAccount for each account the user has configured.
 *
 * Each PurpleAccount has at most one active connections. When the user enables
 * the account, a PurpleConnection is made, and we attach a MatrixConnectionData
 * to it - this is managed in matrix-connection.c. If there is an error on the
 * connection, or the user explicitly disables the account, the PurpleConnection
 * is deleted, and the MatrixConnectionData along with it.
 *
 * A PurpleChat (and a PurpleBuddy, but we don't have much to do with them)
 * represents an entry on the buddylist. It has a hashtable called 'components'
 * which stores the necessary information about the chat - in our case this is
 * just the room id.
 *
 * A PurpleConversation represents an active conversation, and has a chat window
 * associated with it. Its 'name' is not visible to the user; instead it is
 * a unique id for the conversation - in our case the room id. Libpurple has
 * some magic which looks up the 'name' against the 'components' of PurpleChats
 * in the buddy list, and if a match is found, will set the title automatically.
 *
 * The PurpleConversation also has a hashtable which is used to track a range of
 * protocol-specific data: see PURPLE_CONV_DATA_* in matrix-room.c for details
 * of this.
 */

#ifndef LIBMATRIX_H
#define LIBMATRIX_H

#include <glib.h>

/* stub out the gettext macros for now */
#define _(a) (a)
#define N_(a) (a)

/* data for our 'about' box */
#define DISPLAY_VERSION "1.0"
#define MATRIX_WEBSITE "http://matrix.org"

/* our protocol ID string */
#define PRPL_ID "prpl-matrix"

/* identifiers for account options
 *
 * some of these are registered as options for the UI, and some are strictly
 * internal. But they end up in the same place in the settings file, so they
 * share a namespace.
 */
#define PRPL_ACCOUNT_OPT_HOME_SERVER "home_server"
#define PRPL_ACCOUNT_OPT_NEXT_BATCH "next_batch"
#define PRPL_ACCOUNT_OPT_SKIP_OLD_MESSAGES "skip_old_messages"
/* Pickled account info from olm_pickle_account */
#define PRPL_ACCOUNT_OPT_OLM_ACCOUNT_KEYS "olm_account_keys"
/* Access token, after a login */
#define PRPL_ACCOUNT_OPT_ACCESS_TOKEN "access_token"

/* defaults for account options */
#define DEFAULT_HOME_SERVER "https://matrix.org"

/* identifiers for the chat info / "components" */
#define PRPL_CHAT_INFO_ROOM_ID "room_id"

#endif
