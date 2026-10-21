/*
 * SimpletonOS UI - minimal MPD client (implementation). See mpdc.h.
 *
 * Protocol shape: connect, read "OK MPD x.y.z", then for each command write
 * "cmd args\n" and read lines until one is exactly "OK" or starts with "ACK ".
 * Arguments are double-quoted with \ and " escaped.
 *
 * Three connections (see mpdc.h): `ui` for everything the UI thread does,
 * `art` used only by mpd_readpicture() from the art worker thread, and
 * `idle`, which sits in MPD's idle state and is read only when poll() says
 * MPD has spoken. Each has its own socket and buffered reader, so there is
 * no shared state between threads inside this file.
 */
#include "mpdc.h"
#include "strings.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MPD_HOST "127.0.0.1"
#define MPD_PORT 6600
#define IO_TIMEOUT_S 3

/* readpicture chunk size we ask MPD for. Default is 8 KiB, which would turn
 * a 1 MB cover into ~130 round trips; 256 KiB makes it four. */
#define BINARY_LIMIT (256 * 1024)

typedef struct {
    int    sock;
    FILE * rf;              /* buffered reader over sock */
    const char * name;      /* for log lines */
} conn_t;

static conn_t ui  = { .sock = -1, .name = "ui"  };
static conn_t art = { .sock = -1, .name = "art" };
static conn_t idle = { .sock = -1, .name = "idle" };

/* Everything the UI reacts to. MPD exempts idling clients from its
 * connection_timeout, so this connection can wait indefinitely. */
#define IDLE_CMD "idle database update player mixer options playlist output\n"

static void disconnect(conn_t * c)
{
    if(c->rf) { fclose(c->rf); c->rf = NULL; }   /* fclose closes sock too */
    else if(c->sock >= 0) close(c->sock);
    c->sock = -1;
}

static void (*before_change)(void);                /* 0.31k: see mpd_set_before_change */
#define BEFORE_CHANGE() do { if(before_change) before_change(); } while(0)

bool mpd_is_connected(void) { return ui.sock >= 0; }

static bool read_line(conn_t * c, char * buf, size_t len)
{
    if(!c->rf || !fgets(buf, len, c->rf)) { disconnect(c); return false; }
    size_t n = strlen(buf);
    if(n && buf[n - 1] == '\n') buf[n - 1] = 0;
    return true;
}

static bool send_raw(conn_t * c, const char * s)
{
    size_t len = strlen(s), off = 0;
    while(off < len) {
        ssize_t n = write(c->sock, s + off, len - off);
        if(n <= 0) { disconnect(c); return false; }
        off += (size_t)n;
    }
    return true;
}

/* Read the response tail: returns true on "OK", false on "ACK ..." or drop. */
static bool read_ok(conn_t * c)
{
    char line[1024];
    for(;;) {
        if(!read_line(c, line, sizeof(line))) return false;
        if(strcmp(line, "OK") == 0) return true;
        if(strncmp(line, "ACK ", 4) == 0) { fprintf(stderr, "simpleton-ui: mpd: %s\n", line); return false; }
        /* any other line is payload we didn't ask to keep */
    }
}

/* MPD closes a command connection that has been quiet for its
 * connection_timeout (60 s by default); the idle connection is exempt. A
 * closed-by-peer socket shows as readable with nothing to read, so one
 * free poll() before a command tells us to reconnect instead of sending
 * into a dead socket and reading EOF - which is how the album page lost
 * its playing mark on the first track change after a few quiet minutes
 * (10 Oct 2026). */
static bool conn_alive(conn_t * c)
{
    if(c->sock < 0) return false;
    struct pollfd pf = { .fd = c->sock, .events = POLLIN };
    if(poll(&pf, 1, 0) <= 0) return true;                  /* nothing pending: fine */
    char b;
    ssize_t n = recv(c->sock, &b, 1, MSG_PEEK | MSG_DONTWAIT);
    if(n > 0) return true;                                 /* a reply we haven't read yet: leave it */
    fprintf(stderr, "simpleton-ui: mpd[%s]: connection was closed (quiet too long), reconnecting\n", c->name);
    disconnect(c);
    return false;
}

static bool connect_conn(conn_t * c)
{
    if(c->sock >= 0 && conn_alive(c)) return true;
    if(c->sock >= 0) return false;

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(fd < 0) return false;

    struct timeval tv = { .tv_sec = IO_TIMEOUT_S };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    const char * port_env = getenv("MPD_PORT");          /* off-device tests; same variable mpc uses */
    int port = port_env && atoi(port_env) > 0 ? atoi(port_env) : MPD_PORT;
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    inet_pton(AF_INET, MPD_HOST, &addr.sin_addr);
    if(connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return false; }

    c->sock = fd;
    c->rf = fdopen(fd, "r");
    if(!c->rf) { close(fd); c->sock = -1; return false; }

    char line[256];
    if(!read_line(c, line, sizeof(line)) || strncmp(line, "OK MPD", 6) != 0) { disconnect(c); return false; }
    fprintf(stderr, "simpleton-ui: mpd[%s]: connected (%s)\n", c->name, line);

    if(c == &art) {
        /* Bigger readpicture chunks. Older MPD without binarylimit just ACKs;
         * that's harmless, we fall back to its default chunk size. */
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "binarylimit %d\n", BINARY_LIMIT);
        if(send_raw(c, cmd)) read_ok(c);
    }
    return c->sock >= 0;
}

bool mpd_connect(void) { return connect_conn(&ui); }

/* Append a quoted+escaped argument to a command buffer. */
static void append_quoted(char * buf, size_t len, const char * arg)
{
    size_t n = strlen(buf);
    if(n + 3 >= len) return;
    buf[n++] = ' ';
    buf[n++] = '"';
    for(const char * p = arg; *p && n + 3 < len; p++) {
        if(*p == '"' || *p == '\\') buf[n++] = '\\';
        buf[n++] = *p;
    }
    buf[n++] = '"';
    buf[n] = 0;
}

static bool simple_command(const char * cmd)
{
    if(!mpd_connect()) return false;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s\n", cmd);
    return send_raw(&ui, buf) && read_ok(&ui);
}

/* Strip directory and (for files) the extension for a fallback label. */
static char * basename_label(const char * uri, bool strip_ext)
{
    const char * slash = strrchr(uri, '/');
    const char * base = slash ? slash + 1 : uri;
    char * out = strdup(base);
    if(out && strip_ext) {
        char * dot = strrchr(out, '.');
        if(dot && dot != out) *dot = 0;
    }
    return out;
}

static void listing_push(mpd_listing_t * l, mpd_entry_kind_t kind, const char * uri)
{
    mpd_entry_t * grown = realloc(l->items, sizeof(mpd_entry_t) * (size_t)(l->count + 1));
    if(!grown) return;
    l->items = grown;
    mpd_entry_t * e = &l->items[l->count++];
    e->kind = kind;
    e->uri = strdup(uri);
    e->display = basename_label(uri, kind == MPD_ENTRY_FILE);
    e->section = NULL;
    e->mtime = 0;
}

/* Replace a file entry's display label with "NN Title" if MPD gave us tags.
 * A track number alone (SACD tracks with no text on the disc) reads
 * "Track NN" (string table: track.numbered) rather than the container's
 * internal file name. */
static long iso_time(const char * s);

static void apply_tags(mpd_entry_t * e, const char * title, const char * track)
{
    if(!e) return;
    char label[512];
    if(title[0] && track[0]) snprintf(label, sizeof(label), "%s  %s", track, title);
    else if(title[0])        snprintf(label, sizeof(label), "%s", title);
    else if(track[0])        ui_strf(label, sizeof(label), S_TRACK_NUMBERED, "n", track, NULL);
    else return;
    free(e->display);
    e->display = strdup(label);
}

bool mpd_lsinfo(const char * uri, mpd_listing_t * out)
{
    memset(out, 0, sizeof(*out));
    if(!mpd_connect()) return false;

    char cmd[2048] = "lsinfo";
    if(uri && uri[0]) append_quoted(cmd, sizeof(cmd), uri);
    strncat(cmd, "\n", sizeof(cmd) - strlen(cmd) - 1);
    /* A connection MPD dropped while we were idle only shows when we use it:
     * the write fails, or the first reply line never comes. Reconnect and
     * ask once more before giving up (0.16b: an album sometimes opened the
     * card list instead, because this failed once and the browser fell back
     * to the root). */
    for(int attempt = 0;; attempt++) {
        if(!mpd_connect()) return false;
        if(send_raw(&ui, cmd) && ui.rf) {
            int c = fgetc(ui.rf);
            if(c != EOF) { ungetc(c, ui.rf); break; }
            disconnect(&ui);
        }
        if(attempt >= 1) return false;
    }

    char line[2048];
    char title[256] = "", track[16] = "";
    mpd_entry_t * cur = NULL;          /* the file entry tags apply to      */
    mpd_entry_t * last = NULL;         /* the entry (file or folder) a Last-Modified line belongs to */

    for(;;) {
        if(!read_line(&ui, line, sizeof(line))) { mpd_listing_free(out); return false; }
        if(strcmp(line, "OK") == 0) break;
        if(strncmp(line, "ACK ", 4) == 0) { fprintf(stderr, "simpleton-ui: mpd: %s\n", line); mpd_listing_free(out); return false; }

        if(strncmp(line, "directory: ", 11) == 0) {
            apply_tags(cur, title, track); title[0] = track[0] = 0;
            listing_push(out, MPD_ENTRY_DIR, line + 11);
            cur = NULL;
            last = out->count ? &out->items[out->count - 1] : NULL;
        }
        else if(strncmp(line, "file: ", 6) == 0) {
            apply_tags(cur, title, track); title[0] = track[0] = 0;
            listing_push(out, MPD_ENTRY_FILE, line + 6);
            cur = out->count ? &out->items[out->count - 1] : NULL;
            last = cur;
        }
        else if(strncmp(line, "Last-Modified: ", 15) == 0) {
            if(last) last->mtime = iso_time(line + 15);       /* folders and files alike (0.14: the recently-changed sort) */
        }
        else if(strncmp(line, "Title: ", 7) == 0) {
            snprintf(title, sizeof(title), "%s", line + 7);
        }
        else if(strncmp(line, "Track: ", 7) == 0) {
            /* keep the leading number only ("3/12" -> "3"); zero-pad to 2 */
            int n = atoi(line + 7);
            if(n > 0) snprintf(track, sizeof(track), "%02d", n);
        }
        /* playlist:, other tags: ignored */
    }
    apply_tags(cur, title, track);
    return true;
}

/* "2026-10-04T20:50:27Z" -> epoch seconds, 0 when unparseable. */
static long iso_time(const char * s)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if(sscanf(s, "%d-%d-%dT%d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) < 3) return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return (long)timegm(&tm);
}

static bool stream_songs(const char * cmd, bool (*cb)(const mpd_song_info_t * song, void * ctx), void * ctx);
static void (*before_play)(void);

bool mpd_listallinfo(const char * uri, bool (*cb)(const mpd_song_info_t * song, void * ctx), void * ctx)
{
    char cmd[2048] = "listallinfo";
    if(uri && uri[0]) append_quoted(cmd, sizeof(cmd), uri);
    strncat(cmd, "\n", sizeof(cmd) - strlen(cmd) - 1);
    return stream_songs(cmd, cb, ctx);
}

/* The queue, in order (0.23: now-playing's track list); song.track is the
 * queue position, song.added the song id. */
bool mpd_queue(bool (*cb)(const mpd_song_info_t * song, void * ctx), void * ctx)
{
    return stream_songs("playlistinfo\n", cb, ctx);
}

bool mpd_play_pos(int pos)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "play %d", pos);
    BEFORE_CHANGE();
    if(before_play) before_play();
    return simple_command(cmd);
}

bool mpd_set_mode(bool repeat, bool single, bool random)
{
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "command_list_begin\nrepeat %d\nsingle %d\nrandom %d\ncommand_list_end", repeat ? 1 : 0, single ? 1 : 0, random ? 1 : 0);
    return simple_command(cmd);
}

static bool stream_songs(const char * cmd, bool (*cb)(const mpd_song_info_t * song, void * ctx), void * ctx)
{
    if(!mpd_connect()) return false;
    if(!send_raw(&ui, cmd)) return false;

    static mpd_song_info_t song;        /* 3 KB: not on the stack of a UI thread */
    bool in_song = false, go_on = true, ok = true;
    char line[2048];
    for(;;) {
        if(!read_line(&ui, line, sizeof(line))) return false;
        if(strcmp(line, "OK") == 0) break;
        if(strncmp(line, "ACK ", 4) == 0) { fprintf(stderr, "simpleton-ui: mpd: %s\n", line); ok = false; break; }
        if(!go_on) continue;

        bool new_entry = strncmp(line, "file: ", 6) == 0 || strncmp(line, "directory: ", 11) == 0 || strncmp(line, "playlist: ", 10) == 0;
        if(new_entry && in_song) { in_song = false; if(!cb(&song, ctx)) go_on = false; }
        if(strncmp(line, "file: ", 6) == 0) {
            memset(&song, 0, sizeof(song));
            snprintf(song.uri, sizeof(song.uri), "%.1023s", line + 6);   /* URIs longer than this are not addressable anyway */
            in_song = true;
            continue;
        }
        if(!in_song) continue;
        char * colon = strchr(line, ':');
        if(!colon || colon[1] != ' ') continue;
        *colon = 0;
        const char * key = line, * val = colon + 2;
        if(strcasecmp(key, "Title") == 0)              snprintf(song.title, sizeof(song.title), "%s", val);
        else if(strcasecmp(key, "Artist") == 0)        { if(!song.artist[0]) snprintf(song.artist, sizeof(song.artist), "%s", val); }
        else if(strcasecmp(key, "AlbumArtist") == 0)   { if(!song.album_artist[0]) snprintf(song.album_artist, sizeof(song.album_artist), "%s", val); }
        else if(strcasecmp(key, "Album") == 0)         { if(!song.album[0]) snprintf(song.album, sizeof(song.album), "%s", val); }
        else if(strcasecmp(key, "Date") == 0)          { if(!song.date[0]) snprintf(song.date, sizeof(song.date), "%s", val); }
        else if(strcasecmp(key, "OriginalDate") == 0)  { if(!song.original_date[0]) snprintf(song.original_date, sizeof(song.original_date), "%s", val); }
        else if(strcasecmp(key, "Track") == 0)         song.track = atoi(val);
        else if(strcasecmp(key, "Pos") == 0)           song.track = atoi(val);       /* the queue: its position */
        else if(strcasecmp(key, "Id") == 0)            song.added = atol(val);
        else if(strcasecmp(key, "Disc") == 0)          song.disc = atoi(val);
        else if(strcasecmp(key, "duration") == 0)      song.duration = (float)atof(val);
        else if(strcasecmp(key, "Time") == 0)          { if(song.duration <= 0) song.duration = (float)atoi(val); }
        else if(strcasecmp(key, "Last-Modified") == 0) song.mtime = iso_time(val);
        else if(strcasecmp(key, "Added") == 0)         song.added = iso_time(val);
        else if(strcasecmp(key, "Format") == 0)        snprintf(song.format, sizeof(song.format), "%s", val);
        else if(strcasecmp(key, "Genre") == 0) {
            size_t o = strlen(song.genre);
            if(o == 0) snprintf(song.genre, sizeof(song.genre), "%s", val);
            else if(o + 3 < sizeof(song.genre)) snprintf(song.genre + o, sizeof(song.genre) - o, " / %s", val);
        }
    }
    if(in_song && go_on) cb(&song, ctx);
    return ok;
}

void mpd_listing_free(mpd_listing_t * l)
{
    for(int i = 0; i < l->count; i++) { free(l->items[i].uri); free(l->items[i].display); free(l->items[i].section); }
    free(l->items);
    l->items = NULL;
    l->count = 0;
}

/* ------------------------------------------------ transparent containers */

static const char * ext_of(const char * uri)
{
    const char * slash = strrchr(uri, '/');
    const char * base = slash ? slash + 1 : uri;
    const char * dot = strrchr(base, '.');
    return (dot && dot != base) ? dot + 1 : NULL;
}

static bool ext_in(const char * ext, const char * const * list)
{
    if(!ext) return false;
    for(int i = 0; list[i]; i++) if(strcasecmp(ext, list[i]) == 0) return true;
    return false;
}

/* Anything MPD may have turned into a directory of tracks: disc images,
 * multi-track DSDIFF, cue sheets, and audio files carrying an embedded cue
 * sheet. A real folder named "Disc 1.flac" would be probed too - harmless,
 * the probe just finds a folder and leaves it alone. */
static const char * const container_exts[] = {
    "iso", "dat", "dff", "cue", "flac", "wv", "ape", "wav", "tak", "m4a", "dsf", NULL };

/* A file entry that could hide a container behind it (what row_click used
 * to probe one by one). Plain audio files are never probed. */
static const char * const probe_file_exts[] = { "iso", "dat", "dff", "cue", NULL };

bool mpd_is_container_name(const char * uri)
{
    return ext_in(ext_of(uri), container_exts);
}

/* "Disc 1" from "Album/Disc 1.iso" - the divider label. */
static char * stem_of(const char * uri) { return basename_label(uri, true); }

static void listing_push_copy(mpd_listing_t * l, const mpd_entry_t * e)
{
    mpd_entry_t * grown = realloc(l->items, sizeof(mpd_entry_t) * (size_t)(l->count + 1));
    if(!grown) return;
    l->items = grown;
    mpd_entry_t * n = &l->items[l->count++];
    n->kind = e->kind;
    n->uri = strdup(e->uri);
    n->display = strdup(e->display);
    n->section = e->section ? strdup(e->section) : NULL;
    n->mtime = e->mtime;
}

bool mpd_lsinfo_expanded(const char * uri, mpd_listing_t * out)
{
    mpd_listing_t raw;
    memset(out, 0, sizeof(*out));
    if(!mpd_lsinfo(uri, &raw)) return false;

    mpd_listing_t * sub = calloc((size_t)(raw.count ? raw.count : 1), sizeof(mpd_listing_t));
    bool * expanded = calloc((size_t)(raw.count ? raw.count : 1), sizeof(bool));
    if(!sub || !expanded) { free(sub); free(expanded); mpd_listing_free(&raw); return false; }

    /* pass 1: ask MPD what is inside every container-looking entry */
    for(int i = 0; i < raw.count; i++) {
        const mpd_entry_t * e = &raw.items[i];
        bool candidate = e->kind == MPD_ENTRY_DIR ? ext_in(ext_of(e->uri), container_exts)
                                                  : ext_in(ext_of(e->uri), probe_file_exts);
        if(!candidate) continue;
        if(!mpd_lsinfo(e->uri, &sub[i])) continue;
        int files = 0;
        for(int j = 0; j < sub[i].count; j++)
            if(sub[i].items[j].kind == MPD_ENTRY_FILE && strcmp(sub[i].items[j].uri, e->uri) != 0) files++;
        if(files > 0) expanded[i] = true;
        else mpd_listing_free(&sub[i]);
    }

    /* pass 2: rebuild the listing in order, containers replaced by their
     * tracks, the audio file behind a cue sheet dropped. Two discs in one
     * folder simply run on (decided 1 Oct 2026: no divider - the next disc
     * starts at track 1, which says it all). */
    for(int i = 0; i < raw.count; i++) {
        const mpd_entry_t * e = &raw.items[i];
        if(expanded[i]) {
            for(int j = 0; j < sub[i].count; j++) {
                const mpd_entry_t * t = &sub[i].items[j];
                if(t->kind != MPD_ENTRY_FILE || strcmp(t->uri, e->uri) == 0) continue;
                listing_push_copy(out, t);
            }
            continue;
        }
        if(e->kind == MPD_ENTRY_FILE) {
            /* "Album.flac" next to an expanded "Album.cue": the sheet's
             * tracks already cover it */
            bool hidden = false;
            char * mine = stem_of(e->uri);
            for(int k = 0; k < raw.count && !hidden && mine; k++) {
                if(k == i || !expanded[k]) continue;
                char * other = stem_of(raw.items[k].uri);
                hidden = other && strcmp(other, mine) == 0;
                free(other);
            }
            free(mine);
            if(hidden) continue;
        }
        listing_push_copy(out, e);
    }

    for(int i = 0; i < raw.count; i++) mpd_listing_free(&sub[i]);
    free(sub);
    free(expanded);
    mpd_listing_free(&raw);
    return true;
}

/* ------------------------------------------------------------- queue */

/* 0.21c: called before any command that starts playback (dacpower.c wakes
 * a sleeping DAC first). */
void mpd_set_before_play(void (*cb)(void)) { before_play = cb; }

/* 0.31k: called before any command that moves to another track or place
 * (viz.c takes its fifo feed off first: with the fifo output on, MPD
 * stalled at the start of a DSD track and the DAC got silence). */
void mpd_set_before_change(void (*cb)(void)) { before_change = cb; }


bool mpd_play_uris(char * const * uris, int count, int start_index)
{
    if(!mpd_connect() || count <= 0) return false;
    BEFORE_CHANGE();
    if(before_play) before_play();
    if(!send_raw(&ui, "command_list_begin\nclear\n")) return false;
    for(int i = 0; i < count; i++) {
        char cmd[2048] = "add";
        append_quoted(cmd, sizeof(cmd), uris[i]);
        strncat(cmd, "\n", sizeof(cmd) - strlen(cmd) - 1);
        if(!send_raw(&ui, cmd)) return false;
    }
    char play[64];
    snprintf(play, sizeof(play), "play %d\ncommand_list_end\n", start_index);
    return send_raw(&ui, play) && read_ok(&ui);
}

bool mpd_add_uris(char * const * uris, int count)
{
    if(!mpd_connect() || count <= 0) return false;
    if(!send_raw(&ui, "command_list_begin\n")) return false;
    for(int i = 0; i < count; i++) {
        char cmd[2048] = "add";
        append_quoted(cmd, sizeof(cmd), uris[i]);
        strncat(cmd, "\n", sizeof(cmd) - strlen(cmd) - 1);
        if(!send_raw(&ui, cmd)) return false;
    }
    return send_raw(&ui, "command_list_end\n") && read_ok(&ui);
}

bool mpd_toggle_pause(void)
{
    mpd_status_t st;
    if(!mpd_status(&st)) return false;
    if(strcmp(st.state, "play") == 0)  return simple_command("pause 1");
    if(before_play) before_play();
    if(strcmp(st.state, "pause") == 0) return simple_command("pause 0");
    return simple_command("play");          /* stopped: start the queue */
}

/* "play" with no argument resumes from pause or starts a stopped queue;
 * "pause 1" while stopped does nothing. Neither needs a status round trip. */
bool mpd_resume(void) { if(before_play) before_play(); return simple_command("play"); }
bool mpd_pause(void)  { return simple_command("pause 1"); }

/* ------------------------------------------------- idle notifications */

int mpd_idle_fd(void) { return idle.sock; }

bool mpd_idle_start(void)
{
    if(idle.sock >= 0) return false;
    if(!connect_conn(&idle)) return false;
    if(!send_raw(&idle, IDLE_CMD)) return false;
    return true;
}

unsigned mpd_idle_poll(void)
{
    if(idle.sock < 0) return MPD_IDLE_LOST;
    static const struct { const char * name; unsigned bit; } map[] = {
        { "database", MPD_CHANGED_DATABASE }, { "update", MPD_CHANGED_UPDATE },
        { "player", MPD_CHANGED_PLAYER },     { "mixer", MPD_CHANGED_MIXER },
        { "options", MPD_CHANGED_OPTIONS },   { "playlist", MPD_CHANGED_PLAYLIST },
        { "output", MPD_CHANGED_OUTPUT },
    };
    unsigned changed = 0;
    char line[256];
    /* poll() said readable: MPD sends the whole reply at once ("changed: x"
     * lines then "OK"), so these reads don't wait. EOF = MPD went away. */
    for(;;) {
        if(!read_line(&idle, line, sizeof(line))) return MPD_IDLE_LOST;
        if(strcmp(line, "OK") == 0) break;
        if(strncmp(line, "ACK ", 4) == 0) { fprintf(stderr, "simpleton-ui: mpd[idle]: %s\n", line); break; }
        if(strncmp(line, "changed: ", 9) == 0)
            for(size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
                if(strcmp(line + 9, map[i].name) == 0) changed |= map[i].bit;
    }
    if(!send_raw(&idle, IDLE_CMD)) return changed | MPD_IDLE_LOST;   /* re-arm */
    return changed;
}

bool mpd_library_state(long * db_update, bool * updating)
{
    *db_update = 0;
    *updating = false;
    if(!mpd_connect()) return false;
    if(!send_raw(&ui, "command_list_ok_begin\nstatus\nstats\ncommand_list_end\n")) return false;
    char line[512];
    for(;;) {
        if(!read_line(&ui, line, sizeof(line))) return false;
        if(strcmp(line, "OK") == 0) break;
        if(strncmp(line, "ACK ", 4) == 0) { fprintf(stderr, "simpleton-ui: mpd: %s\n", line); return false; }
        if(strncmp(line, "updating_db: ", 13) == 0) *updating = true;
        else if(strncmp(line, "db_update: ", 11) == 0) *db_update = atol(line + 11);
    }
    return true;
}

bool mpd_next(void)     { BEFORE_CHANGE(); return simple_command("next"); }
bool mpd_update(void)
{
    if(!mpd_connect()) return false;
    if(!send_raw(&ui, "update\n")) return false;
    /* reply: "updating_db: N" then OK */
    char line[128];
    for(;;) {
        if(!read_line(&ui, line, sizeof(line))) return false;
        if(strcmp(line, "OK") == 0) return true;
        if(strncmp(line, "ACK ", 4) == 0) { fprintf(stderr, "simpleton-ui: mpd: %s\n", line); return false; }
    }
}
bool mpd_previous(void) { BEFORE_CHANGE(); return simple_command("previous"); }

/* An output by its name in mpd.conf: on or off while playing (0.31: the
 * visualizers' fifo feed, "viz", on only while one is on screen). */
bool mpd_output_set(const char * name, bool on)
{
    if(!mpd_connect()) return false;
    if(!send_raw(&ui, "outputs\n")) return false;
    char line[256]; int id = -1, cur = -1;
    for(;;) {
        if(!read_line(&ui, line, sizeof(line))) return false;
        if(strcmp(line, "OK") == 0) break;
        if(strncmp(line, "ACK ", 4) == 0) { fprintf(stderr, "simpleton-ui: mpd: %s\n", line); return false; }
        if(strncmp(line, "outputid: ", 10) == 0) cur = atoi(line + 10);
        else if(strncmp(line, "outputname: ", 12) == 0 && strcmp(line + 12, name) == 0) id = cur;
    }
    if(id < 0) { fprintf(stderr, "simpleton-ui: mpd: no output named %s\n", name); return false; }
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "%s %d", on ? "enableoutput" : "disableoutput", id);
    return simple_command(cmd);
}

/* The sync page's metronome (0.35a, Ian: "it is impossible to time it with
 * regular music"). The click track has to come through MPD itself - that
 * is the path being measured - and MPD only plays what its database knows,
 * so: the queue is saved as a playlist, a folder holding the click is
 * mounted as a storage of its own (patch 0004 allows folders under
 * /var/media), indexed, the click queued and looped; on leaving everything
 * is put back - the queue, the song, the position, the options. */
#define SYNC_PLAYLIST "__simpleton-sync"
#define SYNC_STORAGE  "sync-click"
static struct { bool saved; int song; float elapsed; char state[8]; bool repeat, single, random; } sync_was;

static bool wait_update_done(void)
{
    for(int i = 0; i < 100; i++) {                     /* up to 5 s */
        if(!send_raw(&ui, "status\n")) return false;
        char line[256]; bool updating = false;
        for(;;) {
            if(!read_line(&ui, line, sizeof(line))) return false;
            if(strcmp(line, "OK") == 0) break;
            if(strncmp(line, "ACK ", 4) == 0) return false;
            if(strncmp(line, "updating_db:", 12) == 0) updating = true;
        }
        if(!updating) return true;
        usleep(50000);
    }
    return false;
}

bool mpd_sync_begin(const char * dir)
{
    mpd_status_t st;
    if(!mpd_status(&st)) return false;
    sync_was.song = st.song; sync_was.elapsed = st.elapsed; snprintf(sync_was.state, sizeof(sync_was.state), "%s", st.state);
    sync_was.repeat = st.repeat; sync_was.single = st.single; sync_was.random = st.random;
    simple_command("rm \"" SYNC_PLAYLIST "\"");
    sync_was.saved = st.playlistlength > 0 && simple_command("save \"" SYNC_PLAYLIST "\"");
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "mount \"" SYNC_STORAGE "\" \"file://%s\"", dir);
    simple_command(cmd);                                /* already mounted from a page left in a hurry: fine */
    if(!simple_command("update \"" SYNC_STORAGE "\"")) { fprintf(stderr, "simpleton-ui: sync: the click folder could not be indexed\n"); return false; }
    wait_update_done();
    if(!simple_command("clear")) return false;
    if(!simple_command("add \"" SYNC_STORAGE "/click.wav\"")) { fprintf(stderr, "simpleton-ui: sync: the click is not in MPD's database\n"); mpd_sync_end(); return false; }
    simple_command("random 0"); simple_command("repeat 1"); simple_command("single 1");
    if(before_play) before_play();
    return simple_command("play 0");
}

void mpd_sync_end(void)
{
    simple_command("stop");
    simple_command("clear");
    if(sync_was.saved) { simple_command("load \"" SYNC_PLAYLIST "\""); simple_command("rm \"" SYNC_PLAYLIST "\""); }
    simple_command("unmount \"" SYNC_STORAGE "\"");
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "repeat %d", sync_was.repeat ? 1 : 0); simple_command(cmd);
    snprintf(cmd, sizeof(cmd), "single %d", sync_was.single ? 1 : 0); simple_command(cmd);
    snprintf(cmd, sizeof(cmd), "random %d", sync_was.random ? 1 : 0); simple_command(cmd);
    if(sync_was.saved && sync_was.song >= 0) {
        snprintf(cmd, sizeof(cmd), "play %d", sync_was.song); simple_command(cmd);
        snprintf(cmd, sizeof(cmd), "seekcur %.1f", (double)sync_was.elapsed); simple_command(cmd);
        if(strcmp(sync_was.state, "pause") == 0) simple_command("pause 1");
        else if(strcmp(sync_was.state, "stop") == 0) simple_command("stop");
    }
    sync_was.saved = false;
}

bool mpd_seek_relative(float seconds)
{
    char cmd[64];
    /* MPD wants an explicit sign for a relative seek */
    snprintf(cmd, sizeof(cmd), "seekcur %+.1f", (double)seconds);
    BEFORE_CHANGE();
    return simple_command(cmd);
}

static void copy_field(char * dst, size_t len, const char * src)
{
    snprintf(dst, len, "%s", src);
}

bool mpd_status(mpd_status_t * out)
{
    memset(out, 0, sizeof(*out));
    out->songid = -1;
    out->song = -1;
    out->nextsong = -1;
    if(!mpd_connect()) return false;

    /* Both in one command list; "list_OK" separates the two replies. */
    if(!send_raw(&ui, "command_list_ok_begin\nstatus\ncurrentsong\ncommand_list_end\n")) return false;

    char line[2048];
    for(;;) {
        if(!read_line(&ui, line, sizeof(line))) return false;
        if(strcmp(line, "OK") == 0) break;
        if(strcmp(line, "list_OK") == 0) continue;
        if(strncmp(line, "ACK ", 4) == 0) { fprintf(stderr, "simpleton-ui: mpd: %s\n", line); return false; }

        char * colon = strchr(line, ':');
        if(!colon || colon[1] != ' ') continue;
        *colon = 0;
        const char * key = line, * val = colon + 2;

        if(strcmp(key, "state") == 0)         copy_field(out->state, sizeof(out->state), val);
        else if(strcmp(key, "songid") == 0)   out->songid = atoi(val);
        else if(strcmp(key, "song") == 0)     out->song = atoi(val);
        else if(strcmp(key, "nextsong") == 0) out->nextsong = atoi(val);
        else if(strcmp(key, "playlistlength") == 0) out->playlistlength = atoi(val);
        else if(strcmp(key, "repeat") == 0)   out->repeat = atoi(val) != 0;
        else if(strcmp(key, "random") == 0)   out->random = atoi(val) != 0;
        else if(strcmp(key, "single") == 0)   out->single = strcmp(val, "0") != 0;   /* "1" or "oneshot" */
        else if(strcmp(key, "elapsed") == 0)  out->elapsed = (float)atof(val);
        else if(strcmp(key, "duration") == 0) out->duration = (float)atof(val);
        else if(strcmp(key, "audio") == 0)    copy_field(out->audio, sizeof(out->audio), val);
        else if(strcmp(key, "file") == 0)     copy_field(out->file, sizeof(out->file), val);
        else if(strcmp(key, "Title") == 0)    copy_field(out->title, sizeof(out->title), val);
        else if(strcmp(key, "Artist") == 0)   copy_field(out->artist, sizeof(out->artist), val);
        else if(strcmp(key, "Album") == 0)    copy_field(out->album, sizeof(out->album), val);
        else if(strcmp(key, "error") == 0)    copy_field(out->error, sizeof(out->error), val);
        /* "Time:" (integer duration) is a fallback when "duration" is absent */
        else if(strcmp(key, "Time") == 0 && out->duration == 0) out->duration = (float)atof(val);
    }
    if(!out->title[0] && out->file[0]) {
        /* untagged: show the file name, like the browser does */
        char * label = basename_label(out->file, true);
        if(label) { copy_field(out->title, sizeof(out->title), label); free(label); }
    }
    return true;
}

/*
 * readpicture reply, per chunk:
 *   size: <total>\n  type: image/jpeg\n  binary: <n>\n  <n raw bytes>\n  OK\n
 * Loop with increasing offset until we hold `size` bytes. A song without a
 * picture answers with just "OK" (no size line).
 */
bool mpd_readpicture(const char * uri, uint8_t ** data, size_t * len)
{
    *data = NULL;
    *len = 0;
    if(!connect_conn(&art)) return false;

    uint8_t * buf = NULL;
    size_t total = 0, have = 0;

    for(;;) {
        char cmd[2048] = "readpicture";
        append_quoted(cmd, sizeof(cmd), uri);
        char off[32];
        snprintf(off, sizeof(off), " %zu\n", have);
        strncat(cmd, off, sizeof(cmd) - strlen(cmd) - 1);
        if(!send_raw(&art, cmd)) goto fail;

        size_t chunk = 0, size_now = 0;
        bool got_binary = false;
        char line[256];
        for(;;) {
            if(!read_line(&art, line, sizeof(line))) goto fail;
            if(strcmp(line, "OK") == 0) break;
            if(strncmp(line, "ACK ", 4) == 0) { fprintf(stderr, "simpleton-ui: mpd[art]: %s\n", line); goto fail; }
            if(strncmp(line, "size: ", 6) == 0)   size_now = (size_t)strtoul(line + 6, NULL, 10);
            else if(strncmp(line, "binary: ", 8) == 0) {
                chunk = (size_t)strtoul(line + 8, NULL, 10);
                got_binary = true;
                if(total == 0) {
                    total = size_now;
                    if(total == 0 || total > (64u << 20)) goto fail;   /* nothing, or absurd */
                    buf = malloc(total);
                    if(!buf) goto fail;
                }
                if(have + chunk > total) goto fail;
                if(chunk && fread(buf + have, 1, chunk, art.rf) != chunk) { disconnect(&art); goto fail; }
                have += chunk;
                /* the raw bytes are followed by one "\n" before "OK" */
                int c = fgetc(art.rf);
                if(c != '\n') { disconnect(&art); goto fail; }
            }
        }
        if(!got_binary) goto fail;              /* "OK" alone: no picture */
        if(chunk == 0 || have >= total) break;  /* done (or MPD stopped sending) */
    }
    if(have != total) goto fail;
    *data = buf;
    *len = total;
    return true;

fail:
    free(buf);
    return false;
}
