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

/* identifiers for account options */
#define PRPL_ACCOUNT_OPT_HOME_SERVER "home_server"

/* defaults for account options */
#define DEFAULT_HOME_SERVER "https://matrix.org"

/* identifiers for the chat info / "components" */
#define PRPL_CHAT_INFO_ROOM_ID "room_id"

typedef struct _MatrixAccount {
    struct _PurpleAccount *pa;
    struct _PurpleConnection *pc;
    gchar *homeserver;      /* hostname (:port) of the homeserver */
    gchar *access_token;    /* access token corresponding to our user */
} MatrixAccount;

#endif
