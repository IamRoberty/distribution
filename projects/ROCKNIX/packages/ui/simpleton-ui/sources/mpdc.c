/*
 * SimpletonOS UI - minimal MPD client (implementation). See mpdc.h.
 *
 * Protocol shape: connect, read "OK MPD x.y.z", then for each command write
 * "cmd args\n" and read lines until one is exactly "OK" or starts with "ACK ".
 * Arguments are double-quoted with \ and " escaped.
 *
 * Two connections (see mpdc.h): `ui` for everything the UI thread does, and
 * `art` used only by mpd_readpicture() from the art worker thread. Each has
 * its own socket and buffered reader, so there is no shared state between
 * threads inside this file.
 */
#include "mpdc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
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

static void disconnect(conn_t * c)
{
    if(c->rf) { fclose(c->rf); c->rf = NULL; }   /* fclose closes sock too */
    else if(c->sock >= 0) close(c->sock);
    c->sock = -1;
}

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

static bool connect_conn(conn_t * c)
{
    if(c->sock >= 0) return true;

    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if(fd < 0) return false;

    struct timeval tv = { .tv_sec = IO_TIMEOUT_S };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(MPD_PORT) };
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
}

/* Replace a file entry's display label with "NN Title" if MPD gave us tags. */
static void apply_tags(mpd_entry_t * e, const char * title, const char * track)
{
    if(!e || !title[0]) return;
    char label[512];
    if(track[0]) snprintf(label, sizeof(label), "%s  %s", track, title);
    else         snprintf(label, sizeof(label), "%s", title);
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
    if(!send_raw(&ui, cmd)) return false;

    char line[2048];
    char title[256] = "", track[16] = "";
    mpd_entry_t * cur = NULL;

    for(;;) {
        if(!read_line(&ui, line, sizeof(line))) { mpd_listing_free(out); return false; }
        if(strcmp(line, "OK") == 0) break;
        if(strncmp(line, "ACK ", 4) == 0) { fprintf(stderr, "simpleton-ui: mpd: %s\n", line); mpd_listing_free(out); return false; }

        if(strncmp(line, "directory: ", 11) == 0) {
            apply_tags(cur, title, track); title[0] = track[0] = 0;
            listing_push(out, MPD_ENTRY_DIR, line + 11);
            cur = NULL;
        }
        else if(strncmp(line, "file: ", 6) == 0) {
            apply_tags(cur, title, track); title[0] = track[0] = 0;
            listing_push(out, MPD_ENTRY_FILE, line + 6);
            cur = out->count ? &out->items[out->count - 1] : NULL;
        }
        else if(strncmp(line, "Title: ", 7) == 0) {
            snprintf(title, sizeof(title), "%s", line + 7);
        }
        else if(strncmp(line, "Track: ", 7) == 0) {
            /* keep the leading number only ("3/12" -> "3"); zero-pad to 2 */
            int n = atoi(line + 7);
            if(n > 0) snprintf(track, sizeof(track), "%02d", n);
        }
        /* playlist:, Last-Modified:, other tags: ignored */
    }
    apply_tags(cur, title, track);
    return true;
}

void mpd_listing_free(mpd_listing_t * l)
{
    for(int i = 0; i < l->count; i++) { free(l->items[i].uri); free(l->items[i].display); }
    free(l->items);
    l->items = NULL;
    l->count = 0;
}

bool mpd_play_uris(char * const * uris, int count, int start_index)
{
    if(!mpd_connect() || count <= 0) return false;
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

bool mpd_toggle_pause(void)
{
    mpd_status_t st;
    if(!mpd_status(&st)) return false;
    if(strcmp(st.state, "play") == 0)  return simple_command("pause 1");
    if(strcmp(st.state, "pause") == 0) return simple_command("pause 0");
    return simple_command("play");          /* stopped: start the queue */
}

bool mpd_next(void)     { return simple_command("next"); }
bool mpd_previous(void) { return simple_command("previous"); }

bool mpd_seek_relative(float seconds)
{
    char cmd[64];
    /* MPD wants an explicit sign for a relative seek */
    snprintf(cmd, sizeof(cmd), "seekcur %+.1f", (double)seconds);
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
        else if(strcmp(key, "elapsed") == 0)  out->elapsed = (float)atof(val);
        else if(strcmp(key, "duration") == 0) out->duration = (float)atof(val);
        else if(strcmp(key, "audio") == 0)    copy_field(out->audio, sizeof(out->audio), val);
        else if(strcmp(key, "file") == 0)     copy_field(out->file, sizeof(out->file), val);
        else if(strcmp(key, "Title") == 0)    copy_field(out->title, sizeof(out->title), val);
        else if(strcmp(key, "Artist") == 0)   copy_field(out->artist, sizeof(out->artist), val);
        else if(strcmp(key, "Album") == 0)    copy_field(out->album, sizeof(out->album), val);
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
