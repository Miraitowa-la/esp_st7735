/**
 * @file st7735.c
 * @brief ST7735 SPI LCD 驱动实现文件。
 *
 * 本文件实现了一个轻量级、可多实例化的 ST7735 驱动。驱动没有在内部初始化 SPI 总线，
 * 而是由用户在 main.c 或业务层调用 spi_bus_initialize()，这样可以让 LCD、Flash、
 * 触摸芯片、其他 SPI 外设共享同一条 SPI 总线。
 *
 * 数据格式说明：
 * - ST7735 使用 RGB565 作为 16bit 像素格式；
 * - LCD 控制器接收像素时要求高字节在前；
 * - ESP32 为小端架构，因此写入前需要进行字节交换。
 */

#include "st7735.h"

#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

/* ESP-IDF 日志标签。esp_check.h 中的 ESP_RETURN_ON_* 宏会使用该 TAG 输出错误信息。 */
#define TAG "st7735"

/*
 * ST7735 常用命令定义。
 * 这里只列出驱动实际使用到的命令，便于维护和减少无关宏。
 */
#define ST7735_NOP     0x00  /*!< 空操作 */
#define ST7735_SWRESET 0x01  /*!< 软件复位 */
#define ST7735_RDDID   0x04  /*!< 读取显示 ID，本驱动暂未使用 */
#define ST7735_RDDST   0x09  /*!< 读取显示状态，本驱动暂未使用 */
#define ST7735_SLPIN   0x10  /*!< 进入睡眠模式，本驱动暂未使用 */
#define ST7735_SLPOUT  0x11  /*!< 退出睡眠模式 */
#define ST7735_PTLON   0x12  /*!< 部分显示模式，本驱动暂未使用 */
#define ST7735_NORON   0x13  /*!< 普通显示模式 */
#define ST7735_INVOFF  0x20  /*!< 关闭颜色反转 */
#define ST7735_INVON   0x21  /*!< 开启颜色反转 */
#define ST7735_DISPOFF 0x28  /*!< 关闭显示输出 */
#define ST7735_DISPON  0x29  /*!< 打开显示输出 */
#define ST7735_CASET   0x2A  /*!< Column Address Set：设置列地址窗口 */
#define ST7735_RASET   0x2B  /*!< Row Address Set：设置行地址窗口 */
#define ST7735_RAMWR   0x2C  /*!< Memory Write：向显存写入像素数据 */
#define ST7735_MADCTL  0x36  /*!< Memory Data Access Control：扫描方向与颜色顺序 */
#define ST7735_COLMOD  0x3A  /*!< Interface Pixel Format：像素格式 */
#define ST7735_FRMCTR1 0xB1  /*!< 帧率控制：普通模式 */
#define ST7735_FRMCTR2 0xB2  /*!< 帧率控制：空闲模式 */
#define ST7735_FRMCTR3 0xB3  /*!< 帧率控制：部分模式 */
#define ST7735_INVCTR  0xB4  /*!< 显示反转控制 */
#define ST7735_PWCTR1  0xC0  /*!< 电源控制 1 */
#define ST7735_PWCTR2  0xC1  /*!< 电源控制 2 */
#define ST7735_PWCTR3  0xC2  /*!< 电源控制 3 */
#define ST7735_PWCTR4  0xC3  /*!< 电源控制 4 */
#define ST7735_PWCTR5  0xC4  /*!< 电源控制 5 */
#define ST7735_VMCTR1  0xC5  /*!< VCOM 控制 */
#define ST7735_GMCTRP1 0xE0  /*!< 正极性 Gamma 校正 */
#define ST7735_GMCTRN1 0xE1  /*!< 负极性 Gamma 校正 */

/* MADCTL 寄存器位定义：用于控制刷新方向、行列交换、RGB/BGR 顺序。 */
#define MADCTL_MY  0x80      /*!< Row Address Order：Y 方向镜像 */
#define MADCTL_MX  0x40      /*!< Column Address Order：X 方向镜像 */
#define MADCTL_MV  0x20      /*!< Row/Column Exchange：行列交换，用于 90/270 度旋转 */
#define MADCTL_BGR 0x08      /*!< RGB-BGR Order：置位后使用 BGR 顺序 */

/**
 * @brief LCD 实例内部状态。
 *
 * 该结构体不暴露给用户，外部只能通过 lcd_st7735_t* 句柄调用 API。
 * 这样可以避免用户误改内部 SPI 句柄、偏移、旋转状态等关键数据。
 */
struct lcd_st7735_t {
    spi_device_handle_t spi;           /*!< 通过 spi_bus_add_device() 得到的 SPI 设备句柄 */
    gpio_num_t pin_dc;                 /*!< 命令/数据选择引脚 */
    gpio_num_t pin_rst;                /*!< 硬件复位引脚，未使用时为 GPIO_NUM_NC */
    gpio_num_t pin_bl;                 /*!< 背光控制引脚，未使用时为 GPIO_NUM_NC */

    int raw_width;                     /*!< 0 度方向下的物理/逻辑宽度 */
    int raw_height;                    /*!< 0 度方向下的物理/逻辑高度 */
    int width;                         /*!< 当前旋转方向下的逻辑宽度 */
    int height;                        /*!< 当前旋转方向下的逻辑高度 */
    uint8_t x_offset;                  /*!< 0 度方向下的列偏移 */
    uint8_t y_offset;                  /*!< 0 度方向下的行偏移 */
    uint8_t x_offset_rot;              /*!< 当前旋转方向下实际使用的列偏移 */
    uint8_t y_offset_rot;              /*!< 当前旋转方向下实际使用的行偏移 */
    lcd_st7735_color_order_t color_order; /*!< 当前颜色顺序 */
    lcd_st7735_rotation_t rotation;    /*!< 当前旋转方向 */
    bool invert_color;                 /*!< 当前是否开启颜色反转 */
    bool reset_level;                  /*!< RST 有效电平 */
};

/**
 * @brief 向 LCD 写入 1 字节命令。
 *
 * DC=0 表示当前传输内容为命令。这里使用 polling transmit，代码简单可靠；
 * 对小屏幕来说性能已经足够。若后续需要更高帧率，可扩展为 queued DMA 传输。
 */
static esp_err_t lcd_write_cmd(lcd_st7735_t *lcd, uint8_t cmd)
{
    ESP_RETURN_ON_FALSE(lcd, ESP_ERR_INVALID_ARG, TAG, "lcd is null");

    spi_transaction_t t = {0};
    t.length = 8;                      /* SPI 传输长度单位为 bit，1 字节命令即 8 bit */
    t.tx_buffer = &cmd;

    gpio_set_level(lcd->pin_dc, 0);    /* 命令模式 */
    return spi_device_polling_transmit(lcd->spi, &t);
}

/**
 * @brief 向 LCD 写入数据。
 *
 * DC=1 表示当前传输内容为参数或像素数据。
 * len 的单位为字节，函数内部转换为 bit 数传给 SPI 驱动。
 */
static esp_err_t lcd_write_data(lcd_st7735_t *lcd, const void *data, int len)
{
    ESP_RETURN_ON_FALSE(lcd && data && len > 0, ESP_ERR_INVALID_ARG, TAG, "invalid data");

    spi_transaction_t t = {0};
    t.length = len * 8;
    t.tx_buffer = data;

    gpio_set_level(lcd->pin_dc, 1);    /* 数据模式 */
    return spi_device_polling_transmit(lcd->spi, &t);
}

/**
 * @brief 写命令并可选写入随后的参数数据。
 *
 * ST7735 大多数寄存器配置都遵循“先写命令，再写 N 字节参数”的格式。
 */
static esp_err_t lcd_write_cmd_data(lcd_st7735_t *lcd, uint8_t cmd, const void *data, int len)
{
    ESP_RETURN_ON_ERROR(lcd_write_cmd(lcd, cmd), TAG, "write cmd failed");
    if (data && len > 0) {
        ESP_RETURN_ON_ERROR(lcd_write_data(lcd, data, len), TAG, "write data failed");
    }
    return ESP_OK;
}

/**
 * @brief FreeRTOS 毫秒延时封装。
 *
 * ST7735 退出睡眠、复位后必须等待一段时间，否则后续命令可能无效。
 */
static void lcd_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/**
 * @brief 执行 LCD 复位。
 *
 * 如果配置了 RST 引脚，则进行硬件复位；否则发送软件复位命令。
 */
static esp_err_t lcd_hw_reset(lcd_st7735_t *lcd)
{
    if (lcd->pin_rst == GPIO_NUM_NC) {
        /* 未连接 RST 时使用软件复位。软件复位后需要等待控制器重新进入可通信状态。 */
        ESP_RETURN_ON_ERROR(lcd_write_cmd(lcd, ST7735_SWRESET), TAG, "software reset failed");
        lcd_delay_ms(150);
        return ESP_OK;
    }

    /*
     * reset_level 表示复位有效电平。
     * 常见 LCD 模组为低电平复位：reset_level=false。
     */
    gpio_set_level(lcd->pin_rst, !lcd->reset_level);
    lcd_delay_ms(20);
    gpio_set_level(lcd->pin_rst, lcd->reset_level);
    lcd_delay_ms(20);
    gpio_set_level(lcd->pin_rst, !lcd->reset_level);
    lcd_delay_ms(150);
    return ESP_OK;
}

/**
 * @brief 设置 ST7735 显存写入窗口。
 *
 * 后续像素数据会从该窗口左上角开始，按控制器扫描顺序连续写入。
 * 注意：x_offset_rot/y_offset_rot 用于补偿模组玻璃可视区域与 ST7735 显存起点之间的偏移。
 */
static esp_err_t lcd_set_window(lcd_st7735_t *lcd, int x0, int y0, int x1, int y1)
{
    uint16_t xs = (uint16_t)(x0 + lcd->x_offset_rot);
    uint16_t xe = (uint16_t)(x1 + lcd->x_offset_rot);
    uint16_t ys = (uint16_t)(y0 + lcd->y_offset_rot);
    uint16_t ye = (uint16_t)(y1 + lcd->y_offset_rot);

    /* CASET/RASET 参数均为：起始高字节、起始低字节、结束高字节、结束低字节。 */
    uint8_t caset[4] = { xs >> 8, xs & 0xFF, xe >> 8, xe & 0xFF };
    uint8_t raset[4] = { ys >> 8, ys & 0xFF, ye >> 8, ye & 0xFF };

    ESP_RETURN_ON_ERROR(lcd_write_cmd_data(lcd, ST7735_CASET, caset, sizeof(caset)), TAG, "CASET failed");
    ESP_RETURN_ON_ERROR(lcd_write_cmd_data(lcd, ST7735_RASET, raset, sizeof(raset)), TAG, "RASET failed");
    ESP_RETURN_ON_ERROR(lcd_write_cmd(lcd, ST7735_RAMWR), TAG, "RAMWR failed");
    return ESP_OK;
}

/**
 * @brief 将 ESP32 主机端 RGB565 字节序转换为 LCD 传输字节序。
 *
 * ESP32 小端内存中 0xF800 存储为 00 F8，但 ST7735 期望接收 F8 00。
 */
static uint16_t color_to_be(uint16_t color)
{
    return (uint16_t)((color >> 8) | (color << 8));
}

esp_err_t lcd_st7735_new(const lcd_st7735_config_t *config, lcd_st7735_t **out_lcd)
{
    ESP_RETURN_ON_FALSE(config && out_lcd, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(config->width > 0 && config->height > 0, ESP_ERR_INVALID_ARG, TAG, "invalid size");
    ESP_RETURN_ON_FALSE(config->pin_cs != GPIO_NUM_NC && config->pin_dc != GPIO_NUM_NC, ESP_ERR_INVALID_ARG, TAG, "CS/DC required");

    /* 分配实例对象。使用 calloc 可以保证所有字段默认清零，减少未初始化风险。 */
    lcd_st7735_t *lcd = calloc(1, sizeof(lcd_st7735_t));
    ESP_RETURN_ON_FALSE(lcd, ESP_ERR_NO_MEM, TAG, "no memory for lcd");

    /* 保存配置到实例内部，后续所有 API 均通过 lcd 句柄访问这些状态。 */
    lcd->pin_dc = config->pin_dc;
    lcd->pin_rst = config->pin_rst;
    lcd->pin_bl = config->pin_bl;
    lcd->raw_width = config->width;
    lcd->raw_height = config->height;
    lcd->width = config->width;
    lcd->height = config->height;
    lcd->x_offset = config->x_offset;
    lcd->y_offset = config->y_offset;
    lcd->x_offset_rot = config->x_offset;
    lcd->y_offset_rot = config->y_offset;
    lcd->color_order = config->color_order;
    lcd->invert_color = config->invert_color;
    lcd->reset_level = config->reset_level;

    /* DC/RST/BL 都是普通 GPIO 输出。CS 由 SPI Master 驱动自动控制，因此这里不配置 CS。 */
    gpio_config_t io_conf = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << config->pin_dc),
    };
    if (config->pin_rst != GPIO_NUM_NC) {
        io_conf.pin_bit_mask |= (1ULL << config->pin_rst);
    }
    if (config->pin_bl != GPIO_NUM_NC) {
        io_conf.pin_bit_mask |= (1ULL << config->pin_bl);
    }
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        free(lcd);
        return ret;
    }

    /*
     * 添加 SPI 设备。
     * mode=0 是 ST7735 常见 SPI 模式；queue_size 预留给后续异步队列扩展。
     */
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = config->spi_clock_hz > 0 ? config->spi_clock_hz : 20 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = config->pin_cs,
        .queue_size = 7,
        .flags = SPI_DEVICE_NO_DUMMY,
    };

    ret = spi_bus_add_device(config->spi_host, &devcfg, &lcd->spi);
    if (ret != ESP_OK) {
        free(lcd);
        return ret;
    }

    /* 创建后立即初始化屏幕，使返回给用户的句柄处于可绘制状态。 */
    ret = lcd_st7735_init(lcd);
    if (ret != ESP_OK) {
        spi_bus_remove_device(lcd->spi);
        free(lcd);
        return ret;
    }

    *out_lcd = lcd;
    return ESP_OK;
}

esp_err_t lcd_st7735_del(lcd_st7735_t *lcd)
{
    ESP_RETURN_ON_FALSE(lcd, ESP_ERR_INVALID_ARG, TAG, "lcd is null");

    /* 只移除该 LCD 设备，不释放 SPI 总线，避免影响同总线上的其他外设。 */
    esp_err_t ret = spi_bus_remove_device(lcd->spi);
    free(lcd);
    return ret;
}

esp_err_t lcd_st7735_init(lcd_st7735_t *lcd)
{
    ESP_RETURN_ON_FALSE(lcd, ESP_ERR_INVALID_ARG, TAG, "lcd is null");

    ESP_RETURN_ON_ERROR(lcd_hw_reset(lcd), TAG, "reset failed");

    /* 退出睡眠模式后，ST7735 数据手册通常要求等待约 120ms。 */
    ESP_RETURN_ON_ERROR(lcd_write_cmd(lcd, ST7735_SLPOUT), TAG, "sleep out failed");
    lcd_delay_ms(120);

    /*
     * 以下初始化序列参考 ST7735R 常用配置，适用于大多数 ST7735 小尺寸 SPI 模组。
     * 若屏幕显示异常，可根据模组供应商初始化表调整这些参数。
     */
    const uint8_t frmctr1[] = {0x01, 0x2C, 0x2D};
    const uint8_t frmctr2[] = {0x01, 0x2C, 0x2D};
    const uint8_t frmctr3[] = {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D};
    const uint8_t invctr[]  = {0x07};
    const uint8_t pwctr1[]  = {0xA2, 0x02, 0x84};
    const uint8_t pwctr2[]  = {0xC5};
    const uint8_t pwctr3[]  = {0x0A, 0x00};
    const uint8_t pwctr4[]  = {0x8A, 0x2A};
    const uint8_t pwctr5[]  = {0x8A, 0xEE};
    const uint8_t vmctr1[]  = {0x0E};
    const uint8_t colmod[]  = {0x05}; /* 0x05 表示 16bit/pixel，即 RGB565。 */

    ESP_RETURN_ON_ERROR(lcd_write_cmd_data(lcd, ST7735_FRMCTR1, frmctr1, sizeof(frmctr1)), TAG, "FRMCTR1 failed");
    ESP_RETURN_ON_ERROR(lcd_write_cmd_data(lcd, ST7735_FRMCTR2, frmctr2, sizeof(frmctr2)), TAG, "FRMCTR2 failed");
    ESP_RETURN_ON_ERROR(lcd_write_cmd_data(lcd, ST7735_FRMCTR3, frmctr3, sizeof(frmctr3)), TAG, "FRMCTR3 failed");
    ESP_RETURN_ON_ERROR(lcd_write_cmd_data(lcd, ST7735_INVCTR, invctr, sizeof(invctr)), TAG, "INVCTR failed");
    ESP_RETURN_ON_ERROR(lcd_write_cmd_data(lcd, ST7735_PWCTR1, pwctr1, sizeof(pwctr1)), TAG, "PWCTR1 failed");
    ESP_RETURN_ON_ERROR(lcd_write_cmd_data(lcd, ST7735_PWCTR2, pwctr2, sizeof(pwctr2)), TAG, "PWCTR2 failed");
    ESP_RETURN_ON_ERROR(lcd_write_cmd_data(lcd, ST7735_PWCTR3, pwctr3, sizeof(pwctr3)), TAG, "PWCTR3 failed");
    ESP_RETURN_ON_ERROR(lcd_write_cmd_data(lcd, ST7735_PWCTR4, pwctr4, sizeof(pwctr4)), TAG, "PWCTR4 failed");
    ESP_RETURN_ON_ERROR(lcd_write_cmd_data(lcd, ST7735_PWCTR5, pwctr5, sizeof(pwctr5)), TAG, "PWCTR5 failed");
    ESP_RETURN_ON_ERROR(lcd_write_cmd_data(lcd, ST7735_VMCTR1, vmctr1, sizeof(vmctr1)), TAG, "VMCTR1 failed");
    ESP_RETURN_ON_ERROR(lcd_write_cmd_data(lcd, ST7735_COLMOD, colmod, sizeof(colmod)), TAG, "COLMOD failed");

    /* Gamma 参数会影响灰阶、对比度、色彩过渡。不同屏幕观感可能略有差异。 */
    const uint8_t gmctrp1[] = {0x02,0x1C,0x07,0x12,0x37,0x32,0x29,0x2D,0x29,0x25,0x2B,0x39,0x00,0x01,0x03,0x10};
    const uint8_t gmctrn1[] = {0x03,0x1D,0x07,0x06,0x2E,0x2C,0x29,0x2D,0x2E,0x2E,0x37,0x3F,0x00,0x00,0x02,0x10};
    ESP_RETURN_ON_ERROR(lcd_write_cmd_data(lcd, ST7735_GMCTRP1, gmctrp1, sizeof(gmctrp1)), TAG, "GMCTRP1 failed");
    ESP_RETURN_ON_ERROR(lcd_write_cmd_data(lcd, ST7735_GMCTRN1, gmctrn1, sizeof(gmctrn1)), TAG, "GMCTRN1 failed");

    /* 初始化默认方向、颜色反转、普通显示模式，并打开显示。 */
    ESP_RETURN_ON_ERROR(lcd_st7735_set_rotation(lcd, LCD_ST7735_ROTATION_0), TAG, "rotation failed");
    ESP_RETURN_ON_ERROR(lcd_st7735_invert_color(lcd, lcd->invert_color), TAG, "invert failed");
    ESP_RETURN_ON_ERROR(lcd_write_cmd(lcd, ST7735_NORON), TAG, "normal display failed");
    lcd_delay_ms(10);
    ESP_RETURN_ON_ERROR(lcd_st7735_display_on(lcd, true), TAG, "display on failed");
    lcd_delay_ms(100);

    /* 默认打开背光。若项目需要省电，可初始化后调用 lcd_st7735_backlight(lcd, false)。 */
    if (lcd->pin_bl != GPIO_NUM_NC) {
        gpio_set_level(lcd->pin_bl, 1);
    }
    return ESP_OK;
}

esp_err_t lcd_st7735_display_on(lcd_st7735_t *lcd, bool on)
{
    return lcd_write_cmd(lcd, on ? ST7735_DISPON : ST7735_DISPOFF);
}

esp_err_t lcd_st7735_backlight(lcd_st7735_t *lcd, bool on)
{
    ESP_RETURN_ON_FALSE(lcd, ESP_ERR_INVALID_ARG, TAG, "lcd is null");
    if (lcd->pin_bl == GPIO_NUM_NC) {
        return ESP_OK;
    }

    /* 默认假设背光高电平有效。低电平有效模块可在此处反向。 */
    gpio_set_level(lcd->pin_bl, on ? 1 : 0);
    return ESP_OK;
}

esp_err_t lcd_st7735_set_rotation(lcd_st7735_t *lcd, lcd_st7735_rotation_t rotation)
{
    ESP_RETURN_ON_FALSE(lcd, ESP_ERR_INVALID_ARG, TAG, "lcd is null");

    uint8_t madctl = 0;
    if (lcd->color_order == LCD_ST7735_COLOR_ORDER_BGR) {
        madctl |= MADCTL_BGR;
    }

    /*
     * 旋转的核心是配置 MADCTL：
     * - MX/MY 控制 X/Y 镜像；
     * - MV 控制行列交换；
     * 同时更新 width/height，使上层始终使用当前方向下的逻辑坐标。
     */
    lcd->rotation = rotation;
    switch (rotation) {
    case LCD_ST7735_ROTATION_0:
        madctl |= MADCTL_MX | MADCTL_MY;
        lcd->width = lcd->raw_width;
        lcd->height = lcd->raw_height;
        lcd->x_offset_rot = lcd->x_offset;
        lcd->y_offset_rot = lcd->y_offset;
        break;
    case LCD_ST7735_ROTATION_90:
        madctl |= MADCTL_MY | MADCTL_MV;
        lcd->width = lcd->raw_height;
        lcd->height = lcd->raw_width;
        lcd->x_offset_rot = lcd->y_offset;
        lcd->y_offset_rot = lcd->x_offset;
        break;
    case LCD_ST7735_ROTATION_180:
        madctl |= 0;
        lcd->width = lcd->raw_width;
        lcd->height = lcd->raw_height;
        lcd->x_offset_rot = lcd->x_offset;
        lcd->y_offset_rot = lcd->y_offset;
        break;
    case LCD_ST7735_ROTATION_270:
        madctl |= MADCTL_MX | MADCTL_MV;
        lcd->width = lcd->raw_height;
        lcd->height = lcd->raw_width;
        lcd->x_offset_rot = lcd->y_offset;
        lcd->y_offset_rot = lcd->x_offset;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    return lcd_write_cmd_data(lcd, ST7735_MADCTL, &madctl, 1);
}

esp_err_t lcd_st7735_invert_color(lcd_st7735_t *lcd, bool invert)
{
    ESP_RETURN_ON_FALSE(lcd, ESP_ERR_INVALID_ARG, TAG, "lcd is null");
    lcd->invert_color = invert;
    return lcd_write_cmd(lcd, invert ? ST7735_INVON : ST7735_INVOFF);
}

esp_err_t lcd_st7735_fill_screen(lcd_st7735_t *lcd, uint16_t color_rgb565)
{
    ESP_RETURN_ON_FALSE(lcd, ESP_ERR_INVALID_ARG, TAG, "lcd is null");
    return lcd_st7735_fill_rect(lcd, 0, 0, lcd->width, lcd->height, color_rgb565);
}

esp_err_t lcd_st7735_fill_rect(lcd_st7735_t *lcd, int x, int y, int w, int h, uint16_t color_rgb565)
{
    ESP_RETURN_ON_FALSE(lcd, ESP_ERR_INVALID_ARG, TAG, "lcd is null");
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }

    /* 自动裁剪：允许用户传入超出屏幕边界的矩形，驱动只绘制可见区域。 */
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w - 1) >= lcd->width ? (lcd->width - 1) : (x + w - 1);
    int y1 = (y + h - 1) >= lcd->height ? (lcd->height - 1) : (y + h - 1);
    if (x0 > x1 || y0 > y1) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(lcd_set_window(lcd, x0, y0, x1, y1), TAG, "set window failed");

    /*
     * 使用 DMA-capable 小缓冲分块发送，避免为全屏填充申请大块内存。
     * 512 像素 = 1024 字节，适合大多数 ESP32 系列芯片的 DMA 传输。
     */
    const int chunk_pixels = 512;
    uint16_t *buf = heap_caps_malloc(chunk_pixels * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "no DMA buffer");

    uint16_t be = color_to_be(color_rgb565);
    for (int i = 0; i < chunk_pixels; i++) {
        buf[i] = be;
    }

    int total = (x1 - x0 + 1) * (y1 - y0 + 1);
    while (total > 0) {
        int n = total > chunk_pixels ? chunk_pixels : total;
        esp_err_t ret = lcd_write_data(lcd, buf, n * sizeof(uint16_t));
        if (ret != ESP_OK) {
            free(buf);
            return ret;
        }
        total -= n;
    }

    free(buf);
    return ESP_OK;
}

esp_err_t lcd_st7735_draw_pixel(lcd_st7735_t *lcd, int x, int y, uint16_t color_rgb565)
{
    ESP_RETURN_ON_FALSE(lcd, ESP_ERR_INVALID_ARG, TAG, "lcd is null");
    if (x < 0 || y < 0 || x >= lcd->width || y >= lcd->height) {
        return ESP_OK;
    }

    uint16_t be = color_to_be(color_rgb565);
    ESP_RETURN_ON_ERROR(lcd_set_window(lcd, x, y, x, y), TAG, "set window failed");
    return lcd_write_data(lcd, &be, sizeof(be));
}

esp_err_t lcd_st7735_draw_bitmap(lcd_st7735_t *lcd, int x, int y, int w, int h, const uint16_t *colors)
{
    ESP_RETURN_ON_FALSE(lcd && colors, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    if (w <= 0 || h <= 0) {
        return ESP_OK;
    }

    /* 计算位图与屏幕可见区域的交集，避免越界访问和无效传输。 */
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w - 1) >= lcd->width ? (lcd->width - 1) : (x + w - 1);
    int y1 = (y + h - 1) >= lcd->height ? (lcd->height - 1) : (y + h - 1);
    if (x0 > x1 || y0 > y1) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(lcd_set_window(lcd, x0, y0, x1, y1), TAG, "set window failed");

    int clipped_w = x1 - x0 + 1;
    int clipped_h = y1 - y0 + 1;
    const int chunk_pixels = 512;
    uint16_t *buf = heap_caps_malloc(chunk_pixels * sizeof(uint16_t), MALLOC_CAP_DMA);
    ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "no DMA buffer");

    /* src_x_offset/src_y_offset 表示裁剪后可见区域在原始位图中的起始位置。 */
    int src_x_offset = x0 - x;
    int src_y_offset = y0 - y;
    int pending = 0;

    for (int row = 0; row < clipped_h; row++) {
        const uint16_t *src = colors + (src_y_offset + row) * w + src_x_offset;
        for (int col = 0; col < clipped_w; col++) {
            buf[pending++] = color_to_be(src[col]);
            if (pending == chunk_pixels) {
                esp_err_t ret = lcd_write_data(lcd, buf, pending * sizeof(uint16_t));
                if (ret != ESP_OK) {
                    free(buf);
                    return ret;
                }
                pending = 0;
            }
        }
    }

    /* 发送最后不足一个 chunk 的像素数据。 */
    if (pending > 0) {
        esp_err_t ret = lcd_write_data(lcd, buf, pending * sizeof(uint16_t));
        if (ret != ESP_OK) {
            free(buf);
            return ret;
        }
    }

    free(buf);
    return ESP_OK;
}

int lcd_st7735_get_width(const lcd_st7735_t *lcd)
{
    return lcd ? lcd->width : 0;
}

int lcd_st7735_get_height(const lcd_st7735_t *lcd)
{
    return lcd ? lcd->height : 0;
}
