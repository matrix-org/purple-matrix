# purple-matrix [![#purple on matrix.org](https://img.shields.io/matrix/purple:matrix.org.svg?label=%23purple%3Amatrix.org&logo=matrix&server_fqdn=matrix.org)](https://matrix.to/#/#purple:matrix.org)

This project is a plugin for
[libpurple](https://developer.pidgin.im/wiki/WhatIsLibpurple) which adds the
ability to communicate with [matrix.org](http://matrix.org) homeservers to any
libpurple-based clients (such as [Pidgin](http://www.pidgin.im)).

If you want to bridge the other way, using a matrix client to communicate with any backend supported by libpurple, see [matrix-bifrost](https://github.com/matrix-org/matrix-bifrost).

# Update 2022/04/11

This project is essentially unmaintained. It may still work for you, in which case
good luck to you; however, it lacks many important features that are critical to a
modern Matrix client (not least of which is [end-to-end encryption support](https://github.com/matrix-org/purple-matrix/issues/18)).

# Status

This project is somewhat alpha, and only basic functionality has been
implemented. Sending and receiving simple text messages is supported, as is
joining rooms you are invited to by other users.

The following are not yet supported:
 * Creating new rooms (and one-to-one chats)
 * Presence indication
 * Typing indication
 * Videos/rich text in messages
 * Account registration
 * Room topics
 * Voice/video calling

The following are in progress:
 * End-To-End encryption via Olm ([ticket](https://github.com/matrix-org/purple-matrix/issues/18))
   * [Decyption is supported but not encryption](https://github.com/matrix-org/purple-matrix/issues/18#issuecomment-410336278)

The plugin requires a matrix homeserver supporting client-server API r0.0.0 Synapse
v0.12.0-rc1 or later is sufficient.

# Installation

Pre-built binaries are available for Ubuntu since version 17.04 (Zesty Zapus).
You should be able to install them giving the following commands in a terminal
window:

```
sudo apt update
sudo apt install purple-matrix
```

For other GNU/Linux systems the plugin needs to be built
from source.

You will need development headers/libraries for the following:
* libpurple 2.x [libpurple-dev]
* libjson-glib  [libjson-glib-dev]
* libglib [libglib-dev (or libglib2.0-dev on Ubuntu 16.04 xenial)]
* libhttp_parser [libhttp-parser-dev].
* sqlite3 [libsqlite3-dev]
* libolm [libolm-dev] (if not available, compile with `make MATRIX_NO_E2E=1`)
* libgcrypt [libgcrypt20-dev] (if not available, compile with `make MATRIX_NO_E2E=1`)

You should then be able to:

```
make
sudo make install
```

If you do not have root access, you can simply copy `libmatrix.so` into
`~/.purple/plugins`.

You will then need to restart Pidgin, after which you should be able to add a
'Matrix' account.

## Building on Windows

Set up a build environment using
[the Pidgin BuildingWinPidgin docs](https://developer.pidgin.im/wiki/BuildingWinPidgin)

You should then be able to:
```
make -f Makefile.mingw
make -f Makefile.mingw install
```

You will then need to restart Pidgin, after which you should be able to add a
'Matrix' account.


# Usage

* Open the 'Manage accounts' dialog (under the 'Accounts' menu) and click
  'Add'.
* If the plugin was loaded successfully, you will be able to select 'Matrix'
  from the 'Protocol' dropdown.
* Enter your matrix ID on the homeserver (e.g. '@bob:matrix.org' or 'bob') as
  the 'username', and the password in the 'password' field.
  * If you don't enter your password, you'll be prompted for it when you try
    to connect;  this won't get saved unless you click 'save password' but an
    access token is stored instead.
* On the 'Advanced' tab, enter the URL of your homeserver.


The Advanced account option 'On reconnect, skip messages which were received in
a previous session' is disabled by default. This means that pidgin will show
the last few messages for each room each time it starts.  If this option is
enabled, only new messages will be shown.
