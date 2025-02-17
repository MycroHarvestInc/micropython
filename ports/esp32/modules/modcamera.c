#include <string.h>
#include "py/nlr.h"
#include "py/obj.h"
#include "py/runtime.h"
#include "py/binary.h"
#include "mphalport.h"

#include "esp_system.h"
#include "esp_spi_flash.h"
#include "esp_camera.h"
#include "esp_log.h"

#define TAG "camera"

// Pin definitions for M5Stack PoE CAM
#define CAMERA_POWER_DOWN_PIN -1
#define CAMERA_RESET_PIN 15
#define CAMERA_XCLK_PIN 27
#define CAMERA_SIOD_PIN 14
#define CAMERA_SIOC_PIN 12
#define CAMERA_Y9_PIN 19
#define CAMERA_Y8_PIN 36
#define CAMERA_Y7_PIN 18
#define CAMERA_Y6_PIN 39
#define CAMERA_Y5_PIN 5
#define CAMERA_Y4_PIN 34
#define CAMERA_Y3_PIN 35
#define CAMERA_Y2_PIN 32
#define CAMERA_VSYNC_PIN 22
#define CAMERA_HREF_PIN 26
#define CAMERA_PCLK_PIN 21

static camera_config_t camera_config = {
    .pin_pwdn = CAMERA_POWER_DOWN_PIN,
    .pin_reset = CAMERA_RESET_PIN,
    .pin_xclk = CAMERA_XCLK_PIN,
    .pin_sscb_sda = CAMERA_SIOD_PIN,
    .pin_sscb_scl = CAMERA_SIOC_PIN,
    .pin_d7 = CAMERA_Y9_PIN,
    .pin_d6 = CAMERA_Y8_PIN,
    .pin_d5 = CAMERA_Y7_PIN,
    .pin_d4 = CAMERA_Y6_PIN,
    .pin_d3 = CAMERA_Y5_PIN,
    .pin_d2 = CAMERA_Y4_PIN,
    .pin_d1 = CAMERA_Y3_PIN,
    .pin_d0 = CAMERA_Y2_PIN,
    .pin_vsync = CAMERA_VSYNC_PIN,
    .pin_href = CAMERA_HREF_PIN,
    .pin_pclk = CAMERA_PCLK_PIN,

    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,

    .pixel_format = PIXFORMAT_JPEG,    // Match M5PoECAM default format
    .frame_size = FRAMESIZE_QVGA,      // Start with smaller resolution
    .jpeg_quality = 12,                // 0-63, lower means higher quality
    .fb_count = 2,                     // Match M5PoECAM: two frame buffers
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_LATEST,
    .sccb_i2c_port = 0
};

static enum {
    E_CAMERA_INIT,
    E_CAMERA_DEINIT
} status = E_CAMERA_DEINIT;

static bool camera_init_helper(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum {ARG_pixformat, ARG_framesize, ARG_fb_count, ARG_fb_location};
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_pixformat, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = PIXFORMAT_JPEG} },
        { MP_QSTR_framesize, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = FRAMESIZE_QVGA} },
        { MP_QSTR_fb_count, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 2} },
        { MP_QSTR_fb_location, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = CAMERA_FB_IN_PSRAM} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    int format = args[ARG_pixformat].u_int;
    if ((format != PIXFORMAT_RGB565) && (format != PIXFORMAT_GRAYSCALE) && (format != PIXFORMAT_JPEG)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Pixelformat is not valid"));
    }
    camera_config.pixel_format = format;

    int size = args[ARG_framesize].u_int;
    if ((size < 0) || (size > FRAMESIZE_UXGA)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Image framesize is not valid"));
    }
    camera_config.frame_size = size;

    int fb_count = args[ARG_fb_count].u_int;
    if ((fb_count < 1) || (fb_count > 2)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Framebuffer count is not valid"));
    }
    camera_config.fb_count = fb_count;

    int fb_location = args[ARG_fb_location].u_int;
    if ((fb_location != CAMERA_FB_IN_DRAM) && (fb_location != CAMERA_FB_IN_PSRAM)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Framebuffer location is not valid"));
    }
    camera_config.fb_location = fb_location;

    if (status == E_CAMERA_INIT) {
        esp_camera_deinit();
    }

    // Log camera configuration
    ESP_LOGD(TAG, "Initializing camera with pins:");
    ESP_LOGD(TAG, "SIOD (SDA): %d, SIOC (SCL): %d", CAMERA_SIOD_PIN, CAMERA_SIOC_PIN);
    ESP_LOGD(TAG, "XCLK: %d, PCLK: %d", CAMERA_XCLK_PIN, CAMERA_PCLK_PIN);
    ESP_LOGD(TAG, "VSYNC: %d, HREF: %d", CAMERA_VSYNC_PIN, CAMERA_HREF_PIN);
    ESP_LOGD(TAG, "Data pins: Y2=%d, Y3=%d, Y4=%d, Y5=%d, Y6=%d, Y7=%d, Y8=%d, Y9=%d",
             CAMERA_Y2_PIN, CAMERA_Y3_PIN, CAMERA_Y4_PIN, CAMERA_Y5_PIN,
             CAMERA_Y6_PIN, CAMERA_Y7_PIN, CAMERA_Y8_PIN, CAMERA_Y9_PIN);

    // Add delay for power and I2C bus stabilization
    ESP_LOGD(TAG, "Waiting for power and I2C bus stabilization...");
    vTaskDelay(pdMS_TO_TICKS(100));
    
    ESP_LOGD(TAG, "Starting camera initialization...");
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", err);
        if (err == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Camera sensor not found - SCCB probe failed on pins SDA=%d, SCL=%d", 
                    CAMERA_SIOD_PIN, CAMERA_SIOC_PIN);
            ESP_LOGE(TAG, "Please check: 1) Camera power 2) I2C pins 3) I2C pull-ups");
        } else if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "Camera driver in invalid state - possible pin conflict");
        }
        status = E_CAMERA_DEINIT;
        return false;
    }
    ESP_LOGD(TAG, "Camera initialization successful");
    
    // Configure OV3660 sensor settings
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        // First set the pixel format to ensure it's applied before other settings
        s->set_pixformat(s, camera_config.pixel_format);
        ESP_LOGI(TAG, "Set sensor pixel format to: %d (JPEG=%d)", camera_config.pixel_format, PIXFORMAT_JPEG);
        
        // Set PLL settings for JPEG mode
        if (camera_config.pixel_format == PIXFORMAT_JPEG) {
            s->set_pll(s, 0, 24, 1, 1, 3, 0, 1, 8);  // Configure for JPEG mode
            ESP_LOGI(TAG, "Configured PLL for JPEG mode");
        }
        
        s->set_brightness(s, 1);     // -2 to 2
        s->set_contrast(s, 1);       // -2 to 2
        s->set_saturation(s, 0);     // -2 to 2
        s->set_special_effect(s, 0); // 0 to 6 (0 - No Effect, 1 - Negative, 2 - Grayscale, 3 - Red Tint, 4 - Green Tint, 5 - Blue Tint, 6 - Sepia)
        s->set_whitebal(s, 1);       // 0 = disable , 1 = enable
        s->set_awb_gain(s, 1);       // 0 = disable , 1 = enable
        s->set_wb_mode(s, 0);        // 0 to 4 - if awb_gain enabled (0 - Auto, 1 - Sunny, 2 - Cloudy, 3 - Office, 4 - Home)
        s->set_exposure_ctrl(s, 1);  // 0 = disable , 1 = enable
        s->set_aec2(s, 0);          // 0 = disable , 1 = enable
        s->set_gain_ctrl(s, 1);      // 0 = disable , 1 = enable
        s->set_agc_gain(s, 0);       // 0 to 30
        s->set_gainceiling(s, (gainceiling_t)0);  // 0 to 6
        s->set_bpc(s, 0);           // 0 = disable , 1 = enable
        s->set_wpc(s, 1);           // 0 = disable , 1 = enable
        s->set_raw_gma(s, 1);       // 0 = disable , 1 = enable
        s->set_lenc(s, 1);          // 0 = disable , 1 = enable
        s->set_hmirror(s, 0);       // 0 = disable , 1 = enable
        s->set_vflip(s, 0);         // 0 = disable , 1 = enable
        s->set_dcw(s, 1);           // 0 = disable , 1 = enable
        s->set_colorbar(s, 0);      // 0 = disable , 1 = enable
    }
    
    status = E_CAMERA_INIT;
    return true;
}

static mp_obj_t camera_init(size_t n_pos_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    bool init = camera_init_helper(n_pos_args, pos_args, kw_args);
    if (init) {
        return mp_const_true;
    } else {
        ESP_LOGE(TAG, "Camera init Failed");
        return mp_const_false;
    }
}
static MP_DEFINE_CONST_FUN_OBJ_KW(camera_init_obj, 0, camera_init);

static mp_obj_t camera_deinit(void) {
    esp_err_t err = esp_camera_deinit();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera deinit Failed");
        return mp_const_false;
    }
    status = E_CAMERA_DEINIT;
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_0(camera_deinit_obj, camera_deinit);

static mp_obj_t camera_capture(void) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Camera capture Failed");
        return mp_const_false;
    }
    
    // Log capture format and size
    ESP_LOGD(TAG, "Captured frame - Format: %d, Size: %d bytes", fb->format, fb->len);
    
    // Verify format
    if (fb->format != PIXFORMAT_JPEG) {
        ESP_LOGW(TAG, "Unexpected format: %d (expected JPEG)", fb->format);
    }
    
    mp_obj_t image = mp_obj_new_bytes(fb->buf, fb->len);
    esp_camera_fb_return(fb);
    return image;
}
static MP_DEFINE_CONST_FUN_OBJ_0(camera_capture_obj, camera_capture);

static mp_obj_t camera_framesize(mp_obj_t size_in) {
    int size = mp_obj_get_int(size_in);
    if ((size < 0) || (size > FRAMESIZE_UXGA)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Invalid frame size"));
    }
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        ESP_LOGE(TAG, "Camera sensor not found");
        return mp_const_false;
    }
    if (s->set_framesize(s, size) != 0) {
        return mp_const_false;
    }
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_1(camera_framesize_obj, camera_framesize);

static mp_obj_t camera_quality(mp_obj_t quality_in) {
    int quality = mp_obj_get_int(quality_in);
    if ((quality < 10) || (quality > 63)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Invalid quality value"));
    }
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        ESP_LOGE(TAG, "Camera sensor not found");
        return mp_const_false;
    }
    if (s->set_quality(s, quality) != 0) {
        return mp_const_false;
    }
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_1(camera_quality_obj, camera_quality);

static mp_obj_t camera_contrast(mp_obj_t contrast_in) {
    int contrast = mp_obj_get_int(contrast_in);
    if ((contrast < -2) || (contrast > 2)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Invalid contrast value"));
    }
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        ESP_LOGE(TAG, "Camera sensor not found");
        return mp_const_false;
    }
    if (s->set_contrast(s, contrast) != 0) {
        return mp_const_false;
    }
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_1(camera_contrast_obj, camera_contrast);

static mp_obj_t camera_colorbar(mp_obj_t enable_in) {
    bool enable = mp_obj_is_true(enable_in);
    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        ESP_LOGE(TAG, "Camera sensor not found");
        return mp_const_false;
    }
    if (s->set_colorbar(s, enable) != 0) {
        return mp_const_false;
    }
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_1(camera_colorbar_obj, camera_colorbar);

static const mp_rom_map_elem_t camera_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_camera) },
    
    // Format constants
    { MP_ROM_QSTR(MP_QSTR_JPEG), MP_ROM_INT(PIXFORMAT_JPEG) },
    { MP_ROM_QSTR(MP_QSTR_RGB565), MP_ROM_INT(PIXFORMAT_RGB565) },
    { MP_ROM_QSTR(MP_QSTR_GRAYSCALE), MP_ROM_INT(PIXFORMAT_GRAYSCALE) },
    
    // Functions
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&camera_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&camera_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_capture), MP_ROM_PTR(&camera_capture_obj) },
    { MP_ROM_QSTR(MP_QSTR_framesize), MP_ROM_PTR(&camera_framesize_obj) },
    { MP_ROM_QSTR(MP_QSTR_quality), MP_ROM_PTR(&camera_quality_obj) },
    { MP_ROM_QSTR(MP_QSTR_contrast), MP_ROM_PTR(&camera_contrast_obj) },
    { MP_ROM_QSTR(MP_QSTR_colorbar), MP_ROM_PTR(&camera_colorbar_obj) },
    
    // Constants
    { MP_ROM_QSTR(MP_QSTR_FRAME_96X96), MP_ROM_INT(FRAMESIZE_96X96) },
    { MP_ROM_QSTR(MP_QSTR_FRAME_QQVGA), MP_ROM_INT(FRAMESIZE_QQVGA) },
    { MP_ROM_QSTR(MP_QSTR_FRAME_QCIF), MP_ROM_INT(FRAMESIZE_QCIF) },
    { MP_ROM_QSTR(MP_QSTR_FRAME_HQVGA), MP_ROM_INT(FRAMESIZE_HQVGA) },
    { MP_ROM_QSTR(MP_QSTR_FRAME_240X240), MP_ROM_INT(FRAMESIZE_240X240) },
    { MP_ROM_QSTR(MP_QSTR_FRAME_QVGA), MP_ROM_INT(FRAMESIZE_QVGA) },
    { MP_ROM_QSTR(MP_QSTR_FRAME_CIF), MP_ROM_INT(FRAMESIZE_CIF) },
    { MP_ROM_QSTR(MP_QSTR_FRAME_VGA), MP_ROM_INT(FRAMESIZE_VGA) },
    { MP_ROM_QSTR(MP_QSTR_FRAME_SVGA), MP_ROM_INT(FRAMESIZE_SVGA) },
    { MP_ROM_QSTR(MP_QSTR_FRAME_XGA), MP_ROM_INT(FRAMESIZE_XGA) },
    { MP_ROM_QSTR(MP_QSTR_FRAME_HD), MP_ROM_INT(FRAMESIZE_HD) },
    { MP_ROM_QSTR(MP_QSTR_FRAME_SXGA), MP_ROM_INT(FRAMESIZE_SXGA) },
    { MP_ROM_QSTR(MP_QSTR_FRAME_UXGA), MP_ROM_INT(FRAMESIZE_UXGA) },
};
static MP_DEFINE_CONST_DICT(camera_module_globals, camera_module_globals_table);

const mp_obj_module_t camera_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&camera_module_globals,
};

// Register the module to be available in Python
#if MICROPY_PY_CAMERA
MP_REGISTER_MODULE(MP_QSTR_camera, camera_module);
#endif
