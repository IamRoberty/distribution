/*
 * SimpletonOS UI - minimal MPD client (implementation). See mpdc.h.
 *
 * Protocol shape: connect, read "OK MPD x.y.z", then for each command write
 * "cmd args\n" and read lines until one is exactly "OK" or starts with "ACK ".
 * Arguments are double-quoted with \ and " escaped.
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

static int    sock = -1;
static FILE * rf   = NULL;      /* buffered reader over sock */

static void disconnect(void)
{
    if(rf) { fclose(rf); rf = NULL; }   /* fclose closes sock too */
    else if(sock >= 0) close(sock);
    sock = -1;
}

bool mpd_is_connected(void) { return sock >= 0; }

static bool read_line(char * buf, size_t len)
{
    if(!rf || !fgets(buf, len, rf)) { disconnect(); return false; }
    size_t n = strlen(buf);
    if(n && buf[n - 1] == '\n') buf[n - 1] = 0;
    return true;
}

bool mpd_connect(void)
{
    if(sock >= 0) return true;

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

    sock = fd;
    rf = fdopen(fd, "r");
    if(!rf) { close(fd); sock = -1; return false; }

    char line[256];
    if(!read_line(line, sizeof(line)) || strncmp(line, "OK MPD", 6) != 0) { disconnect(); return false; }
    fprintf(stderr, "simpleton-ui: mpd: connected (%s)\n", line);
    return true;
}

static bool send_raw(const char * s)
{
    size_t len = strlen(s), off = 0;
    while(off < len) {
        ssize_t n = write(sock, s + off, len - off);
        if(n <= 0) { disconnect(); return false; }
        off += (size_t)n;
    }
    return true;
}

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

/* Read the response tail: returns true on "OK", false on "ACK ..." or drop. */
static bool read_ok(void)
{
    char line[1024];
    for(;;) {
        if(!read_line(line, sizeof(line))) return false;
        if(strcmp(line, "OK") == 0) return true;
        if(strncmp(line, "ACK ", 4) == 0) { fprintf(stderr, "simpleton-ui: mpd: %s\n", line); return false; }
        /* any other line is payload we didn't ask to keep */
    }
}

static bool simple_command(const char * cmd)
{
    if(!mpd_connect()) return false;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s\n", cmd);
    return send_raw(buf) && read_ok();
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
    if(!send_raw(cmd)) return false;

    char line[2048];
    char title[256] = "", track[16] = "";
    mpd_entry_t * cur = NULL;

    for(;;) {
        if(!read_line(line, sizeof(line))) { mpd_listing_free(out); return false; }
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
    if(!send_raw("command_list_begin\nclear\n")) return false;
    for(int i = 0; i < count; i++) {
        char cmd[2048] = "add";
        append_quoted(cmd, sizeof(cmd), uris[i]);
        strncat(cmd, "\n", sizeof(cmd) - strlen(cmd) - 1);
        if(!send_raw(cmd)) return false;
    }
    char play[64];
    snprintf(play, sizeof(play), "play %d\ncommand_list_end\n", start_index);
    return send_raw(play) && read_ok();
}

bool mpd_toggle_pause(void)
{
    if(!mpd_connect()) return false;
    if(!send_raw("status\n")) return false;
    char line[256], state[16] = "";
    for(;;) {
        if(!read_line(line, sizeof(line))) return false;
        if(strcmp(line, "OK") == 0) break;
        if(strncmp(line, "ACK ", 4) == 0) return false;
        if(strncmp(line, "state: ", 7) == 0) snprintf(state, sizeof(state), "%s", line + 7);
    }
    if(strcmp(state, "play") == 0) return simple_command("pause 1");
    if(strcmp(state, "pause") == 0) return simple_command("pause 0");
    return simple_command("play");          /* stopped: start the queue */
}

bool mpd_next(void)     { return simple_command("next"); }
bool mpd_previous(void) { return simple_command("previous"); }
