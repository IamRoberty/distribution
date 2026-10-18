/*
 * SimpletonOS UI - Bluetooth for input devices (implementation). See btctl.h.
 *
 * bluetoothctl runs as a child (SIMPLETON_BTCTL names another program for
 * the rig), commands go down its stdin one per line, its stdout is read
 * without blocking and parsed line by line with the colour codes stripped:
 *
 *   [NEW] Device MAC Name            a device seen (scanning) or known
 *   [DEL] Device MAC Name            gone
 *   [CHG] Device MAC Key: Value      Name, Connected, Paired, Trusted, ...
 *   Device MAC (public)              the head of an `info` block, then
 *   \tKey: Value                     its lines (Icon, UUID, Appearance...)
 *   Pairing successful / Failed to pair: ...
 *   Connection successful / Failed to connect: ...
 *   [agent] Passkey: 123456          a keyboard wants this typed
 *
 * The device list is everything BlueZ told us about, kept small (BT_MAX);
 * what the page sees is filtered: input devices with a name, paired first.
 */
#define _GNU_SOURCE                 /* ptsname, grantpt, unlockpt */
#include "btctl.h"
#include "config.h"
#include "strings.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define BT_MAX 24

static bool enabled;
static pid_t child = -1;
static int   to_child = -1, from_child = -1;
static char  line[512];
static int   line_len;
static bt_device_t devs[BT_MAX];
static int   ndevs;
static int   listed[BT_MAX], nlisted;
static bool  changed, scanning;
static char  info_mac[18];             /* the `info` block being read */
static int   status_id;
static char  status_name[64];
static char  passkey[8];
static char  pairing_mac[18];
static bool  trace;                    /* SIMPLETON_BT_TRACE: every line bluetoothctl says, in the log */

/* ------------------------------------------------------------ child */

static void send(const char * cmd)
{
    if(to_child < 0) return;
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s\n", cmd);
    if(write(to_child, buf, (size_t)n) < 0) fprintf(stderr, "simpleton-ui: bluetooth: write failed: %s\n", strerror(errno));
}

static void child_stop(void)
{
    if(to_child >= 0) { send("quit"); close(to_child); if(from_child == to_child) from_child = -1; to_child = -1; }
    if(from_child >= 0) { close(from_child); from_child = -1; }
    if(child > 0) {
        int st;
        for(int i = 0; i < 20 && waitpid(child, &st, WNOHANG) == 0; i++) usleep(50000);
        if(waitpid(child, &st, WNOHANG) == 0) { kill(child, SIGTERM); waitpid(child, &st, 0); }
        child = -1;
    }
    ndevs = nlisted = 0; scanning = false; line_len = 0; info_mac[0] = 0;
    changed = true;
}

/* bluetoothctl runs on a pseudo-terminal (0.27a): with a plain pipe its
 * stdout is block-buffered and its news - a device found, a pairing done -
 * sits in its buffer unseen; on a terminal every line comes out at once.
 * The terminal is "dumb" and wide, so readline redraws nothing and wraps
 * nothing; our command lines come back echoed and are ignored. */
static bool child_start(void)
{
    int mfd = posix_openpt(O_RDWR | O_NOCTTY);
    if(mfd < 0 || grantpt(mfd) < 0 || unlockpt(mfd) < 0) { fprintf(stderr, "simpleton-ui: bluetooth: no pty: %s\n", strerror(errno)); return false; }
    char * sname = ptsname(mfd);
    if(!sname) return false;
    char slave[64];
    snprintf(slave, sizeof(slave), "%s", sname);
    const char * prog = getenv("SIMPLETON_BTCTL");
    if(!prog || !prog[0]) prog = "bluetoothctl";
    pid_t p = fork();
    if(p < 0) return false;
    if(p == 0) {
        setsid();
        int sfd = open(slave, O_RDWR);
        if(sfd < 0) _exit(126);
        ioctl(sfd, TIOCSCTTY, 0);
        struct winsize ws = { .ws_row = 50, .ws_col = 400 };
        ioctl(sfd, TIOCSWINSZ, &ws);
        struct termios t;
        if(tcgetattr(sfd, &t) == 0) { t.c_lflag &= ~(tcflag_t)ECHO; t.c_oflag &= ~(tcflag_t)ONLCR; tcsetattr(sfd, TCSANOW, &t); }
        dup2(sfd, 0); dup2(sfd, 1); dup2(sfd, 2);
        if(sfd > 2) close(sfd);
        close(mfd);
        setenv("TERM", "dumb", 1);
        setenv("COLUMNS", "400", 1);
        execlp(prog, prog, "--agent", "DisplayOnly", (char *)NULL);
        _exit(127);
    }
    to_child = from_child = mfd;
    fcntl(mfd, F_SETFL, fcntl(mfd, F_GETFL) | O_NONBLOCK);
    fcntl(mfd, F_SETFD, FD_CLOEXEC);
    child = p;
    send("power on");
    send("devices");                  /* everything BlueZ knows (paired or cached): [NEW] lines, then their info */
    fprintf(stderr, "simpleton-ui: bluetooth: %s started\n", prog);
    return true;
}

static void service(bool on)
{
    const char * cmd = getenv("SIMPLETON_BT_SERVICE");       /* the rig: a stand-in */
    char buf[160];
    if(cmd && cmd[0]) snprintf(buf, sizeof(buf), "%s %s", cmd, on ? "start" : "stop");
    else snprintf(buf, sizeof(buf), "systemctl %s bluetooth 2>/dev/null", on ? "start" : "stop");
    int rc = system(buf);
    fprintf(stderr, "simpleton-ui: bluetooth: service %s (%d)\n", on ? "started" : "stopped", rc);
}

void bt_init(void)
{
    trace = getenv("SIMPLETON_BT_TRACE") != NULL;    /* every line bluetoothctl says, in the log */
    enabled = config_read_bool("bluetooth", false);
    if(enabled) { service(true); child_start(); }
    else fprintf(stderr, "simpleton-ui: bluetooth: off\n");
}

bool bt_enabled(void) { return enabled; }

void bt_set_enabled(bool on)
{
    if(on == enabled) return;
    enabled = on;
    if(on) { service(true); child_start(); }
    else { child_stop(); service(false); }
}

int bt_fd(void) { return from_child; }

/* ------------------------------------------------------------ devices */

static bt_device_t * find(const char * mac)
{
    for(int i = 0; i < ndevs; i++) if(strcmp(devs[i].mac, mac) == 0) return &devs[i];
    return NULL;
}

/* 17 characters of "E4:17:D8:96:D8:F7" (or with dashes: a device with no
 * name yet shows its address that way). */
static bool mac_at(const char * s)
{
    for(int i = 0; i < 17; i++) {
        if(!s[i]) return false;
        if(i % 3 == 2) { if(s[i] != '-' && s[i] != ':') return false; }
        else if(!isxdigit((unsigned char)s[i])) return false;
    }
    return true;
}
static bool mac_like(const char * s) { return strlen(s) == 17 && mac_at(s); }

static bt_device_t * add(const char * mac, const char * name)
{
    bt_device_t * d = find(mac);
    if(!d) {
        if(ndevs == BT_MAX) return NULL;
        d = &devs[ndevs++];
        memset(d, 0, sizeof(*d));
        snprintf(d->mac, sizeof(d->mac), "%s", mac);
    }
    if(name && name[0] && !mac_like(name)) { snprintf(d->name, sizeof(d->name), "%s", name); d->named = true; }
    else if(!d->named) snprintf(d->name, sizeof(d->name), "%s", mac);
    return d;
}

static void relist(void)
{
    nlisted = 0;
    for(int pass = 0; pass < 2; pass++)
        for(int i = 0; i < ndevs; i++) {
            const bt_device_t * d = &devs[i];
            bool input = d->hid || d->kind == BT_KIND_GAMEPAD || d->kind == BT_KIND_KEYBOARD || d->kind == BT_KIND_REMOTE || d->kind == BT_KIND_POINTER;
            if(!input || !d->named) continue;
            if((pass == 0) != d->paired) continue;
            listed[nlisted++] = i;
        }
    changed = true;
}

int bt_device_count(void) { return nlisted; }
const bt_device_t * bt_device(int i) { return i >= 0 && i < nlisted ? &devs[listed[i]] : NULL; }
const bt_device_t * bt_find(const char * mac) { return mac ? find(mac) : NULL; }

static bool yes(const char * v) { return strncmp(v, "yes", 3) == 0; }

static void set_kind_from_icon(bt_device_t * d, const char * icon)
{
    if(strncmp(icon, "input-gaming", 12) == 0) d->kind = BT_KIND_GAMEPAD;
    else if(strncmp(icon, "input-keyboard", 14) == 0) d->kind = BT_KIND_KEYBOARD;
    else if(strncmp(icon, "input-mouse", 11) == 0 || strncmp(icon, "input-tablet", 12) == 0) d->kind = BT_KIND_POINTER;
    else if(strncmp(icon, "input-", 6) == 0) { if(d->kind == BT_KIND_UNKNOWN) d->kind = BT_KIND_OTHER; }
}

static void set_kind_from_appearance(bt_device_t * d, unsigned a)
{
    unsigned cat = a >> 6;
    if(cat == 0x006) d->kind = BT_KIND_REMOTE;                      /* 0x0180: remote control */
    else if(cat == 0x00f) {                                        /* 0x03c0: HID */
        unsigned sub = a & 0x3f;
        d->kind = sub == 1 ? BT_KIND_KEYBOARD : sub == 2 ? BT_KIND_POINTER : (sub == 3 || sub == 4) ? BT_KIND_GAMEPAD : BT_KIND_OTHER;
        d->hid = true;
    }
}

/* A line of an `info` block or a [CHG] value: "Key: Value". */
static void field(bt_device_t * d, const char * key, const char * val)
{
    if(strcmp(key, "Name") == 0 || strcmp(key, "Alias") == 0) { if(val[0] && !mac_like(val)) { snprintf(d->name, sizeof(d->name), "%s", val); d->named = true; } }
    else if(strcmp(key, "Icon") == 0) set_kind_from_icon(d, val);
    else if(strcmp(key, "Appearance") == 0) set_kind_from_appearance(d, (unsigned)strtoul(val, NULL, 0));
    else if(strcmp(key, "UUID") == 0) {
        if(strstr(val, "00001124-") || strstr(val, "00001812-") || strncmp(val, "Human Interface Device", 22) == 0) { d->hid = true; if(d->kind == BT_KIND_UNKNOWN) d->kind = BT_KIND_OTHER; }
    }
    else if(strcmp(key, "Paired") == 0) d->paired = yes(val);
    else if(strcmp(key, "Trusted") == 0) d->trusted = yes(val);
    else if(strcmp(key, "Connected") == 0) {
        bool was = d->connected;
        d->connected = yes(val);
        if(d->connected && !was && d->paired && !d->pairing) { status_id = S_BT_CONNECTED; snprintf(status_name, sizeof(status_name), "%s", d->name); }
        if(!d->connected && was) { status_id = S_BT_DISCONNECTED; snprintf(status_name, sizeof(status_name), "%s", d->name); }
    }
    else if(strcmp(key, "ServicesResolved") == 0 && yes(val)) { char c[40]; snprintf(c, sizeof(c), "info %s", d->mac); send(c); }
}

/* ------------------------------------------------------------ parsing */

static void strip_ansi(char * s)
{
    char * w = s;
    for(char * r = s; *r; r++) {
        if(*r == 0x1b) { while(*r && !isalpha((unsigned char)*r)) r++; if(!*r) break; continue; }
        if(*r == '\r') continue;
        *w++ = *r;
    }
    *w = 0;
}

static void handle_line(char * s)
{
    strip_ansi(s);
    if(trace) fprintf(stderr, "simpleton-ui: bluetooth: < %s\n", s);
    /* readline on the terminal pads lines with spaces and leaves prompts
     * ("[bluetooth]# ", "[8BitDo Pro 2]> ") in front of what came next:
     * skip the air and cut every prompt */
    char * p = s;
    for(;;) {
        while(*p == ' ') p++;
        if(*p != '[' || strncmp(p, "[NEW]", 5) == 0 || strncmp(p, "[CHG]", 5) == 0 || strncmp(p, "[DEL]", 5) == 0 || strncmp(p, "[agent]", 7) == 0) break;
        char * e = strchr(p, ']');
        if(!e || e - p > 48 || (e[1] != '#' && e[1] != '>')) break;
        p = e + 2;
    }
    if(!*p) return;

    /* an info block's fields: "Key: Value", indented or not (the terminal
     * may have eaten the tab) */
    if(info_mac[0]) {
        const char * q = p; while(*q == '\t' || *q == ' ') q++;
        static const char * const keys[] = { "Name", "Alias", "Icon", "Class", "Appearance", "Paired", "Bonded", "Trusted", "Blocked", "Connected",
                                             "LegacyPairing", "UUID", "Modalias", "ServicesResolved", "RSSI", "TxPower", "WakeAllowed", "Adapter", NULL };
        const char * c = strchr(q, ':');
        bool is_field = false;
        if(c) for(int i = 0; keys[i]; i++) { size_t kl = strlen(keys[i]); if((size_t)(c - q) == kl && strncmp(q, keys[i], kl) == 0) { is_field = true; break; } }
        if(is_field) {
            bt_device_t * d = find(info_mac);
            if(d) {
                char key[32];
                snprintf(key, sizeof(key), "%.*s", (int)(c - q), q);
                const char * v = c + 1; while(*v == ' ') v++;
                field(d, key, v);
                relist();
            }
            return;
        }
    }
    info_mac[0] = 0;

    if(strncmp(p, "Device ", 7) == 0 && strlen(p) >= 25 && mac_at(p + 7) && p[24] == ' ') {
        const char * rest = p + 25; while(*rest == ' ') rest++;
        if(strncmp(rest, "(public)", 8) == 0 || strncmp(rest, "(random)", 8) == 0) {
            /* the head of an `info` block */
            snprintf(info_mac, sizeof(info_mac), "%.17s", p + 7);
            add(info_mac, NULL);
            return;
        }
        /* a line of `devices`: "Device MAC Name" - ask for its info */
        char mac[18]; snprintf(mac, sizeof(mac), "%.17s", p + 7);
        bool knew = find(mac) != NULL;
        if(add(mac, rest) && !knew) { char c[40]; snprintf(c, sizeof(c), "info %s", mac); send(c); }
        relist();
        return;
    }
    if(strncmp(p, "[NEW] Device ", 13) == 0 || strncmp(p, "[CHG] Device ", 13) == 0 || strncmp(p, "[DEL] Device ", 13) == 0) {
        char mac[18]; snprintf(mac, sizeof(mac), "%.17s", p + 13);
        const char * rest = p + 13 + 17; while(*rest == ' ') rest++;
        if(p[1] == 'D') {
            bt_device_t * d = find(mac);
            if(d) { *d = devs[--ndevs]; relist(); }
            return;
        }
        if(p[1] == 'N') {
            bt_device_t * d = add(mac, rest);
            fprintf(stderr, "simpleton-ui: bluetooth: seen %s \"%s\"\n", mac, rest);
            if(d) { char c[40]; snprintf(c, sizeof(c), "info %s", mac); send(c); }
            relist();
            return;
        }
        bool knew = find(mac) != NULL;
        bt_device_t * d = add(mac, NULL);
        if(!d) return;
        if(!knew) { char c[40]; snprintf(c, sizeof(c), "info %s", mac); send(c); }   /* a device BlueZ already had: no [NEW] for it */
        char key[32]; const char * c = strchr(rest, ':');
        if(!c) return;
        snprintf(key, sizeof(key), "%.*s", (int)(c - rest) < 31 ? (int)(c - rest) : 31, rest);
        const char * v = c + 1; while(*v == ' ') v++;
        field(d, key, v);
        relist();
        return;
    }
    if(strncmp(p, "[agent] Passkey:", 16) == 0 || strncmp(p, "Passkey:", 8) == 0) {
        const char * v = strchr(p, ':') + 1; while(*v == ' ') v++;
        snprintf(passkey, sizeof(passkey), "%.6s", v);
        changed = true;
        return;
    }
    if(strncmp(p, "Pairing successful", 18) == 0) {
        bt_device_t * d = find(pairing_mac);
        if(d) {
            d->paired = true; d->pairing = false;
            char c[40];
            snprintf(c, sizeof(c), "trust %s", d->mac); send(c);
            snprintf(c, sizeof(c), "connect %s", d->mac); send(c);
            status_id = S_BT_PAIRED; snprintf(status_name, sizeof(status_name), "%s", d->name);
        }
        relist();
        return;
    }
    if(strncmp(p, "Failed to pair", 14) == 0 || strncmp(p, "Pairing failed", 14) == 0) {
        bt_device_t * d = find(pairing_mac);
        if(d) { d->pairing = false; status_id = S_BT_PAIR_FAILED; snprintf(status_name, sizeof(status_name), "%s", d->name); }
        fprintf(stderr, "simpleton-ui: bluetooth: %s\n", p);
        relist();
        return;
    }
    if(strncmp(p, "Failed to connect", 17) == 0) {
        bt_device_t * d = find(pairing_mac);
        if(d) { status_id = S_BT_CONNECT_FAILED; snprintf(status_name, sizeof(status_name), "%s", d->name); }
        fprintf(stderr, "simpleton-ui: bluetooth: %s\n", p);
        changed = true;
        return;
    }
    if(strncmp(p, "Discovery started", 17) == 0) { scanning = true; changed = true; return; }
    if(strncmp(p, "Discovery stopped", 17) == 0) { scanning = false; changed = true; return; }
    if(strstr(p, "Waiting to connect to bluetoothd") || strncmp(p, "Failed", 6) == 0 || strstr(p, "not available"))
        fprintf(stderr, "simpleton-ui: bluetooth: %s\n", p);
}

void bt_poll(void)
{
    if(from_child < 0) return;
    char buf[1024];
    for(;;) {
        ssize_t n = read(from_child, buf, sizeof(buf));
        if(n < 0) { if(errno == EAGAIN || errno == EINTR) break; }
        if(n <= 0) {
            if(n == 0 || (errno != EAGAIN && errno != EINTR)) {           /* a pty says EIO when the child is gone */
                fprintf(stderr, "simpleton-ui: bluetooth: bluetoothctl went away\n");
                child_stop();
            }
            break;
        }
        for(ssize_t i = 0; i < n; i++) {
            if(buf[i] == '\n') { line[line_len] = 0; handle_line(line); line_len = 0; }
            else if(line_len < (int)sizeof(line) - 1) line[line_len++] = buf[i];
        }
    }
}

/* ------------------------------------------------------------ actions */

void bt_scan(bool on)
{
    if(to_child < 0) return;
    send(on ? "pairable on" : "pairable off");
    send(on ? "scan on" : "scan off");
    scanning = on;
    changed = true;
}
bool bt_scanning(void) { return scanning; }

static void cmd_mac(const char * what, const char * mac)
{
    char c[48];
    snprintf(c, sizeof(c), "%s %s", what, mac);
    send(c);
}

void bt_pair(const char * mac)
{
    bt_device_t * d = find(mac);
    if(!d) return;
    snprintf(pairing_mac, sizeof(pairing_mac), "%s", mac);
    d->pairing = true;
    cmd_mac("pair", mac);
    fprintf(stderr, "simpleton-ui: bluetooth: pairing with \"%s\" (%s)\n", d->name, mac);
    changed = true;
}
void bt_connect(const char * mac)    { snprintf(pairing_mac, sizeof(pairing_mac), "%s", mac); cmd_mac("connect", mac); }
void bt_disconnect(const char * mac) { cmd_mac("disconnect", mac); }
void bt_forget(const char * mac)
{
    bt_device_t * d = find(mac);
    if(d) { status_id = S_BT_FORGOTTEN; snprintf(status_name, sizeof(status_name), "%s", d->name); }
    cmd_mac("remove", mac);
    if(d) { *d = devs[--ndevs]; relist(); }
}

bool bt_changed(void) { bool c = changed; changed = false; return c; }
int bt_take_status(char * name, int len)
{
    int id = status_id;
    if(name && len > 0) snprintf(name, (size_t)len, "%s", status_name);
    status_id = 0;
    return id;
}
void bt_take_passkey(char * out, int len)
{
    snprintf(out, (size_t)len, "%s", passkey);
    passkey[0] = 0;
}
