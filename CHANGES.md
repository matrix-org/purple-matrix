Changes in purple-matrix 0.1.0 (2021-10-26)
===========================================

Features
--------
 * Allow user to set their online status. ([#5](https://github.com/matrix-org/purple-matrix/pull/5))
 * Add Emote support ([#12](https://github.com/matrix-org/purple-matrix/pull/12))
 * Send images ([#14](https://github.com/matrix-org/purple-matrix/pull/14), ([#46](https://github.com/matrix-org/purple-matrix/pull/46))
 * Allow building on libpurple 2.10.x ([#17](https://github.com/matrix-org/purple-matrix/pull/17))
 * Receive images ([#21](https://github.com/matrix-org/purple-matrix/pull/21), [#44](https://github.com/matrix-org/purple-matrix/pull/44))
 * Use thumbnails for big images ([#23](https://github.com/matrix-org/purple-matrix/pull/23))
 * Enable room invites when matrix handle is known ([#24](https://github.com/matrix-org/purple-matrix/pull/24))
 * Join by alias or id ([#29](https://github.com/matrix-org/purple-matrix/pull/29))
 * Typing notifications ([#38](https://github.com/matrix-org/purple-matrix/pull/38))
 * Support display of room topics ([#39](https://github.com/matrix-org/purple-matrix/pull/39))
 * Support HTML formatting on sending/receiving messages ([#47](https://github.com/matrix-org/purple-matrix/pull/47))
 * Handle media messages (m.video, m.audio, m.file) ([#62](https://github.com/matrix-org/purple-matrix/pull/62))
 * Support decryption of encrypted events. ([#70](https://github.com/matrix-org/purple-matrix/pull/70), [#84](https://github.com/matrix-org/purple-matrix/pull/84), [#87](https://github.com/matrix-org/purple-matrix/pull/87), [#90](https://github.com/matrix-org/purple-matrix/pull/90), [#97](https://github.com/matrix-org/purple-matrix/pull/97))
 * api: Handle chunked messages ([#73](https://github.com/matrix-org/purple-matrix/pull/73))
 * Add an option to get markdown, not HTML ([#117](https://github.com/matrix-org/purple-matrix/pull/117))

Bugfixes
--------

 * Fix a number of incompatibilities with Cygwin builds. ([#2](https://github.com/matrix-org/purple-matrix/pull/2), [#3](https://github.com/matrix-org/purple-matrix/pull/3))
 * Ignore empty m.room.name ([#11](https://github.com/matrix-org/purple-matrix/pull/11))
 * Unescape the HTML-escaped HTML. ([#19](https://github.com/matrix-org/purple-matrix/pull/19))
 * Escape incoming message bodies ([#25](https://github.com/matrix-org/purple-matrix/pull/25))
 * Re-use the device-id provided by the server ([#40](https://github.com/matrix-org/purple-matrix/pull/40))
 * bump up max reply size ([#56](https://github.com/matrix-org/purple-matrix/pull/56))
 * typing: Fix crash on typing notification to someone not present ([#69](https://github.com/matrix-org/purple-matrix/pull/69))
 * turn off deprecation warnings to workaround GParameter ([#103](https://github.com/matrix-org/purple-matrix/pull/103))
 * Login updates: Avoid the outdated login API and store access tokens ([#104](https://github.com/matrix-org/purple-matrix/pull/104))
 * matrix-connection: Only change state and progress if we're not already connected ([#111](https://github.com/matrix-org/purple-matrix/pull/111))
 * matrix-api: use a non-fatal error code on HTTP 429 and >500 ([#113](https://github.com/matrix-org/purple-matrix/pull/113))
 * Build failure workaround: `_on_typing` ([#118](https://github.com/matrix-org/purple-matrix/pull/118))

Documentation
------------
 * mention OLM in readme ([#53](https://github.com/matrix-org/purple-matrix/pull/53))
 * [README] Add xenial-specific libglib package ([#60](https://github.com/matrix-org/purple-matrix/pull/60))
 * [README] Add Ubuntu pre-built binaries. Fixes ([#58](https://github.com/matrix-org/purple-matrix/pull/58)) ([#66](https://github.com/matrix-org/purple-matrix/pull/66))
 * README: Remove unimplemented item from now implemented feature ([#79](https://github.com/matrix-org/purple-matrix/pull/79))
 * Add badge to README with link to room ([#91](https://github.com/matrix-org/purple-matrix/pull/91))

Internal changes
----------------
 * [Makefile] Change fixed pkg-config into variable PKG_CONFIG ([#68](https://github.com/matrix-org/purple-matrix/pull/68))
 * Small improvements to make installation easier ([#94](https://github.com/matrix-org/purple-matrix/pull/94))

Changes in purple-matrix 0.0.0 (2016-01-02)
===========================================

 * Initial release; supports sending and receiving plain-text messages.
