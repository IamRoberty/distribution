/*
 * SimpletonOS UI skeleton - bring-up test only.
 *
 * Goal: prove the full chain (cross-compile -> image -> boot -> visible
 * pixels on the real panel) works, before any real screen logic is written
 * on top of it. Draws a solid background and a line of text. Nothing else -
 * no MPD connection, no input handling yet.
 *
 * DRM setup sequence below is copied from the verified v9.5.0 API contract
 * (src/drivers/display/drm/lv_linux_drm.h and .c):
 *   - lv_linux_drm_set_file() does NOT auto-detect a device from NULL; it
 *     open()s whatever path you hand it. lv_linux_drm_find_device_path()
 *     is the actual auto-detect call and must be used explicitly.
 *   - lv_linux_drm_create() registers its own tick source internally
 *     (calls lv_tick_set_cb() itself), so no manual tick setup is needed
 *     for this driver.
 *   - connector_id -1 auto-selects the first available connector and its
 *     preferred/native mode - fine for a single fixed panel or a single
 *     HDMI output, which is all this bring-up test needs.
 */

#include "lvgl.h"
#include "src/drivers/display/drm/lv_linux_drm.h"

#include <stdio.h>
#include <unistd.h>

int main(void)
{
    lv_init();

    lv_display_t * disp = lv_linux_drm_create();
    if(disp == NULL) {
        fprintf(stderr, "simpleton-ui: lv_linux_drm_create() failed\n");
        return 1;
    }

    char * dev_path = lv_linux_drm_find_device_path();
    if(dev_path == NULL) {
        fprintf(stderr, "simpleton-ui: no DRM device found (no /dev/dri/cardN with dumb-buffer support)\n");
        return 1;
    }

    lv_result_t res = lv_linux_drm_set_file(disp, dev_path, -1);
    lv_free(dev_path);

    if(res != LV_RESULT_OK) {
        fprintf(stderr, "simpleton-ui: lv_linux_drm_set_file() failed - check LV_LOG output above\n");
        return 1;
    }

    /* Solid background */
    lv_obj_t * scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* One line of centered text, proving font rendering + the draw path work */
    lv_obj_t * label = lv_label_create(scr);
    lv_label_set_text(label, "SimpletonOS");
    lv_obj_set_style_text_color(label, lv_color_hex(0xE8E8E8), LV_PART_MAIN);
    lv_obj_center(label);

    while(1) {
        uint32_t idle_ms = lv_timer_handler();
        usleep(idle_ms * 1000);
    }

    return 0;
}
