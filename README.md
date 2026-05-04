# ESP-IDF ST7735 SPI LCD Driver

## ESP Component Registry usage

After this component is published, add it to an ESP-IDF project with:

```bash
idf.py add-dependency "miraitowa-la/esp_st7735^0.1.1"
```

Or add it manually in the consuming project's `main/idf_component.yml`:

```yaml
dependencies:
  miraitowa-la/esp_st7735: "^0.1.1"
```

The application should initialize the SPI bus with `spi_bus_initialize()` first,
then create the display instance with `lcd_st7735_new()`.

本工程是一个面向 **ESP-IDF** 的 ST7735 SPI LCD 驱动示例，重点适配常见 **0.96 寸 ST7735 彩屏模块**。驱动文件采用实例化设计，支持同一条 SPI 总线挂载多块 LCD，适合教学、比赛项目、嵌入式产品原型和后续 GUI 框架移植。

默认示例参数适配常见 0.96 寸窄屏：

```c
.width = 80,
.height = 160,
.x_offset = 26,
.y_offset = 1,
.color_order = LCD_ST7735_COLOR_ORDER_BGR,
.invert_color = true,
```

> 说明：ST7735 模组厂家较多，即使都叫“0.96 寸 ST7735”，可视区域、显存偏移、颜色顺序也可能不同。如果画面偏移、颜色异常，需要根据实际模组调整参数。

---

## 1. 组件文件结构

```text
esp_st7735/
├── CMakeLists.txt
├── idf_component.yml
├── LICENSE
├── README.md
├── include/
│   └── st7735.h
└── src/
    └── st7735.c
```

### 文件说明

| 文件 | 作用 |
|---|---|
| `include/st7735.h` | 驱动对外 API 头文件，包含配置结构体、枚举、函数声明和 RGB565 辅助宏 |
| `src/st7735.c` | ST7735 驱动实现，包含 SPI 写命令、初始化序列、显示方向、绘图函数等 |
| `CMakeLists.txt` | ESP-IDF 组件构建配置 |
| `idf_component.yml` | ESP Component Registry 元数据与依赖声明 |
| `LICENSE` | 开源许可证 |
| `README.md` | 组件说明文档 |

---

## 2. 驱动设计思路

### 2.1 总线与设备解耦

本驱动不在 `src/st7735.c` 内部初始化 SPI 总线，而是在用户工程中初始化：

```c
spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
```

驱动只在 `lcd_st7735_new()` 中调用：

```c
spi_bus_add_device(config->spi_host, &devcfg, &lcd->spi);
```

这样设计的好处是：

1. SPI 总线可以被多个设备共享；
2. 同一条 MOSI/SCLK 可以挂多块 LCD；
3. 用户可以自由控制 SPI 总线参数、DMA 通道和最大传输长度；
4. 驱动可以更容易移植到已有 ESP-IDF 工程。

### 2.2 支持多实例

每调用一次：

```c
lcd_st7735_new(&lcd_cfg, &lcd);
```

就会创建一个独立 LCD 实例。每个实例内部保存自己的：

- SPI device 句柄；
- DC/RST/BL 引脚；
- 分辨率；
- x/y 偏移；
- 颜色顺序；
- 当前旋转方向；
- 是否颜色反转。

因此，多块屏幕可以拥有不同的 CS、DC、RST、BL、旋转方向和显示参数。

### 2.3 方法解耦

驱动内部按照职责拆分：

| 内部函数 | 职责 |
|---|---|
| `lcd_write_cmd()` | 写 1 字节 ST7735 命令 |
| `lcd_write_data()` | 写参数或像素数据 |
| `lcd_write_cmd_data()` | 写命令并跟随写入参数 |
| `lcd_hw_reset()` | 硬件复位或软件复位 |
| `lcd_set_window()` | 设置显存写入窗口 |
| `color_to_be()` | RGB565 字节序转换 |

外部 API 只暴露实例创建、初始化、显示控制和绘图接口，业务代码不需要关心 ST7735 命令细节。

---

## 3. 硬件接线说明

常见 ST7735 SPI 模块引脚如下：

| LCD 引脚 | 说明 | ESP32 示例引脚 |
|---|---|---|
| `VCC` | 电源，通常 3.3V | 3V3 |
| `GND` | 地 | GND |
| `SCL` / `SCK` | SPI 时钟 | GPIO18 |
| `SDA` / `DIN` / `MOSI` | SPI 数据输入 | GPIO23 |
| `RES` / `RST` | 复位 | GPIO4 |
| `DC` / `A0` | 命令/数据选择 | GPIO2 |
| `CS` | 片选 | GPIO5 |
| `BLK` / `BL` | 背光 | GPIO15 或直接接 3.3V |

示例代码中的默认宏：

```c
#define LCD_HOST       SPI2_HOST
#define PIN_NUM_MOSI   23
#define PIN_NUM_MISO   GPIO_NUM_NC
#define PIN_NUM_CLK    18

#define LCD1_PIN_CS    5
#define LCD1_PIN_DC    2
#define LCD1_PIN_RST   4
#define LCD1_PIN_BL    15
```

> 注意：LCD 通常不需要 MISO，所以 `PIN_NUM_MISO` 可以设置为 `GPIO_NUM_NC`。

---

## 4. 编译与烧录

进入工程目录：

```bash
cd your_esp_idf_project
```

选择芯片目标，例如 ESP32：

```bash
idf.py set-target esp32
```

编译：

```bash
idf.py build
```

烧录并打开串口监视器：

```bash
idf.py flash monitor
```

如果使用 ESP32-S3、ESP32-C3 等芯片，请修改目标：

```bash
idf.py set-target esp32s3
```

或：

```bash
idf.py set-target esp32c3
```

---

## 5. 快速使用流程

### 5.1 初始化 SPI 总线

```c
spi_bus_config_t buscfg = {
    .mosi_io_num = PIN_NUM_MOSI,
    .miso_io_num = GPIO_NUM_NC,
    .sclk_io_num = PIN_NUM_CLK,
    .quadwp_io_num = GPIO_NUM_NC,
    .quadhd_io_num = GPIO_NUM_NC,
    .max_transfer_sz = 160 * 128 * 2 + 8,
};

ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));
```

`max_transfer_sz` 表示一次 SPI 传输允许的最大数据长度。对于 80x160 RGB565 屏幕，全屏数据量为：

```text
80 * 160 * 2 = 25600 bytes
```

示例中写成 `160 * 128 * 2 + 8` 是为了兼容更多 ST7735 模组。

### 5.2 配置 LCD 参数

```c
lcd_st7735_config_t lcd1_cfg = {
    .spi_host = LCD_HOST,
    .pin_cs = LCD1_PIN_CS,
    .pin_dc = LCD1_PIN_DC,
    .pin_rst = LCD1_PIN_RST,
    .pin_bl = LCD1_PIN_BL,

    .width = 80,
    .height = 160,
    .x_offset = 26,
    .y_offset = 1,

    .spi_clock_hz = 40 * 1000 * 1000,
    .color_order = LCD_ST7735_COLOR_ORDER_BGR,
    .invert_color = true,
    .reset_level = 0,
};
```

### 5.3 创建 LCD 实例

```c
lcd_st7735_t *lcd1 = NULL;
ESP_ERROR_CHECK(lcd_st7735_new(&lcd1_cfg, &lcd1));
```

创建成功后，驱动已经完成 ST7735 初始化，可以直接绘图。

### 5.4 清屏与绘图

```c
lcd_st7735_fill_screen(lcd1, LCD_ST7735_RGB565(0, 0, 0));
lcd_st7735_fill_rect(lcd1, 10, 10, 40, 30, LCD_ST7735_RGB565(255, 0, 0));
lcd_st7735_draw_pixel(lcd1, 0, 0, LCD_ST7735_RGB565(255, 255, 255));
```

---

## 6. API 详细说明

### 6.1 `lcd_st7735_config_t`

LCD 实例配置结构体。

```c
typedef struct {
    spi_host_device_t spi_host;
    gpio_num_t pin_cs;
    gpio_num_t pin_dc;
    gpio_num_t pin_rst;
    gpio_num_t pin_bl;

    int width;
    int height;
    uint8_t x_offset;
    uint8_t y_offset;

    int spi_clock_hz;
    lcd_st7735_color_order_t color_order;
    bool invert_color;
    bool reset_level;
} lcd_st7735_config_t;
```

字段说明：

| 字段 | 说明 |
|---|---|
| `spi_host` | SPI 主机编号，例如 `SPI2_HOST`、`SPI3_HOST` |
| `pin_cs` | LCD 片选引脚。多实例时每块屏幕必须不同 |
| `pin_dc` | 命令/数据选择引脚。低电平为命令，高电平为数据 |
| `pin_rst` | 复位引脚。未连接时填 `GPIO_NUM_NC` |
| `pin_bl` | 背光控制引脚。未连接时填 `GPIO_NUM_NC` |
| `width` | 0 度方向下的逻辑宽度 |
| `height` | 0 度方向下的逻辑高度 |
| `x_offset` | 显存列偏移，用于修正画面横向偏移 |
| `y_offset` | 显存行偏移，用于修正画面纵向偏移 |
| `spi_clock_hz` | SPI 时钟频率，建议 20MHz 到 40MHz |
| `color_order` | RGB/BGR 颜色顺序 |
| `invert_color` | 是否开启颜色反转 |
| `reset_level` | RST 有效电平，常见模块为低电平复位，填 `0` |

### 6.2 `lcd_st7735_new()`

```c
esp_err_t lcd_st7735_new(const lcd_st7735_config_t *config, lcd_st7735_t **out_lcd);
```

功能：创建并初始化一块 ST7735 LCD。

内部会完成：

1. 参数检查；
2. 分配 LCD 实例对象；
3. 配置 DC/RST/BL GPIO；
4. 将 LCD 添加到已初始化的 SPI 总线；
5. 执行 ST7735 初始化序列；
6. 打开显示和背光。

使用示例：

```c
lcd_st7735_t *lcd = NULL;
ESP_ERROR_CHECK(lcd_st7735_new(&lcd_cfg, &lcd));
```

返回值：

| 返回值 | 说明 |
|---|---|
| `ESP_OK` | 成功 |
| `ESP_ERR_INVALID_ARG` | 参数错误，例如配置为空、宽高非法、CS/DC 未配置 |
| `ESP_ERR_NO_MEM` | 内存不足 |
| 其他错误 | SPI 或 GPIO 初始化失败 |

### 6.3 `lcd_st7735_del()`

```c
esp_err_t lcd_st7735_del(lcd_st7735_t *lcd);
```

功能：删除 LCD 实例，并从 SPI 总线上移除该设备。

注意：该函数不会释放 SPI 总线。如果整个工程不再使用该 SPI 总线，用户可以在所有 SPI 设备移除后自行调用：

```c
spi_bus_free(LCD_HOST);
```

### 6.4 `lcd_st7735_init()`

```c
esp_err_t lcd_st7735_init(lcd_st7735_t *lcd);
```

功能：重新执行 ST7735 初始化序列。

`lcd_st7735_new()` 内部已经调用过该函数。通常不需要手动调用，除非：

- 屏幕通信异常后需要重新初始化；
- LCD 断电又重新上电；
- 从低功耗模式恢复后需要重新配置。

### 6.5 `lcd_st7735_display_on()`

```c
esp_err_t lcd_st7735_display_on(lcd_st7735_t *lcd, bool on);
```

功能：发送 ST7735 显示开关命令。

```c
lcd_st7735_display_on(lcd, true);   // 打开显示
lcd_st7735_display_on(lcd, false);  // 关闭显示
```

该函数控制 LCD 控制器显示输出，不一定关闭背光。要关闭背光，请使用 `lcd_st7735_backlight()`。

### 6.6 `lcd_st7735_backlight()`

```c
esp_err_t lcd_st7735_backlight(lcd_st7735_t *lcd, bool on);
```

功能：控制背光引脚。

```c
lcd_st7735_backlight(lcd, true);   // 打开背光
lcd_st7735_backlight(lcd, false);  // 关闭背光
```

如果 `pin_bl = GPIO_NUM_NC`，该函数不做任何操作并返回 `ESP_OK`。

> 当前实现默认背光高电平有效。如果你的模组是低电平点亮背光，可以修改 `lcd_st7735_backlight()` 中的 `gpio_set_level()` 逻辑。

### 6.7 `lcd_st7735_set_rotation()`

```c
esp_err_t lcd_st7735_set_rotation(lcd_st7735_t *lcd, lcd_st7735_rotation_t rotation);
```

功能：设置显示方向。

可选方向：

```c
typedef enum {
    LCD_ST7735_ROTATION_0 = 0,
    LCD_ST7735_ROTATION_90,
    LCD_ST7735_ROTATION_180,
    LCD_ST7735_ROTATION_270,
} lcd_st7735_rotation_t;
```

使用示例：

```c
lcd_st7735_set_rotation(lcd, LCD_ST7735_ROTATION_90);
```

旋转后，逻辑宽高会变化：

```c
int w = lcd_st7735_get_width(lcd);
int h = lcd_st7735_get_height(lcd);
```

对于 80x160 屏幕：

| 旋转方向 | 逻辑宽度 | 逻辑高度 |
|---|---:|---:|
| `ROTATION_0` | 80 | 160 |
| `ROTATION_90` | 160 | 80 |
| `ROTATION_180` | 80 | 160 |
| `ROTATION_270` | 160 | 80 |

### 6.8 `lcd_st7735_invert_color()`

```c
esp_err_t lcd_st7735_invert_color(lcd_st7735_t *lcd, bool invert);
```

功能：开启或关闭颜色反转。

```c
lcd_st7735_invert_color(lcd, true);
lcd_st7735_invert_color(lcd, false);
```

很多 0.96 寸 IPS ST7735 模组需要开启颜色反转，否则可能出现：

- 黑色不黑；
- 颜色发白；
- 背景色异常；
- 图片像底片效果。

### 6.9 `lcd_st7735_fill_screen()`

```c
esp_err_t lcd_st7735_fill_screen(lcd_st7735_t *lcd, uint16_t color_rgb565);
```

功能：全屏填充指定 RGB565 颜色。

```c
lcd_st7735_fill_screen(lcd, LCD_ST7735_RGB565(0, 0, 0));      // 黑屏
lcd_st7735_fill_screen(lcd, LCD_ST7735_RGB565(255, 255, 255)); // 白屏
```

### 6.10 `lcd_st7735_fill_rect()`

```c
esp_err_t lcd_st7735_fill_rect(lcd_st7735_t *lcd, int x, int y, int w, int h, uint16_t color_rgb565);
```

功能：填充矩形区域。

```c
lcd_st7735_fill_rect(lcd, 10, 20, 50, 30, LCD_ST7735_RGB565(255, 0, 0));
```

参数说明：

| 参数 | 说明 |
|---|---|
| `x` | 矩形左上角 X 坐标 |
| `y` | 矩形左上角 Y 坐标 |
| `w` | 矩形宽度 |
| `h` | 矩形高度 |
| `color_rgb565` | RGB565 颜色 |

函数内部会自动裁剪超出屏幕边界的部分。例如：

```c
lcd_st7735_fill_rect(lcd, -10, -10, 30, 30, LCD_ST7735_RGB565(0, 255, 0));
```

上面的代码不会报错，只会绘制屏幕可见范围内的部分。

### 6.11 `lcd_st7735_draw_pixel()`

```c
esp_err_t lcd_st7735_draw_pixel(lcd_st7735_t *lcd, int x, int y, uint16_t color_rgb565);
```

功能：绘制一个像素点。

```c
lcd_st7735_draw_pixel(lcd, 0, 0, LCD_ST7735_RGB565(255, 255, 255));
```

注意：单点绘制效率较低，因为每画一个点都需要设置一次显存窗口。大量绘图时建议使用：

- `lcd_st7735_fill_rect()`；
- `lcd_st7735_draw_bitmap()`；
- 或者在上层维护 framebuffer 后整块刷新。

### 6.12 `lcd_st7735_draw_bitmap()`

```c
esp_err_t lcd_st7735_draw_bitmap(lcd_st7735_t *lcd, int x, int y, int w, int h, const uint16_t *colors);
```

功能：绘制一块 RGB565 位图。

`colors` 是 RGB565 数组，长度至少为 `w * h`。

示例：绘制 20x20 红色位图。

```c
uint16_t img[20 * 20];
for (int i = 0; i < 20 * 20; i++) {
    img[i] = LCD_ST7735_RGB565(255, 0, 0);
}

lcd_st7735_draw_bitmap(lcd, 30, 40, 20, 20, img);
```

函数内部会：

1. 自动裁剪超出屏幕的区域；
2. 将 RGB565 转换为 LCD 需要的高字节在前格式；
3. 使用 DMA-capable 分块缓冲发送。

### 6.13 `lcd_st7735_get_width()` / `lcd_st7735_get_height()`

```c
int lcd_st7735_get_width(const lcd_st7735_t *lcd);
int lcd_st7735_get_height(const lcd_st7735_t *lcd);
```

功能：获取当前旋转方向下的逻辑宽高。

```c
int w = lcd_st7735_get_width(lcd);
int h = lcd_st7735_get_height(lcd);
```

建议在绘图前动态获取宽高，而不是在业务代码里写死 80 和 160。

### 6.14 `LCD_ST7735_RGB565()`

```c
#define LCD_ST7735_RGB565(r, g, b) ...
```

功能：将 RGB888 转换为 RGB565。

示例：

```c
uint16_t red   = LCD_ST7735_RGB565(255, 0, 0);
uint16_t green = LCD_ST7735_RGB565(0, 255, 0);
uint16_t blue  = LCD_ST7735_RGB565(0, 0, 255);
```

RGB565 位分布：

```text
R: 5 bit
G: 6 bit
B: 5 bit
```

---

## 7. 多实例示例

多块屏幕共享 SPI 总线时，先初始化一次 SPI 总线：

```c
ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));
```

然后分别创建 LCD 实例：

```c
lcd_st7735_t *lcd1 = NULL;
lcd_st7735_t *lcd2 = NULL;

lcd_st7735_config_t lcd1_cfg = {
    .spi_host = LCD_HOST,
    .pin_cs = 5,
    .pin_dc = 2,
    .pin_rst = 4,
    .pin_bl = 15,
    .width = 80,
    .height = 160,
    .x_offset = 26,
    .y_offset = 1,
    .spi_clock_hz = 40 * 1000 * 1000,
    .color_order = LCD_ST7735_COLOR_ORDER_BGR,
    .invert_color = true,
    .reset_level = 0,
};

lcd_st7735_config_t lcd2_cfg = lcd1_cfg;
lcd2_cfg.pin_cs = 21;
lcd2_cfg.pin_dc = 22;
lcd2_cfg.pin_rst = GPIO_NUM_NC;
lcd2_cfg.pin_bl = GPIO_NUM_NC;

ESP_ERROR_CHECK(lcd_st7735_new(&lcd1_cfg, &lcd1));
ESP_ERROR_CHECK(lcd_st7735_new(&lcd2_cfg, &lcd2));

lcd_st7735_fill_screen(lcd1, LCD_ST7735_RGB565(255, 0, 0));
lcd_st7735_fill_screen(lcd2, LCD_ST7735_RGB565(0, 0, 255));
```

多实例注意事项：

1. 多个 LCD 可以共用 MOSI 和 SCLK；
2. 每个 LCD 必须使用不同 CS；
3. DC/RST/BL 建议独立，避免不同屏幕控制互相影响；
4. 每块屏幕可以设置不同分辨率、偏移、颜色顺序和旋转方向；
5. SPI 总线只初始化一次，不要重复调用 `spi_bus_initialize()`。

---

## 8. 参数调试指南

### 8.1 画面整体偏移

现象：画面没有铺满可视区域，整体向某个方向偏移，边缘有黑边或花边。

优先调整：

```c
.x_offset = 26,
.y_offset = 1,
```

常见组合：

| 模组类型 | width | height | x_offset | y_offset |
|---|---:|---:|---:|---:|
| 0.96 寸 80x160 常见 IPS | 80 | 160 | 26 | 1 |
| 0.96 寸 80x160 另一类 | 80 | 160 | 24 | 0 |
| 1.8 寸 128x160 常见 ST7735 | 128 | 160 | 0 | 0 |
| 部分 128x160 模组 | 128 | 160 | 2 | 1 |

调试方法：

1. 先使用 `lcd_st7735_fill_screen()` 全屏填色；
2. 再绘制四宫格或边框；
3. 如果左侧缺内容，尝试减小 `x_offset`；
4. 如果右侧缺内容，尝试增大或减小 `x_offset`；
5. 如果顶部/底部缺内容，调整 `y_offset`。

### 8.2 红色和蓝色反了

现象：你画红色，屏幕显示蓝色；画蓝色，屏幕显示红色。

修改：

```c
.color_order = LCD_ST7735_COLOR_ORDER_RGB
```

或：

```c
.color_order = LCD_ST7735_COLOR_ORDER_BGR
```

### 8.3 颜色像底片或发白

现象：黑色不黑，颜色异常发白，或者像负片效果。

修改：

```c
.invert_color = true
```

或：

```c
.invert_color = false
```

### 8.4 屏幕无显示

排查顺序：

1. 确认 VCC/GND 是否正确，模块是否支持 3.3V；
2. 确认 SCL/SDA 是否接反；
3. 确认 CS/DC/RST/BL 引脚是否与代码一致；
4. 如果背光不亮，尝试将 BL 直接接 3.3V；
5. 将 `.spi_clock_hz` 降低到 `20 * 1000 * 1000` 或 `10 * 1000 * 1000`；
6. 如果 RST 未连接，设置 `.pin_rst = GPIO_NUM_NC`；
7. 查看串口日志是否有 SPI 或 GPIO 报错。

---

## 9. 性能说明

当前驱动使用 `spi_device_polling_transmit()`，特点是：

- 代码简单；
- 调试方便；
- 对 0.96 寸小屏刷新足够；
- 适合教学和中小型项目。

对于更高刷新率需求，可以继续优化：

1. 使用 `spi_device_queue_trans()` 和 `spi_device_get_trans_result()` 做异步队列发送；
2. 使用更大的 DMA 缓冲；
3. 在上层维护 framebuffer，一次性刷新大区域；
4. 接入 LVGL 时使用局部刷新缓冲；
5. 避免大量调用 `draw_pixel()`，优先使用矩形和位图批量绘制。

---

## 10. 与 LVGL 结合的思路

本驱动可以作为 LVGL flush 回调的底层接口。核心思路：

1. LVGL 配置一个或两个 draw buffer；
2. 在 `flush_cb` 中调用 `lcd_st7735_draw_bitmap()`；
3. 刷新完成后调用 `lv_display_flush_ready()` 或对应版本的 ready API。

伪代码示例：

```c
static void my_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    int x = area->x1;
    int y = area->y1;
    int w = area->x2 - area->x1 + 1;
    int h = area->y2 - area->y1 + 1;

    lcd_st7735_t *lcd = lv_display_get_user_data(disp);
    lcd_st7735_draw_bitmap(lcd, x, y, w, h, (const uint16_t *)px_map);

    lv_display_flush_ready(disp);
}
```

> 具体 LVGL API 名称会随 LVGL 版本变化，请按你使用的 LVGL 版本调整。

---

## 11. 常见问题 FAQ

### Q1：为什么驱动不自己初始化 SPI 总线？

因为同一个工程里可能有多个 SPI 设备，例如 LCD、触摸芯片、传感器、外部 Flash。SPI 总线由用户统一初始化更灵活，也更符合 ESP-IDF 的工程组织方式。

### Q2：为什么 RGB565 要字节交换？

ESP32 是小端架构，而 ST7735 接收 RGB565 像素时要求高字节先发送。例如红色 `0xF800`，LCD 需要收到 `F8 00`。驱动内部通过 `color_to_be()` 完成转换。

### Q3：为什么单点绘制比较慢？

因为画一个点需要：

1. 设置列地址；
2. 设置行地址；
3. 发送 RAMWR；
4. 发送 2 字节像素数据。

大量绘制点时开销很大，应尽量合并成矩形或位图刷新。

### Q4：第二块屏幕为什么不显示？

重点检查：

1. 第二块 LCD 的 CS 是否独立；
2. `LCD2_ENABLE` 是否改成 `1`；
3. DC/RST/BL 引脚是否接对；
4. 共用 SPI 线是否连接可靠；
5. 第二块屏幕是否需要不同的 `x_offset/y_offset/invert_color/color_order`。

### Q5：0.96 寸屏一定是 80x160 吗？

不一定。市面上 0.96 寸 ST7735 多数是 80x160，但也可能存在不同可视区域或不同偏移。请以购买页面、屏幕 FPC 标识、卖家资料或实际测试为准。

---

## 12. 示例效果说明

默认 `main.c` 会绘制：

1. 黑色背景；
2. 左上红色矩形；
3. 右上绿色矩形；
4. 左下蓝色矩形；
5. 右下黄色矩形；
6. 两条白色对角线。

如果显示结果与预期不同，可以按照下表处理：

| 现象 | 可能原因 | 处理方式 |
|---|---|---|
| 红蓝反了 | RGB/BGR 顺序不匹配 | 修改 `color_order` |
| 颜色像底片 | 颜色反转设置不匹配 | 修改 `invert_color` |
| 图像偏移 | 显存偏移不匹配 | 调整 `x_offset/y_offset` |
| 画面横竖方向不对 | 旋转方向不合适 | 调用 `lcd_st7735_set_rotation()` |
| 屏幕不亮 | 背光或电源问题 | 检查 BL/VCC/GND |
| 花屏 | SPI 过快或接线太长 | 降低 `spi_clock_hz` |

---

## 13. 适合后续扩展的方向

当前驱动是基础图形驱动，可以继续扩展：

- 字符显示，例如 ASCII、GB2312、UTF-8 字库；
- 基础图形，例如直线、圆、圆角矩形、进度条；
- 图片解码，例如 RGB565 数组、BMP、PNG；
- DMA 异步刷新；
- LVGL 显示接口；
- 局部刷新缓存；
- 背光 PWM 亮度调节；
- TE 引脚同步刷新，减少撕裂。

---

## 14. 许可与使用建议

该工程代码结构简单，适合直接复制到 ESP-IDF 项目中使用。用于课程设计、技能竞赛、创新创业项目或产品原型时，建议保留清晰注释，并根据实际屏幕模组补充你的硬件参数说明。
