/**
 * @file st7735.h
 * @brief 基于 ESP-IDF SPI Master 的 ST7735 LCD 驱动头文件。
 *
 * 本驱动面向常见 0.96 寸 ST7735 SPI 彩屏模块，默认推荐配置为 80x160、RGB565。
 * 由于 ST7735 屏幕模组厂家较多，不同模组可能存在：
 * - 实际可视区域不同，例如 80x160、128x160；
 * - 显存起始地址偏移不同，例如 x_offset/y_offset 需要微调；
 * - RGB/BGR 颜色顺序不同；
 * - 是否需要颜色反转不同。
 *
 * 设计原则：
 * - SPI 总线初始化由用户工程负责，驱动只添加 SPI 设备；
 * - 每个 LCD 对象对应一个 ST7735 实例，支持同一 SPI 总线多屏幕；
 * - 公开 API 只处理实例对象，便于后续扩展字体、GUI、DMA 队列等能力。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief ST7735 像素颜色顺序。
 *
 * ST7735 内部的 MADCTL 寄存器可以配置 RGB/BGR 顺序。
 * 当红蓝颜色显示相反时，通常只需要切换该枚举值。
 */
typedef enum {
    LCD_ST7735_COLOR_ORDER_RGB = 0,    /*!< RGB 顺序：适用于显示红色正常、蓝色正常的模组 */
    LCD_ST7735_COLOR_ORDER_BGR = 1,    /*!< BGR 顺序：很多 0.96 寸 IPS 小屏需要使用该模式 */
} lcd_st7735_color_order_t;

/**
 * @brief 屏幕显示方向。
 *
 * 方向设置会同时影响：
 * - ST7735 MADCTL 扫描方向；
 * - lcd_st7735_get_width()/lcd_st7735_get_height() 返回值；
 * - 后续画点、填充、位图绘制时的逻辑坐标系。
 */
typedef enum {
    LCD_ST7735_ROTATION_0 = 0,         /*!< 0 度方向，逻辑宽高等于 config.width/config.height */
    LCD_ST7735_ROTATION_90,            /*!< 顺时针旋转 90 度，逻辑宽高互换 */
    LCD_ST7735_ROTATION_180,           /*!< 旋转 180 度，逻辑宽高不变 */
    LCD_ST7735_ROTATION_270,           /*!< 顺时针旋转 270 度，逻辑宽高互换 */
} lcd_st7735_rotation_t;

/**
 * @brief ST7735 LCD 单实例配置结构体。
 *
 * @note SPI 总线必须在调用 lcd_st7735_new() 前由用户自行完成初始化：
 *       spi_bus_initialize(spi_host, &buscfg, SPI_DMA_CH_AUTO)。
 *
 * @note 0.96 寸 ST7735 模组常见分辨率为 80x160，但也可能是 128x160。
 *       如果画面整体偏移或只显示一部分，优先调整 width/height/x_offset/y_offset。
 */
typedef struct {
    spi_host_device_t spi_host;        /*!< SPI 主机编号，例如 SPI2_HOST 或 SPI3_HOST，必须与 spi_bus_initialize() 一致 */
    gpio_num_t pin_cs;                 /*!< LCD 片选 CS 引脚；多实例时每块屏幕必须使用不同 CS */
    gpio_num_t pin_dc;                 /*!< LCD D/C 引脚，低电平表示命令，高电平表示数据；不可为 GPIO_NUM_NC */
    gpio_num_t pin_rst;                /*!< LCD 复位 RST 引脚；未连接时填 GPIO_NUM_NC，驱动会使用软件复位 */
    gpio_num_t pin_bl;                 /*!< LCD 背光 BL 引脚；未连接或常亮时填 GPIO_NUM_NC */

    int width;                         /*!< 0 度方向下的逻辑宽度，0.96 寸窄屏通常为 80 */
    int height;                        /*!< 0 度方向下的逻辑高度，0.96 寸窄屏通常为 160 */
    uint8_t x_offset;                  /*!< 0 度方向下显存列偏移；80x160 ST7735 常见值为 24、26 或 0 */
    uint8_t y_offset;                  /*!< 0 度方向下显存行偏移；80x160 ST7735 常见值为 0、1 或 24 */

    int spi_clock_hz;                  /*!< SPI 时钟频率，建议 20MHz~40MHz；填 0 时默认 20MHz */
    lcd_st7735_color_order_t color_order; /*!< 颜色顺序，红蓝反色时切换 RGB/BGR */
    bool invert_color;                 /*!< 是否开启颜色反转；很多 0.96 寸 IPS ST7735 模组需要 true */
    bool reset_level;                  /*!< RST 有效电平；大多数模组低电平复位，因此通常填 false 或 0 */
} lcd_st7735_config_t;

/**
 * @brief ST7735 LCD 句柄类型。
 *
 * 该结构体在 .c 文件内定义，用户只通过指针使用，避免外部直接访问内部状态。
 */
typedef struct lcd_st7735_t lcd_st7735_t;

/**
 * @brief 创建并初始化一个 ST7735 LCD 实例。
 *
 * 函数内部会完成：
 * - 分配 LCD 对象；
 * - 配置 DC/RST/BL GPIO；
 * - 通过 spi_bus_add_device() 将该 LCD 加入已经初始化的 SPI 总线；
 * - 执行 ST7735 初始化序列；
 * - 打开显示与背光。
 *
 * @param[in]  config  LCD 配置参数，不能为空。
 * @param[out] out_lcd 创建成功后返回 LCD 实例句柄，不能为空。
 * @return
 * - ESP_OK：创建并初始化成功；
 * - ESP_ERR_INVALID_ARG：参数错误；
 * - ESP_ERR_NO_MEM：内存不足；
 * - 其他：SPI/GPIO/初始化过程返回的错误码。
 */
esp_err_t lcd_st7735_new(const lcd_st7735_config_t *config, lcd_st7735_t **out_lcd);

/**
 * @brief 删除一个 LCD 实例并从 SPI 总线移除设备。
 *
 * @note 该函数只释放 LCD 实例和 SPI 设备，不会调用 spi_bus_free()。
 *       SPI 总线可能还被其他设备使用，是否释放总线由用户工程决定。
 *
 * @param[in] lcd LCD 实例句柄。
 * @return ESP_OK 或 spi_bus_remove_device() 返回的错误码。
 */
esp_err_t lcd_st7735_del(lcd_st7735_t *lcd);

/**
 * @brief 重新执行 ST7735 初始化序列。
 *
 * lcd_st7735_new() 已经自动调用该函数。只有在屏幕异常、休眠恢复、热复位后，才需要手动调用。
 *
 * @param[in] lcd LCD 实例句柄。
 * @return ESP_OK 或初始化过程中产生的错误码。
 */
esp_err_t lcd_st7735_init(lcd_st7735_t *lcd);

/**
 * @brief 控制 LCD 显示开关。
 *
 * 该函数发送 ST7735_DISPON/ST7735_DISPOFF 命令，只影响显示输出，通常不等价于背光控制。
 * 若需要关闭背光，请同时调用 lcd_st7735_backlight()。
 *
 * @param[in] lcd LCD 实例句柄。
 * @param[in] on  true 打开显示，false 关闭显示。
 * @return ESP_OK 或 SPI 传输错误码。
 */
esp_err_t lcd_st7735_display_on(lcd_st7735_t *lcd, bool on);

/**
 * @brief 控制背光引脚。
 *
 * @note 当前默认按“高电平点亮背光”处理。如你的模块为低电平点亮，可在硬件上调整，
 *       或在该函数实现中反转输出逻辑。
 *
 * @param[in] lcd LCD 实例句柄。
 * @param[in] on  true 打开背光，false 关闭背光。
 * @return ESP_OK；如果未配置 pin_bl 也返回 ESP_OK。
 */
esp_err_t lcd_st7735_backlight(lcd_st7735_t *lcd, bool on);

/**
 * @brief 设置屏幕旋转方向。
 *
 * @param[in] lcd LCD 实例句柄。
 * @param[in] rotation 目标旋转方向。
 * @return ESP_OK 或 ESP_ERR_INVALID_ARG。
 */
esp_err_t lcd_st7735_set_rotation(lcd_st7735_t *lcd, lcd_st7735_rotation_t rotation);

/**
 * @brief 设置颜色反转。
 *
 * 部分 IPS ST7735 屏幕在不反转时会出现颜色发白、底色异常等现象，此时应开启反转。
 *
 * @param[in] lcd LCD 实例句柄。
 * @param[in] invert true 开启反转，false 关闭反转。
 * @return ESP_OK 或 SPI 传输错误码。
 */
esp_err_t lcd_st7735_invert_color(lcd_st7735_t *lcd, bool invert);

/**
 * @brief 使用 RGB565 颜色填充整个屏幕。
 *
 * @param[in] lcd LCD 实例句柄。
 * @param[in] color_rgb565 RGB565 颜色值，建议使用 LCD_ST7735_RGB565() 生成。
 * @return ESP_OK 或 SPI/内存错误码。
 */
esp_err_t lcd_st7735_fill_screen(lcd_st7735_t *lcd, uint16_t color_rgb565);

/**
 * @brief 填充矩形区域。
 *
 * 坐标以当前旋转方向下的逻辑坐标系为准。函数内部会自动裁剪超出屏幕的区域。
 *
 * @param[in] lcd LCD 实例句柄。
 * @param[in] x 矩形左上角 X 坐标。
 * @param[in] y 矩形左上角 Y 坐标。
 * @param[in] w 矩形宽度。
 * @param[in] h 矩形高度。
 * @param[in] color_rgb565 RGB565 颜色值。
 * @return ESP_OK 或 SPI/内存错误码。
 */
esp_err_t lcd_st7735_fill_rect(lcd_st7735_t *lcd, int x, int y, int w, int h, uint16_t color_rgb565);

/**
 * @brief 绘制单个像素点。
 *
 * @note 单点绘制会频繁设置窗口并发送少量数据，效率较低。绘制大量图形时建议合并为矩形或位图。
 *
 * @param[in] lcd LCD 实例句柄。
 * @param[in] x 像素 X 坐标。
 * @param[in] y 像素 Y 坐标。
 * @param[in] color_rgb565 RGB565 颜色值。
 * @return ESP_OK 或 SPI 传输错误码。
 */
esp_err_t lcd_st7735_draw_pixel(lcd_st7735_t *lcd, int x, int y, uint16_t color_rgb565);

/**
 * @brief 绘制 RGB565 位图。
 *
 * colors 为主机端字节序的 RGB565 数组，元素数量至少为 w*h。
 * 函数内部会转换为 ST7735 需要的高字节在前格式，并自动裁剪超出屏幕的区域。
 *
 * @param[in] lcd LCD 实例句柄。
 * @param[in] x 位图左上角 X 坐标。
 * @param[in] y 位图左上角 Y 坐标。
 * @param[in] w 位图宽度。
 * @param[in] h 位图高度。
 * @param[in] colors RGB565 像素数组，长度至少 w*h。
 * @return ESP_OK 或 SPI/内存错误码。
 */
esp_err_t lcd_st7735_draw_bitmap(lcd_st7735_t *lcd, int x, int y, int w, int h, const uint16_t *colors);

/**
 * @brief 获取当前旋转方向下的逻辑宽度。
 *
 * @param[in] lcd LCD 实例句柄。
 * @return 当前逻辑宽度；lcd 为空时返回 0。
 */
int lcd_st7735_get_width(const lcd_st7735_t *lcd);

/**
 * @brief 获取当前旋转方向下的逻辑高度。
 *
 * @param[in] lcd LCD 实例句柄。
 * @return 当前逻辑高度；lcd 为空时返回 0。
 */
int lcd_st7735_get_height(const lcd_st7735_t *lcd);

/**
 * @brief RGB888 转 RGB565 辅助宏。
 *
 * @param r 红色分量，范围 0~255。
 * @param g 绿色分量，范围 0~255。
 * @param b 蓝色分量，范围 0~255。
 * @return RGB565 16 位颜色值。
 */
#define LCD_ST7735_RGB565(r, g, b) \
    (uint16_t)((((uint16_t)(r) & 0xF8) << 8) | (((uint16_t)(g) & 0xFC) << 3) | ((uint16_t)(b) >> 3))

#ifdef __cplusplus
}
#endif
