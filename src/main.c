#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <arm_neon.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <png.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/camera.h>
#include <psp2/power.h>
#include <psp2/appmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/sysmodule.h>
#include <psp2/jpegenc.h>
#include <psp2/jpegencarm.h>
#include <psp2/rtc.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/io/devctl.h>
#include <psp2/appmgr.h>
#include <psp2/audioin.h>
#include <sys/statvfs.h>
#include <vita2d.h>
#include "webserver.h"
#include "qrcodegen.h"
#include "font8x8.h"
#include "psvita_logo.h"

static float get_ux0_free_gb() {
    uint64_t max_size = 0, free_size = 0;
    int ret = sceAppMgrGetDevInfo("ux0:", &max_size, &free_size);
    if (ret >= 0 && max_size > 0) {
        return (float)((double)free_size / (1024.0 * 1024.0 * 1024.0));
    }
    struct statvfs vfs;
    memset(&vfs, 0, sizeof(vfs));
    if (statvfs("ux0:", &vfs) == 0 && vfs.f_bsize > 0) {
        uint64_t free_bytes = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_bsize;
        if (free_bytes > 0) {
            return (float)((double)free_bytes / (1024.0 * 1024.0 * 1024.0));
        }
    }
    return 0.0f;
}

#define CAM_WIDTH 640
#define CAM_HEIGHT 480
#define FRAME_YUV420_SIZE (CAM_WIDTH * CAM_HEIGHT * 3 / 2)
#define JPEG_OUT_BUF_SIZE (1024 * 1024)

static volatile int cam_thread_run = 1;
static SceUID cam_thid = -1;
static int cam_dev = SCE_CAMERA_DEVICE_BACK;
static SceCameraInfo info;
static vita2d_texture *cam_tex[2] = { NULL, NULL };
static volatile int cam_tex_front = 0;
static volatile int cam_tex_back = 1;

static void *cam_buf = NULL;
static void *snap_buf = NULL;
static void *jpeg_out_buf = NULL;
static volatile int capture_requested = 0;
static volatile int capture_ready = 0;

static char status_msg[128] = "";
static unsigned int status_msg_color = 0;
static int status_msg_timer = 0;

typedef enum {
    FRONT_FLASH_OFF = 0,
    FRONT_FLASH_SCREEN = 1,
    FRONT_FLASH_BORDER = 2,
    FRONT_FLASH_COUNT = 3
} FrontFlashMode;

static FrontFlashMode front_flash_mode = FRONT_FLASH_OFF;
static int flash_trigger_anim = 0;
static int shutter_pressed_anim = 0;
static int watermark_enabled = 1;

static void trigger_camera_shot(void) {
    shutter_pressed_anim = 8;
    if (cam_dev == SCE_CAMERA_DEVICE_FRONT && front_flash_mode != FRONT_FLASH_OFF) {
        flash_trigger_anim = 16;
    } else {
        capture_requested = 1;
    }
}

// ── Estado de Grabación de Video & Audio (Micrófono) ──────────────────────
static volatile int is_recording_video = 0;
static uint64_t video_start_time = 0;
static int video_frame_count = 0;
static SceUID video_fd = -1;
static uint32_t video_movi_size = 0;
static void *video_enc_buf = NULL;
static void *video_raw_buf = NULL;
static void *video_arm_ctx = NULL;
static char current_video_path[256] = "";

#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_GRAIN 512
#define AUDIO_BUF_SIZE (AUDIO_GRAIN * sizeof(int16_t))
static int audio_port = -1;
static int16_t audio_buf[AUDIO_GRAIN];
static uint32_t video_audio_bytes = 0;
static uint32_t video_audio_samples = 0;

static void video_record_start(void);
static void video_record_stop(void);

// =========================================================================
// Estado de la Aplicación y Galería Continua Estilo Smartphone
// =========================================================================
typedef enum {
    APP_MODE_CAMERA = 0,
    APP_MODE_GALLERY = 1
} AppMode;

typedef enum {
    GALLERY_VIEW_GRID = 0,
    GALLERY_VIEW_FULLSCREEN = 1
} GalleryViewMode;

typedef enum {
    GALLERY_SOURCE_VITACAM = 0,
    GALLERY_SOURCE_PHOTO = 1,
    GALLERY_SOURCE_SCREENSHOT = 2,
    GALLERY_SOURCE_ALL = 3,
    GALLERY_SOURCE_COUNT = 4
} GallerySourceTab;

static volatile AppMode app_mode = APP_MODE_CAMERA;
static GalleryViewMode gallery_view = GALLERY_VIEW_GRID;
static GallerySourceTab gallery_source_tab = GALLERY_SOURCE_VITACAM;
static int gallery_show_wifi_modal = 0;
static int gallery_show_info_modal = 0;
static int wifi_dialog_focused = 1;

#define MAX_GALLERY_PHOTOS 256
#define GRID_COLS 3
#define THUMB_POOL_SIZE 27
#define THUMB_WIDTH 240
#define THUMB_HEIGHT 180

// 3x3 compact grid: 3 cols, 4:3 aspect ratio photos (280x210)
// Screen 960x544, header=48px, bottom_bar=42px
// 3 * 280 + 2 * 20 = 880px. Margins 40px left and 40px right.
#define CARD_W 280
#define CARD_H 210
#define GAP_X  20
#define GAP_Y  20
#define GRID_START_X 40
#define GRID_START_Y 56
#define HEADER_H 48
#define BOTTOM_BAR_H 42

typedef struct {
    char filename[64];
    char fullpath[256];
    SceOff size;
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    uint64_t timestamp_key;
    char date_label[32];
    char time_label[16];
    int is_video; // 0 = JPG, 1 = AVI
} GalleryPhoto;

// ── Estado de Reproducción de Video en Galería ────────────────────────────
static int is_video_playing = 0;
static int video_play_frame = 0;
static int video_play_total_frames = 0;
static int video_play_fps = 30;
static uint64_t video_play_last_time = 0;
static SceUID video_play_fd = -1;
static uint32_t video_play_cur_offset = 2048;
static void video_playback_stop(void);
static void video_playback_start(void);

typedef struct {
    char date_title[32];
    int start_photo_idx;
    int count;
    float y_pos;
    float height;
} DateGroup;

#define MAX_DATE_GROUPS 64
static DateGroup date_groups[MAX_DATE_GROUPS];
static int date_group_count = 0;

typedef struct {
    int photo_idx;
    uint32_t last_used_frame;
    uint8_t is_loaded;
} ThumbSlot;

static GalleryPhoto gallery_photos[MAX_GALLERY_PHOTOS];
static uint8_t gallery_selected[MAX_GALLERY_PHOTOS];
static int gallery_count = 0;
static int gallery_idx = 0;

static int gallery_selection_mode = 0; // 0 = Normal, 1 = Modo Seleccion

static float gallery_scroll_y = 0.0f;
static float gallery_target_scroll_y = 0.0f;

// Variables de Modo Cámara Pro (Samsung Expert Camera Style)
static int show_grid = 1;
static int active_cam_param = 0;
static int cam_slider_open = 1;
static int slider_focused = 0; // 0 = Navegación barra inferior, 1 = Ajuste de slider/selector enfocado
static float cam_slider_drag_accum = 0.0f;

#define PRO_BAR_COUNT 6
static const int pro_bar_params[PRO_BAR_COUNT] = { 1, 2, 0, 3, 7, 4 }; // ISO, SPEED, EV, WB, EFECTO, ZOOM
static const char *pro_bar_labels[PRO_BAR_COUNT] = { "ISO", "SPEED", "EV", "WB", "EFECTO", "ZOOM" };

static vita2d_texture *cam_last_thumb_tex = NULL;

static vita2d_texture *gallery_tex = NULL;
static vita2d_texture *thumb_tex[THUMB_POOL_SIZE] = { NULL };
static ThumbSlot thumb_slots[THUMB_POOL_SIZE];
static uint32_t current_render_frame = 0;

// Variables para vista a pantalla completa
static int fullscreen_hide_ui = 0;
static float fullscreen_zoom = 1.0f;
static float fullscreen_pan_x = 0.0f;
static float fullscreen_pan_y = 0.0f;
static float fullscreen_aspect_ratio = 4.0f / 3.0f; // Aspect ratio real de la imagen

static int gallery_confirm_delete = 0;
static int delete_dialog_focused = 1; // 0 = Eliminar (Sí), 1 = Cancelar (No, por defecto)

// ── Icon button textures (loaded from app0:sce_sys/icons/) ──────────────────
typedef enum {
    ICON_NONE = -1,
    ICON_CROSS = 0,
    ICON_CIRCLE,
    ICON_TRIANGLE,
    ICON_TRASH,
    ICON_BACK,
    ICON_CHECK,
    ICON_SELECT,
    ICON_CAMERA,
    ICON_LEFT,
    ICON_RIGHT,
    ICON_CAM_FLIP,
    ICON_CAM_FLASH_OFF,
    ICON_CAM_FLASH_SCREEN,
    ICON_CAM_FLASH_RING,
    ICON_CAM_GRID,
    ICON_CAM_WM,
    ICON_COUNT
} IconId;

static vita2d_texture *icon_tex[ICON_COUNT] = { NULL };
static const char *icon_paths[ICON_COUNT] = {
    "app0:sce_sys/icons/btn_cross.png",
    "app0:sce_sys/icons/btn_circle.png",
    "app0:sce_sys/icons/btn_triangle.png",
    "app0:sce_sys/icons/btn_trash.png",
    "app0:sce_sys/icons/btn_back.png",
    "app0:sce_sys/icons/btn_check.png",
    "app0:sce_sys/icons/btn_select.png",
    "app0:sce_sys/icons/btn_camera.png",
    "app0:sce_sys/icons/btn_left.png",
    "app0:sce_sys/icons/btn_right.png",
    "app0:sce_sys/icons/cam_flip.png",
    "app0:sce_sys/icons/cam_flash_off.png",
    "app0:sce_sys/icons/cam_flash_screen.png",
    "app0:sce_sys/icons/cam_flash_ring.png",
    "app0:sce_sys/icons/cam_grid.png",
    "app0:sce_sys/icons/cam_wm.png",
};

// Variables para detección precisa de toques físicos y táctiles
static int x_hold_frames = 0;
static int x_long_fired = 0;
static int modal_just_closed = 0;

static int touch_active = 0;
static int touch_start_x = 0;
static int touch_start_y = 0;
static int touch_prev_x = 0;
static int touch_prev_y = 0;
static int touch_hold_frames = 0;
static int touch_is_dragging = 0;
static int touch_long_fired = 0;
static int touch_item_down = -1;
static float touch_prev_pinch_dist = 0.0f;
static uint64_t touch_last_tap_time = 0;

// ── Draw Helpers ────────────────────────────────────────────────────────────
// Draw an icon + label in a perfectly centered pill button / badge.
static void draw_centered_pill_button(vita2d_pgf *pgf,
                                     float x, float y, float w, float h,
                                     IconId icon_id,
                                     const char *label,
                                     float font_scale,
                                     unsigned int label_color,
                                     unsigned int bg_color,
                                     unsigned int border_color) {
    if (bg_color != 0) {
        vita2d_draw_rectangle(x, y, w, h, bg_color);
    }
    if (border_color != 0) {
        vita2d_draw_rectangle(x, y, w, 1.0f, border_color);
        vita2d_draw_rectangle(x, y + h - 1.0f, w, 1.0f, border_color);
        vita2d_draw_rectangle(x, y, 1.0f, h, border_color);
        vita2d_draw_rectangle(x + w - 1.0f, y, 1.0f, h, border_color);
    }

    float tw = (pgf && label && label[0]) ? (float)vita2d_pgf_text_width(pgf, font_scale, label) : 0.0f;
    float icon_draw_w = 0.0f;
    float icon_scale = 1.0f;
    if (icon_id >= 0 && icon_id < ICON_COUNT && icon_tex[icon_id]) {
        float tex_h = (float)vita2d_texture_get_height(icon_tex[icon_id]);
        float tex_w = (float)vita2d_texture_get_width(icon_tex[icon_id]);
        float target_h = (h > 36.0f) ? 24.0f : 18.0f;
        icon_scale = target_h / tex_h;
        icon_draw_w = tex_w * icon_scale;
    }

    float spacing = (icon_draw_w > 0.0f && tw > 0.0f) ? 6.0f : 0.0f;
    float total_w = icon_draw_w + spacing + tw;
    float start_x = x + (w - total_w) * 0.5f;

    if (icon_draw_w > 0.0f) {
        float target_h = (h > 36.0f) ? 24.0f : 18.0f;
        float icon_y = y + (h - target_h) * 0.5f;
        vita2d_draw_texture_scale(icon_tex[icon_id], start_x, icon_y, icon_scale, icon_scale);
        start_x += icon_draw_w + spacing;
    }

    if (pgf && label && label[0]) {
        float text_y = y + h * 0.5f + (font_scale * 7.5f);
        vita2d_pgf_draw_text(pgf, (int)start_x, (int)text_y, label_color, font_scale, label);
    }
}

static void draw_icon_label(vita2d_pgf *pgf,
                            float x, float y, float h,
                            IconId icon_id,
                            const char *label,
                            unsigned int label_color) {
    float ix = x;
    float iy = y + (h - 22) * 0.5f;
    if (icon_id >= 0 && icon_id < ICON_COUNT && icon_tex[icon_id]) {
        float tex_w = (float)vita2d_texture_get_width(icon_tex[icon_id]);
        float tex_h = (float)vita2d_texture_get_height(icon_tex[icon_id]);
        float scale = 22.0f / tex_h;
        vita2d_draw_texture_scale(icon_tex[icon_id], ix, iy, scale, scale);
        ix += tex_w * scale + 4.0f;
    }
    if (label && label[0]) {
        vita2d_pgf_draw_text(pgf, (int)ix, (int)(y + h * 0.5f + 5.0f), label_color, 0.70f, label);
    }
}

static uint8_t cached_qrcode[qrcodegen_BUFFER_LEN_MAX];
static char qr_code_last_url[128] = "";
static int cached_qr_size = 0;

static void draw_qr_modal(vita2d_pgf *pgf) {
    vita2d_draw_rectangle(0.0f, 0.0f, 960.0f, 544.0f, RGBA8(5, 7, 14, 225));

    float mw = 720.0f, mh = 430.0f;
    float mx = (960.0f - mw) * 0.5f;
    float my = (544.0f - mh) * 0.5f;

    // Caja modal glassmorphic
    vita2d_draw_rectangle(mx, my, mw, mh, RGBA8(14, 20, 44, 250));
    vita2d_draw_rectangle(mx, my, mw, 2.0f, RGBA8(0, 160, 255, 230));
    vita2d_draw_rectangle(mx, my, 1.0f, mh, RGBA8(35, 55, 115, 180));
    vita2d_draw_rectangle(mx + mw - 1, my, 1.0f, mh, RGBA8(35, 55, 115, 180));
    vita2d_draw_rectangle(mx, my + mh - 1, mw, 1.0f, RGBA8(35, 55, 115, 180));

    const char *ip_str = webserver_get_ip();
    int is_online = (strcmp(ip_str, "127.0.0.1") != 0 && ip_str[0] != '\0');
    int is_active = webserver_is_active();
    const char *pwd = webserver_get_password();

    vita2d_pgf_draw_text(pgf, (int)mx + 28, (int)my + 38, RGBA8(240, 245, 255, 255), 0.92f, "VitaCam Web Hub • Wi-Fi");

    // Badge de estado en vivo
    if (is_active && is_online) {
        vita2d_draw_rectangle(mx + mw - 200.0f, my + 22.0f, 172.0f, 26.0f, RGBA8(0, 200, 83, 35));
        vita2d_draw_rectangle(mx + mw - 200.0f, my + 22.0f, 172.0f, 1.0f, RGBA8(0, 200, 83, 140));
        vita2d_draw_rectangle(mx + mw - 200.0f, my + 47.0f, 172.0f, 1.0f, RGBA8(0, 200, 83, 140));
        vita2d_draw_rectangle(mx + mw - 200.0f, my + 22.0f, 1.0f, 26.0f, RGBA8(0, 200, 83, 140));
        vita2d_draw_rectangle(mx + mw - 29.0f, my + 22.0f, 1.0f, 26.0f, RGBA8(0, 200, 83, 140));
        vita2d_draw_rectangle(mx + mw - 188.0f, my + 31.0f, 8.0f, 8.0f, RGBA8(0, 230, 118, 255));
        vita2d_pgf_draw_text(pgf, (int)(mx + mw - 172.0f), (int)(my + 40.0f), RGBA8(0, 230, 118, 255), 0.65f, "SERVIDOR ACTIVO");
    } else {
        vita2d_draw_rectangle(mx + mw - 200.0f, my + 22.0f, 172.0f, 26.0f, RGBA8(255, 60, 60, 35));
        vita2d_draw_rectangle(mx + mw - 200.0f, my + 22.0f, 172.0f, 1.0f, RGBA8(255, 60, 60, 140));
        vita2d_draw_rectangle(mx + mw - 200.0f, my + 47.0f, 172.0f, 1.0f, RGBA8(255, 60, 60, 140));
        vita2d_draw_rectangle(mx + mw - 200.0f, my + 22.0f, 1.0f, 26.0f, RGBA8(255, 60, 60, 140));
        vita2d_draw_rectangle(mx + mw - 29.0f, my + 22.0f, 1.0f, 26.0f, RGBA8(255, 60, 60, 140));
        vita2d_draw_rectangle(mx + mw - 188.0f, my + 31.0f, 8.0f, 8.0f, RGBA8(255, 80, 80, 255));
        vita2d_pgf_draw_text(pgf, (int)(mx + mw - 172.0f), (int)(my + 40.0f), RGBA8(255, 100, 100, 255), 0.65f, "SERVIDOR APAGADO");
    }

    char clean_url_str[128];
    char qr_url_str[128];
    if (is_online) {
        snprintf(clean_url_str, sizeof(clean_url_str), "http://%s:8080", ip_str);
        if (pwd[0] != '\0') {
            snprintf(qr_url_str, sizeof(qr_url_str), "http://%s:8080/?pin=%s", ip_str, pwd);
        } else {
            snprintf(qr_url_str, sizeof(qr_url_str), "http://%s:8080", ip_str);
        }
    } else {
        snprintf(clean_url_str, sizeof(clean_url_str), "Sin conexion Wi-Fi");
        snprintf(qr_url_str, sizeof(qr_url_str), "Sin conexion Wi-Fi");
    }

    float qr_card_x = mx + 28.0f;
    float qr_card_y = my + 60.0f;
    float qr_card_size = 280.0f;

    // Tarjeta blanca con margen generoso
    vita2d_draw_rectangle(qr_card_x, qr_card_y, qr_card_size, qr_card_size, RGBA8(255, 255, 255, 255));

    if (is_online && is_active) {
        if (cached_qr_size == 0 || strcmp(qr_code_last_url, qr_url_str) != 0) {
            uint8_t tempBuffer[qrcodegen_BUFFER_LEN_MAX];
            bool ok = qrcodegen_encodeText(qr_url_str, tempBuffer, cached_qrcode, qrcodegen_Ecc_LOW,
                                           qrcodegen_VERSION_MIN, 10, qrcodegen_Mask_AUTO, true);
            if (ok) {
                cached_qr_size = qrcodegen_getSize(cached_qrcode);
                strncpy(qr_code_last_url, qr_url_str, sizeof(qr_code_last_url) - 1);
            }
        }

        if (cached_qr_size > 0) {
            int quiet_zone = 4;
            int total_modules = cached_qr_size + quiet_zone * 2;
            
            // Calculamos el factor de escala entero máximo que cabe en la tarjeta
            int scale = (int)qr_card_size / total_modules;
            if (scale < 1) scale = 1;
            
            int draw_size = total_modules * scale;
            int margin = ((int)qr_card_size - draw_size) / 2;
            
            int base_x = (int)qr_card_x + margin;
            int base_y = (int)qr_card_y + margin;

            for (int qy = 0; qy < cached_qr_size; qy++) {
                for (int qx = 0; qx < cached_qr_size; qx++) {
                    if (qrcodegen_getModule(cached_qrcode, qx, qy)) {
                        int rx = base_x + (quiet_zone + qx) * scale;
                        int ry = base_y + (quiet_zone + qy) * scale;
                        vita2d_draw_rectangle((float)rx, (float)ry, (float)scale, (float)scale, RGBA8(0, 0, 0, 255));
                    }
                }
            }
        }
    } else {
        if (!is_online) {
            vita2d_draw_rectangle(qr_card_x + 20, qr_card_y + 110, qr_card_size - 40, 60, RGBA8(240, 240, 240, 255));
            vita2d_pgf_draw_text(pgf, (int)qr_card_x + 35, (int)qr_card_y + 145, RGBA8(200, 30, 30, 255), 0.72f, "Wi-Fi no disponible");
        } else {
            vita2d_draw_rectangle(qr_card_x + 20, qr_card_y + 110, qr_card_size - 40, 60, RGBA8(240, 240, 240, 255));
            vita2d_pgf_draw_text(pgf, (int)qr_card_x + 30, (int)qr_card_y + 145, RGBA8(180, 100, 0, 255), 0.70f, "Pulsa Activar Servidor");
        }
    }

    // Caja de PIN ubicada directamente DEBAJO del código QR
    float pin_box_x = qr_card_x;
    float pin_box_y = qr_card_y + qr_card_size + 10.0f;
    float pin_box_w = qr_card_size;
    float pin_box_h = 52.0f;

    vita2d_draw_rectangle(pin_box_x, pin_box_y, pin_box_w, pin_box_h, RGBA8(8, 14, 32, 240));
    vita2d_draw_rectangle(pin_box_x, pin_box_y, pin_box_w, 1.0f, RGBA8(0, 160, 255, 140));
    vita2d_draw_rectangle(pin_box_x, pin_box_y + pin_box_h - 1.0f, pin_box_w, 1.0f, RGBA8(0, 160, 255, 140));
    vita2d_draw_rectangle(pin_box_x, pin_box_y, 1.0f, pin_box_h, RGBA8(0, 160, 255, 140));
    vita2d_draw_rectangle(pin_box_x + pin_box_w - 1.0f, pin_box_y, 1.0f, pin_box_h, RGBA8(0, 160, 255, 140));

    vita2d_pgf_draw_text(pgf, (int)pin_box_x + 14, (int)pin_box_y + 18, RGBA8(160, 195, 240, 220), 0.58f, "PIN DE ACCESO PRIVADO:");

    char pwd_val_str[64];
    if (pwd[0] != '\0') {
        snprintf(pwd_val_str, sizeof(pwd_val_str), "PIN: %s", pwd);
        vita2d_pgf_draw_text(pgf, (int)pin_box_x + 14, (int)pin_box_y + 42, RGBA8(255, 215, 0, 255), 0.88f, pwd_val_str);
    } else {
        vita2d_pgf_draw_text(pgf, (int)pin_box_x + 14, (int)pin_box_y + 42, RGBA8(0, 230, 118, 255), 0.70f, "Pública (Sin PIN)");
    }

    // Panel Lateral de Información
    float info_x = mx + 328.0f;
    float info_y = my + 60.0f;

    vita2d_pgf_draw_text(pgf, (int)info_x, (int)info_y + 8, RGBA8(170, 190, 230, 220), 0.72f, "Dirección Web en tu red Wi-Fi:");
    
    // URL Bar
    vita2d_draw_rectangle(info_x, info_y + 18.0f, 364.0f, 42.0f, RGBA8(8, 12, 28, 230));
    vita2d_draw_rectangle(info_x, info_y + 18.0f, 364.0f, 1.0f, RGBA8(0, 160, 255, 200));
    vita2d_draw_rectangle(info_x, info_y + 59.0f, 364.0f, 1.0f, RGBA8(0, 160, 255, 200));
    vita2d_draw_rectangle(info_x, info_y + 18.0f, 1.0f, 42.0f, RGBA8(0, 160, 255, 200));
    vita2d_draw_rectangle(info_x + 363.0f, info_y + 18.0f, 1.0f, 42.0f, RGBA8(0, 160, 255, 200));

    vita2d_pgf_draw_text(pgf, (int)info_x + 14, (int)info_y + 46, RGBA8(0, 220, 255, 255), 0.84f, clean_url_str);

    vita2d_pgf_draw_text(pgf, (int)info_x, (int)info_y + 84, RGBA8(240, 245, 255, 240), 0.72f, "Instrucciones de conexión:");
    vita2d_pgf_draw_text(pgf, (int)info_x, (int)info_y + 110, RGBA8(160, 180, 215, 220), 0.65f, "• Escanea el QR para entrar directo sin escribir clave.");
    vita2d_pgf_draw_text(pgf, (int)info_x, (int)info_y + 134, RGBA8(160, 180, 215, 220), 0.65f, "• O entra a la URL e ingresa el PIN mostrado abajo.");
    vita2d_pgf_draw_text(pgf, (int)info_x, (int)info_y + 158, RGBA8(160, 180, 215, 220), 0.65f, "• Pulsa 'Nuevo PIN' para cambiar clave y revocar accesos.");
    vita2d_pgf_draw_text(pgf, (int)info_x, (int)info_y + 182, RGBA8(160, 180, 215, 220), 0.65f, "• El servidor continúa activo en segundo plano.");

    // Botones de acción navegables con D-Pad, Joystick y Táctil
    float btn_y = my + mh - 56.0f;
    float btn_w = 118.0f;
    float btn_h = 38.0f;
    float btn0_x = info_x;
    float btn1_x = info_x + 123.0f;
    float btn2_x = info_x + 246.0f;

    // Botón 0: Encender / Apagar Servidor
    const char *toggle_lbl = webserver_is_enabled() ? "Apagar" : "Activar";
    if (wifi_dialog_focused == 0) {
        draw_centered_pill_button(pgf, btn0_x, btn_y, btn_w, btn_h, ICON_CROSS,
                                 toggle_lbl, 0.70f, RGBA8(255, 255, 255, 255),
                                 webserver_is_enabled() ? RGBA8(190, 40, 40, 240) : RGBA8(0, 140, 255, 240),
                                 RGBA8(255, 255, 255, 240));
    } else {
        draw_centered_pill_button(pgf, btn0_x, btn_y, btn_w, btn_h, ICON_NONE,
                                 toggle_lbl, 0.70f, RGBA8(180, 195, 225, 220),
                                 RGBA8(20, 28, 54, 200), RGBA8(40, 60, 110, 160));
    }

    // Botón 1: Nuevo PIN
    if (wifi_dialog_focused == 1) {
        draw_centered_pill_button(pgf, btn1_x, btn_y, btn_w, btn_h, ICON_CROSS,
                                 "Nuevo PIN", 0.68f, RGBA8(255, 255, 255, 255),
                                 RGBA8(210, 140, 0, 245), RGBA8(255, 255, 255, 240));
    } else {
        draw_centered_pill_button(pgf, btn1_x, btn_y, btn_w, btn_h, ICON_NONE,
                                 "Nuevo PIN", 0.68f, RGBA8(180, 195, 225, 220),
                                 RGBA8(20, 28, 54, 200), RGBA8(40, 60, 110, 160));
    }

    // Botón 2: Cerrar
    if (wifi_dialog_focused == 2) {
        draw_centered_pill_button(pgf, btn2_x, btn_y, btn_w, btn_h, ICON_CROSS,
                                 "Cerrar", 0.70f, RGBA8(255, 255, 255, 255),
                                 RGBA8(40, 65, 125, 250), RGBA8(255, 255, 255, 240));
    } else {
        draw_centered_pill_button(pgf, btn2_x, btn_y, btn_w, btn_h, ICON_NONE,
                                 "Cerrar", 0.70f, RGBA8(180, 195, 225, 220),
                                 RGBA8(20, 28, 54, 200), RGBA8(40, 60, 110, 160));
    }
}

static void draw_info_modal(vita2d_pgf *pgf) {
    vita2d_draw_rectangle(0.0f, 0.0f, 960.0f, 544.0f, RGBA8(5, 7, 14, 225));

    float dw = 620.0f, dh = 410.0f;
    float dx = (960.0f - dw) * 0.5f;
    float dy = (544.0f - dh) * 0.5f;

    // Caja modal glassmorphic
    vita2d_draw_rectangle(dx, dy, dw, dh, RGBA8(14, 18, 38, 252));
    vita2d_draw_rectangle(dx, dy, dw, 2.0f, RGBA8(0, 210, 255, 240));
    vita2d_draw_rectangle(dx, dy + dh - 1.5f, dw, 1.5f, RGBA8(35, 55, 115, 180));
    vita2d_draw_rectangle(dx, dy, 1.5f, dh, RGBA8(35, 55, 115, 180));
    vita2d_draw_rectangle(dx + dw - 1.5f, dy, 1.5f, dh, RGBA8(35, 55, 115, 180));

    // Encabezado
    float tw_t = vita2d_pgf_text_width(pgf, 0.95f, "VitaCam Pro");
    vita2d_pgf_draw_text(pgf, (int)(dx + (dw - tw_t) * 0.5f), (int)dy + 38, RGBA8(0, 210, 255, 255), 0.95f, "VitaCam Pro");

    float tw_sub = vita2d_pgf_text_width(pgf, 0.56f, "Camara Avanzada, Galeria & Servidor Wi-Fi para PS Vita");
    vita2d_pgf_draw_text(pgf, (int)(dx + (dw - tw_sub) * 0.5f), (int)dy + 58, RGBA8(180, 195, 225, 210), 0.56f, "Camara Avanzada, Galeria & Servidor Wi-Fi para PS Vita");

    // Tarjeta destacada de Autor / Desarrollador: darking101
    float ab_w = dw - 44.0f, ab_h = 56.0f, ab_x = dx + 22.0f, ab_y = dy + 72.0f;
    vita2d_draw_rectangle(ab_x, ab_y, ab_w, ab_h, RGBA8(22, 30, 60, 220));
    vita2d_draw_rectangle(ab_x, ab_y, ab_w, 1.0f, RGBA8(245, 197, 24, 210));
    vita2d_draw_rectangle(ab_x, ab_y + ab_h - 1.0f, ab_w, 1.0f, RGBA8(245, 197, 24, 210));
    vita2d_draw_rectangle(ab_x, ab_y, 1.0f, ab_h, RGBA8(245, 197, 24, 210));
    vita2d_draw_rectangle(ab_x + ab_w - 1.0f, ab_y, 1.0f, ab_h, RGBA8(245, 197, 24, 210));

    vita2d_pgf_draw_text(pgf, (int)ab_x + 16, (int)ab_y + 24, RGBA8(245, 197, 24, 255), 0.68f, "Desarrollado y Creado por: darking101");
    vita2d_pgf_draw_text(pgf, (int)ab_x + 16, (int)ab_y + 46, RGBA8(160, 215, 255, 230), 0.58f, "GitHub: https://github.com/darking101/vitacam");

    // Atajos y controles del sistema
    float sy = ab_y + ab_h + 22.0f;
    vita2d_pgf_draw_text(pgf, (int)dx + 24, (int)sy, RGBA8(255, 255, 255, 245), 0.64f, "Guia de Atajos y Controles:");
    sy += 22.0f;
    vita2d_pgf_draw_text(pgf, (int)dx + 28, (int)sy, RGBA8(210, 225, 245, 220), 0.56f, "- Gatillo R / Touch: Disparador con Sello oficial [PS] VITA adaptativo.");
    sy += 20.0f;
    vita2d_pgf_draw_text(pgf, (int)dx + 28, (int)sy, RGBA8(210, 225, 245, 220), 0.56f, "- Cuadrado: Modo seleccion multiple en Galeria (borrado en lote).");
    sy += 20.0f;
    vita2d_pgf_draw_text(pgf, (int)dx + 28, (int)sy, RGBA8(210, 225, 245, 220), 0.56f, "- Triangulo: Servidor Web Wi-Fi con enlace directo por Codigo QR.");
    sy += 20.0f;
    vita2d_pgf_draw_text(pgf, (int)dx + 28, (int)sy, RGBA8(210, 225, 245, 220), 0.56f, "- Select: Conmutar camara Trasera / Frontal (con Softbox blanco).");
    sy += 20.0f;
    vita2d_pgf_draw_text(pgf, (int)dx + 28, (int)sy, RGBA8(210, 225, 245, 220), 0.56f, "- Gatillos L / R en Galeria: Conmutar origen (VitaCam / Fotos / Capturas).");
    sy += 20.0f;
    vita2d_pgf_draw_text(pgf, (int)dx + 28, (int)sy, RGBA8(210, 225, 245, 220), 0.56f, "- Stick / Pellizco tactil: Zoom dinamico de 1.0x a 5.0x y paneo fluido.");

    // Botón Cerrar interactivo
    float cb_w = 170.0f, cb_h = 36.0f;
    float cb_x = dx + (dw - cb_w) * 0.5f;
    float cb_y = dy + dh - 48.0f;
    draw_centered_pill_button(pgf, cb_x, cb_y, cb_w, cb_h, ICON_CROSS,
                             "Cerrar", 0.70f, RGBA8(255, 255, 255, 255),
                             RGBA8(35, 55, 115, 250), RGBA8(0, 210, 255, 240));
}

static int gallery_count_selected() {
    int cnt = 0;
    for (int i = 0; i < gallery_count; i++) {
        if (gallery_selected[i]) cnt++;
    }
    return cnt;
}

static void gallery_toggle_select(int idx) {
    if (idx >= 0 && idx < gallery_count) {
        gallery_selected[idx] = !gallery_selected[idx];
        int sel = gallery_count_selected();
        if (sel == 0) {
            gallery_selection_mode = 0;
        } else {
            gallery_selection_mode = 1;
        }
    }
}

static void gallery_toggle_select_all() {
    int sel = gallery_count_selected();
    uint8_t target = (sel == gallery_count && gallery_count > 0) ? 0 : 1;
    for (int i = 0; i < gallery_count; i++) {
        gallery_selected[i] = target;
    }
    if (target) {
        gallery_selection_mode = 1;
    } else {
        gallery_selection_mode = 0;
    }
}

static void gallery_clear_selection() {
    memset(gallery_selected, 0, sizeof(gallery_selected));
    gallery_selection_mode = 0;
}

// =========================================================================
// Estructura de Parámetros de Cámara
// =========================================================================
typedef struct {
    const char *name;
    int current_idx;
    int num_options;
    const char *option_names[64];
    int option_values[64];
    int default_idx;
    int (*apply_func)(int dev, int val);
    int (*query_func)(int dev, int *val);
    int last_ret;
    int hw_val;
} CameraParam;

static float zoom_factor = 1.0f;

// EV: Configura tanto la compensación EV del ISP como el bias de luminancia de hardware
static int apply_ev(int dev, int val) {
    int ret = sceCameraSetEV(dev, val);
    // Bias de luminancia con rango dinámico visible (-20 -> 32 oscuro, 0 -> 128 normal, +20 -> 224 luminoso)
    int brightness = 128 + (val * 24 / 5);
    if (brightness < 20) brightness = 20;
    if (brightness > 240) brightness = 240;
    sceCameraSetBrightness(dev, brightness);
    return ret;
}

static int apply_iso(int dev, int val) { return sceCameraSetISO(dev, val); }
static int apply_wb(int dev, int val) { return sceCameraSetWhiteBalance(dev, val); }
static int apply_sharpness(int dev, int val) { return sceCameraSetSharpness(dev, val); }
static int apply_saturation(int dev, int val) { return sceCameraSetSaturation(dev, val); }
static int apply_effect(int dev, int val) { return sceCameraSetEffect(dev, val); }
static int apply_lock(int dev, int val) { return sceCameraSetAutoControlHold(dev, val); }

// Zoom digital suave por GPU (1.0x hasta 4.0x en pasos de 0.1x)
static int apply_zoom(int dev, int val) {
    (void)dev;
    zoom_factor = (float)val / 10.0f;
    return 0;
}

static int cam_worker_thread(SceSize args, void *argp);
static void apply_and_query_param(int cam_dev, int param_idx);

#define NUM_PARAMS 9

static CameraParam params[NUM_PARAMS];

static int apply_framerate(int dev, int val) {
    if (info.framerate == (uint16_t)val) return 0;

    cam_thread_run = 0;
    if (cam_thid >= 0) {
        sceKernelWaitThreadEnd(cam_thid, NULL, NULL);
        sceKernelDeleteThread(cam_thid);
        cam_thid = -1;
    }

    sceCameraStop(dev);
    sceCameraClose(dev);

    info.framerate = (uint16_t)val;
    if (sceCameraOpen(dev, &info) < 0) {
        info.framerate = SCE_CAMERA_FRAMERATE_30_FPS;
        sceCameraOpen(dev, &info);
    }
    sceCameraStart(dev);

    for (int i = 0; i < NUM_PARAMS; i++) {
        if (params[i].apply_func != apply_framerate) {
            apply_and_query_param(dev, i);
        }
    }

    cam_thread_run = 1;
    cam_thid = sceKernelCreateThread("VitaCam_Capture", cam_worker_thread, 0x10000100, 0x10000, 0, 0, NULL);
    if (cam_thid >= 0) {
        sceKernelStartThread(cam_thid, 0, NULL);
    }

    return 0;
}

static char ev_names[41][16];
static char zoom_names[31][16];

static void init_camera_params_table(void) {
    // 0. EV (-2.0 EV a +2.0 EV en pasos finos de 0.1)
    params[0].name = "EV (Exposicion)";
    params[0].num_options = 41;
    params[0].default_idx = 20;
    params[0].current_idx = 20;
    params[0].apply_func = apply_ev;
    params[0].query_func = sceCameraGetEV;
    for (int i = 0; i <= 40; i++) {
        int v = -20 + i;
        params[0].option_values[i] = v;
        if (v == 0) snprintf(ev_names[i], sizeof(ev_names[i]), " 0.0 EV");
        else if (v > 0) snprintf(ev_names[i], sizeof(ev_names[i]), "+%.1f EV", (float)v / 10.0f);
        else snprintf(ev_names[i], sizeof(ev_names[i]), "%.1f EV", (float)v / 10.0f);
        params[0].option_names[i] = ev_names[i];
    }

    // 1. ISO (AUTO, 100, 200, 400)
    params[1].name = "ISO";
    params[1].num_options = 4;
    params[1].default_idx = 0;
    params[1].current_idx = 0;
    params[1].option_names[0] = "AUTO"; params[1].option_values[0] = SCE_CAMERA_ISO_AUTO;
    params[1].option_names[1] = "100";  params[1].option_values[1] = SCE_CAMERA_ISO_100;
    params[1].option_names[2] = "200";  params[1].option_values[2] = SCE_CAMERA_ISO_200;
    params[1].option_names[3] = "400";  params[1].option_values[3] = SCE_CAMERA_ISO_400;
    params[1].apply_func = apply_iso;
    params[1].query_func = sceCameraGetISO;

    // 2. Framerate / FPS (15, 20, 30, 60)
    params[2].name = "Velocidad (FPS)";
    params[2].num_options = 4;
    params[2].default_idx = 2; // 30 FPS por defecto
    params[2].current_idx = 2;
    params[2].option_names[0] = "15"; params[2].option_values[0] = SCE_CAMERA_FRAMERATE_15_FPS;
    params[2].option_names[1] = "20"; params[2].option_values[1] = SCE_CAMERA_FRAMERATE_20_FPS;
    params[2].option_names[2] = "30"; params[2].option_values[2] = SCE_CAMERA_FRAMERATE_30_FPS;
    params[2].option_names[3] = "60"; params[2].option_values[3] = SCE_CAMERA_FRAMERATE_60_FPS;
    params[2].apply_func = apply_framerate;
    params[2].query_func = NULL;

    // 3. White Balance (AUTO, SOL, FRIO, CALIDO)
    params[3].name = "Balance Blancos";
    params[3].num_options = 4;
    params[3].default_idx = 0;
    params[3].current_idx = 0;
    params[3].option_names[0] = "AUTO";   params[3].option_values[0] = SCE_CAMERA_WB_AUTO;
    params[3].option_names[1] = "SOL";    params[3].option_values[1] = SCE_CAMERA_WB_DAY;
    params[3].option_names[2] = "FRIO";   params[3].option_values[2] = SCE_CAMERA_WB_CWF;
    params[3].option_names[3] = "CALIDO"; params[3].option_values[3] = SCE_CAMERA_WB_SLSA;
    params[3].apply_func = apply_wb;
    params[3].query_func = sceCameraGetWhiteBalance;

    // 4. Zoom Digital Suave (1.0x hasta 4.0x en incrementos de 0.1x)
    params[4].name = "Zoom Digital";
    params[4].num_options = 31;
    params[4].default_idx = 0;
    params[4].current_idx = 0;
    params[4].apply_func = apply_zoom;
    params[4].query_func = NULL;
    for (int i = 0; i <= 30; i++) {
        int v = 10 + i;
        params[4].option_values[i] = v;
        snprintf(zoom_names[i], sizeof(zoom_names[i]), "%.1fx", (float)v / 10.0f);
        params[4].option_names[i] = zoom_names[i];
    }

    // 5. Nitidez (Sharpness)
    params[5].name = "Nitidez";
    params[5].num_options = 4;
    params[5].default_idx = 0;
    params[5].current_idx = 0;
    params[5].option_names[0] = "100%"; params[5].option_values[0] = SCE_CAMERA_SHARPNESS_100;
    params[5].option_names[1] = "200%"; params[5].option_values[1] = SCE_CAMERA_SHARPNESS_200;
    params[5].option_names[2] = "300%"; params[5].option_values[2] = SCE_CAMERA_SHARPNESS_300;
    params[5].option_names[3] = "400%"; params[5].option_values[3] = SCE_CAMERA_SHARPNESS_400;
    params[5].apply_func = apply_sharpness;
    params[5].query_func = sceCameraGetSharpness;

    // 6. Saturación
    params[6].name = "Saturacion";
    params[6].num_options = 5;
    params[6].default_idx = 2;
    params[6].current_idx = 2;
    params[6].option_names[0] = "0.0x"; params[6].option_values[0] = SCE_CAMERA_SATURATION_0;
    params[6].option_names[1] = "0.5x"; params[6].option_values[1] = SCE_CAMERA_SATURATION_5;
    params[6].option_names[2] = "1.0x"; params[6].option_values[2] = SCE_CAMERA_SATURATION_10;
    params[6].option_names[3] = "2.0x"; params[6].option_values[3] = SCE_CAMERA_SATURATION_20;
    params[6].option_names[4] = "3.0x"; params[6].option_values[4] = SCE_CAMERA_SATURATION_30;
    params[6].apply_func = apply_saturation;
    params[6].query_func = sceCameraGetSaturation;

    // 7. Filtros / Efectos
    params[7].name = "Filtro / Efecto";
    params[7].num_options = 7;
    params[7].default_idx = 0;
    params[7].current_idx = 0;
    params[7].option_names[0] = "NORM";  params[7].option_values[0] = SCE_CAMERA_EFFECT_NORMAL;
    params[7].option_names[1] = "NEG";   params[7].option_values[1] = SCE_CAMERA_EFFECT_NEGATIVE;
    params[7].option_names[2] = "B/N";   params[7].option_values[2] = SCE_CAMERA_EFFECT_BLACKWHITE;
    params[7].option_names[3] = "SEPIA"; params[7].option_values[3] = SCE_CAMERA_EFFECT_SEPIA;
    params[7].option_names[4] = "AZUL";  params[7].option_values[4] = SCE_CAMERA_EFFECT_BLUE;
    params[7].option_names[5] = "ROJO";  params[7].option_values[5] = SCE_CAMERA_EFFECT_RED;
    params[7].option_names[6] = "VERDE"; params[7].option_values[6] = SCE_CAMERA_EFFECT_GREEN;
    params[7].apply_func = apply_effect;
    params[7].query_func = sceCameraGetEffect;

    // 8. Bloqueo AE/AWB
    params[8].name = "Bloqueo AE/AWB";
    params[8].num_options = 2;
    params[8].default_idx = 0;
    params[8].current_idx = 0;
    params[8].option_names[0] = "Auto"; params[8].option_values[0] = 0;
    params[8].option_names[1] = "Lock"; params[8].option_values[1] = 1;
    params[8].apply_func = apply_lock;
    params[8].query_func = sceCameraGetAutoControlHold;
}

static inline int is_pro_param_discrete(int pro_bar_idx) {
    // 0: ISO, 1: SPEED (FPS), 3: WB, 4: EFECTO son discretos con botones flotantes circulares
    // 2: EV, 5: ZOOM son continuos con slider de dial milimétrico
    return (pro_bar_idx == 0 || pro_bar_idx == 1 || pro_bar_idx == 3 || pro_bar_idx == 4);
}

static void apply_and_query_param(int cam_dev, int param_idx) {
    CameraParam *p = &params[param_idx];
    if (p->apply_func) {
        p->last_ret = p->apply_func(cam_dev, p->option_values[p->current_idx]);
    }
    if (p->query_func) {
        int val = 0;
        if (p->query_func(cam_dev, &val) >= 0) {
            p->hw_val = val;
        }
    }
}

static void switch_camera_device() {
    cam_thread_run = 0;
    if (cam_thid >= 0) {
        sceKernelWaitThreadEnd(cam_thid, NULL, NULL);
        sceKernelDeleteThread(cam_thid);
        cam_thid = -1;
    }

    sceCameraStop(cam_dev);
    sceCameraClose(cam_dev);

    cam_dev = (cam_dev == SCE_CAMERA_DEVICE_BACK) ? SCE_CAMERA_DEVICE_FRONT : SCE_CAMERA_DEVICE_BACK;

    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    info.priority = SCE_CAMERA_PRIORITY_SHARE;
    info.resolution = SCE_CAMERA_RESOLUTION_640_480;
    info.framerate = (uint16_t)params[2].option_values[params[2].current_idx];
    if (info.framerate == 0) info.framerate = SCE_CAMERA_FRAMERATE_30_FPS;
    info.format = SCE_CAMERA_FORMAT_YUV420_PLANE;
    info.range = 1;
    info.pitch = 0;
    info.buffer = 0;

    info.sizeIBase = CAM_WIDTH * CAM_HEIGHT;
    info.sizeUBase = (CAM_WIDTH / 2) * (CAM_HEIGHT / 2);
    info.sizeVBase = (CAM_WIDTH / 2) * (CAM_HEIGHT / 2);

    info.pIBase = cam_buf;
    info.pUBase = (uint8_t *)cam_buf + info.sizeIBase;
    info.pVBase = (uint8_t *)cam_buf + info.sizeIBase + info.sizeUBase;

    if (sceCameraOpen(cam_dev, &info) < 0) {
        cam_dev = (cam_dev == SCE_CAMERA_DEVICE_BACK) ? SCE_CAMERA_DEVICE_FRONT : SCE_CAMERA_DEVICE_BACK;
        sceCameraOpen(cam_dev, &info);
    }
    sceCameraStart(cam_dev);

    for (int i = 0; i < NUM_PARAMS; i++) {
        if (params[i].apply_func != apply_framerate) {
            apply_and_query_param(cam_dev, i);
        }
    }

    cam_thread_run = 1;
    cam_thid = sceKernelCreateThread("VitaCam_Capture", cam_worker_thread, 0x10000100, 0x10000, 0, 0, NULL);
    if (cam_thid >= 0) {
        sceKernelStartThread(cam_thid, 0, NULL);
    }

    snprintf(status_msg, sizeof(status_msg), "CÁMARA: %s", (cam_dev == SCE_CAMERA_DEVICE_BACK) ? "POSTERIOR" : "FRONTAL");
    status_msg_color = RGBA8(245, 197, 24, 255);
    status_msg_timer = 90;
}

// =========================================================================
// Conversión de Color Ultrarrápida con ARM NEON SIMD (YUV420p -> RGBA8888)
// =========================================================================
static inline void yuv420_to_rgba_neon_exact(
    const uint8_t *__restrict y_plane,
    const uint8_t *__restrict u_plane,
    const uint8_t *__restrict v_plane,
    uint32_t *__restrict out_rgba,
    int width,
    int height,
    unsigned int stride_bytes
) {
    int stride_pixels = stride_bytes / 4;
    int half_width = width >> 1;
    uint8x8_t alpha = vdup_n_u8(255);

    for (int y = 0; y < height; y += 2) {
        const uint8_t *y_row0 = y_plane + (y * width);
        const uint8_t *y_row1 = y_plane + ((y + 1) * width);
        const uint8_t *u_row = u_plane + ((y >> 1) * half_width);
        const uint8_t *v_row = v_plane + ((y >> 1) * half_width);

        uint8_t *dst_row0 = (uint8_t *)(out_rgba + (y * stride_pixels));
        uint8_t *dst_row1 = (uint8_t *)(out_rgba + ((y + 1) * stride_pixels));

        for (int x = 0; x < width; x += 8) {
            uint8x8_t y0_vec = vld1_u8(y_row0 + x);
            uint8x8_t y1_vec = vld1_u8(y_row1 + x);

            int uv_offset = x >> 1;
            uint32_t u_scalar = *(const uint32_t *)(u_row + uv_offset);
            uint32_t v_scalar = *(const uint32_t *)(v_row + uv_offset);

            uint8x8_t u_half = vreinterpret_u8_u32(vdup_n_u32(u_scalar));
            uint8x8_t v_half = vreinterpret_u8_u32(vdup_n_u32(v_scalar));

            uint8x8x2_t u_zip = vzip_u8(u_half, u_half);
            uint8x8x2_t v_zip = vzip_u8(v_half, v_half);

            uint8x8_t u_vec = u_zip.val[0];
            uint8x8_t v_vec = v_zip.val[0];

            int16x8_t y0_16 = vreinterpretq_s16_u16(vsubl_u8(y0_vec, vdup_n_u8(16)));
            int16x8_t y1_16 = vreinterpretq_s16_u16(vsubl_u8(y1_vec, vdup_n_u8(16)));
            int16x8_t u_16  = vreinterpretq_s16_u16(vsubl_u8(u_vec, vdup_n_u8(128)));
            int16x8_t v_16  = vreinterpretq_s16_u16(vsubl_u8(v_vec, vdup_n_u8(128)));

            int32x4_t y0_lo = vmull_n_s16(vget_low_s16(y0_16), 298);
            int32x4_t y0_hi = vmull_n_s16(vget_high_s16(y0_16), 298);
            int32x4_t y1_lo = vmull_n_s16(vget_low_s16(y1_16), 298);
            int32x4_t y1_hi = vmull_n_s16(vget_high_s16(y1_16), 298);

            int32x4_t r_coeff_lo = vmulq_n_s32(vmovl_s16(vget_low_s16(v_16)), 409);
            int32x4_t r_coeff_hi = vmulq_n_s32(vmovl_s16(vget_high_s16(v_16)), 409);

            int32x4_t g_coeff_lo = vaddq_s32(vmulq_n_s32(vmovl_s16(vget_low_s16(u_16)), -100),
                                              vmulq_n_s32(vmovl_s16(vget_low_s16(v_16)), -208));
            int32x4_t g_coeff_hi = vaddq_s32(vmulq_n_s32(vmovl_s16(vget_high_s16(u_16)), -100),
                                              vmulq_n_s32(vmovl_s16(vget_high_s16(v_16)), -208));

            int32x4_t b_coeff_lo = vmulq_n_s32(vmovl_s16(vget_low_s16(u_16)), 516);
            int32x4_t b_coeff_hi = vmulq_n_s32(vmovl_s16(vget_high_s16(u_16)), 516);

            int32x4_t r0_lo = vrshrq_n_s32(vaddq_s32(y0_lo, r_coeff_lo), 8);
            int32x4_t r0_hi = vrshrq_n_s32(vaddq_s32(y0_hi, r_coeff_hi), 8);
            int32x4_t g0_lo = vrshrq_n_s32(vaddq_s32(y0_lo, g_coeff_lo), 8);
            int32x4_t g0_hi = vrshrq_n_s32(vaddq_s32(y0_hi, g_coeff_hi), 8);
            int32x4_t b0_lo = vrshrq_n_s32(vaddq_s32(y0_lo, b_coeff_lo), 8);
            int32x4_t b0_hi = vrshrq_n_s32(vaddq_s32(y0_hi, b_coeff_hi), 8);

            int32x4_t r1_lo = vrshrq_n_s32(vaddq_s32(y1_lo, r_coeff_lo), 8);
            int32x4_t r1_hi = vrshrq_n_s32(vaddq_s32(y1_hi, r_coeff_hi), 8);
            int32x4_t g1_lo = vrshrq_n_s32(vaddq_s32(y1_lo, g_coeff_lo), 8);
            int32x4_t g1_hi = vrshrq_n_s32(vaddq_s32(y1_hi, g_coeff_hi), 8);
            int32x4_t b1_lo = vrshrq_n_s32(vaddq_s32(y1_lo, b_coeff_lo), 8);
            int32x4_t b1_hi = vrshrq_n_s32(vaddq_s32(y1_hi, b_coeff_hi), 8);

            uint8x8_t r0 = vqmovun_s16(vcombine_s16(vqmovn_s32(r0_lo), vqmovn_s32(r0_hi)));
            uint8x8_t g0 = vqmovun_s16(vcombine_s16(vqmovn_s32(g0_lo), vqmovn_s32(g0_hi)));
            uint8x8_t b0 = vqmovun_s16(vcombine_s16(vqmovn_s32(b0_lo), vqmovn_s32(b0_hi)));

            uint8x8_t r1 = vqmovun_s16(vcombine_s16(vqmovn_s32(r1_lo), vqmovn_s32(r1_hi)));
            uint8x8_t g1 = vqmovun_s16(vcombine_s16(vqmovn_s32(g1_lo), vqmovn_s32(g1_hi)));
            uint8x8_t b1 = vqmovun_s16(vcombine_s16(vqmovn_s32(b1_lo), vqmovn_s32(b1_hi)));

            uint8x8x4_t rgba0;
            rgba0.val[0] = r0;
            rgba0.val[1] = g0;
            rgba0.val[2] = b0;
            rgba0.val[3] = alpha;
            vst4_u8(dst_row0 + x * 4, rgba0);

            uint8x8x4_t rgba1;
            rgba1.val[0] = r1;
            rgba1.val[1] = g1;
            rgba1.val[2] = b1;
            rgba1.val[3] = alpha;
            vst4_u8(dst_row1 + x * 4, rgba1);
        }
    }
}

// =========================================================================
// Funciones de Gestión de Galería de Fotos (100% Seguras, Sin Alloc GXM)
// =========================================================================
struct custom_jpeg_error_mgr {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void custom_jpeg_error_exit(j_common_ptr cinfo) {
    struct custom_jpeg_error_mgr *myerr = (struct custom_jpeg_error_mgr *)cinfo->err;
    longjmp(myerr->setjmp_buffer, 1);
}

static int load_jpeg_mem_into_texture(const void *buf, int read_bytes, vita2d_texture **tex_ptr) {
    if (!buf || read_bytes <= 0 || !tex_ptr) return -1;

    struct jpeg_decompress_struct cinfo;
    struct custom_jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = custom_jpeg_error_exit;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return -4;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (const unsigned char *)buf, read_bytes);
    jpeg_read_header(&cinfo, TRUE);

    // Escalar JPEGs si son enormes (para no agotar la RAM)
    if (cinfo.image_width > 1280) {
        cinfo.scale_num = 1;
        cinfo.scale_denom = 2;
    }
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int img_w = cinfo.output_width;
    int img_h = cinfo.output_height;

    vita2d_texture *tex = NULL;
    if (*tex_ptr) {
        if (vita2d_texture_get_width(*tex_ptr) == img_w && vita2d_texture_get_height(*tex_ptr) == img_h) {
            tex = *tex_ptr;
        } else {
            vita2d_free_texture(*tex_ptr);
            *tex_ptr = NULL;
        }
    }
    
    if (!tex) {
        tex = vita2d_create_empty_texture(img_w, img_h);
        if (!tex) {
            jpeg_destroy_decompress(&cinfo);
            return -5;
        }
        *tex_ptr = tex;
    }

    uint32_t *tex_data = (uint32_t *)vita2d_texture_get_datap(tex);
    unsigned int tex_stride = vita2d_texture_get_stride(tex) / sizeof(uint32_t);

    int row_stride = img_w * 3;
    uint8_t *temp_rgb = malloc(img_h * row_stride);
    if (!temp_rgb) {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return -6;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row_pointer = temp_rgb + (cinfo.output_scanline * row_stride);
        jpeg_read_scanlines(&cinfo, &row_pointer, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    for (int dy = 0; dy < img_h; dy++) {
        uint8_t *src_row = temp_rgb + (dy * row_stride);
        uint32_t *dst_row = tex_data + (dy * tex_stride);
        for (int dx = 0; dx < img_w; dx++) {
            uint8_t r = src_row[dx * 3 + 0];
            uint8_t g = src_row[dx * 3 + 1];
            uint8_t b = src_row[dx * 3 + 2];
            dst_row[dx] = RGBA8(r, g, b, 255);
        }
    }

    free(temp_rgb);
    fullscreen_aspect_ratio = (float)img_w / (float)img_h;
    return 0;
}

static int load_jpeg_mem_into_thumb_texture(const void *buf, int read_bytes, vita2d_texture *tex) {
    if (!buf || read_bytes <= 0 || !tex) return -1;

    struct jpeg_decompress_struct cinfo;
    struct custom_jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = custom_jpeg_error_exit;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        return -4;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (const unsigned char *)buf, read_bytes);
    jpeg_read_header(&cinfo, TRUE);

    // Decodificación rápida a escala 1/2 (320x240)
    cinfo.scale_num = 1;
    cinfo.scale_denom = 2;
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int img_w = cinfo.output_width;
    int img_h = cinfo.output_height;

    uint32_t *tex_data = (uint32_t *)vita2d_texture_get_datap(tex);
    unsigned int tex_stride = vita2d_texture_get_stride(tex) / sizeof(uint32_t);

    int row_stride = img_w * 3;
    uint8_t *temp_rgb = malloc(img_h * row_stride);
    if (!temp_rgb) {
        jpeg_finish_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return -5;
    }

    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row_pointer = temp_rgb + (cinfo.output_scanline * row_stride);
        jpeg_read_scanlines(&cinfo, &row_pointer, 1);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    // Remuestreo uniforme y limpio sin duplicar o saltar líneas
    for (int dy = 0; dy < THUMB_HEIGHT; dy++) {
        int sy = (dy * img_h) / THUMB_HEIGHT;
        if (sy >= img_h) sy = img_h - 1;
        const uint8_t *src_row = temp_rgb + (sy * row_stride);
        uint32_t *dst_row = tex_data + (dy * tex_stride);

        for (int dx = 0; dx < THUMB_WIDTH; dx++) {
            int sx = (dx * img_w) / THUMB_WIDTH;
            if (sx >= img_w) sx = img_w - 1;
            uint8_t r = src_row[sx * 3 + 0];
            uint8_t g = src_row[sx * 3 + 1];
            uint8_t b = src_row[sx * 3 + 2];
            dst_row[dx] = RGBA8(r, g, b, 255);
        }
    }

    free(temp_rgb);
    return 0;
}

static int extract_avi_frame_to_buffer(const char *avi_path, int target_frame_idx, void *out_buf, int max_out_size, int *out_len) {
    if (!avi_path || !out_buf) return -1;
    SceUID fd = sceIoOpen(avi_path, SCE_O_RDONLY, 0);
    if (fd < 0) return -2;

    sceIoLseek(fd, 2048, SCE_SEEK_SET);

    int cur_idx = 0;
    while (cur_idx <= target_frame_idx) {
        uint8_t tag[8];
        int r = sceIoRead(fd, tag, 8);
        if (r < 8) break;

        uint32_t chunk_len = *(uint32_t *)(tag + 4);
        if (tag[0] == '0' && tag[1] == '0' && tag[2] == 'd' && tag[3] == 'c') {
            if (cur_idx == target_frame_idx) {
                if ((int)chunk_len > max_out_size) chunk_len = max_out_size;
                int read_len = sceIoRead(fd, out_buf, chunk_len);
                sceIoClose(fd);
                if (out_len) *out_len = read_len;
                return (read_len > 0) ? 0 : -3;
            }
            cur_idx++;
        }

        int skip = (int)chunk_len + (chunk_len & 1);
        if (skip <= 0 || skip > 2000000) break;
        sceIoLseek(fd, skip, SCE_SEEK_CUR);
    }

    sceIoClose(fd);
    return -4;
}
// Carga un PNG a resolucion completa
static int load_png_into_texture(const char *filepath, vita2d_texture **tex_ptr) {
    if (!filepath || !tex_ptr) return -1;

    FILE *fp = fopen(filepath, "rb");
    if (!fp) return -2;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return -3; }

    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(fp); return -4; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return -5;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    int img_w = png_get_image_width(png, info);
    int img_h = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth  = png_get_bit_depth(png, info);

    // Normalizar a RGBA8
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(png, info);

    vita2d_texture *tex = NULL;
    if (*tex_ptr) {
        if (vita2d_texture_get_width(*tex_ptr) == img_w && vita2d_texture_get_height(*tex_ptr) == img_h) {
            tex = *tex_ptr;
        } else {
            vita2d_free_texture(*tex_ptr);
            *tex_ptr = NULL;
        }
    }
    
    if (!tex) {
        tex = vita2d_create_empty_texture(img_w, img_h);
        if (!tex) {
            png_destroy_read_struct(&png, &info, NULL);
            fclose(fp);
            return -6;
        }
        *tex_ptr = tex;
    }

    uint32_t *tex_data = (uint32_t *)vita2d_texture_get_datap(tex);
    unsigned int tex_stride = vita2d_texture_get_stride(tex) / sizeof(uint32_t);

    png_bytep *rows = malloc(img_h * sizeof(png_bytep));
    if (!rows) { png_destroy_read_struct(&png, &info, NULL); fclose(fp); return -7; }

    // Leer directamente en la textura
    for (int r = 0; r < img_h; r++)
        rows[r] = (png_bytep)(tex_data + r * tex_stride);

    png_read_image(png, rows);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);

    fullscreen_aspect_ratio = (float)img_w / (float)img_h;
    return 0;
}

static int load_jpeg_into_texture(const char *filepath, vita2d_texture **tex_ptr) {
    if (!filepath || !tex_ptr) return -1;
    SceUID fd = sceIoOpen(filepath, SCE_O_RDONLY, 0);
    if (fd < 0) return -2;
    int read_bytes = sceIoRead(fd, jpeg_out_buf, JPEG_OUT_BUF_SIZE);
    sceIoClose(fd);
    if (read_bytes <= 0) return -3;
    return load_jpeg_mem_into_texture(jpeg_out_buf, read_bytes, tex_ptr);
}

static int load_jpeg_into_thumb_texture(const char *filepath, vita2d_texture *tex) {
    if (!filepath || !tex) return -1;
    SceUID fd = sceIoOpen(filepath, SCE_O_RDONLY, 0);
    if (fd < 0) return -2;
    int read_bytes = sceIoRead(fd, jpeg_out_buf, JPEG_OUT_BUF_SIZE);
    sceIoClose(fd);
    if (read_bytes <= 0) return -3;
    return load_jpeg_mem_into_thumb_texture(jpeg_out_buf, read_bytes, tex);
}

// Carga un PNG y lo escala al tamaño del thumbnail usando libpng
static int load_png_into_thumb_texture(const char *filepath, vita2d_texture *tex) {
    if (!filepath || !tex) return -1;

    FILE *fp = fopen(filepath, "rb");
    if (!fp) return -2;

    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(fp); return -3; }

    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); fclose(fp); return -4; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(fp);
        return -5;
    }

    png_init_io(png, fp);
    png_read_info(png, info);

    int img_w = png_get_image_width(png, info);
    int img_h = png_get_image_height(png, info);
    png_byte color_type = png_get_color_type(png, info);
    png_byte bit_depth  = png_get_bit_depth(png, info);

    // Normalizar a RGBA8
    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(png, info);

    // Leer pixeles
    uint8_t *raw = malloc(img_h * img_w * 4);
    if (!raw) { png_destroy_read_struct(&png, &info, NULL); fclose(fp); return -6; }

    png_bytep *rows = malloc(img_h * sizeof(png_bytep));
    if (!rows) { free(raw); png_destroy_read_struct(&png, &info, NULL); fclose(fp); return -7; }

    for (int r = 0; r < img_h; r++)
        rows[r] = raw + r * img_w * 4;

    png_read_image(png, rows);
    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(fp);

    // Escalar al thumbnail
    uint32_t *tex_data = (uint32_t *)vita2d_texture_get_datap(tex);
    unsigned int tex_stride = vita2d_texture_get_stride(tex) / sizeof(uint32_t);

    for (int dy = 0; dy < THUMB_HEIGHT; dy++) {
        int sy = (dy * img_h) / THUMB_HEIGHT;
        if (sy >= img_h) sy = img_h - 1;
        for (int dx = 0; dx < THUMB_WIDTH; dx++) {
            int sx = (dx * img_w) / THUMB_WIDTH;
            if (sx >= img_w) sx = img_w - 1;
            uint8_t *px = raw + (sy * img_w + sx) * 4;
            tex_data[dy * tex_stride + dx] = RGBA8(px[0], px[1], px[2], 255);
        }
    }

    free(raw);
    return 0;
}

// Detecta extension y usa el loader correcto (JPEG o PNG)
static int load_image_into_thumb_texture(const char *filepath, vita2d_texture *tex) {
    if (!filepath) return -1;
    int len = strlen(filepath);
    if (len > 4 && strcasecmp(filepath + len - 4, ".png") == 0)
        return load_png_into_thumb_texture(filepath, tex);
    return load_jpeg_into_thumb_texture(filepath, tex);
}

// Detecta extension y usa el loader correcto a resolucion completa (JPEG o PNG)
static int load_image_into_texture(const char *filepath, vita2d_texture **tex_ptr) {
    if (!filepath) return -1;
    int len = strlen(filepath);
    if (len > 4 && strcasecmp(filepath + len - 4, ".png") == 0)
        return load_png_into_texture(filepath, tex_ptr);
    return load_jpeg_into_texture(filepath, tex_ptr);
}

static int load_media_into_texture(const GalleryPhoto *photo, vita2d_texture **tex_ptr) {
    if (!photo || !tex_ptr) return -1;
    if (photo->is_video) {
        char thumb_path[256];
        strncpy(thumb_path, photo->fullpath, sizeof(thumb_path) - 1);
        int tlen = strlen(thumb_path);
        if (tlen > 4) {
            strcpy(thumb_path + tlen - 4, ".jpg");
            SceIoStat st;
            if (sceIoGetstat(thumb_path, &st) >= 0) {
                return load_image_into_texture(thumb_path, tex_ptr);
            }
        }
        int read_bytes = 0;
        int ret = extract_avi_frame_to_buffer(photo->fullpath, 0, jpeg_out_buf, JPEG_OUT_BUF_SIZE, &read_bytes);
        if (ret >= 0 && read_bytes > 0) {
            return load_jpeg_mem_into_texture(jpeg_out_buf, read_bytes, tex_ptr);
        }
        return -2;
    } else {
        return load_image_into_texture(photo->fullpath, tex_ptr);
    }
}

static int load_media_into_thumb_texture(const GalleryPhoto *photo, vita2d_texture *tex) {
    if (!photo || !tex) return -1;
    if (photo->is_video) {
        char thumb_path[256];
        strncpy(thumb_path, photo->fullpath, sizeof(thumb_path) - 1);
        int tlen = strlen(thumb_path);
        if (tlen > 4) {
            strcpy(thumb_path + tlen - 4, ".jpg");
            SceIoStat st;
            if (sceIoGetstat(thumb_path, &st) >= 0) {
                return load_jpeg_into_thumb_texture(thumb_path, tex);
            }
        }
        int read_bytes = 0;
        int ret = extract_avi_frame_to_buffer(photo->fullpath, 0, jpeg_out_buf, JPEG_OUT_BUF_SIZE, &read_bytes);
        if (ret >= 0 && read_bytes > 0) {
            return load_jpeg_mem_into_thumb_texture(jpeg_out_buf, read_bytes, tex);
        }
        return -2;
    } else {
        // Detecta PNG o JPEG segun extension
        return load_image_into_thumb_texture(photo->fullpath, tex);
    }
}

static void update_camera_last_thumb(void) {
    if (!cam_last_thumb_tex) return;
    if (gallery_count > 0) {
        load_media_into_thumb_texture(&gallery_photos[0], cam_last_thumb_tex);
    } else {
        uint32_t *tdata = (uint32_t *)vita2d_texture_get_datap(cam_last_thumb_tex);
        unsigned int tstride = vita2d_texture_get_stride(cam_last_thumb_tex) / sizeof(uint32_t);
        for (int y = 0; y < THUMB_HEIGHT; y++) {
            memset(tdata + (y * tstride), 0, THUMB_WIDTH * sizeof(uint32_t));
        }
    }
}

static void draw_qr_code(const char *text, float center_x, float center_y, float size) {
    if (!text || strlen(text) == 0) return;
    uint8_t qr0[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tempBuffer[qrcodegen_BUFFER_LEN_MAX];
    bool ok = qrcodegen_encodeText(text, tempBuffer, qr0, qrcodegen_Ecc_LOW,
        qrcodegen_VERSION_MIN, 10, qrcodegen_Mask_AUTO, true);
    if (!ok) return;

    int qr_size = qrcodegen_getSize(qr0);
    int border = 2;
    int total_cells = qr_size + border * 2;
    float cell_size = floorf(size / (float)total_cells);
    if (cell_size < 1.0f) cell_size = 1.0f;
    float actual_size = cell_size * (float)total_cells;
    float start_x = center_x - actual_size * 0.5f;
    float start_y = center_y - actual_size * 0.5f;

    // Fondo blanco nítido
    vita2d_draw_rectangle(start_x, start_y, actual_size, actual_size, RGBA8(255, 255, 255, 255));

    // Módulos oscuros del código QR
    for (int y = 0; y < qr_size; y++) {
        for (int x = 0; x < qr_size; x++) {
            if (qrcodegen_getModule(qr0, x, y)) {
                vita2d_draw_rectangle(start_x + (float)(x + border) * cell_size,
                                      start_y + (float)(y + border) * cell_size,
                                      cell_size, cell_size, RGBA8(10, 14, 28, 255));
            }
        }
    }
}

static int gallery_compare_names(const void *a, const void *b) {
    const GalleryPhoto *pa = (const GalleryPhoto *)a;
    const GalleryPhoto *pb = (const GalleryPhoto *)b;
    // Orden cronológico estricto (más recientes primero)
    if (pb->timestamp_key != pa->timestamp_key) {
        return (pb->timestamp_key > pa->timestamp_key) ? 1 : -1;
    }
    return strcmp(pb->filename, pa->filename);
}

static const char *month_names_es[12] = {
    "Enero", "Febrero", "Marzo", "Abril", "Mayo", "Junio",
    "Julio", "Agosto", "Septiembre", "Octubre", "Noviembre", "Diciembre"
};

static void parse_photo_date(GalleryPhoto *p, const SceIoStat *stat) {
    int y = 0, m = 0, d = 0, hh = 12, mm = 0, ss = 0;
    if ((sscanf(p->filename, "photo_%4d%2d%2d_%2d%2d%2d", &y, &m, &d, &hh, &mm, &ss) >= 5 ||
         sscanf(p->filename, "VID_%4d%2d%2d_%2d%2d%2d", &y, &m, &d, &hh, &mm, &ss) >= 5 ||
         sscanf(p->filename, "video_%4d%2d%2d_%2d%2d%2d", &y, &m, &d, &hh, &mm, &ss) >= 5 ||
         sscanf(p->filename, "IMG_%4d%2d%2d_%2d%2d%2d", &y, &m, &d, &hh, &mm, &ss) >= 5 ||
         // Formato nativo de la camara/screenshot de PS Vita: 2026-09-15-130356.jpg
         sscanf(p->filename, "%4d-%2d-%2d-%2d%2d%2d", &y, &m, &d, &hh, &mm, &ss) >= 5) &&
        y > 2000 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
        p->year = y;
        p->month = m;
        p->day = d;
        p->hour = hh;
        p->minute = mm;
        p->second = ss;
    } else if ((sscanf(p->filename, "photo_%4d%2d%2d", &y, &m, &d) == 3 ||
                sscanf(p->filename, "VID_%4d%2d%2d", &y, &m, &d) == 3 ||
                sscanf(p->filename, "video_%4d%2d%2d", &y, &m, &d) == 3 ||
                sscanf(p->filename, "IMG_%4d%2d%2d", &y, &m, &d) == 3) &&
               y > 2000 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
        p->year = y;
        p->month = m;
        p->day = d;
        p->hour = 12;
        p->minute = 0;
        p->second = 0;
    } else if (stat && stat->st_ctime.year > 2000) {
        p->year = stat->st_ctime.year;
        p->month = stat->st_ctime.month;
        p->day = stat->st_ctime.day;
        p->hour = stat->st_ctime.hour;
        p->minute = stat->st_ctime.minute;
        p->second = stat->st_ctime.second;
        hh = p->hour;
        mm = p->minute;
        ss = p->second;
    } else {
        p->year = 2026;
        p->month = 1;
        p->day = 1;
        p->hour = 12;
        p->minute = 0;
        p->second = 0;
        hh = 12;
        mm = 0;
        ss = 0;
    }

    p->timestamp_key = ((uint64_t)p->year * 10000000000ULL) +
                       ((uint64_t)p->month * 100000000ULL) +
                       ((uint64_t)p->day * 1000000ULL) +
                       ((uint64_t)p->hour * 10000ULL) +
                       ((uint64_t)p->minute * 100ULL) +
                       (uint64_t)p->second;

    int h12 = hh % 12;
    if (h12 == 0) h12 = 12;
    const char *ampm = (hh >= 12) ? "PM" : "AM";
    snprintf(p->time_label, sizeof(p->time_label), "%d:%02d %s", h12, mm, ampm);

    SceDateTime now;
    memset(&now, 0, sizeof(now));
    sceRtcGetCurrentClockLocalTime(&now);

    if (p->year == now.year && p->month == now.month && p->day == now.day) {
        snprintf(p->date_label, sizeof(p->date_label), "Hoy");
    } else if (p->year == now.year && p->month == now.month && p->day == now.day - 1) {
        snprintf(p->date_label, sizeof(p->date_label), "Ayer");
    } else if (p->year == now.year) {
        snprintf(p->date_label, sizeof(p->date_label), "%d de %s", p->day, (p->month >= 1 && p->month <= 12) ? month_names_es[p->month - 1] : "");
    } else {
        snprintf(p->date_label, sizeof(p->date_label), "%d %s %04d", p->day, (p->month >= 1 && p->month <= 12) ? month_names_es[p->month - 1] : "", p->year);
    }
}

static void gallery_build_date_groups(void);

static int gallery_has_photo(const char *path) {
    for (int i = 0; i < gallery_count; i++) {
        if (strcasecmp(gallery_photos[i].fullpath, path) == 0) return 1;
    }
    return 0;
}

static void gallery_scan_folder_recursive(const char *dir_path, int depth) {
    if (depth > 8 || gallery_count >= MAX_GALLERY_PHOTOS) return;

    SceUID dfd = sceIoDopen(dir_path);
    if (dfd < 0) return;

    SceIoDirent dirent;
    memset(&dirent, 0, sizeof(dirent));

    while (sceIoDread(dfd, &dirent) > 0 && gallery_count < MAX_GALLERY_PHOTOS) {
        const char *name = dirent.d_name;
        if (name[0] == '.') {
            memset(&dirent, 0, sizeof(dirent));
            continue;
        }

        char subpath[256];
        snprintf(subpath, sizeof(subpath), "%s/%s", dir_path, name);

        int len = strlen(name);
        const char *ext = (len > 4) ? name + len - 4 : "";
        const char *ext5 = (len > 5) ? name + len - 5 : "";
        int is_media = (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext5, ".jpeg") == 0 ||
                        strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".bmp") == 0 ||
                        strcasecmp(ext, ".avi") == 0 || strcasecmp(ext, ".mp4") == 0);

        if (!is_media) {
            // Intentar abrir como subdirectorio
            SceUID sub_dfd = sceIoDopen(subpath);
            if (sub_dfd >= 0) {
                sceIoDclose(sub_dfd);
                // Si estamos en la pestaña de Fotos, ignorar carpetas de SCREENSHOT
                if (gallery_source_tab == GALLERY_SOURCE_PHOTO &&
                    (strncasecmp(name, "SCREENSHOT", 10) == 0 || strncasecmp(name, "SCREENSHOOT", 11) == 0)) {
                    memset(&dirent, 0, sizeof(dirent));
                    continue;
                }
                gallery_scan_folder_recursive(subpath, depth + 1);
                memset(&dirent, 0, sizeof(dirent));
                continue;
            }
        }

        if (is_media) {
            if (gallery_has_photo(subpath)) {
                memset(&dirent, 0, sizeof(dirent));
                continue;
            }

            int is_avi = (strcasecmp(ext, ".avi") == 0 || strcasecmp(ext, ".mp4") == 0);
            int is_jpg = !is_avi;

            if (is_jpg && strncmp(name, "VID_", 4) == 0) {
                char avi_check[256];
                snprintf(avi_check, sizeof(avi_check), "%s/%s", dir_path, name);
                int clen = strlen(avi_check);
                if (clen > 4) {
                    strcpy(avi_check + clen - 4, ".avi");
                    SceIoStat st;
                    if (sceIoGetstat(avi_check, &st) >= 0) {
                        memset(&dirent, 0, sizeof(dirent));
                        continue;
                    }
                }
            }

            strncpy(gallery_photos[gallery_count].filename, name, sizeof(gallery_photos[gallery_count].filename) - 1);
            gallery_photos[gallery_count].filename[sizeof(gallery_photos[gallery_count].filename) - 1] = '\0';
            strncpy(gallery_photos[gallery_count].fullpath, subpath, sizeof(gallery_photos[gallery_count].fullpath) - 1);
            gallery_photos[gallery_count].fullpath[sizeof(gallery_photos[gallery_count].fullpath) - 1] = '\0';
            gallery_photos[gallery_count].size = dirent.d_stat.st_size;
            gallery_photos[gallery_count].is_video = is_avi;
            parse_photo_date(&gallery_photos[gallery_count], &dirent.d_stat);
            gallery_count++;
        }
        memset(&dirent, 0, sizeof(dirent));
    }
    sceIoDclose(dfd);
}

static void gallery_scan_directory(void) {
    gallery_count = 0;
    if (gallery_source_tab == GALLERY_SOURCE_VITACAM) {
        // Fotos tomadas por VitaCam
        gallery_scan_folder_recursive("ux0:data/vitacam", 0);
    } else if (gallery_source_tab == GALLERY_SOURCE_PHOTO) {
        // Camara oficial PS Vita: guarda en CAMERA con subcarpetas de 2 letras (gf, kk, etc.)
        gallery_scan_folder_recursive("ux0:picture/CAMERA", 0);
    } else if (gallery_source_tab == GALLERY_SOURCE_SCREENSHOT) {
        // Capturas de pantalla: SCREENSHOT con subcarpetas de 2 letras (bb, dh, etc.)
        gallery_scan_folder_recursive("ux0:picture/SCREENSHOT", 0);
    } else { // GALLERY_SOURCE_ALL - todo junto
        gallery_scan_folder_recursive("ux0:data/vitacam", 0);
        gallery_scan_folder_recursive("ux0:picture/CAMERA", 0);
        gallery_scan_folder_recursive("ux0:picture/SCREENSHOT", 0);
    }

    if (gallery_count > 1) {
        qsort(gallery_photos, gallery_count, sizeof(GalleryPhoto), gallery_compare_names);
    }
    gallery_build_date_groups();
}

static void gallery_switch_source_tab(GallerySourceTab new_tab) {
    gallery_source_tab = new_tab;
    gallery_idx = 0;
    gallery_scroll_y = 0.0f;
    gallery_target_scroll_y = 0.0f;
    gallery_clear_selection();
    for (int i = 0; i < THUMB_POOL_SIZE; i++) {
        thumb_slots[i].photo_idx = -1;
        thumb_slots[i].is_loaded = 0;
    }
    gallery_scan_directory();
}

static void gallery_build_date_groups() {
    date_group_count = 0;
    if (gallery_count == 0) return;

    float cur_y = 0.0f;
    int cur_start = 0;

    for (int i = 0; i <= gallery_count; i++) {
        int is_new = (i == gallery_count);
        if (!is_new && i > 0) {
            if (gallery_photos[i].year != gallery_photos[i-1].year ||
                gallery_photos[i].month != gallery_photos[i-1].month ||
                gallery_photos[i].day != gallery_photos[i-1].day) {
                is_new = 1;
            }
        }

        if (is_new && i > cur_start) {
            if (date_group_count < MAX_DATE_GROUPS) {
                DateGroup *grp = &date_groups[date_group_count];
                strncpy(grp->date_title, gallery_photos[cur_start].date_label, sizeof(grp->date_title) - 1);
                grp->date_title[sizeof(grp->date_title) - 1] = '\0';
                grp->start_photo_idx = cur_start;
                grp->count = i - cur_start;
                int rows = (grp->count + GRID_COLS - 1) / GRID_COLS;
                grp->y_pos = cur_y;
                grp->height = 36.0f + (float)rows * (CARD_H + GAP_Y) + 12.0f;
                cur_y += grp->height;
                date_group_count++;
            }
            cur_start = i;
        }
    }
}

static void gallery_invalidate_thumbnails() {
    for (int i = 0; i < THUMB_POOL_SIZE; i++) {
        thumb_slots[i].photo_idx = -1;
        thumb_slots[i].last_used_frame = 0;
        thumb_slots[i].is_loaded = 0;
    }
}

static int gallery_find_or_alloc_thumb_slot(int photo_idx) {
    for (int i = 0; i < THUMB_POOL_SIZE; i++) {
        if (thumb_slots[i].photo_idx == photo_idx && thumb_slots[i].is_loaded) {
            thumb_slots[i].last_used_frame = current_render_frame;
            return i;
        }
    }
    int best_slot = -1;
    uint32_t oldest_frame = 0xFFFFFFFF;
    for (int i = 0; i < THUMB_POOL_SIZE; i++) {
        if (thumb_slots[i].last_used_frame == current_render_frame) {
            continue;
        }
        if (thumb_slots[i].photo_idx == -1) {
            best_slot = i;
            break;
        }
        if (thumb_slots[i].last_used_frame < oldest_frame) {
            oldest_frame = thumb_slots[i].last_used_frame;
            best_slot = i;
        }
    }
    return best_slot;
}

static void gallery_get_photo_rect(int photo_idx, float *out_x, float *out_y) {
    for (int g = 0; g < date_group_count; g++) {
        DateGroup *grp = &date_groups[g];
        if (photo_idx >= grp->start_photo_idx && photo_idx < grp->start_photo_idx + grp->count) {
            int local_idx = photo_idx - grp->start_photo_idx;
            int col = local_idx % GRID_COLS;
            int row = local_idx / GRID_COLS;
            *out_x = (float)GRID_START_X + (float)col * (CARD_W + GAP_X);
            *out_y = (float)GRID_START_Y + grp->y_pos + 36.0f + (float)row * (CARD_H + GAP_Y) - gallery_scroll_y;
            return;
        }
    }
    *out_x = -1000.0f;
    *out_y = -1000.0f;
}

static int gallery_get_item_at(int tx, int ty) {
    if (ty < HEADER_H || ty >= 544 - BOTTOM_BAR_H) return -1;
    for (int i = 0; i < gallery_count; i++) {
        float px, py;
        gallery_get_photo_rect(i, &px, &py);
        if (tx >= px && tx <= px + CARD_W && ty >= py && ty <= py + CARD_H) {
            return i;
        }
    }
    return -1;
}

static float gallery_get_max_scroll() {
    if (date_group_count <= 0) return 0.0f;
    DateGroup *last = &date_groups[date_group_count - 1];
    float total_content_h = (float)GRID_START_Y + last->y_pos + last->height + 80.0f;
    float view_bottom = 440.0f;
    float max_s = total_content_h - view_bottom;
    return (max_s > 0.0f) ? max_s : 0.0f;
}

static void gallery_ensure_visible(int idx) {
    if (idx < 0 || idx >= gallery_count) return;
    for (int g = 0; g < date_group_count; g++) {
        DateGroup *grp = &date_groups[g];
        if (idx >= grp->start_photo_idx && idx < grp->start_photo_idx + grp->count) {
            int local_idx = idx - grp->start_photo_idx;
            int row = local_idx / GRID_COLS;
            float canvas_y = (float)GRID_START_Y + grp->y_pos + 36.0f + (float)row * (CARD_H + GAP_Y);
            float view_top = gallery_target_scroll_y + (float)HEADER_H + 10.0f;
            float view_bot = gallery_target_scroll_y + 440.0f - CARD_H - 10.0f;
            if (canvas_y < view_top) {
                gallery_target_scroll_y = canvas_y - (float)HEADER_H - 10.0f;
            } else if (canvas_y > view_bot) {
                gallery_target_scroll_y = canvas_y - (440.0f - CARD_H - 10.0f);
            }
            float max_s = gallery_get_max_scroll();
            if (gallery_target_scroll_y < 0.0f) gallery_target_scroll_y = 0.0f;
            if (gallery_target_scroll_y > max_s) gallery_target_scroll_y = max_s;
            return;
        }
    }
}

static void video_playback_stop(void) {
    is_video_playing = 0;
    if (video_play_fd >= 0) {
        sceIoClose(video_play_fd);
        video_play_fd = -1;
    }
    video_play_frame = 0;
}

static void video_playback_start(void) {
    if (gallery_count <= 0 || gallery_idx < 0 || gallery_idx >= gallery_count) return;
    if (!gallery_photos[gallery_idx].is_video) return;

    video_playback_stop();

    video_play_fd = sceIoOpen(gallery_photos[gallery_idx].fullpath, SCE_O_RDONLY, 0);
    if (video_play_fd < 0) return;

    uint8_t hdr[256];
    int r = sceIoRead(video_play_fd, hdr, sizeof(hdr));
    if (r >= 144) {
        video_play_total_frames = *(int *)(hdr + 52);
        video_play_fps = *(int *)(hdr + 136);
        if (video_play_fps <= 0 || video_play_fps > 60) video_play_fps = 30;
        if (video_play_total_frames <= 0) video_play_total_frames = 1;
    } else {
        video_play_total_frames = 100;
        video_play_fps = 30;
    }

    sceIoLseek(video_play_fd, 2048, SCE_SEEK_SET);
    video_play_frame = 0;
    video_play_last_time = sceKernelGetProcessTimeWide();
    is_video_playing = 1;
}

static void video_playback_tick(void) {
    if (!is_video_playing || video_play_fd < 0) return;

    uint64_t now = sceKernelGetProcessTimeWide();
    uint64_t frame_interval_us = 1000000 / video_play_fps;

    if (now - video_play_last_time >= frame_interval_us) {
        video_play_last_time = now;

        uint8_t tag[8];
        int r = sceIoRead(video_play_fd, tag, 8);
        if (r < 8) {
            sceIoLseek(video_play_fd, 2048, SCE_SEEK_SET);
            video_play_frame = 0;
            return;
        }

        uint32_t chunk_len = *(uint32_t *)(tag + 4);
        if (tag[0] == '0' && tag[1] == '0' && tag[2] == 'd' && tag[3] == 'c') {
            if ((int)chunk_len <= JPEG_OUT_BUF_SIZE) {
                int read_len = sceIoRead(video_play_fd, jpeg_out_buf, chunk_len);
                if (read_len > 0) {
                    load_jpeg_mem_into_texture(jpeg_out_buf, read_len, &gallery_tex);
                    video_play_frame++;
                }
            }
            if (chunk_len & 1) {
                uint8_t pad;
                sceIoRead(video_play_fd, &pad, 1);
            }
        } else {
            sceIoLseek(video_play_fd, 2048, SCE_SEEK_SET);
            video_play_frame = 0;
        }
    }
}

static void gallery_load_current_photo() {
    video_playback_stop();

    // Reset zoom and pan for the new photo
    fullscreen_zoom = 1.0f;
    fullscreen_pan_x = 0.0f;
    fullscreen_pan_y = 0.0f;
    fullscreen_hide_ui = 0;

    if (gallery_count > 0 && gallery_idx >= 0 && gallery_idx < gallery_count) {
        load_media_into_texture(&gallery_photos[gallery_idx], &gallery_tex);
    } else {
        if (gallery_tex) {
            vita2d_free_texture(gallery_tex);
            gallery_tex = NULL;
        }
    }
}

static void gallery_open_delete_dialog(void) {
    gallery_confirm_delete = 1;
    delete_dialog_focused = 1; // Por defecto siempre Cancelar (No)
}

static void gallery_enter() {
    video_playback_stop();
    app_mode = APP_MODE_GALLERY;
    gallery_view = GALLERY_VIEW_GRID;
    gallery_confirm_delete = 0;
    delete_dialog_focused = 1;
    gallery_scroll_y = 0.0f;
    gallery_target_scroll_y = 0.0f;
    gallery_scan_directory();
    gallery_clear_selection();
    gallery_idx = 0;
    gallery_invalidate_thumbnails();
}

static void gallery_exit() {
    video_playback_stop();
    gallery_confirm_delete = 0;
    delete_dialog_focused = 1;
    gallery_clear_selection();
    update_camera_last_thumb();
    app_mode = APP_MODE_CAMERA;
}

static void gallery_delete_selected() {
    int sel_count = gallery_count_selected();
    if (sel_count == 0) {
        if (gallery_count <= 0 || gallery_idx < 0 || gallery_idx >= gallery_count) return;
        char deleted_name[64];
        strncpy(deleted_name, gallery_photos[gallery_idx].filename, sizeof(deleted_name) - 1);
        deleted_name[sizeof(deleted_name) - 1] = '\0';

        if (gallery_photos[gallery_idx].is_video) {
            char thumb_path[256];
            strncpy(thumb_path, gallery_photos[gallery_idx].fullpath, sizeof(thumb_path) - 1);
            int tlen = strlen(thumb_path);
            if (tlen > 4) {
                strcpy(thumb_path + tlen - 4, ".jpg");
                sceIoRemove(thumb_path);
            }
        }
        sceIoRemove(gallery_photos[gallery_idx].fullpath);
        snprintf(status_msg, sizeof(status_msg), "ELEMENTO ELIMINADO: %s", deleted_name);
    } else {
        for (int i = 0; i < gallery_count; i++) {
            if (gallery_selected[i]) {
                if (gallery_photos[i].is_video) {
                    char thumb_path[256];
                    strncpy(thumb_path, gallery_photos[i].fullpath, sizeof(thumb_path) - 1);
                    int tlen = strlen(thumb_path);
                    if (tlen > 4) {
                        strcpy(thumb_path + tlen - 4, ".jpg");
                        sceIoRemove(thumb_path);
                    }
                }
                sceIoRemove(gallery_photos[i].fullpath);
                gallery_selected[i] = 0;
            }
        }
        snprintf(status_msg, sizeof(status_msg), "ELIMINADOS %d ELEMENTOS", sel_count);
    }

    status_msg_color = RGBA8(255, 140, 40, 255);
    status_msg_timer = 150;
    gallery_confirm_delete = 0;

    gallery_scan_directory();
    if (gallery_idx >= gallery_count) {
        gallery_idx = gallery_count > 0 ? gallery_count - 1 : 0;
    }
    gallery_clear_selection();
    gallery_invalidate_thumbnails();
    update_camera_last_thumb();

    float max_s = gallery_get_max_scroll();
    if (gallery_target_scroll_y > max_s) gallery_target_scroll_y = max_s;
    if (gallery_scroll_y > max_s) gallery_scroll_y = max_s;

    if (gallery_view == GALLERY_VIEW_FULLSCREEN) {
        if (gallery_count > 0) {
            gallery_load_current_photo();
        } else {
            gallery_view = GALLERY_VIEW_GRID;
        }
    }
}


// =========================================================================
// Procesamiento Táctil en Modo Cámara (Shutter, Flip, Zoom, Slider, Pro Bar)
// =========================================================================
static void handle_camera_touch() {
    SceTouchData tdata;
    memset(&tdata, 0, sizeof(tdata));
    int ret = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &tdata, 1);
    if (ret < 0) return;

    if (tdata.reportNum > 0) {
        int tx = tdata.report[0].x / 2;
        int ty = tdata.report[0].y / 2;

        if (!touch_active) {
            touch_active = 1;
            touch_start_x = tx;
            touch_start_y = ty;
            touch_prev_x = tx;
            touch_prev_y = ty;
            touch_hold_frames = 0;
            touch_is_dragging = 0;
            cam_slider_drag_accum = 0.0f;
        } else {
            touch_hold_frames++;
            int dx = tx - touch_prev_x;
            if (abs(tx - touch_start_x) > 6 || abs(ty - touch_start_y) > 6) {
                touch_is_dragging = 1;
            }

            // Arrastre horizontal sobre el slider de precisión (EV y ZOOM continuos)
            if (cam_slider_open && !is_pro_param_discrete(active_cam_param) && ty >= 390 && ty <= 485 && tx >= 138 && tx <= 822) {
                cam_slider_drag_accum += (float)dx;
                if (cam_slider_drag_accum <= -8.0f) {
                    cam_slider_drag_accum += 8.0f;
                    int p_idx = pro_bar_params[active_cam_param];
                    CameraParam *p = &params[p_idx];
                    if (p->current_idx < p->num_options - 1) {
                        p->current_idx++;
                        apply_and_query_param(cam_dev, p_idx);
                    }
                    slider_focused = 1;
                } else if (cam_slider_drag_accum >= 8.0f) {
                    cam_slider_drag_accum -= 8.0f;
                    int p_idx = pro_bar_params[active_cam_param];
                    CameraParam *p = &params[p_idx];
                    if (p->current_idx > 0) {
                        p->current_idx--;
                        apply_and_query_param(cam_dev, p_idx);
                    }
                    slider_focused = 1;
                }
            }

            touch_prev_x = tx;
            touch_prev_y = ty;
        }
    } else {
        if (touch_active) {
            int tx = touch_start_x;
            int ty = touch_start_y;

            if (!touch_is_dragging) {
                // 1. Botón de Obturador (Shutter) - Barra Lateral Derecha (x: 842 - 960)
                if (tx >= 842 && tx <= 960 && ty >= 210 && ty <= 335) {
                    trigger_camera_shot();
                }
                // 2. Botón Burbuja de Galería - Barra Lateral Izquierda (x: 0 - 118, abajo)
                else if (tx >= 0 && tx <= 118 && ty >= 420 && ty <= 544) {
                    gallery_enter();
                }
                // 3. Botón Flip Cámara - Barra Lateral Derecha (x: 842 - 960, arriba)
                else if (tx >= 842 && tx <= 960 && ty >= 10 && ty <= 75) {
                    switch_camera_device();
                }
                // 4. Botón Cuadrícula / Grid - Barra Lateral Izquierda (x: 0 - 118, arriba)
                else if (tx >= 0 && tx <= 118 && ty >= 10 && ty <= 75) {
                    show_grid = !show_grid;
                }
                // 5. Botón Flash Frontal (44x44 exacto como los 4 puntos) - Barra Lateral Izquierda (x: 0 - 118, y: 76 - 134)
                else if (cam_dev == SCE_CAMERA_DEVICE_FRONT && tx >= 0 && tx <= 118 && ty >= 76 && ty <= 134) {
                    front_flash_mode = (front_flash_mode + 1) % FRONT_FLASH_COUNT;
                    if (front_flash_mode == FRONT_FLASH_OFF) {
                        snprintf(status_msg, sizeof(status_msg), "Flash Frontal: Desactivado");
                        status_msg_color = RGBA8(180, 190, 210, 255);
                    } else if (front_flash_mode == FRONT_FLASH_SCREEN) {
                        snprintf(status_msg, sizeof(status_msg), "Flash Frontal: Pantalla Completa (Disparo Blanco)");
                        status_msg_color = RGBA8(255, 215, 60, 255);
                    } else {
                        snprintf(status_msg, sizeof(status_msg), "Flash Frontal: Anillo de Luz (Marco Iluminado)");
                        status_msg_color = RGBA8(255, 255, 255, 255);
                    }
                    status_msg_timer = 90;
                }
                // 5b. Botón Marca de Agua (Watermark) - Barra Lateral Izquierda (x: 0 - 118, y: 135 - 195)
                else if (tx >= 0 && tx <= 118 && ty >= 135 && ty <= 195) {
                    watermark_enabled = !watermark_enabled;
                    snprintf(status_msg, sizeof(status_msg), "Marca de Agua: %s (Tomada con PS Vita)", watermark_enabled ? "Activada" : "Desactivada");
                    status_msg_color = watermark_enabled ? RGBA8(0, 220, 255, 255) : RGBA8(180, 190, 210, 255);
                    status_msg_timer = 90;
                }
                // 6. Botón Grabación de Video - Barra Lateral Derecha (x: 842 - 960, abajo del obturador)
                else if (tx >= 842 && tx <= 960 && ty >= 345 && ty <= 425) {
                    if (!is_recording_video) {
                        video_record_start();
                    } else {
                        video_record_stop();
                    }
                }
                // 5. Selector flotante de opciones discretas (FPS, ISO, WB, EFECTO)
                else if (cam_slider_open && is_pro_param_discrete(active_cam_param) && ty >= 400 && ty <= 480 && tx >= 138 && tx <= 822) {
                    int cur_p_idx = pro_bar_params[active_cam_param];
                    CameraParam *cur_p = &params[cur_p_idx];
                    int n_opts = cur_p->num_options;

                    float badge_w = 48.0f;
                    float badge_h = 44.0f;
                    float gap = 10.0f;
                    if (active_cam_param == 4) { badge_w = 46.0f; badge_h = 40.0f; gap = 6.0f; }
                    else if (active_cam_param == 1) { badge_w = 46.0f; badge_h = 46.0f; gap = 14.0f; }
                    else if (active_cam_param == 0) { badge_w = 54.0f; badge_h = 44.0f; gap = 10.0f; }
                    else if (active_cam_param == 3) { badge_w = 56.0f; badge_h = 44.0f; gap = 8.0f; }

                    float total_content_w = (float)n_opts * badge_w + (float)(n_opts - 1) * gap;
                    float pill_pad_x = 14.0f;
                    float pill_w = total_content_w + pill_pad_x * 2.0f;
                    float pill_h = badge_h + 14.0f;
                    float pill_x = 480.0f - pill_w * 0.5f;
                    float pill_y = 486.0f - pill_h - 10.0f;

                    if (tx >= pill_x && tx <= pill_x + pill_w && ty >= pill_y && ty <= pill_y + pill_h) {
                        float cur_bx = pill_x + pill_pad_x;
                        for (int j = 0; j < n_opts; j++) {
                            if (tx >= cur_bx - 4.0f && tx <= cur_bx + badge_w + 4.0f) {
                                cur_p->current_idx = j;
                                apply_and_query_param(cam_dev, cur_p_idx);
                                slider_focused = 1;
                                break;
                            }
                            cur_bx += badge_w + gap;
                        }
                    }
                }
                // 6. Barra Pro de Parámetros Inferior (ISO, SPEED, EV, WB, EFECTO, ZOOM)
                else if (ty >= 480 && ty <= 544 && tx >= 138 && tx <= 822) {
                    float item_w = (822.0f - 138.0f) / (float)PRO_BAR_COUNT;
                    int tapped_idx = (int)((float)(tx - 138) / item_w);
                    if (tapped_idx >= 0 && tapped_idx < PRO_BAR_COUNT) {
                        if (active_cam_param == tapped_idx && cam_slider_open) {
                            // Al tocar el mismo botón activo se deselecciona y oculta el selector
                            cam_slider_open = 0;
                            slider_focused = 0;
                        } else {
                            active_cam_param = tapped_idx;
                            cam_slider_open = 1;
                            slider_focused = 1;
                        }
                    }
                }
                // 7. Toque en el Viewfinder: solo cerrar si estaba abierto (NUNCA reabrir al tocar visor)
                else if (tx >= 118 && tx <= 842 && ty <= 480) {
                    if (cam_slider_open) {
                        cam_slider_open = 0;
                        slider_focused = 0;
                    }
                }
            }

            touch_active = 0;
            touch_is_dragging = 0;
            touch_hold_frames = 0;
        }
    }
}

// =========================================================================
// Procesamiento Táctil de la Pantalla Frontal (Tap, Long Press, Drag Scroll)
// =========================================================================
static void handle_gallery_touch() {
    SceTouchData tdata;
    memset(&tdata, 0, sizeof(tdata));
    int ret = sceTouchPeek(SCE_TOUCH_PORT_FRONT, &tdata, 1);
    if (ret < 0) return;

    if (tdata.reportNum > 0) {
        if (gallery_view == GALLERY_VIEW_FULLSCREEN && tdata.reportNum >= 2) {
            float p0_x = (float)tdata.report[0].x / 2.0f;
            float p0_y = (float)tdata.report[0].y / 2.0f;
            float p1_x = (float)tdata.report[1].x / 2.0f;
            float p1_y = (float)tdata.report[1].y / 2.0f;
            float cur_dist = sqrtf((p0_x - p1_x) * (p0_x - p1_x) + (p0_y - p1_y) * (p0_y - p1_y));

            if (touch_prev_pinch_dist > 10.0f) {
                float diff = cur_dist - touch_prev_pinch_dist;
                fullscreen_zoom += diff * 0.008f;
                if (fullscreen_zoom < 1.0f) {
                    fullscreen_zoom = 1.0f;
                    fullscreen_pan_x = 0.0f;
                    fullscreen_pan_y = 0.0f;
                }
                if (fullscreen_zoom > 5.0f) fullscreen_zoom = 5.0f;
            }
            touch_prev_pinch_dist = cur_dist;
            touch_is_dragging = 1;
            touch_active = 1;
            return;
        } else {
            touch_prev_pinch_dist = 0.0f;
        }

        int tx = tdata.report[0].x / 2;
        int ty = tdata.report[0].y / 2;

        if (!touch_active) {
            touch_active = 1;
            touch_start_x = tx;
            touch_start_y = ty;
            touch_prev_x = tx;
            touch_prev_y = ty;
            touch_hold_frames = 0;
            touch_is_dragging = 0;
            touch_long_fired = 0;
            touch_item_down = (gallery_view == GALLERY_VIEW_GRID && !gallery_show_wifi_modal && !gallery_confirm_delete && !gallery_show_info_modal) ? gallery_get_item_at(tx, ty) : -1;
        } else {
            touch_hold_frames++;
            int dy = ty - touch_prev_y;
            int dx = tx - touch_prev_x;
            int total_dx = abs(tx - touch_start_x);
            int total_dy = abs(ty - touch_start_y);

            if (gallery_view == GALLERY_VIEW_GRID) {
                if (total_dy > 4 || total_dx > 4) {
                    touch_is_dragging = 1;
                    touch_item_down = -1; // Al arrastrar, queda completamente anulado cualquier tap o selección
                }

                if (touch_is_dragging) {
                    gallery_target_scroll_y -= (float)dy * 1.35f;
                    float max_s = gallery_get_max_scroll();
                    if (gallery_target_scroll_y < 0.0f) gallery_target_scroll_y = 0.0f;
                    if (gallery_target_scroll_y > max_s) gallery_target_scroll_y = max_s;
                } else {
                    // Long press de 28 frames (~450ms) para alternar selección sin soltar el dedo
                    if (touch_hold_frames >= 28 && !touch_long_fired && touch_item_down >= 0) {
                        touch_long_fired = 1;
                        gallery_idx = touch_item_down;
                        gallery_toggle_select(touch_item_down);
                    }
                }
            } else if (gallery_view == GALLERY_VIEW_FULLSCREEN) {
                if (total_dx > 5 || total_dy > 5) {
                    touch_is_dragging = 1;
                }
                if (touch_is_dragging && fullscreen_zoom > 1.05f) {
                    fullscreen_pan_x += (float)dx;
                    fullscreen_pan_y += (float)dy;
                }
            }
            touch_prev_x = tx;
            touch_prev_y = ty;
        }
    } else {
        if (touch_active) {
            int tx = touch_start_x;
            int ty = touch_start_y;

            if (gallery_show_wifi_modal) {
                touch_item_down = -1;
                // Botones de acción inferiores: y=[415, 485]
                // Botón 0 (Activar / Apagar): x=[440, 565]
                // Botón 1 (Nuevo PIN):       x=[566, 688]
                // Botón 2 (Cerrar):          x=[689, 825]
                if (ty >= 415 && ty <= 485) {
                    if (tx >= 440 && tx <= 565) {
                        wifi_dialog_focused = 0;
                        webserver_set_enabled(!webserver_is_enabled());
                    } else if (tx >= 566 && tx <= 688) {
                        wifi_dialog_focused = 1;
                        char new_pwd[8];
                        uint64_t tick = sceKernelGetProcessTimeWide();
                        snprintf(new_pwd, sizeof(new_pwd), "%04d", (int)((tick % 9000) + 1000));
                        webserver_set_password(new_pwd);
                    } else if (tx >= 689 && tx <= 825) {
                        wifi_dialog_focused = 2;
                        gallery_show_wifi_modal = 0;
                        modal_just_closed = 1;
                        x_hold_frames = 0;
                        x_long_fired = 1;
                    }
                } else if (tx >= 140 && tx <= 435 && ty >= 400 && ty <= 475) {
                    // Tocar la caja del PIN debajo del QR para regenerar PIN
                    wifi_dialog_focused = 1;
                    char new_pwd[8];
                    uint64_t tick = sceKernelGetProcessTimeWide();
                    snprintf(new_pwd, sizeof(new_pwd), "%04d", (int)((tick % 9000) + 1000));
                    webserver_set_password(new_pwd);
                } else if (tx < 110 || tx > 850 || ty < 50 || ty > 495) {
                    gallery_show_wifi_modal = 0;
                    modal_just_closed = 1;
                    x_hold_frames = 0;
                    x_long_fired = 1;
                }
            } else if (gallery_confirm_delete) {
                touch_item_down = -1;
                // Modal centrado: mw=480 mh=200 mx=240 my=172 btn_y=316 h=42
                // Eliminar: x=[270,465] y=[310,365]   Cancelar: x=[490,685] y=[310,365]
                if (ty >= 310 && ty <= 365) {
                    if (tx >= 260 && tx <= 470) {
                        delete_dialog_focused = 0;
                        gallery_delete_selected();
                        modal_just_closed = 1;
                        x_hold_frames = 0;
                        x_long_fired = 1;
                    } else if (tx >= 475 && tx <= 690) {
                        delete_dialog_focused = 1;
                        gallery_confirm_delete = 0;
                        modal_just_closed = 1;
                        x_hold_frames = 0;
                        x_long_fired = 1;
                    }
                }
            } else if (gallery_show_info_modal) {
                touch_item_down = -1;
                gallery_show_info_modal = 0;
                modal_just_closed = 1;
                x_hold_frames = 0;
                x_long_fired = 1;
            } else if (gallery_view == GALLERY_VIEW_GRID) {
                // Solo ejecutar acción si NO fue arrastre y NO disparó long-press
                if (!touch_is_dragging && !touch_long_fired) {
                    if (ty <= HEADER_H) {
                        if (gallery_selection_mode) {
                            if (tx <= 130) {
                                gallery_clear_selection();
                            } else if (tx >= 800) {
                                if (gallery_count_selected() > 0) gallery_open_delete_dialog();
                            }
                        } else {
                            if (tx <= 130) {
                                gallery_exit();
                            } else if (tx >= 760 && tx <= 808) {
                                gallery_show_info_modal = 1;
                            }
                        }
                    } else if (ty >= 438 && ty <= 488 && !gallery_selection_mode) {
                        // Pestañas Flotantes Inferiores
                        if (tx >= 230 && tx <= 730) {
                            if (tx <= 350) {
                                gallery_switch_source_tab(GALLERY_SOURCE_VITACAM);
                            } else if (tx <= 475) {
                                gallery_switch_source_tab(GALLERY_SOURCE_PHOTO);
                            } else if (tx <= 615) {
                                gallery_switch_source_tab(GALLERY_SOURCE_SCREENSHOT);
                            } else {
                                gallery_switch_source_tab(GALLERY_SOURCE_ALL);
                            }
                        }
                    } else if (ty >= 544 - BOTTOM_BAR_H) {
                        if (gallery_selection_mode) {
                            if (tx >= 450 && tx <= 640) {
                                if (gallery_count_selected() > 0) gallery_open_delete_dialog();
                            } else if (tx > 640 && tx <= 800) {
                                gallery_clear_selection();
                            } else if (tx > 800) {
                                gallery_toggle_select(gallery_idx);
                            }
                        } else {
                            if (tx <= 160) {
                                gallery_show_wifi_modal = 1;
                            } else if (tx >= 450 && tx <= 640) {
                                if (gallery_count > 0) gallery_open_delete_dialog();
                            } else if (tx > 640 && tx <= 800) {
                                gallery_exit();
                            } else if (tx > 800) {
                                if (gallery_count > 0) {
                                    gallery_view = GALLERY_VIEW_FULLSCREEN;
                                    gallery_load_current_photo();
                                }
                            }
                        }
                    } else if (touch_item_down >= 0 && touch_item_down < gallery_count) {
                        if (gallery_selection_mode) {
                            gallery_toggle_select(touch_item_down);
                            gallery_idx = touch_item_down;
                        } else {
                            gallery_idx = touch_item_down;
                            gallery_view = GALLERY_VIEW_FULLSCREEN;
                            gallery_load_current_photo();
                        }
                    }
                }
            } else if (gallery_view == GALLERY_VIEW_FULLSCREEN) {
                touch_prev_pinch_dist = 0.0f;
                int swipe_dx = touch_prev_x - touch_start_x;
                int swipe_dy = abs(touch_prev_y - touch_start_y);

                if (fullscreen_zoom <= 1.05f && abs(swipe_dx) > 40 && swipe_dy < 120) {
                    // Deslizamiento horizontal detectado
                    if (swipe_dx < -40) {
                        // Deslizar hacia la izquierda -> Siguiente foto
                        if (gallery_count > 0) {
                            gallery_idx++;
                            if (gallery_idx >= gallery_count) gallery_idx = 0;
                            gallery_load_current_photo();
                        }
                    } else if (swipe_dx > 40) {
                        // Deslizar hacia la derecha -> Foto anterior
                        if (gallery_count > 0) {
                            gallery_idx--;
                            if (gallery_idx < 0) gallery_idx = gallery_count - 1;
                            gallery_load_current_photo();
                        }
                    }
                } else if (!touch_is_dragging) {
                    uint64_t now = sceKernelGetProcessTimeWide();
                    if (now - touch_last_tap_time < 350000) {
                        // Doble toque rápido: alternar zoom
                        touch_last_tap_time = 0;
                        if (fullscreen_zoom > 1.2f) {
                            fullscreen_zoom = 1.0f;
                            fullscreen_pan_x = 0.0f;
                            fullscreen_pan_y = 0.0f;
                        } else {
                            fullscreen_zoom = 2.5f;
                            fullscreen_pan_x = (480.0f - (float)tx) * 1.5f;
                            fullscreen_pan_y = (272.0f - (float)ty) * 1.5f;
                        }
                    } else {
                        touch_last_tap_time = now;
                        if (fullscreen_hide_ui) {
                            fullscreen_hide_ui = 0; // Mostrar UI al tocar en cualquier lugar
                        } else {
                            if (ty <= HEADER_H && tx <= 160) {
                                video_playback_stop();
                                gallery_view = GALLERY_VIEW_GRID;
                                gallery_ensure_visible(gallery_idx);
                            } else if (ty <= HEADER_H && tx >= 800) {
                                if (gallery_count > 0) {
                                    video_playback_stop();
                                    gallery_open_delete_dialog();
                                }
                            } else if (ty >= 544 - BOTTOM_BAR_H) {
                                if (tx <= 200) {
                                    if (gallery_count > 0) {
                                        gallery_idx--;
                                        if (gallery_idx < 0) gallery_idx = gallery_count - 1;
                                        gallery_load_current_photo();
                                    }
                                } else if (tx >= 200 && tx <= 400) {
                                    if (gallery_count > 0) {
                                        gallery_idx++;
                                        if (gallery_idx >= gallery_count) gallery_idx = 0;
                                        gallery_load_current_photo();
                                    }
                                } else if (tx >= 400 && tx <= 650) {
                                    if (gallery_photos[gallery_idx].is_video) {
                                        if (!is_video_playing) video_playback_start();
                                        else video_playback_stop();
                                    } else {
                                        gallery_view = GALLERY_VIEW_GRID;
                                        gallery_ensure_visible(gallery_idx);
                                    }
                                } else if (tx > 650) {
                                    if (gallery_count > 0) {
                                        video_playback_stop();
                                        gallery_open_delete_dialog();
                                    }
                                }
                            } else {
                                // Tocar en el centro
                                if (gallery_count > 0 && gallery_photos[gallery_idx].is_video) {
                                    if (!is_video_playing) video_playback_start();
                                    else video_playback_stop();
                                } else {
                                    fullscreen_hide_ui = 1;
                                }
                            }
                        }
                    }
                }
            }

            touch_active = 0;
            touch_hold_frames = 0;
            touch_is_dragging = 0;
            touch_long_fired = 0;
            touch_item_down = -1;
        }
    }
}


// =========================================================================
// Renderizado de Marca de Agua Inteligente en Imagen RAW (YUV420 Planar)
// =========================================================================
static void draw_char_yuv(uint8_t *y_plane, int img_w, int img_h, int x0, int y0, char c, int scale, uint8_t fg_y, int shadow_mode) {
    if (c < 32 || c > 126) c = ' ';
    const uint8_t *bitmap = font8x8_basic[c - 32];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = bitmap[row];
        for (int col = 0; col < 8; col++) {
            // Orden MSB a LSB exacto (col 0 es el bit más significativo 0x80)
            if (bits & (0x80 >> col)) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x0 + col * scale + sx;
                        int py = y0 + row * scale + sy;
                        if (px >= 0 && px < img_w && py >= 0 && py < img_h) {
                            // Sombra o halo de contraste según el fondo
                            if (shadow_mode > 0) { // Fondo oscuro -> sombra negra
                                int spx = px + 1, spy = py + 1;
                                if (spx < img_w && spy < img_h) {
                                    y_plane[spy * img_w + spx] = (uint8_t)((y_plane[spy * img_w + spx] * 2) / 7);
                                }
                            } else if (shadow_mode < 0) { // Fondo claro -> halo claro
                                int spx = px + 1, spy = py + 1;
                                if (spx < img_w && spy < img_h) {
                                    y_plane[spy * img_w + spx] = (uint8_t)(180 + (y_plane[spy * img_w + spx] / 4));
                                }
                            }
                            y_plane[py * img_w + px] = fg_y;
                        }
                    }
                }
            }
        }
    }
}

static void draw_string_yuv(uint8_t *y_plane, int img_w, int img_h, int x, int y, const char *str, int scale, uint8_t fg_y, int shadow_mode) {
    int cur_x = x;
    while (*str) {
        draw_char_yuv(y_plane, img_w, img_h, cur_x, y, *str, scale, fg_y, shadow_mode);
        cur_x += 8 * scale;
        str++;
    }
}

static void draw_logo_yuv(uint8_t *y_plane, int img_w, int img_h, int x0, int y0, uint8_t fg_y, int shadow_mode) {
    for (int row = 0; row < PSVITA_LOGO_H; row++) {
        for (int col = 0; col < PSVITA_LOGO_W; col++) {
            uint8_t a = psvita_logo_alpha[row][col];
            if (a > 12) {
                int px = x0 + col;
                int py = y0 + row;
                if (px >= 0 && px < img_w && py >= 0 && py < img_h) {
                    // Contraste sutil (sombra o halo)
                    if (shadow_mode > 0) {
                        int spx = px + 1, spy = py + 1;
                        if (spx < img_w && spy < img_h) {
                            uint8_t bg = y_plane[spy * img_w + spx];
                            y_plane[spy * img_w + spx] = (uint8_t)((bg * (255 - a / 2)) / 255);
                        }
                    } else if (shadow_mode < 0) {
                        int spx = px + 1, spy = py + 1;
                        if (spx < img_w && spy < img_h) {
                            uint8_t bg = y_plane[spy * img_w + spx];
                            y_plane[spy * img_w + spx] = (uint8_t)((bg * (255 - a / 3) + 245 * (a / 3)) / 255);
                        }
                    }
                    // Mezcla antialiased suave en canal Y
                    uint8_t cur_val = y_plane[py * img_w + px];
                    y_plane[py * img_w + px] = (uint8_t)(((uint32_t)cur_val * (255 - a) + (uint32_t)fg_y * a) / 255);
                }
            }
        }
    }
}

static void apply_watermark_to_snap(uint8_t *snap_yuv, int img_w, int img_h, const SceDateTime *t) {
    if (!snap_yuv) return;
    uint8_t *y_plane = snap_yuv;

    // Coordenadas fijas pegadas al borde inferior izquierdo (alineación flush-left perfecta)
    int wm_x = 24;
    int wm_y = img_h - 40; // 440 px en 640x480

    // Muestreo inteligente de luminosidad ambiental de la foto en la esquina inferior izquierda
    uint32_t sum_y = 0;
    int samples = 0;
    for (int sy = wm_y - 2; sy < img_h - 6; sy += 3) {
        for (int sx = wm_x; sx < wm_x + 140 && sx < img_w; sx += 3) {
            sum_y += y_plane[sy * img_w + sx];
            samples++;
        }
    }
    uint8_t avg_y = (samples > 0) ? (uint8_t)(sum_y / samples) : 100;
    int is_bright_bg = (avg_y >= 128);

    // Color adaptativo: Blanco brillante en fondos oscuros, Negro profundo en fondos claros
    uint8_t fg_y = is_bright_bg ? 18 : 252;
    int shadow_mode = is_bright_bg ? -1 : 1;

    // 1. Estampar Logo Oficial de PS VITA (sin la palabra playstation)
    draw_logo_yuv(y_plane, img_w, img_h, wm_x, wm_y, fg_y, shadow_mode);

    // 2. Estampar Fecha y Hora sutilmente abajo del logo, perfectamente alineado a la izquierda
    char time_str[48];
    if (t && t->year > 0) {
        snprintf(time_str, sizeof(time_str), "%04d.%02d.%02d  %02d:%02d",
                 t->year, t->month, t->day, t->hour, t->minute);
        draw_string_yuv(y_plane, img_w, img_h, wm_x, wm_y + PSVITA_LOGO_H + 4, time_str, 1, fg_y, shadow_mode);
    }
}

// =========================================================================
// Captura y Codificación JPEG Nativa
// =========================================================================
static void capture_and_save_photo() {
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/VitaCam", 0777);

    SceDateTime rtc_time;
    memset(&rtc_time, 0, sizeof(rtc_time));
    int rtc_ret = sceRtcGetCurrentClockLocalTime(&rtc_time);

    char filename[256];
    if (rtc_ret >= 0) {
        snprintf(filename, sizeof(filename), "ux0:data/VitaCam/photo_%04d%02d%02d_%02d%02d%02d.jpg",
            rtc_time.year, rtc_time.month, rtc_time.day,
            rtc_time.hour, rtc_time.minute, rtc_time.second);
    } else {
        static int photo_seq = 1;
        snprintf(filename, sizeof(filename), "ux0:data/VitaCam/photo_%04d.jpg", photo_seq++);
    }

    SceIoStat check_stat;
    if (sceIoGetstat(filename, &check_stat) >= 0) {
        for (int suffix = 1; suffix < 100; suffix++) {
            char candidate[256];
            snprintf(candidate, sizeof(candidate), "ux0:data/VitaCam/photo_%04d%02d%02d_%02d%02d%02d_%02d.jpg",
                rtc_time.year, rtc_time.month, rtc_time.day,
                rtc_time.hour, rtc_time.minute, rtc_time.second, suffix);
            if (sceIoGetstat(candidate, &check_stat) < 0) {
                strncpy(filename, candidate, sizeof(filename) - 1);
                filename[sizeof(filename) - 1] = '\0';
                break;
            }
        }
    }

    // Aplicar Marca de Agua (Watermark) si está habilitada
    if (watermark_enabled) {
        apply_watermark_to_snap((uint8_t *)snap_buf, CAM_WIDTH, CAM_HEIGHT, (rtc_ret >= 0) ? &rtc_time : NULL);
    }

    int jpeg_size = 0;
    const char *encoder_type = "UNKNOWN";

    // Intento 1: Hardware ISP
    int hw_ctx_size = sceJpegEncoderGetContextSize();
    int hw_init_ret = -1, hw_header_ret = -1, hw_ratio_ret = -1, hw_encode_ret = -1, hw_end_ret = -1;

    if (hw_ctx_size > 0) {
        void *hw_ctx = malloc(hw_ctx_size);
        if (hw_ctx) {
            SceJpegEncoderInitParam init_param;
            memset(&init_param, 0, sizeof(init_param));
            init_param.size = sizeof(init_param);
            init_param.inWidth = CAM_WIDTH;
            init_param.inHeight = CAM_HEIGHT;
            init_param.pixelFormat = SCE_JPEGENC_PIXELFORMAT_YCBCR420;
            init_param.outBuffer = jpeg_out_buf;
            init_param.outSize = JPEG_OUT_BUF_SIZE;
            init_param.option = SCE_JPEGENC_INIT_PARAM_OPTION_LPDDR2_MEMORY;

            hw_init_ret = sceJpegEncoderInitWithParam(hw_ctx, &init_param);
            if (hw_init_ret >= 0) {
                hw_header_ret = sceJpegEncoderSetHeaderMode(hw_ctx, SCE_JPEGENC_HEADER_MODE_JPEG);
                hw_ratio_ret = sceJpegEncoderSetCompressionRatio(hw_ctx, 64);
                hw_encode_ret = sceJpegEncoderEncode(hw_ctx, snap_buf);
                hw_end_ret = sceJpegEncoderEnd(hw_ctx);

                if (hw_encode_ret > 0) {
                    jpeg_size = hw_encode_ret;
                    encoder_type = "SceJpegEnc (HW)";
                }
            }
            free(hw_ctx);
        }
    }

    // Intento 2: SceJpegEncArm fallback
    int arm_init_ret = -1, arm_header_ret = -1, arm_ratio_ret = -1, arm_encode_ret = -1, arm_end_ret = -1;
    if (jpeg_size <= 0) {
        SceSize arm_ctx_size = sceJpegArmEncoderGetContextSize();
        if (arm_ctx_size > 0) {
            void *arm_ctx = malloc(arm_ctx_size);
            if (arm_ctx) {
                arm_init_ret = sceJpegArmEncoderInit(
                    arm_ctx, 
                    CAM_WIDTH, 
                    CAM_HEIGHT, 
                    SCE_JPEGENCARM_PIXELFORMAT_YCBCR420, 
                    jpeg_out_buf, 
                    JPEG_OUT_BUF_SIZE
                );
                if (arm_init_ret >= 0) {
                    arm_header_ret = sceJpegArmEncoderSetHeaderMode(arm_ctx, SCE_JPEGENCARM_HEADER_MODE_JPEG);
                    arm_ratio_ret = sceJpegArmEncoderSetCompressionRatio(arm_ctx, 64);
                    arm_encode_ret = sceJpegArmEncoderEncode(arm_ctx, snap_buf);
                    arm_end_ret = sceJpegArmEncoderEnd(arm_ctx);

                    if (arm_encode_ret > 0) {
                        jpeg_size = arm_encode_ret;
                        encoder_type = "SceJpegEncArm (NEON)";
                    }
                }
                free(arm_ctx);
            }
        }
    }

    if (jpeg_size > 0) {
        SceUID fd = sceIoOpen(filename, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
        if (fd >= 0) {
            int written = sceIoWrite(fd, jpeg_out_buf, jpeg_size);
            sceIoClose(fd);

            const char *bname = strrchr(filename, '/');
            bname = bname ? bname + 1 : filename;

            if (written == jpeg_size) {
                snprintf(status_msg, sizeof(status_msg), "FOTO GUARDADA (%d KB)", jpeg_size / 1024);
                status_msg_color = RGBA8(60, 255, 120, 255);
                status_msg_timer = 120;
                if (cam_last_thumb_tex) {
                    load_jpeg_into_thumb_texture(filename, cam_last_thumb_tex);
                }
                gallery_scan_directory();
            } else {
                snprintf(status_msg, sizeof(status_msg), "ERROR IO: Escritos %d de %d bytes", written, jpeg_size);
                status_msg_color = RGBA8(255, 60, 60, 255);
                status_msg_timer = 200;
            }
        } else {
            snprintf(status_msg, sizeof(status_msg), "ERROR IO: sceIoOpen fallo (0x%08X)", fd);
            status_msg_color = RGBA8(255, 60, 60, 255);
            status_msg_timer = 200;
        }
    } else {
        snprintf(status_msg, sizeof(status_msg), "HW(Init:0x%X Enc:0x%X) | ARM(Init:0x%X Enc:0x%X)",
            hw_init_ret, hw_encode_ret, arm_init_ret, arm_encode_ret);
        status_msg_color = RGBA8(255, 80, 80, 255);
        status_msg_timer = 250;
    }
}

// ── Grabación de Video AVI MJPEG en Tiempo Real & Audio ───────────────────
static void video_record_start(void) {
    if (is_recording_video) return;
    if (!video_enc_buf) {
        video_enc_buf = malloc(JPEG_OUT_BUF_SIZE);
    }
    if (!video_raw_buf) {
        video_raw_buf = malloc(FRAME_YUV420_SIZE);
    }
    if (!video_arm_ctx) {
        SceSize arm_ctx_size = sceJpegArmEncoderGetContextSize();
        if (arm_ctx_size > 0) {
            video_arm_ctx = malloc(arm_ctx_size);
        }
    }

    SceDateTime rtc_time;
    memset(&rtc_time, 0, sizeof(rtc_time));
    sceRtcGetCurrentClockLocalTime(&rtc_time);
    snprintf(current_video_path, sizeof(current_video_path), "ux0:data/VitaCam/VID_%04d%02d%02d_%02d%02d%02d.avi",
        rtc_time.year, rtc_time.month, rtc_time.day,
        rtc_time.hour, rtc_time.minute, rtc_time.second);

    video_fd = sceIoOpen(current_video_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (video_fd < 0) {
        snprintf(status_msg, sizeof(status_msg), "ERROR: No se pudo crear archivo de video");
        status_msg_color = RGBA8(255, 60, 60, 255);
        status_msg_timer = 120;
        return;
    }

    // Inicializar puerto de audio del micrófono integrado (PCM 16-bit 16kHz)
    audio_port = sceAudioInOpenPort(SCE_AUDIO_IN_PORT_TYPE_VOICE, AUDIO_GRAIN, AUDIO_SAMPLE_RATE, SCE_AUDIO_IN_PARAM_FORMAT_S16_MONO);
    video_audio_bytes = 0;
    video_audio_samples = 0;

    // Guardar miniatura JPEG inicial para visualización en galería
    char thumb_path[256];
    snprintf(thumb_path, sizeof(thumb_path), "ux0:data/VitaCam/VID_%04d%02d%02d_%02d%02d%02d.jpg",
        rtc_time.year, rtc_time.month, rtc_time.day,
        rtc_time.hour, rtc_time.minute, rtc_time.second);
    if (cam_buf && jpeg_out_buf && video_arm_ctx) {
        if (sceJpegArmEncoderInit(video_arm_ctx, CAM_WIDTH, CAM_HEIGHT, SCE_JPEGENCARM_PIXELFORMAT_YCBCR420, jpeg_out_buf, JPEG_OUT_BUF_SIZE) >= 0) {
            sceJpegArmEncoderSetHeaderMode(video_arm_ctx, SCE_JPEGENCARM_HEADER_MODE_JPEG);
            sceJpegArmEncoderSetCompressionRatio(video_arm_ctx, 64);
            int tlen = sceJpegArmEncoderEncode(video_arm_ctx, cam_buf);
            sceJpegArmEncoderEnd(video_arm_ctx);
            if (tlen > 0) {
                SceUID tfd = sceIoOpen(thumb_path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
                if (tfd >= 0) {
                    sceIoWrite(tfd, jpeg_out_buf, tlen);
                    sceIoClose(tfd);
                    if (cam_last_thumb_tex) {
                        load_jpeg_into_thumb_texture(thumb_path, cam_last_thumb_tex);
                    }
                }
            }
        }
    }

    // Reservar 2048 bytes de cabecera AVI
    uint8_t zero_hdr[2048];
    memset(zero_hdr, 0, sizeof(zero_hdr));
    sceIoWrite(video_fd, zero_hdr, sizeof(zero_hdr));

    video_movi_size = 0;
    video_frame_count = 0;
    video_start_time = sceKernelGetProcessTimeWide();
    is_recording_video = 1;

    snprintf(status_msg, sizeof(status_msg), "GRABANDO VIDEO Y AUDIO (● REC)");
    status_msg_color = RGBA8(255, 60, 60, 255);
    status_msg_timer = 90;
}

static void video_record_stop(void) {
    if (!is_recording_video) return;
    is_recording_video = 0;

    if (audio_port >= 0) {
        sceAudioInReleasePort(audio_port);
        audio_port = -1;
    }

    if (video_fd >= 0) {
        uint64_t now = sceKernelGetProcessTimeWide();
        uint32_t duration_ms = (uint32_t)((now - video_start_time) / 1000);
        uint32_t fps = (duration_ms > 0 && video_frame_count > 0) ? (uint32_t)((video_frame_count * 1000ULL) / duration_ms) : 30;
        if (fps == 0) fps = 30;
        uint32_t us_per_frame = 1000000 / fps;

        uint8_t hdr[2048];
        memset(hdr, 0, sizeof(hdr));

        uint32_t total_file_size = 2048 + video_movi_size;
        uint32_t riff_size = total_file_size - 8;
        uint32_t movi_list_size = video_movi_size + 4;
        uint32_t dwStreams = (video_audio_bytes > 0) ? 2 : 1;

        // RIFF header
        memcpy(hdr + 0, "RIFF", 4);
        memcpy(hdr + 4, &riff_size, 4);
        memcpy(hdr + 8, "AVI ", 4);

        // LIST hdrl
        uint32_t hdrl_size = 2048 - 12 - 8 - 12;
        memcpy(hdr + 12, "LIST", 4);
        memcpy(hdr + 16, &hdrl_size, 4);
        memcpy(hdr + 20, "hdrl", 4);

        // avih chunk
        uint32_t avih_size = 56;
        memcpy(hdr + 24, "avih", 4);
        memcpy(hdr + 28, &avih_size, 4);
        uint32_t dwMicroSec = us_per_frame;
        uint32_t dwMaxBytes = 2000000;
        uint32_t dwFlags = 0x810;
        uint32_t dwTotalFrames = video_frame_count;
        uint32_t dwSugBuf = JPEG_OUT_BUF_SIZE;
        uint32_t dwW = CAM_WIDTH;
        uint32_t dwH = CAM_HEIGHT;
        memcpy(hdr + 32, &dwMicroSec, 4);
        memcpy(hdr + 36, &dwMaxBytes, 4);
        memcpy(hdr + 48, &dwFlags, 4);
        memcpy(hdr + 52, &dwTotalFrames, 4);
        memcpy(hdr + 60, &dwStreams, 4);
        memcpy(hdr + 64, &dwSugBuf, 4);
        memcpy(hdr + 68, &dwW, 4);
        memcpy(hdr + 72, &dwH, 4);

        // LIST strl para Video (stream 0)
        uint32_t strl_size = 116;
        memcpy(hdr + 92, "LIST", 4);
        memcpy(hdr + 96, &strl_size, 4);
        memcpy(hdr + 100, "strl", 4);

        // strh chunk Video
        uint32_t strh_size = 56;
        memcpy(hdr + 104, "strh", 4);
        memcpy(hdr + 108, &strh_size, 4);
        memcpy(hdr + 112, "vids", 4);
        memcpy(hdr + 116, "MJPG", 4);
        uint32_t dwScale = 1;
        uint32_t dwRate = fps;
        uint32_t dwLength = video_frame_count;
        memcpy(hdr + 132, &dwScale, 4);
        memcpy(hdr + 136, &dwRate, 4);
        memcpy(hdr + 144, &dwLength, 4);
        memcpy(hdr + 148, &dwSugBuf, 4);
        int16_t rcRight = CAM_WIDTH, rcBottom = CAM_HEIGHT;
        memcpy(hdr + 164, &rcRight, 2);
        memcpy(hdr + 166, &rcBottom, 2);

        // strf chunk Video (BITMAPINFOHEADER)
        uint32_t strf_size = 40;
        memcpy(hdr + 168, "strf", 4);
        memcpy(hdr + 172, &strf_size, 4);
        uint32_t biSize = 40;
        int32_t biWidth = CAM_WIDTH;
        int32_t biHeight = CAM_HEIGHT;
        uint16_t biPlanes = 1;
        uint16_t biBitCount = 24;
        uint32_t biCompression = 0x47504A4D; // 'MJPG'
        uint32_t biSizeImage = CAM_WIDTH * CAM_HEIGHT * 3;
        memcpy(hdr + 176, &biSize, 4);
        memcpy(hdr + 180, &biWidth, 4);
        memcpy(hdr + 184, &biHeight, 4);
        memcpy(hdr + 188, &biPlanes, 2);
        memcpy(hdr + 190, &biBitCount, 2);
        memcpy(hdr + 192, &biCompression, 4);
        memcpy(hdr + 196, &biSizeImage, 4);

        // LIST strl para Audio (stream 1) si hay audio grabado
        if (video_audio_bytes > 0) {
            uint32_t aud_strl_size = 90;
            memcpy(hdr + 216, "LIST", 4);
            memcpy(hdr + 220, &aud_strl_size, 4);
            memcpy(hdr + 224, "strl", 4);

            // strh Audio
            uint32_t aud_strh_size = 56;
            memcpy(hdr + 228, "strh", 4);
            memcpy(hdr + 232, &aud_strh_size, 4);
            memcpy(hdr + 236, "auds", 4);
            uint32_t aud_fcc = 1; // PCM
            memcpy(hdr + 240, &aud_fcc, 4);
            uint32_t aud_scale = 1;
            uint32_t aud_rate = AUDIO_SAMPLE_RATE;
            uint32_t aud_len = video_audio_samples;
            uint32_t aud_sug = 4096;
            uint32_t aud_samp_sz = 2;
            memcpy(hdr + 256, &aud_scale, 4);
            memcpy(hdr + 260, &aud_rate, 4);
            memcpy(hdr + 268, &aud_len, 4);
            memcpy(hdr + 272, &aud_sug, 4);
            memcpy(hdr + 284, &aud_samp_sz, 4);

            // strf Audio (WAVEFORMATEX)
            uint32_t aud_strf_size = 18;
            memcpy(hdr + 292, "strf", 4);
            memcpy(hdr + 296, &aud_strf_size, 4);
            uint16_t wFormatTag = 1; // WAVE_FORMAT_PCM
            uint16_t nChannels = 1;
            uint32_t nSamplesPerSec = AUDIO_SAMPLE_RATE;
            uint32_t nAvgBytesPerSec = AUDIO_SAMPLE_RATE * 2;
            uint16_t nBlockAlign = 2;
            uint16_t wBitsPerSample = 16;
            uint16_t cbSize = 0;
            memcpy(hdr + 300, &wFormatTag, 2);
            memcpy(hdr + 302, &nChannels, 2);
            memcpy(hdr + 304, &nSamplesPerSec, 4);
            memcpy(hdr + 308, &nAvgBytesPerSec, 4);
            memcpy(hdr + 312, &nBlockAlign, 2);
            memcpy(hdr + 314, &wBitsPerSample, 2);
            memcpy(hdr + 316, &cbSize, 2);
        }

        // LIST movi header at 2036
        memcpy(hdr + 2036, "LIST", 4);
        memcpy(hdr + 2040, &movi_list_size, 4);
        memcpy(hdr + 2044, "movi", 4);

        sceIoLseek(video_fd, 0, SCE_SEEK_SET);
        sceIoWrite(video_fd, hdr, sizeof(hdr));
        sceIoClose(video_fd);
        video_fd = -1;

        int secs = duration_ms / 1000;
        snprintf(status_msg, sizeof(status_msg), "VIDEO Y AUDIO GUARDADO (%d s, %d FPS)", secs, fps);
        status_msg_color = RGBA8(60, 255, 120, 255);
        status_msg_timer = 120;

        gallery_scan_directory();
        update_camera_last_thumb();
    }
}

// =========================================================================
// Hilo de Captura y Procesamiento de Cámara (Thread dedicado)
// =========================================================================
static int cam_worker_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;

    uint64_t last_frame = 0;
    while (cam_thread_run) {
        if (app_mode == APP_MODE_GALLERY) {
            sceKernelDelayThread(16000);
            continue;
        }

        if (sceCameraIsActive(cam_dev) > 0) {
            SceCameraRead read_info;
            memset(&read_info, 0, sizeof(read_info));
            read_info.size = sizeof(read_info);
            read_info.mode = 0;

            if (sceCameraRead(cam_dev, &read_info) >= 0) {
                if (read_info.frame != last_frame) {
                    last_frame = read_info.frame;

                    if (capture_requested) {
                        if (snap_buf && cam_buf) {
                            memcpy(snap_buf, cam_buf, FRAME_YUV420_SIZE);
                            capture_ready = 1;
                        }
                        capture_requested = 0;
                    }

                    // Grabación activa de cuadro de video (Init/Encode/End limpio sin malloc)
                    if (is_recording_video && video_fd >= 0 && video_enc_buf && video_raw_buf && video_arm_ctx && cam_buf) {
                        memcpy(video_raw_buf, cam_buf, FRAME_YUV420_SIZE);
                        if (sceJpegArmEncoderInit(video_arm_ctx, CAM_WIDTH, CAM_HEIGHT, SCE_JPEGENCARM_PIXELFORMAT_YCBCR420, video_enc_buf, JPEG_OUT_BUF_SIZE) >= 0) {
                            sceJpegArmEncoderSetHeaderMode(video_arm_ctx, SCE_JPEGENCARM_HEADER_MODE_JPEG);
                            sceJpegArmEncoderSetCompressionRatio(video_arm_ctx, 60);
                            int enc_len = sceJpegArmEncoderEncode(video_arm_ctx, video_raw_buf);
                            sceJpegArmEncoderEnd(video_arm_ctx);
                            if (enc_len > 0) {
                                uint8_t tag[8] = {'0', '0', 'd', 'c', 0, 0, 0, 0};
                                uint32_t chunk_len = (uint32_t)enc_len;
                                memcpy(tag + 4, &chunk_len, 4);
                                sceIoWrite(video_fd, tag, 8);
                                sceIoWrite(video_fd, video_enc_buf, enc_len);
                                video_movi_size += 8 + enc_len;
                                if (enc_len & 1) {
                                    uint8_t pad = 0;
                                    sceIoWrite(video_fd, &pad, 1);
                                    video_movi_size += 1;
                                }
                                video_frame_count++;
                            }
                        }

                        // Grabación intercalada de audio desde el micrófono
                        if (audio_port >= 0) {
                            int aret = sceAudioInInput(audio_port, audio_buf);
                            if (aret >= 0) {
                                uint8_t atag[8] = {'0', '1', 'w', 'b', 0, 0, 0, 0};
                                uint32_t achunk_len = (uint32_t)AUDIO_BUF_SIZE;
                                memcpy(atag + 4, &achunk_len, 4);
                                sceIoWrite(video_fd, atag, 8);
                                sceIoWrite(video_fd, audio_buf, achunk_len);
                                video_movi_size += 8 + achunk_len;
                                video_audio_bytes += achunk_len;
                                video_audio_samples += AUDIO_GRAIN;
                            }
                        }
                    }

                    int cur_back = cam_tex_back;
                    if (cam_tex[cur_back] && app_mode == APP_MODE_CAMERA) {
                        yuv420_to_rgba_neon_exact(
                            (const uint8_t *)info.pIBase,
                            (const uint8_t *)info.pUBase,
                            (const uint8_t *)info.pVBase,
                            (uint32_t *)vita2d_texture_get_datap(cam_tex[cur_back]),
                            CAM_WIDTH,
                            CAM_HEIGHT,
                            vita2d_texture_get_stride(cam_tex[cur_back])
                        );
                        cam_tex_front = cur_back;
                        cam_tex_back = 1 - cur_back;
                    }
                }
            } else {
                sceKernelDelayThread(1000);
            }
        } else {
            sceKernelDelayThread(10000);
        }
    }
    return sceKernelExitDeleteThread(0);
}

// =========================================================================
// Main Entrypoint
// =========================================================================
int main() {
    // 1. Reloj de CPU a 444 MHz y Carga de Módulos
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    sceSysmoduleLoadModuleInternal(SCE_SYSMODULE_INTERNAL_JPEG_ENC_ARM);

    // 2. Inicializar Touchscreen Frontal
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    sceTouchEnableTouchForce(SCE_TOUCH_PORT_FRONT);

    // 3. Inicializar vita2d y cargar fuente PGF
    vita2d_init();
    vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
    vita2d_pgf *pgf = vita2d_load_default_pgf();

    // Iniciar servidor web inalámbrico en segundo plano (Puerto 8080)
    webserver_init();

    // 4. Crear texturas fijas (Cámara Doble Buffer + Fullscreen + Pool de Miniaturas + Iconos)
    cam_tex[0] = vita2d_create_empty_texture(CAM_WIDTH, CAM_HEIGHT);
    cam_tex[1] = vita2d_create_empty_texture(CAM_WIDTH, CAM_HEIGHT);
    gallery_tex = vita2d_create_empty_texture(CAM_WIDTH, CAM_HEIGHT);
    for (int i = 0; i < THUMB_POOL_SIZE; i++) {
        thumb_tex[i] = vita2d_create_empty_texture(THUMB_WIDTH, THUMB_HEIGHT);
        if (thumb_tex[i]) {
            vita2d_texture_set_filters(thumb_tex[i], SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
        }
        thumb_slots[i].photo_idx = -1;
        thumb_slots[i].last_used_frame = 0;
        thumb_slots[i].is_loaded = 0;
    }

    // Cargar iconos PNG desde el VPK
    for (int i = 0; i < ICON_COUNT; i++) {
        icon_tex[i] = vita2d_load_PNG_file(icon_paths[i]);
        if (icon_tex[i]) {
            vita2d_texture_set_filters(icon_tex[i],
                SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
        }
    }

    if (!cam_tex[0] || !cam_tex[1] || !gallery_tex) {
        if (cam_tex[0]) vita2d_free_texture(cam_tex[0]);
        if (cam_tex[1]) vita2d_free_texture(cam_tex[1]);
        if (gallery_tex) vita2d_free_texture(gallery_tex);
        for (int i = 0; i < THUMB_POOL_SIZE; i++) {
            if (thumb_tex[i]) vita2d_free_texture(thumb_tex[i]);
        }
        for (int i = 0; i < ICON_COUNT; i++) {
            if (icon_tex[i]) vita2d_free_texture(icon_tex[i]);
        }
        if (pgf) vita2d_free_pgf(pgf);
        vita2d_fini();
        sceKernelExitProcess(0);
        return 0;
    }
    vita2d_texture_set_filters(cam_tex[0], SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    vita2d_texture_set_filters(cam_tex[1], SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    vita2d_texture_set_filters(gallery_tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);

    // 5. Memoria física contigua para hardware DMA
    SceUID cam_mem_uid = sceKernelAllocMemBlock(
        "CameraI", 
        SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, 
        4 * 1024 * 1024, 
        NULL
    );
    if (cam_mem_uid < 0) {
        if (pgf) vita2d_free_pgf(pgf);
        if (cam_tex[0]) vita2d_free_texture(cam_tex[0]);
        if (cam_tex[1]) vita2d_free_texture(cam_tex[1]);
        if (gallery_tex) vita2d_free_texture(gallery_tex);
        for (int i = 0; i < THUMB_POOL_SIZE; i++) {
            if (thumb_tex[i]) vita2d_free_texture(thumb_tex[i]);
        }
        vita2d_fini();
        sceKernelExitProcess(0);
        return 0;
    }

    void *base_mem = NULL;
    sceKernelGetMemBlockBase(cam_mem_uid, &base_mem);
    cam_buf = base_mem;
    snap_buf = (uint8_t *)base_mem + (512 * 1024);
    jpeg_out_buf = (uint8_t *)base_mem + (1024 * 1024);

    // 6. Configurar SceCamera
    init_camera_params_table();

    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    info.priority = SCE_CAMERA_PRIORITY_SHARE;
    info.resolution = SCE_CAMERA_RESOLUTION_640_480;
    info.framerate = (uint16_t)params[2].option_values[params[2].current_idx];
    if (info.framerate == 0) info.framerate = SCE_CAMERA_FRAMERATE_30_FPS;
    info.format = SCE_CAMERA_FORMAT_YUV420_PLANE;
    info.range = 1;
    info.pitch = 0;
    info.buffer = 0;

    info.sizeIBase = CAM_WIDTH * CAM_HEIGHT;
    info.sizeUBase = (CAM_WIDTH / 2) * (CAM_HEIGHT / 2);
    info.sizeVBase = (CAM_WIDTH / 2) * (CAM_HEIGHT / 2);

    info.pIBase = cam_buf;
    info.pUBase = (uint8_t *)cam_buf + info.sizeIBase;
    info.pVBase = (uint8_t *)cam_buf + info.sizeIBase + info.sizeUBase;

    cam_dev = SCE_CAMERA_DEVICE_BACK;
    if (sceCameraOpen(cam_dev, &info) < 0) {
        sceKernelFreeMemBlock(cam_mem_uid);
        if (pgf) vita2d_free_pgf(pgf);
        if (cam_tex[0]) vita2d_free_texture(cam_tex[0]);
        if (cam_tex[1]) vita2d_free_texture(cam_tex[1]);
        if (gallery_tex) vita2d_free_texture(gallery_tex);
        for (int i = 0; i < THUMB_POOL_SIZE; i++) {
            if (thumb_tex[i]) vita2d_free_texture(thumb_tex[i]);
        }
        vita2d_fini();
        sceKernelExitProcess(0);
        return 0;
    }

    // 7. Iniciar captura continua
    if (sceCameraStart(cam_dev) < 0) {
        sceCameraClose(cam_dev);
        sceKernelFreeMemBlock(cam_mem_uid);
        if (pgf) vita2d_free_pgf(pgf);
        if (cam_tex[0]) vita2d_free_texture(cam_tex[0]);
        if (cam_tex[1]) vita2d_free_texture(cam_tex[1]);
        if (gallery_tex) vita2d_free_texture(gallery_tex);
        for (int i = 0; i < THUMB_POOL_SIZE; i++) {
            if (thumb_tex[i]) vita2d_free_texture(thumb_tex[i]);
        }
        vita2d_fini();
        sceKernelExitProcess(0);
        return 0;
    }

    for (int i = 0; i < NUM_PARAMS; i++) {
        apply_and_query_param(cam_dev, i);
    }

    // 8. Lanzar hilo trabajador de captura de cámara
    cam_thread_run = 1;
    cam_thid = sceKernelCreateThread("VitaCam_Capture", cam_worker_thread, 0x10000100, 0x10000, 0, 0, NULL);
    if (cam_thid >= 0) {
        sceKernelStartThread(cam_thid, 0, NULL);
    }

    cam_last_thumb_tex = vita2d_create_empty_texture(THUMB_WIDTH, THUMB_HEIGHT);
    if (cam_last_thumb_tex) {
        vita2d_texture_set_filters(cam_last_thumb_tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    }
    gallery_scan_directory();
    if (gallery_count > 0 && cam_last_thumb_tex) {
        load_jpeg_into_thumb_texture(gallery_photos[0].fullpath, cam_last_thumb_tex);
    }

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    SceCtrlData pad;
    SceCtrlData old_pad;
    memset(&pad, 0, sizeof(pad));
    memset(&old_pad, 0, sizeof(old_pad));

    // 9. Bucle Principal de Renderizado e Interfaz
    while (1) {
        SceAppMgrSystemEvent app_ev;
        while (sceAppMgrReceiveSystemEvent(&app_ev) == 0) {
            // Drenar eventos del sistema
        }

        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);

        old_pad = pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t pressed = pad.buttons & ~old_pad.buttons;

        // START ya no sale de la app

        // =================================================================
        // MODO CÁMARA PRO (SAMSUNG EXPERT CAMERA STYLE + SLIDER DE PRECISIÓN)
        // =================================================================
        if (app_mode == APP_MODE_CAMERA) {
            handle_camera_touch();

            if (pressed & SCE_CTRL_SELECT) {
                gallery_enter();
            } else if ((pressed & SCE_CTRL_RTRIGGER) || (pressed & SCE_CTRL_LTRIGGER)) {
                trigger_camera_shot();
            }

            if (flash_trigger_anim > 0) {
                if (flash_trigger_anim == 8) {
                    capture_requested = 1;
                }
                flash_trigger_anim--;
            }

            if (capture_ready) {
                capture_ready = 0;
                capture_and_save_photo();
            }

            if (slider_focused == 0) {
                // Modo Navegación Barra Pro Inferior
                if (pressed & SCE_CTRL_LEFT) {
                    active_cam_param--;
                    if (active_cam_param < 0) active_cam_param = PRO_BAR_COUNT - 1;
                    cam_slider_open = 1;
                }
                if (pressed & SCE_CTRL_RIGHT) {
                    active_cam_param++;
                    if (active_cam_param >= PRO_BAR_COUNT) active_cam_param = 0;
                    cam_slider_open = 1;
                }
                if (pressed & SCE_CTRL_CROSS) {
                    slider_focused = 1;
                    cam_slider_open = 1;
                }
                if (pressed & SCE_CTRL_TRIANGLE) {
                    show_grid = !show_grid;
                }
            } else {
                // Modo Ajuste de Slider / Selector Enfocado
                int p_idx = pro_bar_params[active_cam_param];
                CameraParam *p = &params[p_idx];

                if (pressed & SCE_CTRL_LEFT) {
                    p->current_idx--;
                    if (p->current_idx < 0) {
                        p->current_idx = is_pro_param_discrete(active_cam_param) ? p->num_options - 1 : 0;
                    }
                    apply_and_query_param(cam_dev, p_idx);
                }
                if (pressed & SCE_CTRL_RIGHT) {
                    p->current_idx++;
                    if (p->current_idx >= p->num_options) {
                        p->current_idx = is_pro_param_discrete(active_cam_param) ? 0 : p->num_options - 1;
                    }
                    apply_and_query_param(cam_dev, p_idx);
                }
                if ((pressed & SCE_CTRL_TRIANGLE) || (pressed & SCE_CTRL_CIRCLE)) {
                    slider_focused = 0; // Vuelve a la navegación de la barra
                }
                if (pressed & SCE_CTRL_SQUARE) {
                    p->current_idx = p->default_idx;
                    apply_and_query_param(cam_dev, p_idx);
                }
            }

            // -------------------------------------------------------------
            // Renderizado de Cámara Pro (Pure Black AMOLED + Viewfinder + Pro Bar + Slider)
            // -------------------------------------------------------------
            vita2d_start_drawing();
            vita2d_clear_screen();

            // Fondo Negro Puro AMOLED
            vita2d_draw_rectangle(0.0f, 0.0f, 960.0f, 544.0f, RGBA8(0, 0, 0, 255));

            float crop_w = (float)CAM_WIDTH / zoom_factor;
            float crop_h = (float)CAM_HEIGHT / zoom_factor;
            float crop_x = ((float)CAM_WIDTH - crop_w) * 0.5f;
            float crop_y = ((float)CAM_HEIGHT - crop_h) * 0.5f;

            float disp_w = 724.0f;
            float disp_h = 544.0f;
            float disp_x = 118.0f;
            float disp_y = 0.0f;

            float scale_x = disp_w / crop_w;
            float scale_y = disp_h / crop_h;

            int draw_idx = cam_tex_front;
            if (cam_tex[draw_idx]) {
                vita2d_draw_texture_part_scale(
                    cam_tex[draw_idx],
                    disp_x, disp_y,
                    crop_x, crop_y,
                    crop_w, crop_h,
                    scale_x, scale_y
                );
            }

            // ── 1. Guías de Composición (Cuadrícula 3x3 centrada)
            if (show_grid) {
                // Líneas horizontales
                vita2d_draw_rectangle(118.0f, 181.0f, 724.0f, 1.5f, RGBA8(255, 255, 255, 120));
                vita2d_draw_rectangle(118.0f, 362.0f, 724.0f, 1.5f, RGBA8(255, 255, 255, 120));
                // Líneas verticales
                vita2d_draw_rectangle(359.0f, 0.0f, 1.5f, 544.0f, RGBA8(255, 255, 255, 120));
                vita2d_draw_rectangle(601.0f, 0.0f, 1.5f, 544.0f, RGBA8(255, 255, 255, 120));

                // Retículas duales de enfoque central [ ] [ ]
                float fc_y = 264.0f;
                // Bracket izquierdo
                vita2d_draw_rectangle(442.0f, fc_y, 12.0f, 2.0f, RGBA8(255, 255, 255, 180));
                vita2d_draw_rectangle(442.0f, fc_y + 14.0f, 12.0f, 2.0f, RGBA8(255, 255, 255, 180));
                vita2d_draw_rectangle(442.0f, fc_y, 2.0f, 16.0f, RGBA8(255, 255, 255, 180));
                // Bracket derecho
                vita2d_draw_rectangle(505.0f, fc_y, 12.0f, 2.0f, RGBA8(255, 255, 255, 180));
                vita2d_draw_rectangle(505.0f, fc_y + 14.0f, 12.0f, 2.0f, RGBA8(255, 255, 255, 180));
                vita2d_draw_rectangle(515.0f, fc_y, 2.0f, 16.0f, RGBA8(255, 255, 255, 180));
            }

            // ── 2. Selector Flotante / Slider de Precisión Centrado
            if (cam_slider_open && pgf) {
                int cur_p_idx = pro_bar_params[active_cam_param];
                CameraParam *cur_p = &params[cur_p_idx];

                if (is_pro_param_discrete(active_cam_param)) {
                    // Selector Flotante con Botones Circulares (FPS, ISO, WB, EFECTO)
                    int n_opts = cur_p->num_options;

                    float badge_w = 48.0f;
                    float badge_h = 44.0f;
                    float gap = 10.0f;
                    if (active_cam_param == 4) { badge_w = 46.0f; badge_h = 40.0f; gap = 6.0f; }
                    else if (active_cam_param == 1) { badge_w = 46.0f; badge_h = 46.0f; gap = 14.0f; }
                    else if (active_cam_param == 0) { badge_w = 54.0f; badge_h = 44.0f; gap = 10.0f; }
                    else if (active_cam_param == 3) { badge_w = 56.0f; badge_h = 44.0f; gap = 8.0f; }

                    float total_content_w = (float)n_opts * badge_w + (float)(n_opts - 1) * gap;
                    float pill_pad_x = 14.0f;
                    float pill_w = total_content_w + pill_pad_x * 2.0f;
                    float pill_h = badge_h + 14.0f;
                    float pill_x = 480.0f - pill_w * 0.5f;
                    float pill_y = 486.0f - pill_h - 10.0f;

                    // Contenedor Frosted Glass Flotante Translúcido
                    vita2d_draw_rectangle(pill_x, pill_y, pill_w, pill_h, RGBA8(12, 16, 26, 165));
                    unsigned int pill_border = (slider_focused == 1) ? RGBA8(245, 197, 24, 220) : RGBA8(255, 255, 255, 30);
                    vita2d_draw_rectangle(pill_x, pill_y, pill_w, 1.0f, pill_border);
                    vita2d_draw_rectangle(pill_x, pill_y + pill_h - 1.0f, pill_w, 1.0f, pill_border);
                    vita2d_draw_rectangle(pill_x, pill_y, 1.0f, pill_h, pill_border);
                    vita2d_draw_rectangle(pill_x + pill_w - 1.0f, pill_y, 1.0f, pill_h, pill_border);

                    float cur_bx = pill_x + pill_pad_x;
                    float cur_by = pill_y + 7.0f;

                    for (int j = 0; j < n_opts; j++) {
                        const char *opt_name = cur_p->option_names[j];
                        int is_cur = (j == cur_p->current_idx);

                        if (is_cur) {
                            // Círculo Dorado Resaltado (#F5C518)
                            vita2d_draw_rectangle(cur_bx, cur_by, badge_w, badge_h, RGBA8(245, 197, 24, 255));
                            if (slider_focused == 1) {
                                vita2d_draw_rectangle(cur_bx, cur_by, badge_w, 1.5f, RGBA8(255, 255, 255, 255));
                                vita2d_draw_rectangle(cur_bx, cur_by + badge_h - 1.5f, badge_w, 1.5f, RGBA8(255, 255, 255, 255));
                                vita2d_draw_rectangle(cur_bx, cur_by, 1.5f, badge_h, RGBA8(255, 255, 255, 255));
                                vita2d_draw_rectangle(cur_bx + badge_w - 1.5f, cur_by, 1.5f, badge_h, RGBA8(255, 255, 255, 255));
                            }
                            float tw = vita2d_pgf_text_width(pgf, 0.68f, opt_name);
                            vita2d_pgf_draw_text(pgf, (int)(cur_bx + (badge_w - tw) * 0.5f), (int)(cur_by + badge_h * 0.5f + 6.0f),
                                                 RGBA8(10, 14, 22, 255), 0.68f, opt_name);
                        } else {
                            // Círculo Glass Translúcido
                            vita2d_draw_rectangle(cur_bx, cur_by, badge_w, badge_h, RGBA8(20, 26, 40, 180));
                            vita2d_draw_rectangle(cur_bx, cur_by, badge_w, 1.0f, RGBA8(255, 255, 255, 25));
                            vita2d_draw_rectangle(cur_bx, cur_by + badge_h - 1.0f, badge_w, 1.0f, RGBA8(255, 255, 255, 25));
                            vita2d_draw_rectangle(cur_bx, cur_by, 1.0f, badge_h, RGBA8(255, 255, 255, 25));
                            vita2d_draw_rectangle(cur_bx + badge_w - 1.0f, cur_by, 1.0f, badge_h, RGBA8(255, 255, 255, 25));

                            float tw = vita2d_pgf_text_width(pgf, 0.64f, opt_name);
                            vita2d_pgf_draw_text(pgf, (int)(cur_bx + (badge_w - tw) * 0.5f), (int)(cur_by + badge_h * 0.5f + 6.0f),
                                                 RGBA8(210, 220, 245, 210), 0.64f, opt_name);
                        }
                        cur_bx += badge_w + gap;
                    }
                } else {
                    // Slider de Precisión Continuo (EV y ZOOM) con Regla Desplazable Real
                    float sl_x = 148.0f, sl_y = 396.0f, sl_w = 664.0f, sl_h = 80.0f;
                    // Contenedor Glass Translúcido
                    vita2d_draw_rectangle(sl_x, sl_y, sl_w, sl_h, RGBA8(10, 14, 22, 160));
                    unsigned int sl_border = (slider_focused == 1) ? RGBA8(245, 197, 24, 220) : RGBA8(255, 255, 255, 25);
                    vita2d_draw_rectangle(sl_x, sl_y, sl_w, 1.0f, sl_border);
                    vita2d_draw_rectangle(sl_x, sl_y + sl_h - 1.0f, sl_w, 1.0f, sl_border);
                    vita2d_draw_rectangle(sl_x, sl_y, 1.0f, sl_h, sl_border);
                    vita2d_draw_rectangle(sl_x + sl_w - 1.0f, sl_y, 1.0f, sl_h, sl_border);

                    // Valor Central Destacado en Golden Amber (#F5C518)
                    const char *val_name = cur_p->option_names[cur_p->current_idx];
                    float tw_val = vita2d_pgf_text_width(pgf, 0.92f, val_name);
                    float val_x = 480.0f - tw_val * 0.5f;
                    vita2d_pgf_draw_text(pgf, (int)val_x, 424, RGBA8(245, 197, 24, 255), 0.92f, val_name);

                    // Regla Milimétrica Desplazable que se desliza bajo la aguja
                    float cx = 480.0f;
                    float tick_spacing = 18.0f;

                    // Recorte interno al slider box
                    vita2d_set_clip_rectangle((int)(sl_x + 4.0f), (int)sl_y, (int)(sl_x + sl_w - 4.0f), (int)(sl_y + sl_h));

                    for (int j = 0; j < cur_p->num_options; j++) {
                        float tx = cx + (float)(j - cur_p->current_idx) * tick_spacing;
                        if (tx < sl_x - 30.0f || tx > sl_x + sl_w + 30.0f) continue;

                        float dist = fabsf(tx - cx);
                        float alpha_f = 1.0f - (dist / (sl_w * 0.48f));
                        if (alpha_f <= 0.0f) continue;

                        uint8_t a = (uint8_t)(alpha_f * 240.0f);
                        int is_major = 0;
                        if (active_cam_param == 2) { // EV: cada 10 pasos (1.0 EV) es mayor, cada 5 es medio
                            is_major = (j % 10 == 0) ? 2 : ((j % 5 == 0) ? 1 : 0);
                        } else if (active_cam_param == 5) { // ZOOM: cada 10 pasos (1.0x) es mayor, cada 5 es medio
                            is_major = (j % 10 == 0) ? 2 : ((j % 5 == 0) ? 1 : 0);
                        }

                        float th = (is_major == 2) ? 20.0f : ((is_major == 1) ? 14.0f : 9.0f);
                        float ty = 444.0f + (20.0f - th) * 0.5f;

                        if (j == cur_p->current_idx) {
                            vita2d_draw_rectangle(tx - 1.5f, 436.0f, 3.0f, 26.0f, RGBA8(245, 197, 24, 255));
                        } else {
                            vita2d_draw_rectangle(tx - 0.75f, ty, 1.5f, th, RGBA8(240, 245, 255, a));
                        }

                        // Números sobre marcas principales que se deslizan dinámicamente con la regla
                        if (is_major == 2 && j != cur_p->current_idx && alpha_f > 0.35f) {
                            const char *lbl = cur_p->option_names[j];
                            float tw_lbl = vita2d_pgf_text_width(pgf, 0.50f, lbl);
                            vita2d_pgf_draw_text(pgf, (int)(tx - tw_lbl * 0.5f), 434, RGBA8(200, 215, 240, (uint8_t)(alpha_f * 200.0f)), 0.50f, lbl);
                        }
                    }

                    // Aguja central dorada fija superior
                    vita2d_draw_rectangle(cx - 1.5f, 436.0f, 3.0f, 26.0f, RGBA8(245, 197, 24, 255));

                    // Gradientes/sombras laterales para el efecto de borde curvo
                    for (int s = 0; s < 25; s++) {
                        uint8_t edge_a = (uint8_t)((1.0f - (float)s / 25.0f) * 160.0f);
                        vita2d_draw_rectangle(sl_x + (float)s, sl_y + 1.0f, 1.0f, sl_h - 2.0f, RGBA8(10, 14, 22, edge_a));
                        vita2d_draw_rectangle(sl_x + sl_w - 1.0f - (float)s, sl_y + 1.0f, 1.0f, sl_h - 2.0f, RGBA8(10, 14, 22, edge_a));
                    }

                    vita2d_disable_clipping();
                }
            }

            // ── 3. Barra Inferior de Parámetros Pro (ISO, SPEED, EV, WB, EFECTO, ZOOM)
            float pb_x = 138.0f, pb_y = 486.0f, pb_w = 684.0f, pb_h = 48.0f;
            vita2d_draw_rectangle(pb_x, pb_y, pb_w, pb_h, RGBA8(10, 14, 22, 150));
            vita2d_draw_rectangle(pb_x, pb_y, pb_w, 1.0f, RGBA8(255, 255, 255, 25));
            vita2d_draw_rectangle(pb_x, pb_y + pb_h - 1.0f, pb_w, 1.0f, RGBA8(255, 255, 255, 25));
            vita2d_draw_rectangle(pb_x, pb_y, 1.0f, pb_h, RGBA8(255, 255, 255, 25));
            vita2d_draw_rectangle(pb_x + pb_w - 1.0f, pb_y, 1.0f, pb_h, RGBA8(255, 255, 255, 25));

            for (int i = 0; i < PRO_BAR_COUNT; i++) {
                float item_w = pb_w / (float)PRO_BAR_COUNT;
                float item_x = pb_x + (float)i * item_w;
                int p_idx = pro_bar_params[i];
                CameraParam *p = &params[p_idx];
                int is_active = (i == active_cam_param && cam_slider_open);

                if (is_active) {
                    // Pastilla resaltada en ámbar
                    vita2d_draw_rectangle(item_x + 3.0f, pb_y + 4.0f, item_w - 6.0f, pb_h - 8.0f, RGBA8(245, 197, 24, 38));
                    unsigned int bcol = (slider_focused == 1) ? RGBA8(245, 197, 24, 255) : RGBA8(245, 197, 24, 190);
                    vita2d_draw_rectangle(item_x + 3.0f, pb_y + 4.0f, item_w - 6.0f, 1.5f, bcol);
                    vita2d_draw_rectangle(item_x + 3.0f, pb_y + pb_h - 5.5f, item_w - 6.0f, 1.5f, bcol);
                    vita2d_draw_rectangle(item_x + 3.0f, pb_y + 4.0f, 1.5f, pb_h - 8.0f, bcol);
                    vita2d_draw_rectangle(item_x + item_w - 4.5f, pb_y + 4.0f, 1.5f, pb_h - 8.0f, bcol);
                }

                unsigned int title_col = is_active ? RGBA8(245, 197, 24, 255) : RGBA8(180, 190, 210, 210);
                unsigned int val_col = is_active ? RGBA8(255, 255, 255, 255) : RGBA8(215, 225, 240, 220);

                // Título del parámetro
                float tw_title = vita2d_pgf_text_width(pgf, 0.58f, pro_bar_labels[i]);
                vita2d_pgf_draw_text(pgf, (int)(item_x + (item_w - tw_title) * 0.5f), (int)pb_y + 18, title_col, 0.58f, pro_bar_labels[i]);

                // Valor abreviado del parámetro
                const char *cur_v_str = p->option_names[p->current_idx];
                char short_str[16];
                if (i == 0) { // ISO
                    snprintf(short_str, sizeof(short_str), "%s", cur_v_str);
                } else if (i == 1) { // SPEED (FPS)
                    snprintf(short_str, sizeof(short_str), "%s FPS", cur_v_str);
                } else if (i == 2) { // EV
                    if (p->option_values[p->current_idx] == 0) snprintf(short_str, sizeof(short_str), "0.0");
                    else if (p->option_values[p->current_idx] > 0) snprintf(short_str, sizeof(short_str), "+%.1f", (float)p->option_values[p->current_idx] / 10.0f);
                    else snprintf(short_str, sizeof(short_str), "%.1f", (float)p->option_values[p->current_idx] / 10.0f);
                } else if (i == 3) { // WB
                    snprintf(short_str, sizeof(short_str), "%s", cur_v_str);
                } else if (i == 4) { // EFECTO
                    snprintf(short_str, sizeof(short_str), "%s", cur_v_str);
                } else { // ZOOM
                    snprintf(short_str, sizeof(short_str), "%s", cur_v_str);
                }

                float tw_val = vita2d_pgf_text_width(pgf, 0.62f, short_str);
                vita2d_pgf_draw_text(pgf, (int)(item_x + (item_w - tw_val) * 0.5f), (int)pb_y + 38, val_col, 0.62f, short_str);
            }

            // ── 4. Modo Softbox / Aro de Luz Frontal de Alta Potencia (sin tocar la barra de datos) ──
            if (cam_dev == SCE_CAMERA_DEVICE_FRONT && front_flash_mode == FRONT_FLASH_BORDER) {
                unsigned int softbox_col = RGBA8(255, 253, 246, 255); // Luz blanca cálida intensa

                // Franja superior amplia (iluminación cenital)
                vita2d_draw_rectangle(0.0f, 0.0f, 960.0f, 60.0f, softbox_col);

                // Columna izquierda ancha (iluminación lateral izquierda)
                vita2d_draw_rectangle(0.0f, 60.0f, 138.0f, 422.0f, softbox_col);

                // Columna derecha ancha (iluminación lateral derecha)
                vita2d_draw_rectangle(822.0f, 60.0f, 138.0f, 422.0f, softbox_col);

                // Flancos inferiores (deja el centro de la barra flotante intacto y limpio)
                vita2d_draw_rectangle(0.0f, 482.0f, 138.0f, 62.0f, softbox_col);
                vita2d_draw_rectangle(822.0f, 482.0f, 138.0f, 62.0f, softbox_col);

                // Resplandor suave hacia el centro del visor
                vita2d_draw_rectangle(138.0f, 60.0f, 684.0f, 5.0f, RGBA8(255, 250, 238, 140));
                vita2d_draw_rectangle(138.0f, 477.0f, 684.0f, 5.0f, RGBA8(255, 250, 238, 140));
                vita2d_draw_rectangle(138.0f, 60.0f, 5.0f, 422.0f, RGBA8(255, 250, 238, 140));
                vita2d_draw_rectangle(817.0f, 60.0f, 5.0f, 422.0f, RGBA8(255, 250, 238, 140));
            } else {
                // Barras laterales Dark Midnight Glass discretas
                vita2d_draw_rectangle(0.0f, 0.0f, 118.0f, 544.0f, RGBA8(10, 13, 26, 215));
                vita2d_draw_rectangle(842.0f, 0.0f, 118.0f, 544.0f, RGBA8(10, 13, 26, 215));
                vita2d_draw_rectangle(117.0f, 0.0f, 1.0f, 544.0f, RGBA8(255, 255, 255, 18));
                vita2d_draw_rectangle(842.0f, 0.0f, 1.0f, 544.0f, RGBA8(255, 255, 255, 18));
            }

            // ── 5. Controles Flotantes Modernos - Barra Izquierda (Cápsulas Circulares 44x44) ──
            float left_cx = 59.0f;
            float btn_r = 22.0f;

            // Botón Toggle Cuadrícula (Grid)
            float gbtn_cy = 48.0f;
            vita2d_draw_rectangle(left_cx - btn_r, gbtn_cy - btn_r, btn_r * 2.0f, btn_r * 2.0f, RGBA8(14, 18, 34, 235));
            unsigned int gbtn_bcol = show_grid ? RGBA8(0, 210, 255, 240) : RGBA8(255, 255, 255, 40);
            vita2d_draw_rectangle(left_cx - btn_r, gbtn_cy - btn_r, btn_r * 2.0f, 1.5f, gbtn_bcol);
            vita2d_draw_rectangle(left_cx - btn_r, gbtn_cy + btn_r - 1.5f, btn_r * 2.0f, 1.5f, gbtn_bcol);
            vita2d_draw_rectangle(left_cx - btn_r, gbtn_cy - btn_r, 1.5f, btn_r * 2.0f, gbtn_bcol);
            vita2d_draw_rectangle(left_cx + btn_r - 1.5f, gbtn_cy - btn_r, 1.5f, btn_r * 2.0f, gbtn_bcol);
            if (icon_tex[ICON_CAM_GRID]) {
                float sc = 28.0f / (float)vita2d_texture_get_height(icon_tex[ICON_CAM_GRID]);
                vita2d_draw_texture_scale(icon_tex[ICON_CAM_GRID], left_cx - 14.0f, gbtn_cy - 14.0f, sc, sc);
            }

            // Botón Flash Frontal con Icono Vectorial Real
            if (cam_dev == SCE_CAMERA_DEVICE_FRONT) {
                float flbtn_cy = 106.0f;
                vita2d_draw_rectangle(left_cx - btn_r, flbtn_cy - btn_r, btn_r * 2.0f, btn_r * 2.0f, RGBA8(14, 18, 34, 235));
                unsigned int fl_border = (front_flash_mode == FRONT_FLASH_SCREEN) ? RGBA8(255, 214, 10, 240) :
                                         ((front_flash_mode == FRONT_FLASH_BORDER) ? RGBA8(0, 210, 255, 240) : RGBA8(255, 255, 255, 40));
                vita2d_draw_rectangle(left_cx - btn_r, flbtn_cy - btn_r, btn_r * 2.0f, 1.5f, fl_border);
                vita2d_draw_rectangle(left_cx - btn_r, flbtn_cy + btn_r - 1.5f, btn_r * 2.0f, 1.5f, fl_border);
                vita2d_draw_rectangle(left_cx - btn_r, flbtn_cy - btn_r, 1.5f, btn_r * 2.0f, fl_border);
                vita2d_draw_rectangle(left_cx + btn_r - 1.5f, flbtn_cy - btn_r, 1.5f, btn_r * 2.0f, fl_border);

                IconId cur_fl_icon = (front_flash_mode == FRONT_FLASH_SCREEN) ? ICON_CAM_FLASH_SCREEN :
                                     ((front_flash_mode == FRONT_FLASH_BORDER) ? ICON_CAM_FLASH_RING : ICON_CAM_FLASH_OFF);
                if (icon_tex[cur_fl_icon]) {
                    float sc = 28.0f / (float)vita2d_texture_get_height(icon_tex[cur_fl_icon]);
                    vita2d_draw_texture_scale(icon_tex[cur_fl_icon], left_cx - 14.0f, flbtn_cy - 14.0f, sc, sc);
                }
            }

            // Botón Marca de Agua con Icono Vectorial Real
            float wmbtn_cy = 164.0f;
            vita2d_draw_rectangle(left_cx - btn_r, wmbtn_cy - btn_r, btn_r * 2.0f, btn_r * 2.0f, RGBA8(14, 18, 34, 235));
            unsigned int wm_border = watermark_enabled ? RGBA8(0, 210, 255, 240) : RGBA8(255, 255, 255, 40);
            vita2d_draw_rectangle(left_cx - btn_r, wmbtn_cy - btn_r, btn_r * 2.0f, 1.5f, wm_border);
            vita2d_draw_rectangle(left_cx - btn_r, wmbtn_cy + btn_r - 1.5f, btn_r * 2.0f, 1.5f, wm_border);
            vita2d_draw_rectangle(left_cx - btn_r, wmbtn_cy - btn_r, 1.5f, btn_r * 2.0f, wm_border);
            vita2d_draw_rectangle(left_cx + btn_r - 1.5f, wmbtn_cy - btn_r, 1.5f, btn_r * 2.0f, wm_border);
            if (icon_tex[ICON_CAM_WM]) {
                float sc = 28.0f / (float)vita2d_texture_get_height(icon_tex[ICON_CAM_WM]);
                vita2d_draw_texture_scale(icon_tex[ICON_CAM_WM], left_cx - 14.0f, wmbtn_cy - 14.0f, sc, sc);
            }

            // Burbuja de Galería en Barra Izquierda Inferior (Aspecto 4:3 con marco nítido)
            float gal_x = 29.0f, gal_y = 461.0f, gal_w = 60.0f, gal_h = 46.0f;
            vita2d_draw_rectangle(gal_x, gal_y, gal_w, gal_h, RGBA8(14, 18, 34, 235));

            if (gallery_count > 0 && cam_last_thumb_tex) {
                float img_w = (float)vita2d_texture_get_width(cam_last_thumb_tex);
                float img_h = (float)vita2d_texture_get_height(cam_last_thumb_tex);
                float sx = gal_w / img_w;
                float sy = gal_h / img_h;
                float sc = (sx > sy) ? sx : sy;
                float draw_w = img_w * sc;
                float draw_h = img_h * sc;
                float off_x = gal_x + (gal_w - draw_w) * 0.5f;
                float off_y = gal_y + (gal_h - draw_h) * 0.5f;

                vita2d_set_clip_rectangle((int)gal_x, (int)gal_y, (int)(gal_x + gal_w), (int)(gal_y + gal_h));
                vita2d_draw_texture_scale(cam_last_thumb_tex, off_x, off_y, sc, sc);
                vita2d_disable_clipping();
            } else if (icon_tex[ICON_CAMERA]) {
                float th = (float)vita2d_texture_get_height(icon_tex[ICON_CAMERA]);
                float sc = 24.0f / th;
                vita2d_draw_texture_scale(icon_tex[ICON_CAMERA], gal_x + (gal_w - 24.0f) * 0.5f, gal_y + (gal_h - 24.0f) * 0.5f, sc, sc);
            }

            // Marco blanco nítido 1.5px
            vita2d_draw_rectangle(gal_x - 1.0f, gal_y - 1.0f, gal_w + 2.0f, 1.5f, RGBA8(255, 255, 255, 230));
            vita2d_draw_rectangle(gal_x - 1.0f, gal_y + gal_h - 0.5f, gal_w + 2.0f, 1.5f, RGBA8(255, 255, 255, 230));
            vita2d_draw_rectangle(gal_x - 1.0f, gal_y - 1.0f, 1.5f, gal_h + 2.0f, RGBA8(255, 255, 255, 230));
            vita2d_draw_rectangle(gal_x + gal_w - 0.5f, gal_y - 1.0f, 1.5f, gal_h + 2.0f, RGBA8(255, 255, 255, 230));

            // ── 6. Controles Flotantes Modernos - Barra Derecha ──
            float right_cx = 901.0f;

            // Botón Switch Cámara Frontal/Posterior (Flip con icono vectorial real)
            float fbtn_cy = 48.0f;
            vita2d_draw_rectangle(right_cx - btn_r, fbtn_cy - btn_r, btn_r * 2.0f, btn_r * 2.0f, RGBA8(14, 18, 34, 235));
            unsigned int flip_border = (cam_dev == SCE_CAMERA_DEVICE_FRONT) ? RGBA8(0, 210, 255, 240) : RGBA8(255, 255, 255, 40);
            vita2d_draw_rectangle(right_cx - btn_r, fbtn_cy - btn_r, btn_r * 2.0f, 1.5f, flip_border);
            vita2d_draw_rectangle(right_cx - btn_r, fbtn_cy + btn_r - 1.5f, btn_r * 2.0f, 1.5f, flip_border);
            vita2d_draw_rectangle(right_cx - btn_r, fbtn_cy - btn_r, 1.5f, btn_r * 2.0f, flip_border);
            vita2d_draw_rectangle(right_cx + btn_r - 1.5f, fbtn_cy - btn_r, 1.5f, btn_r * 2.0f, flip_border);
            if (icon_tex[ICON_CAM_FLIP]) {
                float sc = 28.0f / (float)vita2d_texture_get_height(icon_tex[ICON_CAM_FLIP]);
                vita2d_draw_texture_scale(icon_tex[ICON_CAM_FLIP], right_cx - 14.0f, fbtn_cy - 14.0f, sc, sc);
            }

            // Botón Obturador Premium (Dual-Ring iOS/Xperia Shutter)
            float s_cx = right_cx, s_cy = 272.0f;
            float s_r_out = 37.0f;
            float s_r_in = (shutter_pressed_anim > 0) ? 25.0f : 29.0f;
            if (shutter_pressed_anim > 0) shutter_pressed_anim--;

            // Bezel exterior oscuro profundo para contraste 100% garantizado
            vita2d_draw_rectangle(s_cx - s_r_out - 4.0f, s_cy - s_r_out - 4.0f, (s_r_out + 4.0f) * 2.0f, (s_r_out + 4.0f) * 2.0f, RGBA8(8, 12, 22, 240));

            // Anillo exterior cromado plateado nítido
            vita2d_draw_rectangle(s_cx - s_r_out, s_cy - s_r_out, s_r_out * 2.0f, 2.5f, RGBA8(230, 235, 248, 255));
            vita2d_draw_rectangle(s_cx - s_r_out, s_cy + s_r_out - 2.5f, s_r_out * 2.0f, 2.5f, RGBA8(230, 235, 248, 255));
            vita2d_draw_rectangle(s_cx - s_r_out, s_cy - s_r_out, 2.5f, s_r_out * 2.0f, RGBA8(230, 235, 248, 255));
            vita2d_draw_rectangle(s_cx + s_r_out - 2.5f, s_cy - s_r_out, 2.5f, s_r_out * 2.0f, RGBA8(230, 235, 248, 255));

            // Espacio interior oscuro que separa el anillo del botón
            vita2d_draw_rectangle(s_cx - s_r_out + 2.5f, s_cy - s_r_out + 2.5f, (s_r_out - 2.5f) * 2.0f, (s_r_out - 2.5f) * 2.0f, RGBA8(12, 16, 30, 255));

            // Botón central blanco nítido con relieve sutil
            vita2d_draw_rectangle(s_cx - s_r_in, s_cy - s_r_in, s_r_in * 2.0f, s_r_in * 2.0f, RGBA8(250, 252, 255, 255));
            vita2d_draw_rectangle(s_cx - s_r_in + 2.0f, s_cy - s_r_in + 2.0f, s_r_in * 2.0f - 4.0f, 2.0f, RGBA8(255, 255, 255, 255));
            vita2d_draw_rectangle(s_cx - s_r_in + 2.0f, s_cy + s_r_in - 4.0f, s_r_in * 2.0f - 4.0f, 2.0f, RGBA8(200, 210, 225, 255));

            // Botón de Grabación de Video
            float vbtn_cx = right_cx, vbtn_cy = 385.0f;
            float vbtn_r_out = 22.0f;
            vita2d_draw_rectangle(vbtn_cx - vbtn_r_out, vbtn_cy - vbtn_r_out, vbtn_r_out * 2.0f, vbtn_r_out * 2.0f, RGBA8(14, 18, 34, 235));
            
            if (is_recording_video) {
                // Anillo rojo pulsante
                vita2d_draw_rectangle(vbtn_cx - vbtn_r_out, vbtn_cy - vbtn_r_out, vbtn_r_out * 2.0f, 2.0f, RGBA8(255, 60, 60, 240));
                vita2d_draw_rectangle(vbtn_cx - vbtn_r_out, vbtn_cy + vbtn_r_out - 2.0f, vbtn_r_out * 2.0f, 2.0f, RGBA8(255, 60, 60, 240));
                vita2d_draw_rectangle(vbtn_cx - vbtn_r_out, vbtn_cy - vbtn_r_out, 2.0f, vbtn_r_out * 2.0f, RGBA8(255, 60, 60, 240));
                vita2d_draw_rectangle(vbtn_cx + vbtn_r_out - 2.0f, vbtn_cy - vbtn_r_out, 2.0f, vbtn_r_out * 2.0f, RGBA8(255, 60, 60, 240));
                
                // Cuadrado rojo Stop central
                vita2d_draw_rectangle(vbtn_cx - 9.0f, vbtn_cy - 9.0f, 18.0f, 18.0f, RGBA8(240, 50, 50, 255));
            } else {
                // Anillo exterior blanco
                vita2d_draw_rectangle(vbtn_cx - vbtn_r_out, vbtn_cy - vbtn_r_out, vbtn_r_out * 2.0f, 1.5f, RGBA8(255, 255, 255, 120));
                vita2d_draw_rectangle(vbtn_cx - vbtn_r_out, vbtn_cy + vbtn_r_out - 1.5f, vbtn_r_out * 2.0f, 1.5f, RGBA8(255, 255, 255, 120));
                vita2d_draw_rectangle(vbtn_cx - vbtn_r_out, vbtn_cy - vbtn_r_out, 1.5f, vbtn_r_out * 2.0f, RGBA8(255, 255, 255, 120));
                vita2d_draw_rectangle(vbtn_cx + vbtn_r_out - 1.5f, vbtn_cy - vbtn_r_out, 1.5f, vbtn_r_out * 2.0f, RGBA8(255, 255, 255, 120));
                
                // Punto circular rojo interior
                float vbtn_r_in = 13.0f;
                vita2d_draw_rectangle(vbtn_cx - vbtn_r_in, vbtn_cy - vbtn_r_in, vbtn_r_in * 2.0f, vbtn_r_in * 2.0f, RGBA8(235, 45, 45, 255));
                vita2d_draw_rectangle(vbtn_cx - vbtn_r_in + 3, vbtn_cy - vbtn_r_in + 3, 5.0f, 3.0f, RGBA8(255, 150, 150, 220));
            }

            // Indicador de Grabación Activa en vivo (Pill superior centrado)
            if (is_recording_video) {
                uint64_t cur_t = sceKernelGetProcessTimeWide();
                uint32_t elapsed_s = (uint32_t)((cur_t - video_start_time) / 1000000);
                uint32_t mins = elapsed_s / 60;
                uint32_t secs = elapsed_s % 60;
                char rec_str[32];
                snprintf(rec_str, sizeof(rec_str), "● REC %02d:%02d", mins, secs);

                float rw = 150.0f, rh = 32.0f;
                float rx = (960.0f - rw) * 0.5f;
                float ry = 14.0f;
                vita2d_draw_rectangle(rx, ry, rw, rh, RGBA8(12, 16, 34, 235));
                vita2d_draw_rectangle(rx, ry, rw, 1.5f, RGBA8(240, 50, 50, 240));
                vita2d_draw_rectangle(rx, ry + rh - 1.5f, rw, 1.5f, RGBA8(240, 50, 50, 240));
                vita2d_draw_rectangle(rx, ry, 1.5f, rh, RGBA8(240, 50, 50, 240));
                vita2d_draw_rectangle(rx + rw - 1.5f, ry, 1.5f, rh, RGBA8(240, 50, 50, 240));

                int blink = (int)((cur_t / 500000) % 2);
                unsigned int rec_col = blink ? RGBA8(255, 60, 60, 255) : RGBA8(255, 255, 255, 220);
                float tw_rec = vita2d_pgf_text_width(pgf, 0.72f, rec_str);
                vita2d_pgf_draw_text(pgf, (int)(rx + (rw - tw_rec) * 0.5f), (int)ry + 23, rec_col, 0.72f, rec_str);
            }

            // Ráfaga / Flash Blanco de Pantalla Completa en Disparo
            if (flash_trigger_anim > 0) {
                vita2d_draw_rectangle(0.0f, 0.0f, 960.0f, 544.0f, RGBA8(255, 255, 255, 255));
            }

            // Toast / Banner de Estado
            if (status_msg_timer > 0 && pgf && !is_recording_video) {
                status_msg_timer--;
                float banner_w = 600.0f;
                float banner_h = 34.0f;
                float banner_x = (960.0f - banner_w) * 0.5f;
                float banner_y = 10.0f;
                vita2d_draw_rectangle(banner_x, banner_y, banner_w, banner_h, RGBA8(12, 16, 34, 230));
                vita2d_draw_rectangle(banner_x, banner_y + banner_h - 2.0f, banner_w, 2.0f, status_msg_color);
                vita2d_pgf_draw_text(pgf, banner_x + 16, banner_y + 22, status_msg_color, 0.78f, status_msg);
            }

            vita2d_end_drawing();
            vita2d_swap_buffers();
        }
        // =================================================================
        // MODO GALERÍA DE FOTOS (CUADRÍCULA CONTINUA + TOUCHSCREEN + SELECCIÓN)
        // =================================================================
        else if (app_mode == APP_MODE_GALLERY) {
            // Suavizado de desplazamiento vertical (Smooth Scroll Interpolation)
            gallery_scroll_y += (gallery_target_scroll_y - gallery_scroll_y) * 0.30f;

            // Procesar entrada táctil de pantalla frontal
            handle_gallery_touch();

            // Avanzar fotograma si la reproducción de video está activa
            if (gallery_view == GALLERY_VIEW_FULLSCREEN) {
                video_playback_tick();
            }

            // -------------------------------------------------------------
            // Modal de Código QR / Servidor Wi-Fi Web
            // -------------------------------------------------------------
            if (gallery_show_wifi_modal) {
                int stick_left = (pad.lx < 64 && old_pad.lx >= 64);
                int stick_right = (pad.lx > 192 && old_pad.lx <= 192);

                if ((pressed & SCE_CTRL_LEFT) || stick_left) {
                    wifi_dialog_focused = (wifi_dialog_focused + 2) % 3; // [ Activar ] <-> [ Nuevo PIN ] <-> [ Cerrar ]
                }
                if ((pressed & SCE_CTRL_RIGHT) || stick_right) {
                    wifi_dialog_focused = (wifi_dialog_focused + 1) % 3;
                }
                if (pressed & SCE_CTRL_CROSS) {
                    if (wifi_dialog_focused == 0) {
                        webserver_set_enabled(!webserver_is_enabled());
                    } else if (wifi_dialog_focused == 1) {
                        char new_pwd[8];
                        uint64_t tick = sceKernelGetProcessTimeWide();
                        snprintf(new_pwd, sizeof(new_pwd), "%04d", (int)((tick % 9000) + 1000));
                        webserver_set_password(new_pwd);
                    } else {
                        gallery_show_wifi_modal = 0;
                        modal_just_closed = 1;
                        x_hold_frames = 0;
                        x_long_fired = 1;
                    }
                } else if ((pressed & SCE_CTRL_CIRCLE) || (pressed & SCE_CTRL_SELECT)) {
                    gallery_show_wifi_modal = 0;
                    modal_just_closed = 1;
                    x_hold_frames = 0;
                    x_long_fired = 1;
                } else if (pressed & SCE_CTRL_TRIANGLE) {
                    char new_pwd[8];
                    uint64_t tick = sceKernelGetProcessTimeWide();
                    snprintf(new_pwd, sizeof(new_pwd), "%04d", (int)((tick % 9000) + 1000));
                    webserver_set_password(new_pwd);
                }
            }
            // -------------------------------------------------------------
            // Modal de Confirmación de Eliminación
            // -------------------------------------------------------------
            else if (gallery_confirm_delete) {
                int stick_left = (pad.lx < 64 && old_pad.lx >= 64);
                int stick_right = (pad.lx > 192 && old_pad.lx <= 192);

                if ((pressed & SCE_CTRL_LEFT) || stick_left) {
                    delete_dialog_focused = 0; // [ Eliminar ] (Sí)
                }
                if ((pressed & SCE_CTRL_RIGHT) || stick_right) {
                    delete_dialog_focused = 1; // [ Cancelar ] (No)
                }
                if (pressed & SCE_CTRL_CROSS) {
                    if (delete_dialog_focused == 0) {
                        gallery_delete_selected();
                    }
                    gallery_confirm_delete = 0;
                    modal_just_closed = 1;
                    x_hold_frames = 0;
                    x_long_fired = 1;
                } else if ((pressed & SCE_CTRL_CIRCLE) || (pressed & SCE_CTRL_TRIANGLE)) {
                    gallery_confirm_delete = 0;
                    modal_just_closed = 1;
                    x_hold_frames = 0;
                    x_long_fired = 1;
                }
            }
            // -------------------------------------------------------------
            // Modal de Información y Créditos (@darking101)
            // -------------------------------------------------------------
            else if (gallery_show_info_modal) {
                if ((pressed & SCE_CTRL_CROSS) || (pressed & SCE_CTRL_CIRCLE) || 
                    (pressed & SCE_CTRL_TRIANGLE) || (pressed & SCE_CTRL_START) || 
                    (pressed & SCE_CTRL_SELECT)) {
                    gallery_show_info_modal = 0;
                    modal_just_closed = 1;
                    x_hold_frames = 0;
                    x_long_fired = 1;
                }
            } 
            // -------------------------------------------------------------
            // Vista de Cuadrícula Continua
            // -------------------------------------------------------------
            else if (gallery_view == GALLERY_VIEW_GRID) {
                // Gatillos L/R para filtrar pestañas superiores
                if (pressed & SCE_CTRL_LTRIGGER) {
                    gallery_switch_source_tab((gallery_source_tab + GALLERY_SOURCE_COUNT - 1) % GALLERY_SOURCE_COUNT);
                }
                if (pressed & SCE_CTRL_RTRIGGER) {
                    gallery_switch_source_tab((gallery_source_tab + 1) % GALLERY_SOURCE_COUNT);
                }
                if (pressed & SCE_CTRL_SELECT) {
                    gallery_show_wifi_modal = 1;
                }

                if (gallery_selection_mode) {
                    if (gallery_count > 0) {
                        if (pressed & SCE_CTRL_LEFT) {
                            gallery_idx--;
                            if (gallery_idx < 0) gallery_idx = gallery_count - 1;
                            gallery_ensure_visible(gallery_idx);
                        }
                        if (pressed & SCE_CTRL_RIGHT) {
                            gallery_idx++;
                            if (gallery_idx >= gallery_count) gallery_idx = 0;
                            gallery_ensure_visible(gallery_idx);
                        }
                        if (pressed & SCE_CTRL_UP) {
                            gallery_idx -= GRID_COLS;
                            if (gallery_idx < 0) gallery_idx = 0;
                            gallery_ensure_visible(gallery_idx);
                        }
                        if (pressed & SCE_CTRL_DOWN) {
                            gallery_idx += GRID_COLS;
                            if (gallery_idx >= gallery_count) gallery_idx = gallery_count - 1;
                            gallery_ensure_visible(gallery_idx);
                        }

                        // En modo selección: Cualquier toque de X alterna selección (si no acaba de cerrar un modal)
                        if (pressed & SCE_CTRL_CROSS) {
                            if (modal_just_closed) {
                                if (!(pad.buttons & SCE_CTRL_CROSS)) {
                                    modal_just_closed = 0;
                                }
                            } else {
                                gallery_toggle_select(gallery_idx);
                            }
                        }
                        // Triángulo: Eliminar fotos seleccionadas
                        if (pressed & SCE_CTRL_TRIANGLE) {
                            if (gallery_count_selected() > 0) gallery_open_delete_dialog();
                        }
                        // Círculo: Cancelar selección
                        if (pressed & SCE_CTRL_CIRCLE) {
                            gallery_clear_selection();
                        }
                    }
                } else {
                    if (gallery_count > 0) {
                        if (pressed & SCE_CTRL_LEFT) {
                            gallery_idx--;
                            if (gallery_idx < 0) gallery_idx = gallery_count - 1;
                            gallery_ensure_visible(gallery_idx);
                        }
                        if (pressed & SCE_CTRL_RIGHT) {
                            gallery_idx++;
                            if (gallery_idx >= gallery_count) gallery_idx = 0;
                            gallery_ensure_visible(gallery_idx);
                        }
                        if (pressed & SCE_CTRL_UP) {
                            gallery_idx -= GRID_COLS;
                            if (gallery_idx < 0) gallery_idx = 0;
                            gallery_ensure_visible(gallery_idx);
                        }
                        if (pressed & SCE_CTRL_DOWN) {
                            gallery_idx += GRID_COLS;
                            if (gallery_idx >= gallery_count) gallery_idx = gallery_count - 1;
                            gallery_ensure_visible(gallery_idx);
                        }

                        // Detección física: X corto (<500ms) = Abrir, X largo (>=500ms) = Entrar a selección
                        if (modal_just_closed) {
                            if (!(pad.buttons & SCE_CTRL_CROSS)) {
                                modal_just_closed = 0;
                            }
                            x_hold_frames = 0;
                            x_long_fired = 0;
                        } else {
                            if (pad.buttons & SCE_CTRL_CROSS) {
                                x_hold_frames++;
                                if (x_hold_frames >= 30 && !x_long_fired) {
                                    x_long_fired = 1;
                                    gallery_toggle_select(gallery_idx);
                                }
                            } else {
                                if (!x_long_fired && x_hold_frames > 0 && x_hold_frames < 30) {
                                    gallery_view = GALLERY_VIEW_FULLSCREEN;
                                    gallery_load_current_photo();
                                }
                                x_hold_frames = 0;
                                x_long_fired = 0;
                            }
                        }

                        // Triángulo: Eliminar foto enfocada
                        if (pressed & SCE_CTRL_TRIANGLE) {
                            gallery_open_delete_dialog();
                        }
                    }

                    // Círculo: Volver a la Cámara
                    if (pressed & SCE_CTRL_CIRCLE) {
                        gallery_exit();
                    }
                }
            }
            // -------------------------------------------------------------
            // Vista a Pantalla Completa
            // -------------------------------------------------------------
            else if (gallery_view == GALLERY_VIEW_FULLSCREEN) {
                if (gallery_count > 0) {
                    if (pressed & SCE_CTRL_LEFT) {
                        gallery_idx--;
                        if (gallery_idx < 0) gallery_idx = gallery_count - 1;
                        gallery_load_current_photo();
                    }
                    if (pressed & SCE_CTRL_RIGHT) {
                        gallery_idx++;
                        if (gallery_idx >= gallery_count) gallery_idx = 0;
                        gallery_load_current_photo();
                    }
                    if (pressed & SCE_CTRL_CROSS) {
                        if (gallery_photos[gallery_idx].is_video) {
                            if (!is_video_playing) video_playback_start();
                            else video_playback_stop();
                        } else {
                            fullscreen_hide_ui = !fullscreen_hide_ui;
                        }
                    }
                    if (pressed & SCE_CTRL_CIRCLE) {
                        if (is_video_playing) {
                            video_playback_stop();
                        } else {
                            gallery_view = GALLERY_VIEW_GRID;
                            gallery_ensure_visible(gallery_idx);
                        }
                    }
                    if (pressed & SCE_CTRL_TRIANGLE) {
                        video_playback_stop();
                        gallery_open_delete_dialog();
                    }
                }
            }

            // -------------------------------------------------------------
            // PRE-DECODE FUERA DE LA ESCENA GPU (Cero Glitches y Cero Tearing)
            // -------------------------------------------------------------
            if (gallery_view == GALLERY_VIEW_GRID && gallery_count > 0) {
                current_render_frame++;
                int decodes_this_frame = 0;
                for (int photo_idx = 0; photo_idx < gallery_count; photo_idx++) {
                    float px, py;
                    gallery_get_photo_rect(photo_idx, &px, &py);
                    float grid_bottom = 544.0f - (float)BOTTOM_BAR_H;
                    if (py + CARD_H < (float)HEADER_H || py > grid_bottom) continue;

                    int slot = gallery_find_or_alloc_thumb_slot(photo_idx);
                    if (slot >= 0) {
                        if ((!thumb_slots[slot].is_loaded || thumb_slots[slot].photo_idx != photo_idx) && thumb_tex[slot]) {
                            if (decodes_this_frame < 1) {
                                load_media_into_thumb_texture(&gallery_photos[photo_idx], thumb_tex[slot]);
                                thumb_slots[slot].photo_idx = photo_idx;
                                thumb_slots[slot].is_loaded = 1;
                                thumb_slots[slot].last_used_frame = current_render_frame;
                                decodes_this_frame++;
                            }
                        } else {
                            thumb_slots[slot].last_used_frame = current_render_frame;
                        }
                    }
                }
            }

            // -------------------------------------------------------------
            // RENDERIZADO DE GALERÍA ESTILO PS VITA (TEMA PLAYSTATION MIDNIGHT BLUE)
            // -------------------------------------------------------------
            vita2d_start_drawing();
            vita2d_clear_screen();

            // ── 1. Fondo Azul Medianoche PlayStation con Ondas Sutiles ───
            vita2d_draw_rectangle(0.0f, 0.0f, 960.0f, 544.0f, RGBA8(11, 14, 30, 255));
            vita2d_draw_rectangle(0.0f, 130.0f, 960.0f, 210.0f, RGBA8(18, 25, 54, 180));
            vita2d_draw_rectangle(0.0f, 310.0f, 960.0f, 234.0f, RGBA8(14, 19, 42, 220));

            if (gallery_view == GALLERY_VIEW_GRID) {
                int sel_count = gallery_count_selected();

                if (gallery_count == 0) {
                    // ── Estado Vacío (Dentro de la misma interfaz con Header y Tabs intactos) ──
                    float card_w = 500.0f, card_h = 110.0f;
                    float card_x = (960.0f - card_w) * 0.5f;
                    float card_y = 190.0f;

                    vita2d_draw_rectangle(card_x, card_y, card_w, card_h, RGBA8(14, 20, 44, 230));
                    vita2d_draw_rectangle(card_x, card_y, card_w, 1.0f, RGBA8(38, 58, 115, 180));
                    vita2d_draw_rectangle(card_x, card_y + card_h - 1.0f, card_w, 1.0f, RGBA8(38, 58, 115, 180));
                    vita2d_draw_rectangle(card_x, card_y, 1.0f, card_h, RGBA8(38, 58, 115, 180));
                    vita2d_draw_rectangle(card_x + card_w - 1.0f, card_y, 1.0f, card_h, RGBA8(38, 58, 115, 180));

                    const char *empty_msg = "No se encontraron fotos en esta seccion";
                    if (gallery_source_tab == GALLERY_SOURCE_VITACAM) empty_msg = "No hay fotos tomadas con VitaCam";
                    else if (gallery_source_tab == GALLERY_SOURCE_PHOTO) empty_msg = "No hay fotos de la camara oficial (ux0:photo)";
                    else if (gallery_source_tab == GALLERY_SOURCE_SCREENSHOT) empty_msg = "No hay capturas de pantalla (Screenshots)";
                    else empty_msg = "No se encontraron fotos en la consola";

                    float tw_empty = vita2d_pgf_text_width(pgf, 0.76f, empty_msg);
                    vita2d_pgf_draw_text(pgf, (int)(card_x + (card_w - tw_empty) * 0.5f), (int)(card_y + 44),
                                         RGBA8(240, 245, 255, 255), 0.76f, empty_msg);

                    const char *empty_sub = "Usa los gatillos L / R o las pestañas inferiores para cambiar";
                    float tw_sub = vita2d_pgf_text_width(pgf, 0.62f, empty_sub);
                    vita2d_pgf_draw_text(pgf, (int)(card_x + (card_w - tw_sub) * 0.5f), (int)(card_y + 76),
                                         RGBA8(140, 165, 210, 220), 0.62f, empty_sub);
                } else {
                    // Recorte GPU por hardware: asegura que nada dibuje sobre header ni bottom bar
                    vita2d_set_clip_rectangle(0, HEADER_H, 960, 544 - BOTTOM_BAR_H);

                    // ── 2. Renderizar grupos de fechas y tarjetas en el flujo real
                    for (int g = 0; g < date_group_count; g++) {
                        DateGroup *grp = &date_groups[g];
                        float grp_header_y = (float)GRID_START_Y + grp->y_pos - gallery_scroll_y;

                        // Divisor de fecha con bullet azul PlayStation
                        if (grp_header_y + 36.0f >= (float)HEADER_H && grp_header_y <= 544.0f - (float)BOTTOM_BAR_H) {
                            vita2d_draw_rectangle(GRID_START_X, (int)(grp_header_y + 11.0f), 4.0f, 14.0f, RGBA8(0, 160, 255, 255));
                            vita2d_pgf_draw_text(pgf, GRID_START_X + 12, (int)(grp_header_y + 23.0f),
                                                 RGBA8(240, 245, 255, 255), 0.82f, grp->date_title);
                            vita2d_draw_rectangle(GRID_START_X, grp_header_y + 28.0f, 880.0f, 1.0f, RGBA8(35, 48, 92, 160));
                        }

                        // Tarjetas de fotos pertenecientes a este día
                        for (int p = 0; p < grp->count; p++) {
                            int photo_idx = grp->start_photo_idx + p;
                            int col = p % GRID_COLS;
                            int row = p / GRID_COLS;

                            float cx = (float)GRID_START_X + (float)col * (CARD_W + GAP_X);
                            float cy = grp_header_y + 36.0f + (float)row * (CARD_H + GAP_Y);

                            float grid_bottom = 544.0f - (float)BOTTOM_BAR_H;
                            if (cy + CARD_H < (float)HEADER_H || cy > grid_bottom) continue;

                            int slot = gallery_find_or_alloc_thumb_slot(photo_idx);

                            // Sombra de tarjeta
                            vita2d_draw_rectangle(cx + 2, cy + 2, CARD_W, CARD_H, RGBA8(0, 0, 0, 70));

                            // Fondo de tarjeta
                            vita2d_draw_rectangle(cx, cy, CARD_W, CARD_H, RGBA8(15, 20, 40, 240));

                            // Imagen miniatura con aspecto exacto 4:3 (280x210, sin cortes)
                            if (slot >= 0 && thumb_slots[slot].is_loaded && thumb_slots[slot].photo_idx == photo_idx && thumb_tex[slot]) {
                                vita2d_draw_texture_scale(thumb_tex[slot], cx, cy, (float)CARD_W / (float)THUMB_WIDTH, (float)CARD_H / (float)THUMB_HEIGHT);
                            } else {
                                // Placeholder shimmer
                                vita2d_draw_rectangle(cx, cy, CARD_W, CARD_H, RGBA8(20, 28, 54, 255));
                                vita2d_draw_rectangle(cx + CARD_W/2 - 16, cy + CARD_H/2 - 2,
                                                      32, 4, RGBA8(45, 60, 105, 200));
                            }

                            // Tira de información inferior translúcida sobrepuesta (formato y hora)
                            vita2d_draw_rectangle(cx, cy + CARD_H - 24, CARD_W, 24, RGBA8(8, 12, 24, 215));
                            vita2d_draw_rectangle(cx, cy + CARD_H - 24, CARD_W, 1, RGBA8(255, 255, 255, 20));

                            // Formato y resolución a la izquierda
                            if (gallery_photos[photo_idx].is_video) {
                                vita2d_pgf_draw_text(pgf, (int)cx + 8, (int)(cy + CARD_H - 7),
                                                     RGBA8(0, 160, 255, 240), 0.56f, "VIDEO • AVI");
                            } else {
                                vita2d_pgf_draw_text(pgf, (int)cx + 8, (int)(cy + CARD_H - 7),
                                                     RGBA8(210, 225, 245, 230), 0.56f, "640x480 • JPG");
                            }

                            // Hora de captura a la derecha (ej. 2:30 PM)
                            const char *time_str = gallery_photos[photo_idx].time_label;
                            float tw_time = (float)vita2d_pgf_text_width(pgf, 0.56f, time_str);
                            vita2d_pgf_draw_text(pgf, (int)(cx + CARD_W - tw_time - 10), (int)(cy + CARD_H - 7),
                                                 RGBA8(160, 185, 230, 220), 0.56f, time_str);

                            // Badge distintivo de Video en la esquina superior izquierda
                            if (gallery_photos[photo_idx].is_video) {
                                vita2d_draw_rectangle(cx + 8.0f, cy + 8.0f, 54.0f, 20.0f, RGBA8(11, 14, 30, 225));
                                vita2d_draw_rectangle(cx + 8.0f, cy + 8.0f, 54.0f, 1.0f, RGBA8(0, 160, 255, 220));
                                vita2d_draw_rectangle(cx + 8.0f, cy + 27.0f, 54.0f, 1.0f, RGBA8(0, 160, 255, 220));
                                vita2d_draw_rectangle(cx + 8.0f, cy + 8.0f, 1.0f, 20.0f, RGBA8(0, 160, 255, 220));
                                vita2d_draw_rectangle(cx + 61.0f, cy + 8.0f, 1.0f, 20.0f, RGBA8(0, 160, 255, 220));
                                vita2d_pgf_draw_text(pgf, (int)cx + 14, (int)cy + 22, RGBA8(0, 160, 255, 255), 0.52f, "▶ VID");
                            }

                            // Indicador de Selección
                            if (gallery_selection_mode) {
                                if (gallery_selected[photo_idx]) {
                                    // Borde verde esmeralda elegante (3px)
                                    vita2d_draw_rectangle(cx, cy, CARD_W, 3, RGBA8(60, 220, 120, 255));
                                    vita2d_draw_rectangle(cx, cy + CARD_H - 3, CARD_W, 3, RGBA8(60, 220, 120, 255));
                                    vita2d_draw_rectangle(cx, cy, 3, CARD_H, RGBA8(60, 220, 120, 255));
                                    vita2d_draw_rectangle(cx + CARD_W - 3, cy, 3, CARD_H, RGBA8(60, 220, 120, 255));

                                    float bx = cx + CARD_W - 28.0f;
                                    float by = cy + 6.0f;
                                    vita2d_draw_rectangle(bx, by, 22, 22, RGBA8(40, 190, 100, 240));
                                    if (icon_tex[ICON_CHECK]) {
                                        float th = (float)vita2d_texture_get_height(icon_tex[ICON_CHECK]);
                                        float sc = 20.0f / th;
                                        vita2d_draw_texture_scale(icon_tex[ICON_CHECK], bx + 1, by + 1, sc, sc);
                                    }
                                } else {
                                    float bx = cx + CARD_W - 28.0f;
                                    float by = cy + 6.0f;
                                    vita2d_draw_rectangle(bx, by, 22, 22, RGBA8(0, 0, 0, 140));
                                    vita2d_draw_rectangle(bx, by, 22, 1, RGBA8(255, 255, 255, 70));
                                    vita2d_draw_rectangle(bx, by + 21, 22, 1, RGBA8(255, 255, 255, 70));
                                    vita2d_draw_rectangle(bx, by, 1, 22, RGBA8(255, 255, 255, 70));
                                    vita2d_draw_rectangle(bx + 21, by, 1, 22, RGBA8(255, 255, 255, 70));
                                }
                            }

                            // Marco de foco PlayStation Neon Cyan/Blue
                            if (photo_idx == gallery_idx && !gallery_selection_mode) {
                                vita2d_draw_rectangle(cx - 2, cy - 2, CARD_W + 4, 3, RGBA8(0, 160, 255, 255));
                                vita2d_draw_rectangle(cx - 2, cy + CARD_H - 1, CARD_W + 4, 3, RGBA8(0, 160, 255, 255));
                                vita2d_draw_rectangle(cx - 2, cy - 2, 3, CARD_H + 4, RGBA8(0, 160, 255, 255));
                                vita2d_draw_rectangle(cx + CARD_W - 1, cy - 2, 3, CARD_H + 4, RGBA8(0, 160, 255, 255));
                            } else if (!gallery_selection_mode) {
                                vita2d_draw_rectangle(cx, cy, CARD_W, 1, RGBA8(35, 50, 95, 200));
                                vita2d_draw_rectangle(cx, cy + CARD_H - 1, CARD_W, 1, RGBA8(35, 50, 95, 200));
                                vita2d_draw_rectangle(cx, cy, 1, CARD_H, RGBA8(35, 50, 95, 200));
                                vita2d_draw_rectangle(cx + CARD_W - 1, cy, 1, CARD_H, RGBA8(35, 50, 95, 200));
                            }
                        }
                    }

                    // Desactivar recorte GPU
                    vita2d_disable_clipping();

                    // ── 3. Scrollbar fino en borde derecho
                    float max_s = gallery_get_max_scroll();
                    if (max_s > 0.0f) {
                        float grid_h_avail = 544.0f - (float)HEADER_H - (float)BOTTOM_BAR_H;
                        float track_y = (float)HEADER_H + 4.0f;
                        float track_h = grid_h_avail - 8.0f;
                        float sb_h = track_h * (grid_h_avail / (grid_h_avail + max_s));
                        if (sb_h < 24.0f) sb_h = 24.0f;
                        float sb_y = track_y + (gallery_scroll_y / max_s) * (track_h - sb_h);
                        vita2d_draw_rectangle(955.0f, track_y, 4.0f, track_h, RGBA8(255, 255, 255, 12));
                        vita2d_draw_rectangle(955.0f, sb_y, 4.0f, sb_h, RGBA8(0, 160, 255, 220));
                    }
                }

                // ── 4. Header Bar (Glassmorphism Midnight Blue + Centrado Matemático)
                vita2d_draw_rectangle(0.0f, 0.0f, 960.0f, (float)HEADER_H, RGBA8(12, 16, 34, 245));
                vita2d_draw_rectangle(0.0f, (float)(HEADER_H - 1), 960.0f, 1.0f, RGBA8(35, 48, 92, 200));

                if (gallery_selection_mode) {
                    // ── Header Modo Selección
                    draw_centered_pill_button(pgf, 12.0f, 8.0f, 120.0f, 32.0f, ICON_CIRCLE,
                                             "Cancelar", 0.72f, RGBA8(240, 245, 255, 255),
                                             RGBA8(24, 34, 64, 230), RGBA8(45, 68, 125, 200));

                    // Contador central
                    char sel_txt[32];
                    snprintf(sel_txt, sizeof(sel_txt), "%d seleccionadas", sel_count);
                    float tw_sel = vita2d_pgf_text_width(pgf, 0.80f, sel_txt);
                    vita2d_pgf_draw_text(pgf, (int)((960.0f - tw_sel) * 0.5f), 30,
                                         RGBA8(240, 245, 255, 255), 0.80f, sel_txt);

                    // Borrar seleccionadas (Pill roja a la derecha)
                    if (sel_count > 0) {
                        char del_btn_txt[32];
                        snprintf(del_btn_txt, sizeof(del_btn_txt), "Borrar (%d)", sel_count);
                        draw_centered_pill_button(pgf, 810.0f, 8.0f, 138.0f, 32.0f, ICON_TRASH,
                                                 del_btn_txt, 0.72f, RGBA8(255, 255, 255, 255),
                                                 RGBA8(210, 36, 36, 240), RGBA8(255, 90, 90, 160));
                    }
                } else {
                    // ── Header Modo Normal
                    // Botón Cámara (izquierda)
                    draw_centered_pill_button(pgf, 12.0f, 8.0f, 108.0f, 32.0f, ICON_CAMERA,
                                             "Cámara", 0.72f, RGBA8(240, 245, 255, 255),
                                             RGBA8(24, 34, 64, 230), RGBA8(45, 68, 125, 200));

                    // Badge de Elementos y Espacio Libre en ux0:
                    float free_gb = get_ux0_free_gb();
                    char info_badge[64];
                    snprintf(info_badge, sizeof(info_badge), "%d fotos • %.1f GB libres", gallery_count, free_gb);
                    draw_centered_pill_button(pgf, 128.0f, 8.0f, 220.0f, 32.0f, (IconId)-1,
                                             info_badge, 0.68f, RGBA8(200, 215, 245, 230),
                                             RGBA8(16, 22, 46, 210), RGBA8(35, 52, 100, 180));

                    // Reloj en Tiempo Real Centrado
                    SceDateTime rtc;
                    sceRtcGetCurrentClockLocalTime(&rtc);
                    char time_str[16];
                    snprintf(time_str, sizeof(time_str), "%02d:%02d", rtc.hour, rtc.minute);
                    float tw_time = vita2d_pgf_text_width(pgf, 0.85f, time_str);
                    vita2d_pgf_draw_text(pgf, (int)((960.0f - tw_time) * 0.5f), 31,
                                         RGBA8(245, 250, 255, 255), 0.85f, time_str);

                    // Botón de Información / Créditos (i) al lado de la batería
                    float info_btn_x = 768.0f, info_btn_y = 8.0f, info_btn_w = 34.0f, info_btn_h = 32.0f;
                    vita2d_draw_rectangle(info_btn_x, info_btn_y, info_btn_w, info_btn_h, RGBA8(18, 26, 52, 220));
                    vita2d_draw_rectangle(info_btn_x, info_btn_y, info_btn_w, 1.0f, RGBA8(0, 210, 255, 180));
                    vita2d_draw_rectangle(info_btn_x, info_btn_y + info_btn_h - 1.0f, info_btn_w, 1.0f, RGBA8(0, 210, 255, 180));
                    vita2d_draw_rectangle(info_btn_x, info_btn_y, 1.0f, info_btn_h, RGBA8(0, 210, 255, 180));
                    vita2d_draw_rectangle(info_btn_x + info_btn_w - 1.0f, info_btn_y, 1.0f, info_btn_h, RGBA8(0, 210, 255, 180));
                    float tw_i = vita2d_pgf_text_width(pgf, 0.85f, "i");
                    vita2d_pgf_draw_text(pgf, (int)(info_btn_x + (info_btn_w - tw_i) * 0.5f), (int)info_btn_y + 24, RGBA8(0, 210, 255, 255), 0.85f, "i");

                    // Indicador de Batería Gráfico Centrado (Verde puro)
                    int bat_pct = scePowerGetBatteryLifePercent();
                    if (bat_pct < 0) bat_pct = 100;
                    int is_charging = scePowerIsBatteryCharging();

                    float pill_x = 812.0f, pill_y = 8.0f, pill_w = 136.0f, pill_h = 32.0f;
                    vita2d_draw_rectangle(pill_x, pill_y, pill_w, pill_h, RGBA8(18, 26, 52, 220));
                    vita2d_draw_rectangle(pill_x, pill_y, pill_w, 1.0f, RGBA8(38, 55, 105, 180));
                    vita2d_draw_rectangle(pill_x, pill_y + pill_h - 1.0f, pill_w, 1.0f, RGBA8(38, 55, 105, 180));
                    vita2d_draw_rectangle(pill_x, pill_y, 1.0f, pill_h, RGBA8(38, 55, 105, 180));
                    vita2d_draw_rectangle(pill_x + pill_w - 1.0f, pill_y, 1.0f, pill_h, RGBA8(38, 55, 105, 180));

                    char bat_str[16];
                    snprintf(bat_str, sizeof(bat_str), "%d%%", bat_pct);
                    float bat_txt_w = vita2d_pgf_text_width(pgf, 0.68f, bat_str);
                    float bat_box_w = 26.0f;
                    float bat_box_h = 14.0f;
                    float bolt_w = is_charging ? 8.0f : 0.0f;
                    float total_bat_content_w = bat_box_w + 3.0f + (is_charging ? bolt_w + 4.0f : 0.0f) + 6.0f + bat_txt_w;

                    float bx = pill_x + (pill_w - total_bat_content_w) * 0.5f;
                    float by = pill_y + (pill_h - bat_box_h) * 0.5f;

                    // Cuerpo de batería
                    vita2d_draw_rectangle(bx, by, bat_box_w, 1.0f, RGBA8(170, 185, 215, 220));
                    vita2d_draw_rectangle(bx, by + bat_box_h - 1.0f, bat_box_w, 1.0f, RGBA8(170, 185, 215, 220));
                    vita2d_draw_rectangle(bx, by, 1.0f, bat_box_h, RGBA8(170, 185, 215, 220));
                    vita2d_draw_rectangle(bx + bat_box_w - 1.0f, by, 1.0f, bat_box_h, RGBA8(170, 185, 215, 220));
                    vita2d_draw_rectangle(bx + bat_box_w, by + 3.0f, 2.5f, 8.0f, RGBA8(170, 185, 215, 220));

                    // Relleno de batería (VERDE PURO)
                    float fill_w = (bat_box_w - 4.0f) * ((float)bat_pct / 100.0f);
                    if (fill_w < 1.0f) fill_w = 1.0f;
                    unsigned int fill_col = (bat_pct > 20) ? RGBA8(45, 215, 85, 255) : (bat_pct > 10 ? RGBA8(240, 180, 30, 255) : RGBA8(240, 50, 50, 255));
                    vita2d_draw_rectangle(bx + 2.0f, by + 2.0f, fill_w, bat_box_h - 4.0f, fill_col);

                    float cur_bx = bx + bat_box_w + 3.0f;
                    if (is_charging) {
                        cur_bx += 2.0f;
                        float ry = by + 1.0f;
                        vita2d_draw_rectangle(cur_bx + 3, ry,     3, 4, RGBA8(255, 225, 40, 255));
                        vita2d_draw_rectangle(cur_bx + 1, ry + 3, 4, 3, RGBA8(255, 225, 40, 255));
                        vita2d_draw_rectangle(cur_bx,     ry + 5, 7, 2, RGBA8(255, 225, 40, 255));
                        vita2d_draw_rectangle(cur_bx + 2, ry + 7, 4, 3, RGBA8(255, 225, 40, 255));
                        vita2d_draw_rectangle(cur_bx + 1, ry + 9, 3, 3, RGBA8(255, 225, 40, 255));
                        cur_bx += bolt_w + 4.0f;
                    } else {
                        cur_bx += 6.0f;
                    }

                    vita2d_pgf_draw_text(pgf, (int)cur_bx, 30, RGBA8(240, 245, 255, 255), 0.68f, bat_str);
                }

                // ── 5. Pestañas Flotantes Inferiores de Origen en Galería (L / R)
                if (!gallery_selection_mode) {
                    float bar_x = 230.0f, bar_y = 442.0f, bar_w = 500.0f, bar_h = 36.0f;
                    vita2d_draw_rectangle(bar_x, bar_y, bar_w, bar_h, RGBA8(12, 16, 36, 220));
                    vita2d_draw_rectangle(bar_x, bar_y, bar_w, 1.0f, RGBA8(45, 68, 130, 180));
                    vita2d_draw_rectangle(bar_x, bar_y + bar_h - 1.0f, bar_w, 1.0f, RGBA8(45, 68, 130, 180));
                    vita2d_draw_rectangle(bar_x, bar_y, 1.0f, bar_h, RGBA8(45, 68, 130, 180));
                    vita2d_draw_rectangle(bar_x + bar_w - 1.0f, bar_y, 1.0f, bar_h, RGBA8(45, 68, 130, 180));

                    // 4 Pestañas limpias (sin texto L / R estorbando)
                    struct { const char *name; float x; float w; } tabs_info[4] = {
                        { "VitaCam",     bar_x + 6.0f,   115.0f },
                        { "Fotos Vita",  bar_x + 125.0f, 120.0f },
                        { "Screenshots", bar_x + 249.0f, 135.0f },
                        { "Todo",        bar_x + 388.0f, 106.0f }
                    };

                    for (int t = 0; t < 4; t++) {
                        int active = (gallery_source_tab == t);
                        if (active) {
                            vita2d_draw_rectangle(tabs_info[t].x, bar_y + 3.0f, tabs_info[t].w, bar_h - 6.0f, RGBA8(0, 140, 255, 230));
                            vita2d_draw_rectangle(tabs_info[t].x, bar_y + 3.0f, tabs_info[t].w, 1.0f, RGBA8(0, 220, 255, 240));
                            vita2d_draw_rectangle(tabs_info[t].x, bar_y + bar_h - 4.0f, tabs_info[t].w, 1.0f, RGBA8(0, 220, 255, 240));
                            vita2d_draw_rectangle(tabs_info[t].x, bar_y + 3.0f, 1.0f, bar_h - 6.0f, RGBA8(0, 220, 255, 240));
                            vita2d_draw_rectangle(tabs_info[t].x + tabs_info[t].w - 1.0f, bar_y + 3.0f, 1.0f, bar_h - 6.0f, RGBA8(0, 220, 255, 240));
                        }
                        float tw = vita2d_pgf_text_width(pgf, 0.68f, tabs_info[t].name);
                        float tx = tabs_info[t].x + (tabs_info[t].w - tw) * 0.5f;
                        unsigned int col = active ? RGBA8(255, 255, 255, 255) : RGBA8(160, 185, 230, 210);
                        vita2d_pgf_draw_text(pgf, (int)tx, (int)bar_y + 24, col, 0.68f, tabs_info[t].name);
                    }
                }

                // ── 6. Bottom Action Bar (Sleek PS Vita Midnight Blue + PlayStation Button Icons)
                float bb_y = 544.0f - (float)BOTTOM_BAR_H;
                vita2d_draw_rectangle(0.0f, bb_y, 960.0f, (float)BOTTOM_BAR_H, RGBA8(12, 16, 34, 245));
                vita2d_draw_rectangle(0.0f, bb_y, 960.0f, 1.0f, RGBA8(35, 48, 92, 200));

                if (gallery_selection_mode) {
                    draw_icon_label(pgf, 480.0f, bb_y + 4.0f, 34.0f, ICON_TRIANGLE,
                                     "Borrar", RGBA8(255, 120, 120, 230));
                    draw_icon_label(pgf, 680.0f, bb_y + 4.0f, 34.0f, ICON_CIRCLE,
                                     "Cancelar", RGBA8(200, 215, 245, 220));
                    draw_icon_label(pgf, 820.0f, bb_y + 4.0f, 34.0f, ICON_CROSS,
                                     "Seleccionar", RGBA8(0, 160, 255, 240));
                } else {
                    int srv_active = webserver_is_active();
                    unsigned int btn_border = srv_active ? RGBA8(0, 230, 118, 200) : RGBA8(0, 160, 255, 160);
                    const char *hub_lbl = srv_active ? "Web Hub (ON)" : "Web Hub";
                    draw_centered_pill_button(pgf, 12.0f, bb_y + 4.0f, 130.0f, 34.0f, (IconId)-1,
                                             hub_lbl, 0.68f,
                                             RGBA8(240, 245, 255, 255),
                                             RGBA8(16, 24, 52, 230), btn_border);

                    draw_icon_label(pgf, 480.0f, bb_y + 4.0f, 34.0f, ICON_TRIANGLE,
                                     "Borrar", RGBA8(255, 120, 120, 230));
                    draw_icon_label(pgf, 660.0f, bb_y + 4.0f, 34.0f, ICON_CIRCLE,
                                     "Salir a Cámara", RGBA8(200, 215, 245, 220));
                    draw_icon_label(pgf, 830.0f, bb_y + 4.0f, 34.0f, ICON_CROSS,
                                     "Ver", RGBA8(0, 160, 255, 240));
                }
            }
            // ── Fullscreen View (Tema PlayStation Midnight Blue) ───────────────────
            else if (gallery_view == GALLERY_VIEW_FULLSCREEN) {
                // Fondo
                vita2d_draw_rectangle(0.0f, 0.0f, 960.0f, 544.0f, RGBA8(11, 14, 30, 255));
                if (!fullscreen_hide_ui) {
                    vita2d_draw_rectangle(0.0f, 130.0f, 960.0f, 210.0f, RGBA8(18, 25, 54, 180));
                    vita2d_draw_rectangle(0.0f, 310.0f, 960.0f, 234.0f, RGBA8(14, 19, 42, 220));
                }

                // Calcular el área de visualización correcta en base al aspect ratio real
                float disp_w, disp_h;
                if (fullscreen_aspect_ratio > (960.0f / 544.0f)) {
                    // Limitado por el ancho (ej. ultrawide)
                    disp_w = 960.0f;
                    disp_h = 960.0f / fullscreen_aspect_ratio;
                } else {
                    // Limitado por el alto (ej. 16:9, 4:3, vertical)
                    disp_h = 544.0f;
                    disp_w = 544.0f * fullscreen_aspect_ratio;
                }

                float disp_x = (960.0f - disp_w) * 0.5f;
                float disp_y = (544.0f - disp_h) * 0.5f;

                if (gallery_tex) {
                    float tex_w = (float)vita2d_texture_get_width(gallery_tex);
                    float tex_h = (float)vita2d_texture_get_height(gallery_tex);
                    float scale_x = (disp_w / tex_w) * fullscreen_zoom;
                    float scale_y = (disp_h / tex_h) * fullscreen_zoom;
                    
                    float final_x = disp_x + fullscreen_pan_x;
                    float final_y = disp_y + fullscreen_pan_y;

                    // Centrar zoom
                    float zoom_offset_x = (disp_w * fullscreen_zoom - disp_w) * 0.5f;
                    float zoom_offset_y = (disp_h * fullscreen_zoom - disp_h) * 0.5f;
                    final_x -= zoom_offset_x;
                    final_y -= zoom_offset_y;

                    // Límite de paneo rudimentario
                    if (fullscreen_pan_x > zoom_offset_x) fullscreen_pan_x = zoom_offset_x;
                    if (fullscreen_pan_x < -zoom_offset_x) fullscreen_pan_x = -zoom_offset_x;
                    if (fullscreen_pan_y > zoom_offset_y) fullscreen_pan_y = zoom_offset_y;
                    if (fullscreen_pan_y < -zoom_offset_y) fullscreen_pan_y = -zoom_offset_y;

                    // Sombra y marco sutil (solo si no hay zoom extremo o UI visible)
                    if (!fullscreen_hide_ui && fullscreen_zoom == 1.0f) {
                        vita2d_draw_rectangle(disp_x - 1.0f, disp_y, 1.0f, disp_h, RGBA8(35, 48, 92, 180));
                        vita2d_draw_rectangle(disp_x + disp_w, disp_y, 1.0f, disp_h, RGBA8(35, 48, 92, 180));
                    }
                    
                    vita2d_draw_texture_scale(gallery_tex, final_x, final_y, scale_x, scale_y);
                }

                // Si está oculto, nos saltamos los controles
                if (fullscreen_hide_ui) goto skip_fullscreen_ui;

                // Superposición de Controles de Video
                if (gallery_count > 0 && gallery_idx >= 0 && gallery_idx < gallery_count && gallery_photos[gallery_idx].is_video) {
                    if (!is_video_playing) {
                        // Botón de Play Central Translúcido
                        float pcx = 480.0f, pcy = 272.0f;
                        vita2d_draw_rectangle(pcx - 34.0f, pcy - 34.0f, 68.0f, 68.0f, RGBA8(11, 14, 30, 210));
                        vita2d_draw_rectangle(pcx - 34.0f, pcy - 34.0f, 68.0f, 2.0f, RGBA8(0, 160, 255, 240));
                        vita2d_draw_rectangle(pcx - 34.0f, pcy + 32.0f, 68.0f, 2.0f, RGBA8(0, 160, 255, 240));
                        vita2d_draw_rectangle(pcx - 34.0f, pcy - 34.0f, 2.0f, 68.0f, RGBA8(0, 160, 255, 240));
                        vita2d_draw_rectangle(pcx + 32.0f, pcy - 34.0f, 2.0f, 68.0f, RGBA8(0, 160, 255, 240));

                        for (int row = 0; row < 22; row++) {
                            int w = (row < 11) ? (row * 2 + 1) : ((21 - row) * 2 + 1);
                            vita2d_draw_rectangle(pcx - 8.0f, pcy - 11.0f + row, (float)w, 1.0f, RGBA8(240, 245, 255, 255));
                        }

                        const char *hint = "Tocar o pulsar X para reproducir";
                        float tw_h = vita2d_pgf_text_width(pgf, 0.64f, hint);
                        vita2d_pgf_draw_text(pgf, (int)(pcx - tw_h * 0.5f), (int)pcy + 58, RGBA8(0, 160, 255, 240), 0.64f, hint);
                    } else {
                        // Barra de progreso de video activa
                        float pb_w = disp_w;
                        float pb_x = disp_x;
                        float pb_y = 544.0f - (float)BOTTOM_BAR_H - 6.0f;
                        float prog = (video_play_total_frames > 0) ? ((float)video_play_frame / (float)video_play_total_frames) : 0.0f;
                        if (prog > 1.0f) prog = 1.0f;

                        vita2d_draw_rectangle(pb_x, pb_y, pb_w, 4.0f, RGBA8(255, 255, 255, 50));
                        vita2d_draw_rectangle(pb_x, pb_y, pb_w * prog, 4.0f, RGBA8(0, 160, 255, 255));
                    }
                }

                // ── Header Bar (Glassmorphism Midnight Blue)
                vita2d_draw_rectangle(0.0f, 0.0f, 960.0f, (float)HEADER_H, RGBA8(12, 16, 34, 245));
                vita2d_draw_rectangle(0.0f, (float)(HEADER_H-1), 960.0f, 1.0f, RGBA8(35, 48, 92, 200));

                // Botón volver a cuadrícula
                draw_centered_pill_button(pgf, 12.0f, 8.0f, 120.0f, 32.0f, ICON_BACK,
                                         "Galería", 0.72f, RGBA8(240, 245, 255, 255),
                                         RGBA8(24, 34, 64, 230), RGBA8(45, 68, 125, 200));

                // Contador de foto e información de captura
                char pos_txt[64];
                if (gallery_count > 0 && gallery_idx >= 0 && gallery_idx < gallery_count) {
                    const char *type_tag = gallery_photos[gallery_idx].is_video ? " [VIDEO]" : "";
                    snprintf(pos_txt, sizeof(pos_txt), "%d / %d%s  •  %s", gallery_idx + 1, gallery_count, type_tag, gallery_photos[gallery_idx].time_label);
                } else {
                    snprintf(pos_txt, sizeof(pos_txt), "%d / %d", gallery_idx + 1, gallery_count);
                }
                float tw_pos = vita2d_pgf_text_width(pgf, 0.76f, pos_txt);
                vita2d_pgf_draw_text(pgf, (int)((960.0f - tw_pos) * 0.5f), 30,
                                     RGBA8(240, 245, 255, 255), 0.76f, pos_txt);

                // Botón borrar
                draw_centered_pill_button(pgf, 820.0f, 8.0f, 130.0f, 32.0f, ICON_TRASH,
                                         "Borrar", 0.72f, RGBA8(255, 255, 255, 255),
                                         RGBA8(210, 36, 36, 240), RGBA8(255, 90, 90, 160));

                // ── Bottom Bar (Glassmorphism Midnight Blue con iconos PlayStation)
                float bb_y = 544.0f - (float)BOTTOM_BAR_H;
                vita2d_draw_rectangle(0.0f, bb_y, 960.0f, (float)BOTTOM_BAR_H, RGBA8(12, 16, 34, 245));
                vita2d_draw_rectangle(0.0f, bb_y, 960.0f, 1.0f, RGBA8(35, 48, 92, 200));

                draw_icon_label(pgf, 20.0f,  bb_y + 4.0f, 34.0f, ICON_LEFT,
                                "Anterior", RGBA8(210, 225, 245, 220));
                draw_icon_label(pgf, 170.0f, bb_y + 4.0f, 34.0f, ICON_RIGHT,
                                "Siguiente", RGBA8(210, 225, 245, 220));

                if (gallery_count > 0 && gallery_photos[gallery_idx].is_video) {
                    draw_icon_label(pgf, 480.0f, bb_y + 4.0f, 34.0f, ICON_CROSS,
                                    is_video_playing ? "Pausar" : "Reproducir", RGBA8(0, 160, 255, 240));
                    draw_icon_label(pgf, 660.0f, bb_y + 4.0f, 34.0f, ICON_TRIANGLE,
                                    "Borrar", RGBA8(255, 120, 120, 230));
                    draw_icon_label(pgf, 810.0f, bb_y + 4.0f, 34.0f, ICON_CIRCLE,
                                    "Salir", RGBA8(200, 215, 245, 220));
                } else {
                    draw_icon_label(pgf, 640.0f, bb_y + 4.0f, 34.0f, ICON_TRIANGLE,
                                    "Borrar", RGBA8(255, 120, 120, 230));
                    draw_icon_label(pgf, 810.0f, bb_y + 4.0f, 34.0f, ICON_CIRCLE,
                                    "Salir", RGBA8(0, 160, 255, 240));
                }
skip_fullscreen_ui: ;
            }

            // ── Modal de Confirmación de Eliminación Proporcional y Transparente ───
            if (gallery_confirm_delete) {
                int sel_count = gallery_count_selected();
                // Translucent backdrop
                vita2d_draw_rectangle(0.0f, 0.0f, 960.0f, 544.0f, RGBA8(0, 0, 0, 110));

                float mw = 480.0f, mh = 210.0f;
                float mx = (960.0f - mw) * 0.5f;
                float my = (544.0f - mh) * 0.5f;

                // Glass frosted box (translucent)
                vita2d_draw_rectangle(mx, my, mw, mh, RGBA8(14, 18, 28, 195));
                vita2d_draw_rectangle(mx, my, mw, 2.0f, RGBA8(230, 60, 60, 220));
                vita2d_draw_rectangle(mx, my, 1.0f, mh, RGBA8(255, 255, 255, 30));
                vita2d_draw_rectangle(mx + mw - 1, my, 1.0f, mh, RGBA8(255, 255, 255, 30));
                vita2d_draw_rectangle(mx, my + mh - 1, mw, 1.0f, RGBA8(255, 255, 255, 30));

                // Title
                char title_str[64];
                if (sel_count > 0) {
                    snprintf(title_str, sizeof(title_str), "¿Eliminar %d fotos seleccionadas?", sel_count);
                } else {
                    snprintf(title_str, sizeof(title_str), "¿Eliminar esta fotografía?");
                }
                float tw_title = vita2d_pgf_text_width(pgf, 0.88f, title_str);
                vita2d_pgf_draw_text(pgf, (int)(mx + (mw - tw_title) * 0.5f), (int)(my + 55),
                                     RGBA8(255, 255, 255, 255), 0.88f, title_str);

                // Subtitle
                const char *sub_str = "Esta acción no se puede deshacer.";
                float tw_sub = vita2d_pgf_text_width(pgf, 0.70f, sub_str);
                vita2d_pgf_draw_text(pgf, (int)(mx + (mw - tw_sub) * 0.5f), (int)(my + 88),
                                     RGBA8(180, 190, 210, 220), 0.70f, sub_str);

                // Action Buttons with focus indicator (Left/Right to choose, X to accept)
                float btn_w = 195.0f, btn_h = 42.0f;
                float btn_y = my + mh - 60.0f;
                float btn1_x = mx + 30.0f;
                float btn2_x = mx + mw - 30.0f - btn_w;

                // [ Eliminar ] (Index 0)
                if (delete_dialog_focused == 0) {
                    // Focused: bright red + focus outline
                    draw_centered_pill_button(pgf, btn1_x, btn_y, btn_w, btn_h,
                                             ICON_CROSS, "Eliminar (Sí)", 0.78f,
                                             RGBA8(255, 255, 255, 255),
                                             RGBA8(210, 36, 36, 250),
                                             RGBA8(255, 255, 255, 240));
                } else {
                    // Unfocused: muted
                    draw_centered_pill_button(pgf, btn1_x, btn_y, btn_w, btn_h,
                                             ICON_NONE, "Eliminar (Sí)", 0.78f,
                                             RGBA8(200, 150, 150, 200),
                                             RGBA8(40, 18, 24, 180),
                                             RGBA8(255, 255, 255, 20));
                }

                // [ Cancelar ] (Index 1 - Default)
                if (delete_dialog_focused == 1) {
                    // Focused: bright slate/blue + focus outline
                    draw_centered_pill_button(pgf, btn2_x, btn_y, btn_w, btn_h,
                                             ICON_CROSS, "Cancelar (No)", 0.78f,
                                             RGBA8(255, 255, 255, 255),
                                             RGBA8(40, 56, 86, 250),
                                             RGBA8(255, 255, 255, 240));
                } else {
                    // Unfocused: muted
                    draw_centered_pill_button(pgf, btn2_x, btn_y, btn_w, btn_h,
                                             ICON_NONE, "Cancelar (No)", 0.78f,
                                             RGBA8(160, 175, 200, 200),
                                             RGBA8(20, 24, 36, 180),
                                             RGBA8(255, 255, 255, 20));
                }
            }

            // Banner de Notificación / Toast
            if (status_msg_timer > 0 && pgf) {
                status_msg_timer--;
                float banner_w = 640.0f;
                float banner_h = 36.0f;
                float banner_x = (960.0f - banner_w) / 2.0f;
                float banner_y = 56.0f;
                vita2d_draw_rectangle(banner_x, banner_y, banner_w, banner_h, RGBA8(10, 16, 24, 230));
                vita2d_draw_rectangle(banner_x, banner_y + banner_h - 2.0f, banner_w, 2.0f, status_msg_color);
                vita2d_pgf_draw_text(pgf, banner_x + 16, banner_y + 24, status_msg_color, 0.82f, status_msg);
            }

            // Modal de Servidor Web y Código QR Wi-Fi
            if (gallery_show_wifi_modal) {
                draw_qr_modal(pgf);
            }

            // Modal de Información y Créditos de Autor
            if (gallery_show_info_modal) {
                draw_info_modal(pgf);
            }

            vita2d_end_drawing();
            vita2d_swap_buffers();
        }
    }

    // 10. Detener hilo de cámara y liberar recursos limpiamente
    cam_thread_run = 0;
    if (cam_thid >= 0) {
        sceKernelWaitThreadEnd(cam_thid, NULL, NULL);
    }

    sceCameraStop(cam_dev);
    sceCameraClose(cam_dev);
    sceKernelFreeMemBlock(cam_mem_uid);
    if (pgf) vita2d_free_pgf(pgf);
    if (cam_tex[0]) vita2d_free_texture(cam_tex[0]);
    if (cam_tex[1]) vita2d_free_texture(cam_tex[1]);
    if (gallery_tex) vita2d_free_texture(gallery_tex);
    if (cam_last_thumb_tex) vita2d_free_texture(cam_last_thumb_tex);
    for (int i = 0; i < THUMB_POOL_SIZE; i++) {
        if (thumb_tex[i]) vita2d_free_texture(thumb_tex[i]);
    }
    for (int i = 0; i < ICON_COUNT; i++) {
        if (icon_tex[i]) vita2d_free_texture(icon_tex[i]);
    }
    webserver_term();
    vita2d_fini();
    sceKernelExitProcess(0);
}

