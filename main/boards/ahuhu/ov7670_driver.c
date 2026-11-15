#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "ov7670_settings.h"
#include "ov7670_driver.h"

#ifndef portTICK_RATE_MS
#define portTICK_RATE_MS portTICK_PERIOD_MS
#endif
#define delay_ms(ms) vTaskDelay((ms > portTICK_PERIOD_MS ? ms / portTICK_PERIOD_MS : 1))

static const char *TAG = "ov7670";

static const esp_cam_sensor_format_t ov7670_format_info[] = {
    {
        .name = "DVP_8bit_20Minput_RGB565_320x240_25fps",
        .format = ESP_CAM_SENSOR_PIXFORMAT_RGB565,
        .port = ESP_CAM_SENSOR_DVP,
        .xclk = 20000000,
        .width = 320,
        .height = 240,
        .regs = ov7670_init_qvga_rgb565,
        .regs_size = ARRAY_SIZE(ov7670_init_qvga_rgb565),
        .fps = 25,
        .isp_info = NULL,
        .mipi_info = {},
        .reserved = NULL,
    },
};

static esp_err_t ov7670_write_reg(esp_sccb_io_handle_t sccb_handle, uint8_t reg, uint8_t val)
{
    return esp_sccb_transmit_reg_a8v8(sccb_handle, reg, val);
}

static esp_err_t ov7670_read_reg(esp_sccb_io_handle_t sccb_handle, uint8_t reg, uint8_t *val)
{
    return esp_sccb_transmit_receive_reg_a8v8(sccb_handle, reg, val);
}

static esp_err_t ov7670_write_array(esp_sccb_io_handle_t sccb_handle, const ov7670_reginfo_t *regs, size_t regs_size)
{
    esp_err_t ret = ESP_OK;
    for (int i = 0; ret == ESP_OK && i < regs_size; i++) {
        if (regs[i].reg == REG_DELAY) {
            delay_ms(regs[i].val);
        } else {
            ret = ov7670_write_reg(sccb_handle, regs[i].reg, regs[i].val);
        }
    }
    return ret;
}

static esp_err_t ov7670_get_sensor_id(esp_cam_sensor_device_t *dev, esp_cam_sensor_id_t *id)
{
    uint8_t pid = 0;
    uint8_t ver = 0;
    esp_err_t ret;

    ret = ov7670_read_reg(dev->sccb_handle, 0x0A, &pid);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = ov7670_read_reg(dev->sccb_handle, 0x0B, &ver);
    if (ret != ESP_OK) {
        return ret;
    }

    if (pid == OV7670_PID) {
        id->pid = pid;
        id->ver = ver;
        id->midl = 0;
        id->midh = 0;
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t ov7670_set_stream(esp_cam_sensor_device_t *dev, int enable)
{
    if (dev->pwdn_pin >= 0) {
        gpio_set_level(dev->pwdn_pin, enable ? 0 : 1);
    }

    uint8_t com7;
    esp_err_t ret = ov7670_read_reg(dev->sccb_handle, 0x12, &com7);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ov7670_set_stream: read COM7 failed, ret=%d", ret);
        return ret;
    }
    ESP_LOGI(TAG, "ov7670_set_stream: enable=%d COM7 before=0x%02x", enable, com7);
    if (enable) {
        // Ensure QVGA(bit4=1) + RGB(bit2=1) + output(bit0=1) = 0x15
        com7 = 0x15;
    } else {
        com7 &= ~0x01;  // COM7 bit0: output disable
    }
    ret = ov7670_write_reg(dev->sccb_handle, 0x12, com7);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ov7670_set_stream: write COM7 failed, ret=%d", ret);
        return ret;
    }
    // Verify write
    uint8_t com7_verify;
    vTaskDelay(pdMS_TO_TICKS(5));
    if (ov7670_read_reg(dev->sccb_handle, 0x12, &com7_verify) == ESP_OK) {
        ESP_LOGI(TAG, "ov7670_set_stream: COM7 after=0x%02x (verify=0x%02x)", com7, com7_verify);
    } else {
        ESP_LOGI(TAG, "ov7670_set_stream: COM7 after=0x%02x", com7);
    }

    /* Verify color bar / scaling registers */
    uint8_t xsc, ysc, clkrc, com10;
    if (ov7670_read_reg(dev->sccb_handle, 0x70, &xsc) == ESP_OK &&
        ov7670_read_reg(dev->sccb_handle, 0x71, &ysc) == ESP_OK) {
        ESP_LOGI(TAG, "ov7670_set_stream: SCALING XSC=0x%02x YSC=0x%02x (color_bar=%s)",
                 xsc, ysc, (xsc & 0x80) ? "ON" : "OFF");
    }
    if (ov7670_read_reg(dev->sccb_handle, 0x11, &clkrc) == ESP_OK &&
        ov7670_read_reg(dev->sccb_handle, 0x15, &com10) == ESP_OK) {
        ESP_LOGI(TAG, "ov7670_set_stream: CLKRC=0x%02x COM10=0x%02x", clkrc, com10);
    }

    dev->stream_status = enable;
    return ESP_OK;
}

static esp_err_t ov7670_set_test_pattern(esp_cam_sensor_device_t *dev, int enable)
{
    uint8_t xsc, ysc;
    esp_err_t ret;

    ret = ov7670_read_reg(dev->sccb_handle, 0x70, &xsc);
    if (ret != ESP_OK) return ret;
    ret = ov7670_read_reg(dev->sccb_handle, 0x71, &ysc);
    if (ret != ESP_OK) return ret;

    if (enable) {
        xsc |= 0x80;   /* SCALING_XSC bit7: color bar enable */
        ysc |= 0x80;   /* SCALING_YSC bit7: 8-bar color bar */
    } else {
        xsc &= ~0x80;
        ysc &= ~0x80;
    }

    ret = ov7670_write_reg(dev->sccb_handle, 0x70, xsc);
    if (ret != ESP_OK) return ret;
    ret = ov7670_write_reg(dev->sccb_handle, 0x71, ysc);
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "ov7670_set_test_pattern: enable=%d xsc=0x%02x ysc=0x%02x", enable, xsc, ysc);
    return ESP_OK;
}

static esp_err_t ov7670_query_para_desc(esp_cam_sensor_device_t *dev, esp_cam_sensor_param_desc_t *qdesc)
{
    switch (qdesc->id) {
    case ESP_CAM_SENSOR_VFLIP:
    case ESP_CAM_SENSOR_HMIRROR:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
        qdesc->number.minimum = 0;
        qdesc->number.maximum = 1;
        qdesc->number.step = 1;
        qdesc->default_value = 0;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t ov7670_get_para_value(esp_cam_sensor_device_t *dev, uint32_t id, void *arg, size_t size)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t ov7670_set_para_value(esp_cam_sensor_device_t *dev, uint32_t id, const void *arg, size_t size)
{
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t ov7670_query_support_formats(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_array_t *formats)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, formats);
    formats->count = ARRAY_SIZE(ov7670_format_info);
    formats->format_array = &ov7670_format_info[0];
    return ESP_OK;
}

static esp_err_t ov7670_query_support_capability(esp_cam_sensor_device_t *dev, esp_cam_sensor_capability_t *sensor_cap)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, sensor_cap);
    sensor_cap->fmt_rgb565 = 1;
    sensor_cap->fmt_yuv = 0;
    sensor_cap->fmt_jpeg = 0;
    return ESP_OK;
}

static esp_err_t ov7670_set_format(esp_cam_sensor_device_t *dev, const esp_cam_sensor_format_t *format)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);

    if (format == NULL) {
        format = &ov7670_format_info[0];
    }

    esp_err_t ret = ov7670_write_array(dev->sccb_handle, (const ov7670_reginfo_t *)format->regs, format->regs_size);
    ESP_RETURN_ON_FALSE(ret == ESP_OK, ESP_CAM_SENSOR_ERR_FAILED_SET_FORMAT, TAG, "format reg list write failed");

    // Verify COM7 register was written correctly, retry if needed
    uint8_t com7;
    for (int retry = 0; retry < 5; retry++) {
        ret = ov7670_read_reg(dev->sccb_handle, 0x12, &com7);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ov7670_set_format: read COM7 verify failed ret=%d", ret);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        // Expected: 0x15 = QVGA(bit4=1) + RGB(bit2=1) + output enable(bit0=1)
        if (com7 == 0x15) {
            ESP_LOGI(TAG, "ov7670_set_format: COM7=0x%02x verified OK", com7);
            break;
        }
        ESP_LOGW(TAG, "ov7670_set_format: COM7=0x%02x (expected 0x15), retry %d/5", com7, retry + 1);
        // Re-write COM7 directly
        ov7670_write_reg(dev->sccb_handle, 0x12, 0x15);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    // Verify critical registers: VREF (0x03), CLKRC (0x11), COM10 (0x15), DBLV (0x6B)
    uint8_t vref, clkrc, com10, dblv;
    if (ov7670_read_reg(dev->sccb_handle, 0x03, &vref) == ESP_OK &&
        ov7670_read_reg(dev->sccb_handle, 0x11, &clkrc) == ESP_OK &&
        ov7670_read_reg(dev->sccb_handle, 0x15, &com10) == ESP_OK &&
        ov7670_read_reg(dev->sccb_handle, 0x6B, &dblv) == ESP_OK) {
        ESP_LOGI(TAG, "ov7670_set_format: VREF=0x%02x CLKRC=0x%02x COM10=0x%02x DBLV=0x%02x",
                 vref, clkrc, com10, dblv);
    }

    dev->cur_format = format;
    return ESP_OK;
}

static esp_err_t ov7670_get_format(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_t *format)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, format);

    if (dev->cur_format != NULL) {
        memcpy(format, dev->cur_format, sizeof(esp_cam_sensor_format_t));
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t ov7670_power_on(esp_cam_sensor_device_t *dev)
{
    if (dev->reset_pin >= 0) {
        gpio_config_t conf = { 0 };
        conf.pin_bit_mask = 1LL << dev->reset_pin;
        conf.mode = GPIO_MODE_OUTPUT;
        gpio_config(&conf);
        gpio_set_level(dev->reset_pin, 0);
        delay_ms(10);
        gpio_set_level(dev->reset_pin, 1);
        delay_ms(10);
    }

    if (dev->pwdn_pin >= 0) {
        gpio_config_t conf = { 0 };
        conf.pin_bit_mask = 1LL << dev->pwdn_pin;
        conf.mode = GPIO_MODE_OUTPUT;
        gpio_config(&conf);
        gpio_set_level(dev->pwdn_pin, 1);
        delay_ms(10);
        gpio_set_level(dev->pwdn_pin, 0);
        delay_ms(10);
    }

    return ESP_OK;
}

static esp_err_t ov7670_power_off(esp_cam_sensor_device_t *dev)
{
    if (dev->pwdn_pin >= 0) {
        gpio_set_level(dev->pwdn_pin, 1);
    }
    if (dev->reset_pin >= 0) {
        gpio_set_level(dev->reset_pin, 0);
    }
    return ESP_OK;
}

static esp_err_t ov7670_priv_ioctl(esp_cam_sensor_device_t *dev, uint32_t cmd, void *arg)
{
    esp_err_t ret = ESP_OK;
    uint8_t regval;
    esp_cam_sensor_reg_val_t *sensor_reg;

    switch (cmd) {
    case ESP_CAM_SENSOR_IOC_HW_RESET:
        if (dev->reset_pin >= 0) {
            gpio_set_level(dev->reset_pin, 0);
            delay_ms(10);
            gpio_set_level(dev->reset_pin, 1);
            delay_ms(10);
        }
        break;
    case ESP_CAM_SENSOR_IOC_SW_RESET:
        ret = ov7670_write_reg(dev->sccb_handle, 0x12, 0x80);
        delay_ms(100);
        break;
    case ESP_CAM_SENSOR_IOC_S_REG:
        sensor_reg = (esp_cam_sensor_reg_val_t *)arg;
        ret = ov7670_write_reg(dev->sccb_handle, sensor_reg->regaddr, sensor_reg->value);
        break;
    case ESP_CAM_SENSOR_IOC_S_STREAM:
        ret = ov7670_set_stream(dev, *(int *)arg);
        break;
    case ESP_CAM_SENSOR_IOC_S_TEST_PATTERN:
        ret = ov7670_set_test_pattern(dev, *(int *)arg);
        break;
    case ESP_CAM_SENSOR_IOC_G_REG:
        sensor_reg = (esp_cam_sensor_reg_val_t *)arg;
        ret = ov7670_read_reg(dev->sccb_handle, sensor_reg->regaddr, &regval);
        if (ret == ESP_OK) {
            sensor_reg->value = regval;
        }
        break;
    case ESP_CAM_SENSOR_IOC_G_CHIP_ID:
        ret = ov7670_get_sensor_id(dev, arg);
        break;
    default:
        ESP_LOGE(TAG, "cmd=%"PRIx32" is not supported", cmd);
        ret = ESP_ERR_INVALID_ARG;
        break;
    }
    return ret;
}

static esp_err_t ov7670_delete(esp_cam_sensor_device_t *dev)
{
    ESP_LOGD(TAG, "del ov7670 (%p)", dev);
    if (dev) {
        free(dev);
    }
    return ESP_OK;
}

static esp_cam_sensor_ops_t ov7670_ops = {
    .query_para_desc = ov7670_query_para_desc,
    .get_para_value = ov7670_get_para_value,
    .set_para_value = ov7670_set_para_value,
    .query_support_formats = ov7670_query_support_formats,
    .query_support_capability = ov7670_query_support_capability,
    .set_format = ov7670_set_format,
    .get_format = ov7670_get_format,
    .priv_ioctl = ov7670_priv_ioctl,
    .del = ov7670_delete
};

esp_cam_sensor_device_t *ov7670_detect(esp_cam_sensor_config_t *config)
{
    if (config == NULL) {
        return NULL;
    }

    esp_cam_sensor_device_t *dev = heap_caps_calloc(1, sizeof(esp_cam_sensor_device_t), MALLOC_CAP_DEFAULT);
    if (dev == NULL) {
        ESP_LOGE(TAG, "No memory for camera");
        return NULL;
    }

    dev->name = (char *)OV7670_SENSOR_NAME;
    dev->ops = &ov7670_ops;
    dev->sccb_handle = config->sccb_handle;
    dev->xclk_pin = config->xclk_pin;
    dev->reset_pin = config->reset_pin;
    dev->pwdn_pin = config->pwdn_pin;
    dev->priv = NULL;
    dev->cur_format = &ov7670_format_info[0];

    if (ov7670_power_on(dev) != ESP_OK) {
        ESP_LOGE(TAG, "power on failed");
        goto err_free;
    }

    if (ov7670_get_sensor_id(dev, &dev->id) != ESP_OK) {
        ESP_LOGE(TAG, "get sensor ID failed");
        goto err_power_off;
    }
    if (dev->id.pid != OV7670_PID) {
        ESP_LOGE(TAG, "sensor is not OV7670, PID=0x%x", dev->id.pid);
        goto err_power_off;
    }

    ESP_LOGI(TAG, "Detected OV7670 camera, PID=0x%x VER=0x%x", dev->id.pid, dev->id.ver);
    return dev;

err_power_off:
    ov7670_power_off(dev);
err_free:
    free(dev);
    return NULL;
}

ESP_CAM_SENSOR_DETECT_FN(ov7670_detect, ESP_CAM_SENSOR_DVP, OV7670_SCCB_ADDR)
{
    ((esp_cam_sensor_config_t *)config)->sensor_port = ESP_CAM_SENSOR_DVP;
    return ov7670_detect(config);
}