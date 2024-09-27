/*
 * This file is part of the OpenMV project.
 *
 * Copyright (c) 2013-2024 Ibrahim Abdelkader <iabdalkader@openmv.io>
 * Copyright (c) 2013-2024 Kwabena W. Agyeman <kwagyeman@openmv.io>
 *
 * This work is licensed under the MIT license, see the file LICENSE for details.
 *
 * GENX320 driver.
 */
#include "omv_boardconfig.h"
#if (OMV_GENX320_ENABLE == 1)

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "omv_i2c.h"
#include "sensor.h"
#include "framebuffer.h"
#include "genx320.h"
#include "py/mphal.h"
#include "evt_2_0.h"
#include "psee_genx320.h"

#define BLANK_LINES                     4
#define BLANK_COLUMNS                   4

#define SENSOR_WIDTH                    324
#define SENSOR_HEIGHT                   324

#define ACTIVE_SENSOR_WIDTH             (SENSOR_WIDTH - BLANK_COLUMNS)
#define ACTIVE_SENSOR_HEIGHT            (SENSOR_HEIGHT - BLANK_LINES)
#define ACTIVE_SENSOR_SIZE              (ACTIVE_SENSOR_WIDTH * ACTIVE_SENSOR_HEIGHT)

#define HSYNC_CLOCK_CYCLES              32
#define VSYNC_CLOCK_CYCLES              32

#define CONTRAST_DEFAULT                16
#define BRIGHTNESS_DEFAULT              128

#define I2C_TIMEOUT                     1000
#define SRAM_INIT_TIMEOUT               100

#define INTEGRATION_DEF_PREIOD          10000 // 10ms (100 FPS)
#define INTEGRATION_MIN_PREIOD          0x10 // 16us
#define INTEGRATION_MAX_PREIOD          0x1FFF0 // 131056us

#define FPS_TO_US(fps)                  (1000000 / (fps))

#define EVENT_THRESHOLD_TO_CALIBRATE    100000
#define EVENT_THRESHOLD_SIGMA           10

#if (OMV_GENX320_CAL_ENABLE == 1)
static bool hot_pixels_disabled = false;
#endif // (OMV_GENX320_CAL_ENABLE == 1)
static int32_t contrast = CONTRAST_DEFAULT;
static int32_t brightness = BRIGHTNESS_DEFAULT;

static int reset(sensor_t *sensor) {
    sensor->color_palette = NULL;

    #if (OMV_GENX320_CAL_ENABLE == 1)
    hot_pixels_disabled = false;
    #endif // (OMV_GENX320_CAL_ENABLE == 1)
    contrast = CONTRAST_DEFAULT;
    brightness = BRIGHTNESS_DEFAULT;

    BIAS_Params_t biases = (sensor->chip_id == SAPHIR_ES_ID) ? genx320es_default_biases : genx320mp_default_biases;

    // Force CPI with chicken bits
    psee_sensor_write(TOP_CHICKEN, TOP_CHICKEN_OVERRIDE_MIPI_MODE_EN |
                      TOP_CHICKEN_OVERRIDE_HISTO_MODE_EN |
                      #if (OMV_GENX320_EHC_ENABLE == 1)
                      1 << TOP_CHICKEN_OVERRIDE_HISTO_MODE_Pos |
                      #else
                      0 << TOP_CHICKEN_OVERRIDE_HISTO_MODE_Pos |
                      #endif // (OMV_GENX320_EHC_ENABLE == 1)
                      I2C_TIMEOUT << TOP_CHICKEN_I2C_TIMEOUT_Pos);

    // Start the Init sequence
    psee_sensor_init(&dcmi_evt);

    // Set EVT20 mode
    psee_sensor_write(EDF_CONTROL, 0);

    // Configure Packet and Frame sizes
    psee_sensor_write(CPI_PACKET_SIZE_CONTROL, ACTIVE_SENSOR_WIDTH);
    psee_sensor_write(CPI_PACKET_TIME_CONTROL, ACTIVE_SENSOR_WIDTH << CPI_PACKET_TIME_CONTROL_PERIOD_Pos |
                      HSYNC_CLOCK_CYCLES << CPI_PACKET_TIME_CONTROL_BLANKING_Pos);
    psee_sensor_write(CPI_FRAME_SIZE_CONTROL, ACTIVE_SENSOR_HEIGHT);
    psee_sensor_write(CPI_FRAME_TIME_CONTROL, VSYNC_CLOCK_CYCLES);

    // Enable dropping
    psee_sensor_write(RO_READOUT_CTRL, RO_READOUT_CTRL_DIGITAL_PIPE_EN |
                      RO_READOUT_CTRL_AVOID_BPRESS_TD |
                      RO_READOUT_CTRL_DROP_EN |
                      RO_READOUT_CTRL_DROP_ON_FULL_EN);

    // Disable the Anti-FlicKering filter
    psee_disable_afk();

    // Operation Mode Configuration
    psee_PM3C_config();

    // Set the default border for the Activity map
    psee_set_default_XY_borders(&genx320mp_default_am_borders);

    // Configure the activity map
    psee_configure_activity_map();

    // Set Standard biases
    psee_sensor_set_biases(&biases);

    // Start the sensor
    psee_sensor_start(&dcmi_evt);

    #if (OMV_GENX320_EHC_ENABLE == 1)
    // Bypass Filter
    psee_sensor_write(EHC_PIPELINE_CONTROL, 0);

    // Start sram init
    psee_sensor_write(SRAM_INITN, SRAM_INITN_EHC_STC);
    uint32_t reg;
    psee_sensor_read(SRAM_PD0, &reg);
    psee_sensor_write(SRAM_PD0, reg & ~(SRAM_PD0_EHC_PD | SRAM_PD0_STC0_PD | SRAM_PD0_STC1_PD));
    psee_sensor_write(EHC_INITIALISATION, EHC_INITIALISATION_REQ_INIT);

    // Configure the block
    psee_sensor_write(EHC_EHC_CONTROL, 0 << EHC_CONTROL_ALGO_SEL_Pos | // diff3
                      1 << EHC_CONTROL_TRIG_SEL_Pos); // integration period
    psee_sensor_write(EHC_BITS_SPLITTING, INT8_T_BITS << EHC_BITS_SPLITTING_NEGATIVE_BIT_LENGTH_Pos |
                      0 << EHC_BITS_SPLITTING_POSITIVE_BIT_LENGTH_Pos |
                      0 << EHC_BITS_SPLITTING_OUT_16BITS_PADDING_MODE_Pos);
    psee_sensor_write(EHC_INTEGRATION_PERIOD, INTEGRATION_DEF_PREIOD);

    // Check SRAM INIT done
    for (mp_uint_t start = mp_hal_ticks_ms();; mp_hal_delay_ms(1)) {
        uint32_t reg;
        psee_sensor_read(EHC_INITIALISATION, &reg);

        if (reg & EHC_INITIALISATION_FLAG_INIT_DONE) {
            psee_sensor_write(EHC_INITIALISATION, EHC_INITIALISATION_FLAG_INIT_DONE);
            break;
        }

        if ((mp_hal_ticks_ms() - start) >= SRAM_INIT_TIMEOUT) {
            return -1;
        }
    }

    // Re-enable filter
    psee_sensor_write(EHC_PIPELINE_CONTROL, EHC_PIPELINE_CONTROL_ENABLE);
    #endif // (OMV_GENX320_EHC_ENABLE == 1)

    return 0;
}

static int sleep(sensor_t *sensor, int enable) {
    if (enable) {
        psee_PM2_config();
    } else {
        psee_PM3C_config();
    }
    return 0;
}

static int read_reg(sensor_t *sensor, uint16_t reg_addr) {
    uint32_t reg_data;
    uint8_t addr[] = {(reg_addr >> 8), reg_addr};
    if (omv_i2c_write_bytes(&sensor->i2c_bus, sensor->slv_addr, addr, 2, OMV_I2C_XFER_NO_STOP) != 0) {
        return -1;
    }
    if (omv_i2c_read_bytes(&sensor->i2c_bus, sensor->slv_addr, (uint8_t *) &reg_data, 4, OMV_I2C_XFER_NO_FLAGS) != 0) {
        return -1;
    }
    reg_data = __REV(reg_data);
    return reg_data;
}

static int write_reg(sensor_t *sensor, uint16_t reg_addr, uint16_t reg_data) {
    uint8_t buf[] = {(reg_addr >> 8), reg_addr, (reg_data >> 24), (reg_data >> 16), (reg_data >> 8), reg_data};
    return omv_i2c_write_bytes(&sensor->i2c_bus, sensor->slv_addr, buf, 6, OMV_I2C_XFER_NO_FLAGS);
}

static int set_pixformat(sensor_t *sensor, pixformat_t pixformat) {
    return (pixformat == PIXFORMAT_GRAYSCALE) ? 0 : -1;
}

static int set_framesize(sensor_t *sensor, framesize_t framesize) {
    return (framesize == FRAMESIZE_320X320) ? 0 : -1;
}

static int set_framerate(sensor_t *sensor, int framerate) {
    #if (OMV_GENX320_EHC_ENABLE == 1)
    int us = FPS_TO_US(framerate);

    if (us < INTEGRATION_MIN_PREIOD) {
        return -1;
    }

    if (us > INTEGRATION_MAX_PREIOD) {
        return -1;
    }

    psee_sensor_write(EHC_INTEGRATION_PERIOD, us);
    #endif // (OMV_GENX320_EHC_ENABLE == 1)
    return 0;
}

static int set_contrast(sensor_t *sensor, int level) {
    contrast = __USAT(level, UINT8_T_BITS);
    return 0;
}

static int set_brightness(sensor_t *sensor, int level) {
    brightness = __USAT(level, UINT8_T_BITS);
    return 0;
}

static int set_colorbar(sensor_t *sensor, int enable) {
    uint32_t reg;
    psee_sensor_read(RO_READOUT_CTRL, &reg);
    reg = (reg & ~RO_READOUT_CTRL_ERC_SELF_TEST_EN) | (enable ? RO_READOUT_CTRL_ERC_SELF_TEST_EN : 0);
    psee_sensor_write(RO_READOUT_CTRL, reg);
    return 0;
}

static int set_hmirror(sensor_t *sensor, int enable) {
    psee_sensor_set_flip(enable, sensor->vflip);
    return 0;
}

static int set_vflip(sensor_t *sensor, int enable) {
    psee_sensor_set_flip(sensor->hmirror, enable);
    return 0;
}

#if (OMV_GENX320_CAL_ENABLE == 1)
static void disable_hot_pixels(uint8_t *histogram) {
    // Compute average
    int32_t avg = 0;

    for (uint32_t i = 0; i < ACTIVE_SENSOR_SIZE; i++) {
        avg += histogram[i];
    }

    avg /= ACTIVE_SENSOR_SIZE;

    // Compute std
    int32_t std = 0;

    for (uint32_t i = 0; i < ACTIVE_SENSOR_SIZE; i++) {
        int32_t diff = histogram[i] - avg;
        std += diff * diff;
    }

    std = fast_sqrtf(std / ACTIVE_SENSOR_SIZE);

    int32_t threshold = avg + (std * EVENT_THRESHOLD_SIGMA);

    for (uint32_t y = 0; y < ACTIVE_SENSOR_HEIGHT; y++) {
        // Reset all blocks
        for (uint32_t i = 0; i < (ACTIVE_SENSOR_WIDTH / UINT32_T_BITS); i++) {
            psee_write_ROI_X(i * sizeof(uint32_t), 0);
        }

        // Select line
        uint32_t offset = y / UINT32_T_BITS;
        psee_write_ROI_Y(offset * sizeof(uint32_t), 1 << (y % UINT32_T_BITS));

        // Trigger shadow
        psee_write_ROI_CTRL(ROI_CTRL_PX_SW_RSTN | ROI_CTRL_TD_SHADOW_TRIGGER);

        uint32_t tmp[ACTIVE_SENSOR_WIDTH / UINT32_T_BITS] = {};
        for (uint32_t x = 0; x < ACTIVE_SENSOR_WIDTH; x++) {
            if (histogram[(y * ACTIVE_SENSOR_WIDTH) + x] > threshold) {
                tmp[x / UINT32_T_BITS] |= 1 << (x % UINT32_T_BITS);
            }
        }

        // Write x values to disable
        for (uint32_t i = 0; i < (ACTIVE_SENSOR_WIDTH / UINT32_T_BITS); i++) {
            psee_write_ROI_X(i * sizeof(uint32_t), tmp[i]);
        }

        // Activate block
        psee_write_ROI_CTRL(ROI_CTRL_PX_SW_RSTN | ROI_CTRL_TD_SHADOW_TRIGGER | ROI_CTRL_TD_EN);

        // Disable roi block
        psee_write_ROI_CTRL(ROI_CTRL_PX_SW_RSTN);
        psee_write_ROI_Y(offset * sizeof(uint32_t), 0);
    }
}
#endif // (OMV_GENX320_CAL_ENABLE == 1)

static void snapshot_post_process(image_t *image) {
    #if (OMV_GENX320_EHC_ENABLE == 1)
    for (uint32_t i = 0; i < ACTIVE_SENSOR_SIZE; i++) {
        image->data[i] = __USAT((((int8_t *) image->data)[i] * contrast) + brightness, UINT8_T_BITS);
    }
    #else
    uint8_t *out = fb_alloc(ACTIVE_SENSOR_SIZE, FB_ALLOC_NO_HINT);
    memset(out, brightness, ACTIVE_SENSOR_SIZE);

    for (uint32_t i = 0; i < (ACTIVE_SENSOR_SIZE / sizeof(uint32_t)); i++) {
        uint32_t val = ((uint32_t *) image->data)[i];
        uint32_t x = __EVT20_X(val);
        uint32_t y = __EVT20_Y(val);
        switch (__EVT20_TYPE(val)) {
            case TD_LOW: {
                if ((x < ACTIVE_SENSOR_WIDTH) && (y < ACTIVE_SENSOR_HEIGHT)) {
                    uint32_t index = (y * ACTIVE_SENSOR_WIDTH) + x;
                    out[index] = __USAT(((int32_t) out[index]) - contrast, UINT8_T_BITS);
                }
                break;
            }
            case TD_HIGH: {
                if ((x < ACTIVE_SENSOR_WIDTH) && (y < ACTIVE_SENSOR_HEIGHT)) {
                    uint32_t index = (y * ACTIVE_SENSOR_WIDTH) + x;
                    out[index] = __USAT(((int32_t) out[index]) + contrast, UINT8_T_BITS);
                }
                break;
            }
            default: {
                break;
            }
        }
    }

    memcpy(image->data, out, ACTIVE_SENSOR_SIZE);
    fb_free();
    #endif // (OMV_GENX320_EHC_ENABLE == 1)

    if (sensor.color_palette && (framebuffer_get_buffer_size() >= (ACTIVE_SENSOR_SIZE * sizeof(uint16_t)))) {
        for (int32_t i = ACTIVE_SENSOR_SIZE - 1; i >= 0; i--) {
            ((uint16_t *) image->data)[i] = sensor.color_palette[image->data[i]];
        }
        image->pixfmt = PIXFORMAT_RGB565;
        MAIN_FB()->pixfmt = PIXFORMAT_RGB565;
    }
}

static int snapshot(sensor_t *sensor, image_t *image, uint32_t flags) {
    #if (OMV_GENX320_CAL_ENABLE == 1)
    if (!hot_pixels_disabled) {
        uint8_t *histogram = fb_alloc0(ACTIVE_SENSOR_SIZE, FB_ALLOC_NO_HINT);

        // Collect events to calibrate hot pixels.
        for (uint32_t i = 0; i < EVENT_THRESHOLD_TO_CALIBRATE; ) {
            int ret = sensor_snapshot(sensor, image, flags);

            if (ret < 0) {
                return ret;
            }

            // Build histogram of events.
            #if (OMV_GENX320_EHC_ENABLE == 1)
            for (uint32_t j = 0; j < ACTIVE_SENSOR_SIZE; j++) {
                int32_t val = abs(((int8_t *) image->data)[j]);
                histogram[j] = val;
                i += val;
            }
            #else
            for (uint32_t j = 0; j < (ACTIVE_SENSOR_SIZE / sizeof(uint32_t)); j++) {
                uint32_t val = ((uint32_t *) image->data)[j];
                switch (__EVT20_TYPE(val)) {
                    case TD_LOW:
                    case TD_HIGH: {
                        uint32_t x = __EVT20_X(val);
                        uint32_t y = __EVT20_Y(val);
                        if ((x < ACTIVE_SENSOR_WIDTH) && (y < ACTIVE_SENSOR_HEIGHT)) {
                            uint32_t index = (y * ACTIVE_SENSOR_WIDTH) + x;
                            histogram[index] = __USAT(histogram[index] + 1, UINT8_T_BITS);
                            i++;
                        }
                        break;
                    }
                    default: {
                        break;
                    }
                }
            }
            #endif // (OMV_GENX320_EHC_ENABLE == 1)

            snapshot_post_process(image);
        }

        disable_hot_pixels(histogram);

        fb_free();
        hot_pixels_disabled = true;
    }
    #endif // (OMV_GENX320_CAL_ENABLE == 1)

    int ret = sensor_snapshot(sensor, image, flags);

    if (ret < 0) {
        return ret;
    }

    snapshot_post_process(image);
    return ret;
}

int genx320_init(sensor_t *sensor) {
    // Initialize sensor structure
    sensor->reset = reset;
    sensor->sleep = sleep;
    sensor->read_reg = read_reg;
    sensor->write_reg = write_reg;
    sensor->set_pixformat = set_pixformat;
    sensor->set_framesize = set_framesize;
    sensor->set_framerate = set_framerate;
    sensor->set_contrast = set_contrast;
    sensor->set_brightness = set_brightness;
    sensor->set_colorbar = set_colorbar;
    sensor->set_hmirror = set_hmirror;
    sensor->set_vflip = set_vflip;
    sensor->snapshot = snapshot;

    // Set sensor flags
    sensor->mono_bpp = sizeof(uint8_t);

    return 0;
}
#endif // (OMV_GENX320_ENABLE == 1)
