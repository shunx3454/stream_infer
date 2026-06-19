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

#ifndef __RGA_SAMPLES_UTILS_H__
#define __RGA_SAMPLES_UTILS_H__

#include <stdlib.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

inline static int64_t get_cur_us() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000000LL + tv.tv_usec;
}

inline static int64_t get_cur_ms() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

void draw_rgba(char *buffer, int width, int height);
void draw_YUV420(char *buffer, int width, int height);
void draw_YUV422(char *buffer, int width, int height);
void draw_gray256(char *buffer, int width, int height);
void draw_image(char *buffer, int width, int height, int format);
int read_image_from_fbc_file(void *buf, const char *path, int sw, int sh, int fmt, int index);
int read_image_from_file(void *buf, const char *path, int sw, int sh, int fmt, int index);
int write_image_to_fbc_file(void *buf, const char *path, int sw, int sh, int fmt, int index);
int write_image_to_file(void *buf, const char *path, int sw, int sh, int fmt, int index);

#ifdef __cplusplus
}
#endif

inline static int64_t get_now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // 硬件单调时钟，不受系统改时间影响
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
std::vector<char> read_file(const std::string &path);
void img_uyvy_save(void *ptr, size_t width, size_t height, size_t x_offset, size_t y_offset, std::string path) ;

#endif /* #ifndef __RGA_SAMPLES_UTILS_H__ */
