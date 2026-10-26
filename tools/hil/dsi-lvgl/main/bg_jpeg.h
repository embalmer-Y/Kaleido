/* bg_jpeg.h — V-36 JPEG 硬解背景（能力封存，壁纸/相框场景启用） */
#ifndef BG_JPEG_H
#define BG_JPEG_H

#include <stddef.h>

#include "lvgl.h"

/* jpg 指向 800×1280 JPEG 码流（调用方 EMBED 资产），解码后经
 * bg_jpeg_dsc() 取描述子垫底 */
int bg_jpeg_init(const void *jpg, size_t jpg_len);
const lv_image_dsc_t *bg_jpeg_dsc(void);

#endif
