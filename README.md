# purple-matrix

This project is a plugin for
[libpurple](https://developer.pidgin.im/wiki/WhatIsLibpurple) which adds the
ability to communicate with [matrix.org](http://matrix.org) homeservers to any
libpurple-based clients (such as [Pidgin](http://www.pidgin.im)).

# Status

This project is somewhat alpha, and only basic functionality has been
implemented. In particular, sending and receiving messages is supported, but
only for rooms you have already joined (using a different client).

The following are not yet supported (or correctly implemented):
 * Joining and leaving rooms
 * Room invites
 * Presence indication
 * Typing indication
 * Videos/images in messages
 * File uploads
 * Account registration
 * Room topics
 * Voice/video calling

The plugin requires a homeserver running the v2_alpha API, which is not
(currently) the case for the matrix.org homeserver. You will therefore need to
run your own homeserver from the 'develop' branch of synapse.

# Installation

Currently there are no pre-built binaries, so the plugin needs to be built
from source.

You will need development headers/libraries for the following:
* libpurple 2.x [libpurple-dev]
* libjson-glib  [libjson-glib-dev]
* libglib [libglib-dev]
* libhttp_parser [libhttp-parser-dev].

You should then be able to:

```
make
sudo make install
```

You will then need to restart Pidgin, after which you should be able to add a
'Matrix' account.

# Usage

* Open the 'Manage accounts' dialog (under the 'Accounts' menu) and click 'Add'.
* If the plugin was loaded successfully, you will be able to select 'Matrix'
  from the 'Protocol' dropdown.
* Enter your matrix ID on the homeserver (e.g. '@bob:matrix.org' or 'bob') as
  the 'username', and the password in the 'password' field.
* On the 'Advanced' tab, enter the URL of your homeserver.
