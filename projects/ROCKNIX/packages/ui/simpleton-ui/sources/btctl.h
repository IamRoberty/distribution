/*
 * SimpletonOS UI - Bluetooth for controllers, remotes and keyboards (0.27,
 * 18 Oct 2026; bluetooth-input-plan.md). No audio, no phone (Ian).
 *
 * A thin layer over `bluetoothctl`, kept running as a child with pipes,
 * the way mpdc.c wraps MPD: screens never talk to BlueZ. The radio is on
 * only while the "bluetooth" setting is on (the service is started then,
 * stopped otherwise - off means no draw). While the pairing page is open
 * the unit scans and is pairable; devices that offer input (HID over
 * classic or LE, or whose icon / appearance says pad, keyboard, remote,
 * pointer) are listed - a DAC's own Bluetooth or a TV never is.
 *
 * Pairing: `pair`, then `trust` (so the device reconnects by itself) and
 * `connect`. The agent is DisplayOnly: a pad just works; a keyboard that
 * asks for a passkey gets one shown by the page (bt_take_passkey) - type
 * it on the keyboard and press Enter. Once connected, the device appears
 * in /dev/input and input.c picks it up like any other (0.26).
 */
#ifndef SIMPLETON_BTCTL_H
#define SIMPLETON_BTCTL_H

#include <stdbool.h>

typedef enum { BT_KIND_UNKNOWN, BT_KIND_GAMEPAD, BT_KIND_KEYBOARD, BT_KIND_REMOTE, BT_KIND_POINTER, BT_KIND_OTHER } bt_kind_t;

typedef struct {
    char      mac[18];
    char      name[64];
    bt_kind_t kind;
    bool      hid;          /* offers an input service */
    bool      paired, trusted, connected;
    bool      pairing;      /* our pair command is in flight */
    bool      named;        /* has a real name (not its address) */
} bt_device_t;

/* Read the setting; start the service and the child when it is on. */
void bt_init(void);
bool bt_enabled(void);
void bt_set_enabled(bool on);      /* Settings: starts / stops the service, keeps the setting */

/* Each main-loop turn: read the child's output. bt_fd() for poll(). */
void bt_poll(void);
int  bt_fd(void);

/* The pairing page: scan + pairable while open. */
void bt_scan(bool on);
bool bt_scanning(void);

/* The devices worth listing (input devices, named), paired ones first. */
int  bt_device_count(void);
const bt_device_t * bt_device(int i);
const bt_device_t * bt_find(const char * mac);

void bt_pair(const char * mac);        /* pair, trust, connect */
void bt_connect(const char * mac);
void bt_disconnect(const char * mac);
void bt_forget(const char * mac);      /* remove: unpaired and gone from the list */

/* Something changed since the last call (the list, a state, a message). */
bool bt_changed(void);
/* A line for the person, once: "Pairing failed", "Connected"... (an id of
 * strings.def), 0 when nothing new. `name` gets the device's name. */
int  bt_take_status(char * name, int len);
/* A passkey a keyboard asked for, once: 6 digits, "" when none. */
void bt_take_passkey(char * out, int len);

#endif
