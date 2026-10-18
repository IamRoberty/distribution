# SPDX-License-Identifier: GPL-2.0-or-later
# SimpletonOS package: the on-device UI. LVGL on DRM/KMS, no compositor.
#
# 0.2 (26 Sep 2026): folder browser driven by MPD's lsinfo, joypad input via
# evdev mapped to abstract actions, transport keys (play/pause, next/prev).
# 0.3 (27 Sep 2026): now-playing screen with full-bleed album art, decoded on
# a worker thread by libjpeg-turbo / libpng; format line reads the DAC's real
# state from /proc/asound.
# 0.4 (28 Sep 2026): designed placeholder art for tracks without a cover -
# themed cassette tinted per colourway, tags written on the label. Assets
# (layers, fonts, themes) ship from sources/share to /usr/share/simpleton.
# 0.5 (28 Sep 2026): art paging - with the now-playing controls hidden,
# D-pad left/right pages through every image in the album folder and its
# Artwork/Scans subfolders; "2 / 11" badge top-right.
# 0.6 (28 Sep 2026): wide pages (gatefolds, booklet spreads) fill the screen
# height and left/right pans across them a quarter screen per press before
# turning the page.
# 0.7 (29 Sep 2026): HDMI. Own DRM backend (display.c) instead of LVGL's
# driver: HDMI when connected else the panel, max 1080p on a TV, the unused
# screen switched off; hotplug restarts the UI on the new output keeping
# the screen and folder. Layouts sit on a square stage sized to the screen's
# short edge (PX()/ui_font() in theme.h); on a TV the now-playing sides take
# a muted colour from the art. Needs LV_FONT_MONTSERRAT_30/42/48 in lv_conf.h.
# 0.8 (30 Sep 2026): HDMI-CEC. The TV remote drives the UI through /dev/cec0
# (cec.c, kernel CEC API, no libcec). Appears as Playback device "Simpleton";
# never wakes or sleeps the TV; TV standby never stops playback.
# 0.9 (30 Sep 2026): text in every script. FreeType fonts at the exact scaled
# size with a fallback chain (theme typeface -> Noto Sans -> Noto CJK -> Thai,
# Arabic, Hebrew, Devanagari, Tamil, Bengali -> Montserrat for LV_SYMBOL_*),
# BIDI + Arabic shaping on in lv_conf.h. Noto files ship in share/fonts (OFL);
# CJK comes from ROCKNIX's noto-sans-cjk package. The cassette label falls
# back to Noto Sans too. Indic scripts render unshaped until a shaping step.
# 0.10 (1 Oct 2026): living-room pass.
#   - Transparent containers: a SACD ISO, a multi-track DFF (the SACD patch
#     makes every .dff a container) or a CUE album lists as its tracks in
#     place, like any album; two discs in one folder simply run on.
#     Done in mpdc.c (mpd_lsinfo_expanded) so every client sees the same.
#   - Play through folders (playthrough.c): HiBy's walk, on by default,
#     /storage/.config/simpleton/play_through = 0 turns it off. The next
#     folder is queued as the last track starts, so it stays gapless.
#   - Lists: left/right page a screenful; one accelerating auto-repeat for
#     the D-pad and the TV remote (CEC only says held/released now), with
#     a safety stop when a TV never sends "released".
# 0.11 (4 Oct 2026): groundwork for the expanded UI (Engineering Note 05,
# build steps 1 and 2). Nothing looks different on screen.
#   - String table (strings.c, strings.def): every word the UI shows of its
#     own is a short key; each language is one plain text file in
#     share/lang (en.txt to start), chosen by
#     /storage/.config/simpleton/language. English is also built in, so a
#     missing file never blanks the screen. No hard-coded text after this.
#   - Screen-size-aware layout (layout.c): the real screen, handheld or TV,
#     and the sizes XXL..S as row counts; grid and list measurements are
#     worked out from those (handheld 1x1..4x4, TV 1x2..5x10). The browser
#     takes its measurements from it; nothing is tied to 720 px.
# 0.12 (4 Oct 2026): the library layer (Note 05 steps 3b and 3c). Nothing
# on screen changes yet; the screens that use it come next.
#   - library.c: the tag layer. One listallinfo per card, grouped into
#     albums (album artist / artist + title; a folder is an album when
#     there is no album tag; compilations marked), artists, the picker's
#     sorts with untagged items last, and folder types (album, collection,
#     artist, library, plain). Saved as library.idx.
#   - cache.c + simpleton-cache (a second binary from the same sources, no
#     LVGL): cover thumbnails at 192/256/384/512 px as JPEG, a colour per
#     album, kept under /storage/.simpleton/cache/<card>/ and mirrored to
#     the card's .simpleton/cache so it travels with the card. Only albums
#     whose tracks changed are redone. Runs at idle priority from
#     simpleton-cache@<card>.service, started by simpleton-card after a scan.
#   - art.c: MUSIC_DIR can be overridden for tests; synchronous cover fetch
#     and decode for the cache. mpdc.c: streaming listallinfo.
# 0.16f (9 Oct 2026): more colour in the grid's no-art tiles. Ian's twelve
# colour sets go in the theme as tilepalette lines and a tilecolours line
# picks how they're used (theme / palettes / mix / generate; tilestyle.c),
# dealt per visit so a screenful doesn't repeat and neighbours differ.
# Fonts unchanged pending a design talk.
# 0.16g (9 Oct 2026): inks after Ian's first look - mostly palette colours,
# a cream (FAEFD0) common but not dominant, some off-black; the contrast
# floor is a theme setting (Archer: 2.0, vibe over contrast). tilecontrast,
# tilecream and tileinks lines in the theme.
# 0.16h (9 Oct 2026): the album grid fills in by itself when a card's index
# appears after it was opened (after a boot it said "No albums yet" until
# the UI restarted).
# 0.16i (10 Oct 2026): cover fonts for the no-art tiles, from Ian's type
# notes: seventeen faces listed in the theme (coverfont lines) with case,
# leading, share and rules (short titles only, must be large, needs
# contrast, first for long titles), dealt per visit like the colours; type
# sizes on one fixed scale. New OFL faces in share/fonts (Jost Light /
# SemiBold / Black cut from the variable Jost; Monoton, Oi, Anton SC,
# Creepster, Wendy One, Gravitas One; Arvo Regular, Dekko, Updock for the
# album page's track lists later).
# 0.16j (10 Oct 2026): a floor on the title size (tiletextmin, a fraction of
# the tile): below it the title is trimmed with dots, and a bracketed tail
# is trimmed or dropped before the title shrinks. Leading tuned (Anton SC
# 65, BBH Hegarty 70, Bevan 70, Arvo 90); Oi, Wendy One and Sigmar One out
# of the cover fonts (kept for the liner pages).
# 0.16k (10 Oct 2026): the size floor is for the title only; a bracketed
# tail may go as small as the ladder allows before it is trimmed.
# 0.16l (10 Oct 2026): a word is never broken in two: a title whose one wide
# word can't sit on a line at the floor goes below the floor just as far as
# it needs (never under 16 px), and a face that can't take a title whole
# passes it to the next.
# 0.17 (10 Oct 2026): the album page (Note 05 section 5 as amended 7 and
# 10 Oct, step 6b; album-page-6b.md). Select on the grid opens it: on a TV
# the cover fills a stage-sized square left, right or centre with the track
# list beside it or split across both sides like a record; on the handheld
# the names in cover fonts above the list, in two states (cover: big names,
# Select plays the album; the D-pad: small names, the list up and larger).
# Colours come from the cover's own palette, which simpleton-cache now
# stores per album in library.idx (palette.c; an older index gets them on
# the next cache run from the thumbnails on disk). Layouts and liner fonts
# are theme lines (pagelayout, linerfont; tilestyle.h). Playing and
# selected tracks in two colours, a triangle before the playing one, a red
# heart after a favourite (dormant until step 9). --album <folder> keeps
# the page across the HDMI restart. The grid's text fitting moved into
# textfit.c and the no-art tile's text into tiletext.c (pixel-identical)
# so the page could share them.
# 0.17a (10 Oct 2026): after Ian's first look at the page on the unit.
# Page colours carry the cover's dominant colour over (every dominant
# scored by its share of the cover, its colourfulness and whether the
# cover's own extremes read on it: a dark or grey cover gets a dark or grey
# page, a big block of bright colour becomes the page); same-colour boxes
# of the median cut are merged so a block's share is real, and the shares
# are stored (library.idx, 21 palette fields; a 13-field 0.17 line or a
# line with none is re-extracted from the 192 px thumbnail, by the page at
# open and by the cache tool). A no-art album's page carries its grid tile
# over: the tile itself as the handheld page and the TV square, a companion
# colourway from the same set beside it, the tile's face for the title. The
# no-art square's artist line grows with the square past 476 px.
# 0.17b (10 Oct 2026, evening): after Ian's second look. Text colours
# turned round (the unselected tracks in one of the cover's colourful
# colours, the selected one in the colour nearest black or white, the
# playing one in another colourful colour); the background goes colourful
# whenever colour has a real presence on the cover (a colourful dominant of
# 12 % or more, or 25 % together), tuned on the 238 palettes of Ian's card.
# TV: now-playing is hosted inside the page's cover square (nowplaying.c
# embed API) - a track starting pops its controls up over the cover and
# they fade; Left/Right towards the cover select it (a frame in the
# selected colour), Select enters now-playing there, Back comes out. The
# handheld goes to now-playing when a track starts, as the browser does;
# its names <-> list change cross-fades, Up from the first track returns to
# the names, Back leaves. A folder that holds one album opens as the album
# page from the browser too (Note 05 section 7, first half).
# 0.17c (10 Oct 2026, night): the handheld page is one page - the names as
# a heading above the list (one big, the other small, as the cover state
# had them), the list at full size under it, the first track selected;
# the separate names state and its cross-fade are gone (Ian: it didn't
# look natural).
# 0.17d (10 Oct 2026, late): the TV page's cover, as Ian found it on the
# unit. The frame is the playing colour; on the cover no track is
# highlighted; Up/Down bring now-playing's controls up (they then have the
# D-pad), Select turns the cover's pages (the frame goes, "n / m" flashes
# even for one image), Left/Right go straight back to the list, and Back
# always takes the controls down first. Now-playing's own cover replaces
# the page's only once it is decoded, and paints the page's colour behind
# a cover that isn't square (the page's picture no longer shows above and
# below it). Grid: no cover comes forward while the alphabet pill is up.
# 0.17e (10 Oct 2026): no flash of the previous track's cover when a new
# track starts on the TV page - hosted now-playing keeps art that isn't the
# expected track's hidden (the page's own picture stays) unless it is an
# earlier track of the same album; the page tells now-playing the new file
# the moment it starts it.
# 0.17f (10 Oct 2026): the playing mark on the page stopped following MPD
# after a few quiet minutes - MPD closes a command connection quiet for
# longer than its connection_timeout (60 s), and the next status request
# read EOF; mpdc.c now notices a closed-by-peer socket (a free poll before
# each command) and reconnects, for every command of the UI and the art
# worker. A second copy of a track (the album ripped twice into two folders
# joins one album) is folded: one of each track shows and plays.
# 0.17g (10 Oct 2026): of two copies of a track the better one plays - the
# index now carries MPD's audio format per track ("96000:24:2", "dsd64:2";
# the T line's 11th field, filled on the next cache run) and the page keeps
# the copy with the higher rank: DSD over PCM, then bits, then rate; equal
# or unknown, the first in library order.
# 0.17h (10 Oct 2026): the cassette comes into the page (Ian, 20:49). A
# no-art album's TV square shows one of three each visit - the no-art tile
# as the grid showed it, the cassette, or a record (when a "record" design
# is under share/placeholder) - in the tile's colours: the background the
# tile's, the label a colour of its set the tile's ink reads on, the shell
# another, a record's disc the set's darkest; the album line in the tile's
# face. The playing track's title is written on the label's lower line
# while one of the album plays; now-playing no longer puts its own tape
# over the square (an album without art keeps its picture; the controls
# still come over it). Now-playing's cassette for a track of the album -
# handheld, or the TV's own now-playing screen - is in the same colours
# (placeholder.c: designs by name, explicit tints, a per-album preference
# keyed by the tracks' paths or their tags). The picture is drawn off the
# UI thread.
# 0.17i (11 Oct 2026): Ian's record (share/placeholder/record) joins the
# page's square: coloured vinyl in the shell colour most visits, black one
# in five; an optional light.png per design adds light on top of the
# colours, so the record's sheen shows on a dark disc. layout.txt can give
# text areas - rectangles that together make one shape - and the name is
# fitted into the shape at the largest size that fits (a short word big, a
# longer one over two or three lines, or one line across the wide part).
# Caps faces upper-case accented Latin, Greek and Cyrillic letters too
# ("BJORK" with its umlaut, a Russian title in capitals), on the tiles,
# the page and the labels (str_upper_utf8 in strings.c).
# 0.17j (11 Oct 2026): digital silence on the HDMI audio link while the UI
# is on an HDMI screen (hdmisilence.c): some TVs hum or buzz when a source
# sends no audio at all; stock ROCKNIX kept the HDMI sound card busy, we
# never opened it. A low-priority thread keeps the HDMI card (found by name
# in /proc/asound/cards) open with zeros, 48 kHz 16-bit stereo. Music still
# goes only to the USB DAC. /storage/.config/simpleton/hdmi_silence = off
# turns it off. Links libasound (alsa-lib, already in the image for MPD).
# 0.17k (11 Oct 2026): after Ian's day with 0.17j.
#   - Releases: the same album in two folders (a remaster, a DSD edition)
#     is two albums (library.c: a folder is a release when it and the
#     album's biggest folder hold most of each other's titles), labelled
#     by what the folder name adds to the title, else the year, else the
#     format ("2019 Remaster", "SACD"; in the grid's name tail and on the
#     page); a few tracks of an album in another folder still join it and
#     fold on the page. library.idx gains the label (last A field).
#   - Folder mode: a folder opened from the browser gets the page's look
#     but lists every track in the folder, in file order, nothing sorted by
#     tag, nothing folded (albumpage_open_folder; --folderpage on the
#     restart line); CD1/CD2 subfolders count as discs when the tracks
#     carry none. An album folder is one whose tracks are mostly (2/3) of
#     one album, so a bonus track tagged otherwise no longer unmakes it.
#   - The page's art: the album's own cover from the start (now-playing
#     decodes the first track's art as a preview, pages and all, so Select
#     turns them before anything plays) and the page's thumbnail placed by
#     now-playing's rules (fitted and letterboxed, or full height from the
#     left edge when wide), so nothing jumps when a track starts. The
#     themed placeholder never comes over the page's own picture.
#   - Handheld: Left/Right turn the album over - the art (badge, Select
#     for the next picture, Up/Down the controls) or the tile, cassette or
#     record - Left/Right/Back turn it back; the list wraps round the ends.
#   - Dekko and Updock are out of the liner faces (Ian).
# 0.17l (11 Oct 2026): a release is a container when the track lies in one
# - two cue (or ISO / DFF) rips side by side in one folder are two albums
# too (Ian's case), labelled from the cue's name; a title's trailing
# punctuation doesn't stop its folder name matching.
# 0.17m (11 Oct 2026): now-playing's format line shows MPD's own error
# when it can't open its output ("Can't open the DAC: hw:CARD=AUDIO,DEV=0")
# instead of "DAC idle" (mpdc.c reads status's error line). Found with
# Ian's survey: MPD had been left on a DAC swapped out hours before. The
# fix proper is in simpleton-mpd-config (the hotplug script waits for the
# card, follows the card that appeared, checks that MPD restarted, says
# what it did in the journal) and needs a full image build.
# 0.17n (11 Oct 2026): no UI change - the version marks the round that found
# the reboots: a DST-compressed SACD track resets the RGB20SX (twice on a
# stereo track, once with the DAC off; memory fine, no kernel line). The
# SACD plugin's DST decoder runs one thread per core; four A55s at 1.8 GHz
# take the SoC past its 95 C hardware thermal shutdown (rockchip,hw-tshut-
# temp) in seconds. Proven by capping the clock: 1104 MHz played the track
# for minutes peaking at 78 C, 1416 MHz still reset the unit. The fix lives
# elsewhere in the tree: projects/ROCKNIX/patches/mpd/0005 (a dstdec_threads
# option, set to 2 in mpd.conf), simpleton-mpd-config's simpleton-thermal
# boot unit (1104 MHz ceiling, passive trips 75/82 C) and restore_paused.
# 0.17o (11 Oct 2026): no UI change either - the round that locks the fix
# in: MPD patch 0006 (MPD aborted at every stop - the SACD/DSDIFF plugins'
# finish() built an AllocatedPath from a null path inside a noexcept
# function; root of the "status=6/ABRT" bug and of the 27 Sep "hot-swap
# crash"), simpleton-mpd-config's package.mk installs simpleton-thermal and
# enables its service, and simpleton-mpd-conf-migrate restores the decoder
# blocks and restore_paused in a unit's existing mpd.conf. Measured: with
# the ceiling and two DST threads the SoC peaks at 73 C on the track that
# reset it at 90+ (threads2.txt).
# 0.18 (12 Oct 2026): power management (power.c/h, power-management-
# proposal.md). The battery indicator (icon + %, smoothed, red under 15 %,
# notices at 10/5 %, clean shutdown at 3 %) on every screen; the power key
# grabbed from the kernel: one press = "Sleeping..." then deep suspend
# written to /sys/power/state by us (ROCKNIX's sleep hook took 13-15 s),
# held 2 s = Sleep / Power off / Cancel with a countdown that powers off
# cleanly while the key stays held; the screen dims and goes off after a
# while (X and Y keep working in the dark, any other button only lights
# the screen), idle sleep, a one-shot sleep timer that finishes the track;
# long tracks (over 20 min, cue/ISO tracks by their own length): X seeks
# 30 s instead of skipping; the lights red charging / green full / off; a
# Settings -> Power page for all of it; "Switching output..." while MPD
# restarts on a DAC swap, with the pressed Play replayed when it is back.
# 0.18a (12 Oct 2026, 21:30): after Ian's first look. The screen fades to
# black in the second before the backlight goes (the panel showed garbage
# as it went) and comes back on a black frame; the battery indicator is
# Ian's design for Archer Ave. - a 50 px disc top right with the number in
# Jost Bold, blue / yellow / red by charge, green on the cable, the ink
# that reads better - as one theme line ("battery ..." in archer.theme,
# tilestyle.c), so another theme can draw its own.
# 0.18b (12 Oct 2026, 22:10): the disc moves to the top left, steps aside
# while the grid's name bar is up, and by default shows only for a few
# seconds after a button (Settings -> Power -> Battery indicator: Never /
# After a button / Always; under 15 % on battery it stays) - Ian: it was
# "screwing up the purity of the album view".
# 0.18c (12 Oct 2026, 22:40): the disc comes up on the first button after a
# quiet minute, on the press that lights the screen and on waking from
# sleep, then fades - not on every press while browsing (Ian).
# 0.19 (13 Oct 2026): Note 05 step 7 and the Tracks / Recent views. A list
# page (listpage.c) for long lists - every track, the newest, a collection
# folder as a mixtape - beside the TV art square, windowed rows, a
# colourway per sort section, the "<< A >>" jump row, Left/Right paging;
# the grid shows albums, tracks or a folder's children (artist pictures,
# name tiles with a keyline); folder types open from the browser as their
# views; the cards' indexes shared in libs.c; play-through's list mode.
# 0.19a (13 Oct 2026, late): Ian's first pass on the unit. The sort key
# (library_sort_key: leading punctuation and track numbers dropped, "#"
# after Z) used by every name sort and jump letter; stops under a
# screenful join their neighbour; one colourway per letter; no jump row
# on the handheld, the TV's pinned above the list; the playing mark
# leaves the track played before; a no-art track's companion in the TV
# square is its album's tile, cassette or record; the Tracks grid is two
# columns of text cells; a folder with few albums fills the screen (1,
# 2x2, 3x3); the grid's name line is a switch (off for folders); a
# folder's picker sorts by name or newest and has a List face (the list
# page's LIST_ALBUMS); an album of many artists opens as the mixtape-style
# list; Recent is what was played last (history.c); a folder view handed
# over from the picker's View row reopens the picker on that row.
# 0.19b (13 Oct 2026, 23:45): the TV square keeps an album's cover up across
# its tracks and across to the next cover (it had shown the tape between);
# the Genre tag in the index (a cache run fills it) - a mixtape's label's
# third line names the folder's commonest genres, an album's cassette its
# own.
# 0.19c (14 Oct 2026, 00:15): the want-ad cells after Ian's look. The type
# size follows the title's length (short big, long smaller - it had been
# the other way round), a bracketed tail smaller on its own line, bigger
# type and a bigger artist line, cell heights in whole units so the two
# columns' corners meet, the selected cell with its colours turned round,
# Left/Right to the cell next door at the same height, and Tracks comes
# back where it was left.
# 0.20 (14 Oct 2026): Artists (Note 05 step 8; Ian's direction of 13-14 Oct
# in browse-views-ian-2026-10-13.md). artists.c lists every album artist of
# every card, merged by name, their albums in year order, Various Artists
# as a row of its own last, and leaves out artists with no album of their
# own and only a handful of tracks. Three faces of one setting
# (style-artists-<kind>, handed over through the picker's View row):
#   - Rows (rows.c): one row an artist, their name tile (or the artist
#     folder's picture) pinned at the left, their albums running right.
#     Right walks to the row's centre slot, then the row slides under the
#     cursor until its last album shows, then the cursor walks on; Left
#     mirrors it. Right on the last album pages down a screenful of
#     artists, Left on the artist tile pages up. Up/Down keep the column,
#     past the centre they land on the centre slot of a row that slides.
#     A row keeps its scroll while on screen; off screen it resets.
#   - Grid (grid.c scope ARTISTS): an artist tile each, picture or name;
#     the band says "n albums". One artist's albums (scope ARTIST) fill
#     the screen as a small folder does (1x1 .. 3x3), sorted by year, title
#     or newest.
#   - List (listpage.c LIST_ARTISTS): artist rows with the album count; on
#     a TV the square is a mosaic of the artist's albums (1, 2x2 .. 5x5;
#     covers, or small name tiles for albums without art). Various Artists
#     is the "VA" stop at the end.
# The picker's Sort row reads "Opens as" on all three: Select on an artist
# opens their albums (the grid), their tracks by title, or their tracks
# album by album (LIST_ARTIST_TRACKS, whose Sort row has both and Newest;
# the jump stops are the albums' years). Back from an artist's albums or
# tracks returns to the artists view with the cursor on the artist; an
# album opened from a row returns to the row. Restart lines --artists,
# --artistsgrid, --artistslist, --artist <name>, --artisttracks <name>.
# Settings -> Artists: view, size, "An artist opens as", names, the
# alphabet picker. Also: a list or grid come back from a deeper view in the
# colours they had (the deal is kept per kind / scope), and the folder
# grid's Newest sort was read back as the wrong sort since 0.19a - fixed.
# 0.20a (14 Oct 2026, evening): Ian on 0.20. The rows no longer slide: an
# artist takes as many lines as their albums need, the artist's tile
# spanning them, and the D-pad moves cell by cell (Right on an artist's last
# album, Left on the tile: a page). Up on the TV jump row goes on up the
# list (it had wrapped to the bottom) and, in one run of Ups, the jump row
# isn't stopped at again. An artist's tracks in album order show a break
# with the album's title and year before each album, and the sort reads
# "Album order". A small folder's or artist's grid fills its spare cells
# with flat squares in dealt colours, as the mosaic does.
# 0.20b (14 Oct 2026, late): a selected cover in the rows was cut at its
# row's edge (the art seemed to outgrow its box) - rows draw overflow now; a
# tall artist tile grows when selected, and selecting it brings all its
# lines on screen at once. An artist's tracks show the album under each
# title instead of the artist. In a list with album breaks, Down on the last
# track goes round to the first (it had landed on the jump row), and paging
# or jumping onto a break lands on the album's first track.
# 0.20c (14 Oct 2026, night): under an artist the count follows "Opens as"
# (tracks when an artist opens as tracks, albums otherwise) in all three
# Artists faces; an artist with one album opens straight on its page when
# artists open as albums, and Back returns to the artist; a long press of A
# on a Home tape (the remote's menu key) opens that view with its picker
# panel up on the View row.
# 0.20d (14 Oct 2026, night): the rows' grown covers were still cut at
# their row's edge (LVGL clips a child to its parent plus the parent's extra
# draw size, even with overflow visible): each row now claims room for a
# grown cover and its shadow. An artist's albums share their lines evenly
# (7 in rows of 5 as 4 + 3, 6 as 3 + 3).
# 0.21 (15 Oct 2026): favourites, mixtapes and collections, kept on the
# music card (.simpleton/curation/: favorites.m3u8, mixtapes/*.m3u8,
# collections/*.txt - plain playlists any player reads; safe writes, a copy
# on the unit that comes back when a card is wiped). A held (the remote's
# Options key, or OK held on a TV that reports the release) opens the
# object menu on a track or an album: Favorite, Add to mixtape..., Add to
# collection...; a new one is named "Mixtape 1" at once. Now-playing's
# Favorite button works; the album page's hearts show. Favorites, Mixtapes
# and Collections open from Home: lists; a mixtape is the mixtape-style
# list, a collection a grid of its albums (a mosaic on the TV list). Recent
# stays on the unit (Ian). CEC: a key trace setting (cec-trace) and the
# TV's vendor id in the log.
# Source layout (all in sources/, copied into ${PKG_BUILD} by scripts/unpack):
#   main.c        - theme/indev setup, screen switch, action dispatch
#   power.c/h     - battery indicator, screen off, sleep, power key, lights
#                   0.22: every sleep sets the RTC alarm (sleep_off, default 1 h); woken by it -> a clean, dark power-off
#   display.c/h   - DRM/KMS output choice, modeset, page flips, hotplug
#   theme.c       - the square stage behind theme.h
#   layout.c/h    - real screen, sizes XXL..S, grid and list measurements
#   strings.c/h   - string table; strings.def lists every key with its English
#   library.c/h   - the library index: albums, artists, sorts, folder types
#   cache.c/h     - thumbnail + index cache for a card, mirrored to the card
#   simpleton-cache.c - the cache tool's main (build / status / albums / folders)
#   fonts.c/h     - FreeType fonts with a Noto fallback chain per size
#   input.c/h     - evdev joypad + volume rocker -> ui_action_t queue, auto-repeat
#   cec.c/h       - HDMI-CEC: TV remote keys -> same ui_action_t queue
#   curation.c/h  - favourites, mixtapes, collections on the music card (0.21)
#   objmenu.c/h   - the object menu (A held) and the toast (0.21)
#   keyboard.c/h  - the small on-unit keyboard for naming (0.21a)
#   npicons.c/h   - the now-playing icons, drawn at any size and colour (0.23)
#   dacpower.c/h  - wake the USB DAC on play, let it sleep when idle (0.21c)
#                   0.21d: the link held down 0.75 s before the resume (an instant one did not wake the DX5 II)
#   mpdc.c/h      - minimal MPD text-protocol client (no libmpdclient)
#   playthrough.c/h - play through folders, driven by MPD notices
#   browser.c/h   - folder browser screen
#   nowplaying.c/h- now-playing screen
#   art.c/h       - album art lookup + decode, worker thread
#   placeholder.c/h - themed placeholder art (tint + label text, stb_truetype)
#   grid.c/h      - the album grid (0.16); thumbs.c/h its thumbnail decoder
#   tilestyle.c/h - the theme's tile colour sets, cover fonts, page layouts
#   textfit.c/h   - fitting text into a box down a size ladder (0.17)
#   tiletext.c/h  - the no-art tile's text (grid tiles, the page's cover square)
#   palette.c/h   - a cover's colours (cache tool: extraction; UI: page roles)
#   albumpage.c/h - the album page (0.17)
#   hdmisilence.c/h - zeros on the HDMI audio link while on HDMI (0.17j)
#   libs.c/h      - the mounted cards' library indexes, shared by the views (0.19)
#   listpage.c/h  - the list page: Tracks (its Grid is text cells since 0.19a),
#                   Recent (last played), mixtape-style folders and albums, a
#                   folder's albums as rows (0.19 / 0.19a); grid.c shows
#                   albums or a folder's children since 0.19
#   history.c/h   - what was played, newest first, for Recent (0.19a)
#   artists.c/h   - every album artist across the cards, Various Artists last (0.20)
#   rows.c/h      - the Artists rows view, an artist's albums running right (0.20)
#   stb_truetype.h- vendored, public domain (github.com/nothings/stb)
#   theme.h       - shared style tokens
#   share/        - placeholder layers, OFL fonts, theme files, lang/<code>.txt
#
# Structure follows kernel-drivers/device-tree-overlays: PKG_TOOLCHAIN="manual"
# with explicit make_target/makeinstall_target. No CMake for a handful of files.
#
# Linking notes:
#   - liblvgl.a is static, so libdrm is linked here (-llvgl before -ldrm).
#   - -I.../include/lvgl resolves "lvgl.h", the installed DRM driver header,
#     and LVGL's own __has_include of the installed lv_conf.h.
#   - LV_KCONFIG_IGNORE / LV_LVGL_H_INCLUDE_SIMPLE mirror the library build.
#     lv_conf settings determine struct layouts; a mismatch is an ABI bug,
#     not a compile error.
#   - libjpeg-turbo and libpng are ROCKNIX's own packages (shared libs, so
#     listing them in PKG_DEPENDS_TARGET is what puts the .so in the image).
#     libpng pulls zlib; -lz is linked explicitly since we link it by hand.
#   - alsa-lib likewise (libasound.so is in the image for MPD); only the UI
#     links it, for the HDMI silence thread.
#   - freetype likewise (shared, already in the image for other packages);
#     liblvgl.a's FreeType code resolves against it at this link, and lvgl.h
#     pulls ft2build.h, hence the freetype2 include path here as well.

PKG_NAME="simpleton-ui"
PKG_VERSION="0.29e"
PKG_LICENSE="GPL-2.0-or-later"
PKG_SITE="https://github.com/IamRoberty/distribution"
PKG_DEPENDS_TARGET="toolchain lvgl libdrm libjpeg-turbo libpng zlib freetype noto-sans-cjk alsa-lib"
PKG_SECTION="graphics"
PKG_SHORTDESC="SimpletonOS on-device UI"
PKG_LONGDESC="LVGL + DRM/KMS user interface for SimpletonOS: MPD-backed folder browser and now-playing screen with album art, joypad navigation and transport control."
PKG_TOOLCHAIN="manual"

# liblvgl.a is linked statically, so a change to lvgl (e.g. lv_conf.h) must
# rebuild this package too; without this line the old LVGL stays baked in.
PKG_NEED_UNPACK="$(get_pkg_directory lvgl)"

make_target() {
  # -g and -rdynamic: the crash handler in main.c prints a backtrace; the
  # image build strips the installed copy, the one under build/ keeps the
  # symbols for addr2line (album-grid-0.16.md, 0.16c).
  ${CC} ${TARGET_CFLAGS} -g -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -DLV_KCONFIG_IGNORE \
    -DLV_LVGL_H_INCLUDE_SIMPLE \
    -DSIMPLETON_VERSION=\"${PKG_VERSION}\" \
    -I$(get_install_dir lvgl)/usr/include/lvgl \
    -I$(get_install_dir libdrm)/usr/include/libdrm \
    -I$(get_install_dir libjpeg-turbo)/usr/include \
    -I$(get_install_dir libpng)/usr/include \
    -I$(get_install_dir freetype)/usr/include/freetype2 \
    -I$(get_install_dir alsa-lib)/usr/include \
    -o ${PKG_BUILD}/simpleton-ui \
    ${PKG_BUILD}/main.c ${PKG_BUILD}/input.c ${PKG_BUILD}/mpdc.c ${PKG_BUILD}/browser.c \
    ${PKG_BUILD}/nowplaying.c ${PKG_BUILD}/art.c ${PKG_BUILD}/placeholder.c \
    ${PKG_BUILD}/display.c ${PKG_BUILD}/theme.c ${PKG_BUILD}/cec.c ${PKG_BUILD}/fonts.c \
    ${PKG_BUILD}/playthrough.c ${PKG_BUILD}/strings.c ${PKG_BUILD}/layout.c \
    ${PKG_BUILD}/library.c ${PKG_BUILD}/config.c ${PKG_BUILD}/shelf.c ${PKG_BUILD}/home.c \
    ${PKG_BUILD}/settings.c ${PKG_BUILD}/notice.c ${PKG_BUILD}/picker.c \
    ${PKG_BUILD}/grid.c ${PKG_BUILD}/thumbs.c ${PKG_BUILD}/cache.c ${PKG_BUILD}/tilestyle.c ${PKG_BUILD}/palette.c \
    ${PKG_BUILD}/textfit.c ${PKG_BUILD}/tiletext.c ${PKG_BUILD}/albumpage.c ${PKG_BUILD}/hdmisilence.c ${PKG_BUILD}/power.c \
    ${PKG_BUILD}/libs.c ${PKG_BUILD}/listpage.c ${PKG_BUILD}/history.c \
    ${PKG_BUILD}/artists.c ${PKG_BUILD}/rows.c ${PKG_BUILD}/curation.c ${PKG_BUILD}/objmenu.c ${PKG_BUILD}/keyboard.c ${PKG_BUILD}/dacpower.c ${PKG_BUILD}/npicons.c ${PKG_BUILD}/btctl.c ${PKG_BUILD}/saver.c \
    ${TARGET_LDFLAGS} -rdynamic \
    -L$(get_install_dir lvgl)/usr/lib \
    -L$(get_install_dir libdrm)/usr/lib \
    -L$(get_install_dir libjpeg-turbo)/usr/lib \
    -L$(get_install_dir libpng)/usr/lib \
    -L$(get_install_dir zlib)/usr/lib \
    -L$(get_install_dir freetype)/usr/lib \
    -L$(get_install_dir alsa-lib)/usr/lib \
    -llvgl -lfreetype -ldrm -ljpeg -lpng16 -lz -lasound -lpthread -lm

  # The cache tool: the same sources without LVGL or the display.
  ${CC} ${TARGET_CFLAGS} -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
    -I$(get_install_dir libjpeg-turbo)/usr/include \
    -I$(get_install_dir libpng)/usr/include \
    -o ${PKG_BUILD}/simpleton-cache \
    ${PKG_BUILD}/simpleton-cache.c ${PKG_BUILD}/cache.c ${PKG_BUILD}/library.c ${PKG_BUILD}/palette.c \
    ${PKG_BUILD}/art.c ${PKG_BUILD}/placeholder.c ${PKG_BUILD}/mpdc.c ${PKG_BUILD}/strings.c \
    ${PKG_BUILD}/config.c \
    ${TARGET_LDFLAGS} \
    -L$(get_install_dir libjpeg-turbo)/usr/lib \
    -L$(get_install_dir libpng)/usr/lib \
    -L$(get_install_dir zlib)/usr/lib \
    -ljpeg -lpng16 -lz -lpthread -lm
}

makeinstall_target() {
  mkdir -p ${INSTALL}/usr/bin ${INSTALL}/usr/share/simpleton
  cp -a ${PKG_BUILD}/simpleton-ui ${INSTALL}/usr/bin
  cp -a ${PKG_BUILD}/simpleton-cache ${INSTALL}/usr/bin
  cp -a ${PKG_BUILD}/share/. ${INSTALL}/usr/share/simpleton/
}

post_install() {
  enable_service simpleton-ui.service
}
