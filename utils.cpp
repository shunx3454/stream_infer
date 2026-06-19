/*
 * Copyright (C) 2022  Rockchip Electronics Co., Ltd.
 * Authors:
 *     YuQiaowei <cerf.yu@rock-chips.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>

#include "RgaUtils.h"
#include "rga.h"

#ifdef __cplusplus
extern "C" {
#endif

void draw_rgba(char *buffer, int width, int height) {
    for (int i = 0; i < height; i++) {
       for (int j = 0; j < width/4; j++) {
           buffer[(i*width*4) + j*4 + 0] = 0xff;   //R
           buffer[(i*width*4) + j*4 + 1] = 0x00;   //G
           buffer[(i*width*4) + j*4 + 2] = 0x00;   //B
           buffer[(i*width*4) + j*4 + 3] = 0xff;   //A
       }
       for (int j = width/4; j < width/4*2; j++) {
           buffer[(i*width*4) + j*4 + 0] = 0x00;
           buffer[(i*width*4) + j*4 + 1] = 0xff;
           buffer[(i*width*4) + j*4 + 2] = 0x00;
           buffer[(i*width*4) + j*4 + 3] = 0xff;
       }
       for (int j = width/4*2; j < width/4*3; j++) {
           buffer[(i*width*4) + j*4 + 0] = 0x00;
           buffer[(i*width*4) + j*4 + 1] = 0x00;
           buffer[(i*width*4) + j*4 + 2] = 0xff;
           buffer[(i*width*4) + j*4 + 3] = 0xff;
       }
       for (int j = width/4*3; j < width; j++) {
           buffer[(i*width*4) + j*4 + 0] = 0xff;
           buffer[(i*width*4) + j*4 + 1] = 0xff;
           buffer[(i*width*4) + j*4 + 2] = 0xff;
           buffer[(i*width*4) + j*4 + 3] = 0xff;
       }
    }
}

void draw_YUV420(char *buffer, int width, int height) {
    /* Y channel */
    memset(buffer, 0xa8, width * height / 2);
    memset(buffer + width * height / 2, 0x54, width * height / 2);
    /* UV channel */
    memset(buffer + width * height, 0x80, width * height / 4);
    memset(buffer + (int)(width * height * 1.25), 0x30, width * height / 4);
}

void draw_YUV422(char *buffer, int width, int height) {
    /* Y channel */
    memset(buffer, 0xa8, width * height / 2);
    memset(buffer + width * height / 2, 0x54, width * height / 2);
    /* UV channel */
    memset(buffer + width * height, 0x80, width * height / 2);
    memset(buffer + (int)(width * height * 1.5), 0x30, width * height / 2);
}

void draw_gray256(char *buffer, int width, int height) {
    for (int i = 0; i < height; i++) {
       for (int j = 0; j < width/4; j++) {
            buffer[(i*width*4) + j*4] = 0xa8;
       }
       for (int j = width/4; j < width/4*2; j++) {
           buffer[(i*width*4) + j*4] = 0x80;
       }
       for (int j = width/4*2; j < width/4*3; j++) {
           buffer[(i*width*4) + j*4] = 0x54;
       }
       for (int j = width/4*3; j < width; j++) {
           buffer[(i*width*4) + j*4] = 0x30;
       }
    }
}

void draw_image(char *buffer, int width, int height, int format) {
    switch (format) {
        case RK_FORMAT_RGBA_8888:
            draw_rgba(buffer, width, height);
            break;
        case RK_FORMAT_YCbCr_420_SP:
            draw_YUV420(buffer, width, height);
            break;
        case RK_FORMAT_YCbCr_422_SP:
            draw_YUV422(buffer, width, height);
            break;
        default:
            draw_gray256(buffer, width * get_bpp_from_format(format), height);
            break;
    }
}

int read_image_from_fbc_file(void *buf, const char *path, int sw, int sh, int fmt, int index) {
    int size;
    char filePath[100];
    const char *inputFbcFilePath = "%s/in%dw%d-h%d-%s-fbc.bin";

    snprintf(filePath, 100, inputFbcFilePath,
             path, index, sw, sh, translate_format_str(fmt));

    FILE *file = fopen(filePath, "rb");
    if (!file) {
        fprintf(stderr, "Could not open %s\n", filePath);
        return -EINVAL;
    }

    size = sw * sh * get_bpp_from_format(fmt) * 1.5;

    fread(buf, size, 1, file);

    fclose(file);

    return 0;
}

int read_image_from_file(void *buf, const char *path, int sw, int sh, int fmt, int index) {
    int size;
    char filePath[100];
    const char *inputFilePath = "%s/in%dw%d-h%d-%s.bin";

    snprintf(filePath, 100, inputFilePath,
             path, index, sw, sh, translate_format_str(fmt));

    FILE *file = fopen(filePath, "rb");
    if (!file) {
        fprintf(stderr, "Could not open %s\n", filePath);
        return -EINVAL;
    }

    size = sw * sh * get_bpp_from_format(fmt);

    fread(buf, size, 1, file);

    fclose(file);

    return 0;
}

int write_image_to_fbc_file(void *buf, const char *path, int sw, int sh, int fmt, int index) {
    int size;
    char filePath[100];
    const char *outputFbcFilePath = "%s/out%dw%d-h%d-%s-fbc.bin";

    snprintf(filePath, 100, outputFbcFilePath,
             path, index, sw, sh, translate_format_str(fmt));

    FILE *file = fopen(filePath, "wb+");
    if (!file) {
        fprintf(stderr, "Could not open %s\n", filePath);
        return false;
    } else {
        fprintf(stderr, "open %s and write ok\n", filePath);
    }

    size = sw * sh * get_bpp_from_format(fmt) * 1.5;

    fwrite(buf, size, 1, file);

    fclose(file);

    return 0;
}

int write_image_to_file(void *buf, const char *path, int sw, int sh, int fmt, int index) {
    int size;
    char filePath[100];
    const char *outputFilePath = "%s/out%dw%d-h%d-%s.bin";

    snprintf(filePath, 100, outputFilePath,
             path, index, sw, sh, translate_format_str(fmt));

    FILE *file = fopen(filePath, "wb+");
    if (!file) {
        fprintf(stderr, "Could not open %s\n", filePath);
        return false;
    } else {
        fprintf(stderr, "open %s and write ok\n", filePath);
    }

    size = sw * sh * get_bpp_from_format(fmt);

    fwrite(buf, size, 1, file);

    fclose(file);

    return 0;
}

#ifdef __cplusplus
}
#endif

#include <vector>
#include <fstream>
#include <opencv2/core/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv4/opencv2/opencv.hpp>

std::vector<char> read_file(const std::string &path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return {};
    }

    ifs.seekg(0, std::ios::end);
    size_t size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    std::vector<char> data(size);
    ifs.read(reinterpret_cast<char *>(data.data()), size);
    return data;
}

void img_uyvy_save(void *ptr, size_t width, size_t height, size_t x_offset, size_t y_offset, std::string path) {
    // 将指针转换为 uint8_t 方便进行字节级操作
    uint8_t *dst = reinterpret_cast<uint8_t *>(ptr);

    // UYVY 格式下，每行的字节跨度 (Stride) 是 宽度 * 2
    size_t stride = width * 2;

    // 定义棋盘格块的大小
    const size_t block_size = 100;

    // 预校准红黑两色在 YUV 颜色空间中的 8-bit 数值
    // 纯黑: Y=16,   U=128, V=128
    // 纯红: Y=82,   U=90,  V=240  (标准 BT.601 转换公式计算所得)
    const uint8_t Y_BLACK = 16, U_BLACK = 128, V_BLACK = 128;
    const uint8_t Y_RED = 82, U_RED = 90, V_RED = 240;

    // 逐行遍历图像 (Y轴)
    for (size_t y = 0; y < height; ++y) {
        // 计算当前像素在全局棋盘格系统中的绝对 Y 坐标（加入 Y 轴偏移）
        size_t global_y = y + y_offset;
        size_t block_y = global_y / block_size;

        // 获取当前行的内存起始指针
        uint8_t *row_ptr = dst + (y * stride);

        // 逐对像素遍历图像 (X轴)，每次处理 2 个像素 (Pixel 0 和 Pixel 1)
        // width 必须为 2 的倍数（由于硬件对齐，1920 / 1088 完美符合）
        for (size_t x = 0; x < width; x += 2) {
            // 计算这两个像素在全局棋盘格系统中的绝对 X 坐标
            size_t global_x0 = x + x_offset;
            size_t global_x1 = (x + 1) + x_offset;

            size_t block_x0 = global_x0 / block_size;
            size_t block_x1 = global_x1 / block_size;

            // 决定 Pixel 0 的颜色：(block_x + block_y) 为偶数时为红，奇数为黑
            bool is_red0 = ((block_x0 + block_y) % 2 == 0);
            uint8_t y0 = is_red0 ? Y_RED : Y_BLACK;
            uint8_t u0 = is_red0 ? U_RED : U_BLACK;
            uint8_t v0 = is_red0 ? V_RED : V_BLACK;

            // 决定 Pixel 1 的颜色
            bool is_red1 = ((block_x1 + block_y) % 2 == 0);
            uint8_t y1 = is_red1 ? Y_RED : Y_BLACK;
            uint8_t u1 = is_red1 ? U_RED : U_BLACK;
            uint8_t v1 = is_red1 ? V_RED : V_BLACK;

            // 核心：由于 UYVY 两个像素共享一对 UV 分量，我们对两者的色度取平均值值
            uint8_t u_shared = (u0 + u1) / 2;
            uint8_t v_shared = (v0 + v1) / 2;

            // 根据 UYVY 内存布局打包填充: [U, Y0, V, Y1]
            size_t byte_idx = x * 2;
            row_ptr[byte_idx] = u_shared;     // Byte 0: U
            row_ptr[byte_idx + 1] = y0;       // Byte 1: Y0
            row_ptr[byte_idx + 2] = v_shared; // Byte 2: V
            row_ptr[byte_idx + 3] = y1;       // Byte 3: Y1
        }
    }

    // 绘制完成后，使用 OpenCV 零拷贝读取这段 UYVY 内存
    // UYVY 是 2 通道数据 (CV_8UC2)，传递明确的 stride (每行字节数)
    cv::Mat uyvy_mat(height, width, CV_8UC2, ptr, stride);

    // 将 UYVY 转换为 OpenCV 常用的 BGR 格式
    cv::Mat bgr_mat;
    cv::cvtColor(uyvy_mat, bgr_mat, cv::COLOR_YUV2BGR_UYVY);

    // 写入文件 (支持 .png, .jpg 等)
    // if (cv::imwrite(path, bgr_mat)) {
    //     std::cout << "Successfully saved pattern image to: " << path << std::endl;
    // } else {
    //     std::cerr << "Failed to write image to: " << path << std::endl;
    // }
}
