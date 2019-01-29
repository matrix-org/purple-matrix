# purple-matrix

This project is a plugin for
[libpurple](https://developer.pidgin.im/wiki/WhatIsLibpurple) which adds the
ability to communicate with [matrix.org](http://matrix.org) homeservers to any
libpurple-based clients (such as [Pidgin](http://www.pidgin.im)).

# Status

This project is somewhat alpha, and only basic functionality has been
implemented. Sending and receiving simple text messages is supported, as is 
joining rooms you are invited to by other users.

The following are not yet supported:
 * Creating new rooms (and one-to-one chats)
 * Joining existing rooms by alias instead of room_id
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

You should then be able to:

```
make
sudo make install
```

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
* On the 'Advanced' tab, enter the URL of your homeserver.


The Advanced account option 'On reconnect, skip messages which were received in
a previous session' is disabled by default. This means that pidgin will show
the last few messages for each room each time it starts.  If this option is
enabled, only new messages will be shown.
