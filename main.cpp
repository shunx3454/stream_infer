/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#include <fcntl.h>
#include <fmt/core.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <ostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <ctime>

#include <RockchipRga.h>
#include <im2d.hpp>
#include <im2d_type.h>

#include <opencv2/core/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv4/opencv2/opencv.hpp>

#include "mpp_enc.h"
#include "rtsp.h"
#include "v4l2_cap.h"

#include <rknn_api.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "dma_alloc.h"
#include "postprocess.h"
#include "preprocess.h"
#include "utils.h"

#include "dmabuf.h"

#define W_ALIGN 16
#define H_ALIGN 2

inline int64_t get_now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts); // 硬件单调时钟，不受系统改时间影响
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static std::vector<uchar> read_file(const std::string &path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        return {};
    }

    ifs.seekg(0, std::ios::end);
    size_t size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    std::vector<uchar> data(size);
    ifs.read(reinterpret_cast<char *>(data.data()), size);
    return data;
}

static void img_uyvy_save(void *ptr, size_t width, size_t height, size_t x_offset, size_t y_offset, std::string path) {
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

int main(int argc, char *argv[]) {

#if 0
  rknn_context ctx;
  rknn_core_mask cm = RKNN_NPU_CORE_0;
  rknn_sdk_version rknn_ver;
  rknn_input_output_num io_num;
  std::vector<rknn_tensor_attr> in_attrs;
  std::vector<rknn_tensor_attr> out_attrs;
  // init rga context
  rga_buffer_t src;
  rga_buffer_t dst;

  // input tensor mem
  std::vector<rknn_input> inputs;
  std::vector<rknn_output> outputs;

  // post process result
  detect_result_group_t group;

  int ret;
  int loop_count = 1;
  if (argc > 1) {
    loop_count = std::stoi(argv[1]);
  }

  std::ifstream model_file(
      "/home/rock/c_cpp/stream_infer/model/person_relu.rknn", std::ios::binary);
  if (!model_file.is_open()) {
    throw std::runtime_error("file can not open\n");
  }

  model_file.seekg(0, std::ios::end);
  std::streamsize size = model_file.tellg();
  std::cout << "model size=" << size << " bytes." << std::endl;
  model_file.seekg(0, std::ios::beg);

  std::unique_ptr<char[]> model_data(new char[size]);
  model_file.read(model_data.get(), size);

  // init
  ret = rknn_init(&ctx, model_data.get(), size, 0, nullptr);
  if (ret < 0) {
    std::cerr << "rknn_init failed: " << ret << std::endl;
    return -1;
  }
  std::cout << "RKNN init success!" << std::endl;

  // set core nu
  ret = rknn_set_core_mask(ctx, cm);
  if (ret < 0) {
    throw std::runtime_error("rknn core mask set error");
  }

  // quary version
  ret = rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &rknn_ver,
                   sizeof(rknn_sdk_version));
  if (ret < 0) {
    throw std::runtime_error("rknn quary error");
  }
  std::cout << "rknn sdk version: " << rknn_ver.api_version
            << ", driver version: " << rknn_ver.drv_version << std::endl;

  // quary io num
  ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num,
                   sizeof(rknn_input_output_num));
  if (ret < 0) {
    throw std::runtime_error("rknn quary io num error");
  }

  // quary in attr
  in_attrs = std::vector<rknn_tensor_attr>(io_num.n_input);
  for (int i = 0; i < io_num.n_input; i++) {
    in_attrs[i].index = i;
    ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(in_attrs[i]),
                     sizeof(rknn_tensor_attr));
    if (ret < 0) {
      throw std::runtime_error("rknn quary in attrs error");
    }
  }
  for (int i = 0; i < io_num.n_input; i++) {
    printf("input %d: name=%s, type=%d, qnt_type=%d, zp=%d, scale=%f, fmt=%d "
           "n_dims=%d, ",
           i, in_attrs[i].name, in_attrs[i].type, in_attrs[i].qnt_type,
           in_attrs[i].zp, in_attrs[i].scale, in_attrs[i].fmt,
           in_attrs[i].n_dims);

    std::cout << "dims=[";
    for (int j = 0; j < in_attrs[i].n_dims; j++) {
      std::cout << in_attrs[i].dims[j];
      if (j != in_attrs[i].n_dims - 1) {
        std::cout << ",";
      }
    }
    std::cout << "], n_elems=" << in_attrs[i].n_elems
              << ", size=" << in_attrs[i].size << " bytes." << std::endl;
  }

  // quary out attr
  out_attrs = std::vector<rknn_tensor_attr>(io_num.n_output);
  for (int i = 0; i < io_num.n_output; i++) {
    out_attrs[i].index = i;
    ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(out_attrs[i]),
                     sizeof(rknn_tensor_attr));
    if (ret < 0) {
      throw std::runtime_error("rknn quary out attrs error");
    }
  }

  for (int i = 0; i < io_num.n_output; i++) {
    printf("output %d: name=%s, type=%d, qnt_type=%d, zp=%d, scale=%f, fmt=%d "
           "n_dims=%d, ",
           i, out_attrs[i].name, out_attrs[i].type, out_attrs[i].qnt_type,
           out_attrs[i].zp, out_attrs[i].scale, out_attrs[i].fmt,
           out_attrs[i].n_dims);

    std::cout << "dims=[";
    for (int j = 0; j < out_attrs[i].n_dims; j++) {
      std::cout << out_attrs[i].dims[j];
      if (j != out_attrs[i].n_dims - 1) {
        std::cout << ",";
      }
    }
    std::cout << "], n_elems=" << out_attrs[i].n_elems
              << ", size=" << out_attrs[i].size << " bytes." << std::endl;
  }

  // 获取图片数据
  cv::Mat img =
      cv::imread("/home/rock/c_cpp/stream_infer/asset/person_cropped.png");
  if (img.empty()) {
    throw std::runtime_error("cv::imread error");
  }
  std::cout << "image color type: " << img.type() << std::endl;
  std::cout << "image size: " << img.cols << "x" << img.rows
            << ", channels: " << img.channels() << std::endl;

  // cv::Mat img_rgb;
  // cv::cvtColor(img, img_rgb, cv::COLOR_BGR2RGB);
  // img = img_rgb; // 直接使用转换后的RGB图像进行后续处理
  //  裁剪 (ROI)
  // cv::Rect roi(10, 10, 1600, 1000);
  // cv::Mat img_rgb = img(roi);
  // std::cout << "image_rgb color type: " << img_rgb.type() << std::endl;
  // std::cout << "image size: " << img_rgb.cols << "x" << img_rgb.rows
  //           << ", channels: " << img_rgb.channels() << std::endl;
  // cv::imwrite("/home/rock/c_cpp/stream_infer/asset/person_crop.png",
  // img_rgb);

  // rga info && set core
  {
      std::cout << querystring(RGA_ALL) << std::endl;
      IM_STATUS status = imconfig(IM_CONFIG_PRIORITY, IM_SCHEDULER_RGA3_DEFAULT);
      if (IM_STATUS_SUCCESS != status) {
          std::cerr << "imconfig error: " << status << std::endl;
          throw std::runtime_error("imconfig error");
      }
  }

  // rga resize
  // 指定目标大小和预处理方式,默认使用LetterBox的预处理
  int model_in_width = in_attrs[0].dims[2];
  int model_in_height = in_attrs[0].dims[1];
  int scale_w = (float)model_in_width / img.cols;
  int scale_h = (float)model_in_height / img.rows;

  BOX_RECT pads;
  memset(&pads, 0, sizeof(BOX_RECT));
  cv::Size target_size(model_in_width, model_in_height);
  cv::Mat resized_img(target_size.height, target_size.width, CV_8UC3);
  // 计算缩放比例
  resize_rga(src, dst, img, resized_img, target_size);
  // letterbox(img, resized_img, pads, std::min(scale_w, scale_h), target_size);
  cv::imwrite("/home/rock/c_cpp/stream_infer/asset/person_resized.png",
              resized_img);

  // rga resize
  // src = wrapbuffer_virtualaddr((void *)img_rgb.data, img_rgb.cols,
  // img_rgb.rows,
  //                              RK_FORMAT_RGB_888); // wstride, hstride,
  //                              format
  // int model_in_width = in_attrs[0].dims[2];
  // int model_in_height = in_attrs[0].dims[1];
  // int wstride = model_in_width + (W_ALIGN - model_in_width % W_ALIGN) %
  // W_ALIGN; int hstride =
  //     model_in_height + (H_ALIGN - model_in_height % H_ALIGN) % H_ALIGN;
  // dst = wrapbuffer_fd_t(in_mems[0]->fd, model_in_width, model_in_height,
  //                       wstride, hstride,
  //                       RK_FORMAT_RGB_888); // wstride, hstride,
  // ret = imcheck(src, dst, src_rect, dst_rect);
  // if (IM_STATUS_NOERROR != ret) {
  //   std::cerr << "imcheck error: " << ret << std::endl;
  //   throw std::runtime_error("imcheck error");
  // }
  // IM_STATUS status = imresize(src, dst);
  // if (IM_STATUS_SUCCESS != status) {
  //   std::cerr << "imresize error: " << status << std::endl;
  //   throw std::runtime_error("imresize error");
  // }

  // input tensor setting
  inputs = std::vector<rknn_input>(io_num.n_input);
  inputs[0].index = 0;
  inputs[0].type = RKNN_TENSOR_UINT8;
  inputs[0].size = model_in_height * model_in_width * 3;
  inputs[0].fmt = RKNN_TENSOR_NHWC;
  inputs[0].pass_through = 0;
  inputs[0].buf = resized_img.data;
  ret = rknn_inputs_set(ctx, io_num.n_input, inputs.data());
  if (ret < 0) {
    std::cerr << "rknn_inputs_set error: " << ret << std::endl;
    throw std::runtime_error("rknn_inputs_set error");
  }
  outputs = std::vector<rknn_output>(io_num.n_output);
  for (int i = 0; i < io_num.n_output; i++) {
    outputs[i].index = i;
    outputs[i].want_float = 0;
  }

  // Run
  printf("Begin inference ...\n");
  for (int i = 0; i < loop_count; ++i) {
    auto start_t = std::chrono::high_resolution_clock::now();
    ret = rknn_run(ctx, NULL);
    auto end_t = std::chrono::high_resolution_clock::now();
    int64_t elapse_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end_t - start_t)
            .count();
    if (ret < 0) {
      printf("rknn run error %d\n", ret);
      return -1;
    }
    printf("%4d: Elapse Time = %.2fms, FPS = %.2f\n", i, elapse_us / 1000.f,
           1000.f * 1000.f / elapse_us);

    // get output
    ret = rknn_outputs_get(ctx, io_num.n_output, outputs.data(), NULL);
    if (ret < 0) {
      std::cerr << "rknn_outputs_get error: " << ret << std::endl;
      throw std::runtime_error("rknn_outputs_get error");
    }

    // post process
    ret = post_process(
        (int8_t *)outputs[0].buf, (int8_t *)outputs[1].buf,
        (int8_t *)outputs[2].buf, model_in_height, model_in_width, BOX_THRESH,
        NMS_THRESH, {0, 0, 0, 0}, (float)resized_img.cols / model_in_width,
        (float)resized_img.rows / model_in_height,
        {out_attrs[0].zp, out_attrs[1].zp, out_attrs[2].zp},
        {out_attrs[0].scale, out_attrs[1].scale, out_attrs[2].scale}, &group);
    if (ret < 0) {
      std::cerr << "post_process error: " << ret << std::endl;
      throw std::runtime_error("post_process error");
    }
    printf("detect result: count=%d\n", group.count);
    for (int n = 0; n < group.count; n++) {
      printf("result %2d: (%4d, %4d, %4d, %4d), %s, prop=%.2f\n", n,
             group.results[n].box.left, group.results[n].box.top,
             group.results[n].box.right, group.results[n].box.bottom,
             group.results[n].name, group.results[n].prop);
    }

    // release output
    ret = rknn_outputs_release(ctx, io_num.n_output, outputs.data());
    if (ret < 0) {
      std::cerr << "rknn_outputs_release error: " << ret << std::endl;
      throw std::runtime_error("rknn_outputs_release error");
    }

    // 画框
    std::vector<im_rect> rects(group.count);
    for (int n = 0; n < group.count; n++) {
      rects[n].x = group.results[n].box.left;
      rects[n].y = group.results[n].box.top;
      rects[n].width = group.results[n].box.right - group.results[n].box.left;
      rects[n].height = group.results[n].box.bottom - group.results[n].box.top;
    }
    for (int n = 0; n < group.count; n++) {
      cv::rectangle(
          resized_img,
          cv::Point(group.results[n].box.left, group.results[n].box.top),
          cv::Point(group.results[n].box.right, group.results[n].box.bottom),
          cv::Scalar(0, 255, 0), // BGR绿色
          2,                     // 线宽
          cv::LINE_8);           // 线型
    }
    cv::imwrite("/home/rock/c_cpp/stream_infer/asset/person_detected.png",
                resized_img);
  }
#endif

    // im draw rect
    // {
    //   imconfig(IM_CONFIG_SCHEDULER_CORE, IM_SCHEDULER_RGA3_CORE0);
    //   cv::Mat img2 =
    //       cv::imread("/home/rock/c_cpp/stream_infer/asset/person_cropped.png");
    //   int img_width = 160;
    //   int img_height = 160;
    //   int img_size = img2.total() * img2.elemSize();

    //   std::cout << "image width=" << img_width << ", height=" << img_height
    //             << ", size=" << img_size << " bytes." << std::endl;
    //   rga_buffer_t img2_buf = wrapbuffer_virtualaddr(
    //       (void *)img2.data, img_width, img_height, RK_FORMAT_RGB_888);

    //   im_rect rect{};
    //   rect.x = 0;
    //   rect.y = 0;
    //   rect.width = 160;
    //   rect.height = 160;
    //   std::cout << "draw rect: (" << rect.x << ", " << rect.y << ", "
    //             << rect.width << ", " << rect.height << ")" << std::endl;

    //   IM_STATUS status = imfill(img2_buf, rect, 0x000000FF);
    //   if (IM_STATUS_SUCCESS != status) {
    //     releasebuffer_handle(img2_buf.handle);
    //     std::cerr << "imfill error: " << status << std::endl;
    //     throw std::runtime_error("imfill error");
    //   }
    //   cv::imwrite("/home/rock/c_cpp/stream_infer/asset/person_cropped_rect.png",
    //               img2);
    //   releasebuffer_handle(img2_buf.handle);
    // }

    {
        FLAGS_minloglevel = 0;
        FLAGS_alsologtostderr = true;
        FLAGS_colorlogtostderr = true;
        google::InitGoogleLogging(argv[0]);

        LOG(INFO) << "This is an info message.";
        LOG(WARNING) << "This is a warning message.";
        LOG(ERROR) << "This is an error message.";
    }

#if 0
    {
        // imconfig(IM_CONFIG_SCHEDULER_CORE, IM_SCHEDULER_RGA3_CORE0);
        std::cout << querystring(RGA_ALL) << std::endl;

        std::string filename = "/home/rock/c_cpp/stream_infer/asset/person_cropped.png";

        int w = 0;
        int h = 0;
        int file_channels = 0;

        if (!stbi_info(filename.c_str(), &w, &h, &file_channels)) {
            printf("stbi_info failed: %s\n", stbi_failure_reason());
            return -1;
        }
        std::cout << "image width=" << w << ", height=" << h << ", channels=" << file_channels << std::endl;

        int ret;
        void *src_buf = nullptr;
        void *dst_buf = nullptr;
        int src_dma_fd = -1;
        int dst_dma_fd = -1;
        int img_height = h;
        int img_width = w;

        int img_target_width = 800;
        int img_target_height = 600;
        img_width = (img_width + W_ALIGN - 1) / W_ALIGN * W_ALIGN;
        img_height = (img_height + H_ALIGN - 1) / H_ALIGN * H_ALIGN;
        size_t src_buf_size = img_height * img_width * file_channels;
        size_t dst_buf_size = img_target_width * img_target_height * file_channels;

        /* Allocate cacheable dma_buf, return dma_fd and virtual address. */
        ret = dma_buf_alloc("/dev/dma_heap/cma", src_buf_size, &src_dma_fd, (void **)&src_buf);
        if (ret < 0) {
            printf("alloc src dma_heap buffer failed!\n");
            return -1;
        }
        ret = dma_buf_alloc("/dev/dma_heap/cma", dst_buf_size, &dst_dma_fd, (void **)&dst_buf);
        if (ret < 0) {
            printf("alloc dst dma_heap buffer failed!\n");
            dma_buf_free(src_buf_size, src_dma_fd, src_buf);
            return -1;
        }

        cv::Mat img(img_height, img_width, CV_8UC3, src_buf);
        std::vector<uchar> raw_data = read_file(filename);
        cv::Mat decoded = cv::imdecode(raw_data, cv::IMREAD_COLOR, &img);
        if (decoded.empty() || decoded.data != img.data) {
            printf("cv::imdecode failed\n");
            dma_buf_free(src_buf_size, src_dma_fd, src_buf);
            dma_buf_free(dst_buf_size, dst_dma_fd, dst_buf);
            return -1;
        }

        cv::Mat resized_img(img_target_height, img_target_width, CV_8UC3, dst_buf);

        rga_buffer_t src;
        rga_buffer_t dst;
        rga_buffer_handle_t src_handle;
        rga_buffer_handle_t dst_handle;
        im_rect src_rect;
        im_rect dst_rect;
        memset(&src_rect, 0, sizeof(src_rect));
        memset(&dst_rect, 0, sizeof(dst_rect));
        // auto src_handle = importbuffer_virtualaddr((void *)img.data,
        //                                            img.total() * img.elemSize());
        // auto dst_handle = importbuffer_virtualaddr(
        //     (void *)resized_img.data, resized_img.total() *
        //     resized_img.elemSize());
        // src = wrapbuffer_handle(src_handle, img.cols, img.rows,
        // RK_FORMAT_RGB_888); dst = wrapbuffer_handle(dst_handle, img_target_width,
        // img_target_height,
        //                         RK_FORMAT_RGB_888);

        // src = wrapbuffer_virtualaddr((void *)img.data, img.cols, img.rows,
        //                              RK_FORMAT_RGB_888);
        // dst = wrapbuffer_virtualaddr((void *)resized_img.data, img_target_width,
        //                              img_target_height, RK_FORMAT_RGB_888);

        // src_handle = importbuffer_fd(src_dma_fd, src_buf_size);
        // dst_handle = importbuffer_fd(dst_dma_fd, dst_buf_size);
        // src = wrapbuffer_handle(src_handle, img.cols, img.rows,
        // RK_FORMAT_RGB_888); dst = wrapbuffer_handle(dst_handle, img_target_width,
        // img_target_height,
        //                         RK_FORMAT_RGB_888);

        src = wrapbuffer_fd(src_dma_fd, img.cols, img.rows, RK_FORMAT_RGB_888);
        dst = wrapbuffer_fd(dst_dma_fd, img_target_width, img_target_height, RK_FORMAT_RGB_888);

        // src_rect.x = 0;
        // src_rect.y = 0;
        // src_rect.width = img.cols;
        // src_rect.height = img.rows;

        // dst_rect.x = 0;
        // dst_rect.y = 0;
        // dst_rect.width = img_target_width;
        // dst_rect.height = img_target_height;

        ret = imcheck(src, dst, src_rect, dst_rect);
        if (IM_STATUS_NOERROR != ret) {
            fprintf(stderr, "rga check error! %s", imStrError((IM_STATUS)ret));
            return -1;
        }

        dma_sync_cpu_to_device(src_dma_fd);
        dma_sync_cpu_to_device(dst_dma_fd);

        IM_STATUS STATUS = imresize(src, dst);
        if (IM_STATUS_SUCCESS != STATUS) {
            fprintf(stderr, "rga resize error! %s", imStrError(STATUS));
            return -1;
        }
        ret = imrectangle(dst, {0, 0, 100, 100}, 0x00000000ff, 4);
        if (IM_STATUS_SUCCESS != ret) {
            fprintf(stderr, "rga fill error! %s", imStrError(STATUS));
            return -1;
        }

        ret = imfill(dst, {500, 500, 80, 80}, 0x0000ff00);
        if (IM_STATUS_SUCCESS != ret) {
            fprintf(stderr, "rga fill error! %s", imStrError(STATUS));
            return -1;
        }

        dma_sync_device_to_cpu(dst_dma_fd);
        cv::imwrite("/home/rock/c_cpp/stream_infer/asset/person_resized_rga.png", resized_img);

        // releasebuffer_handle(src_handle);
        // releasebuffer_handle(dst_handle);

        dma_buf_free(src_buf_size, src_dma_fd, src_buf);
        dma_buf_free(dst_buf_size, dst_dma_fd, dst_buf);

        // imconfig(IM_CONFIG_SCHEDULER_CORE, IM_SCHEDULER_RGA3_CORE0);
        // BOX_RECT pads;
        // memset(&pads, 0, sizeof(BOX_RECT));
        // // 计算缩放比例
        // resize_rga(src, dst, img, resized_img, target_size);
    }
#endif

    {
        std::string device_name = "/dev/video21";
        if (argc > 1) {
            device_name = argv[1];
        }

        // std::vector<DmaBuf> dbufs;
        // dbufs.emplace_back(1920 * 1088 * 2);
        // dbufs.emplace_back(1920 * 1088 * 2);
        // dbufs.emplace_back(1920 * 1088 * 2);
        // dbufs.emplace_back(1920 * 1088 * 2);

        Video video;
        video.init(device_name.c_str());
        // video.streamon_mp_dmabuf(dbufs);

        /*         auto ts = get_now_ms();
                while (true) {
                    auto te = get_now_ms();
                    auto dbuf = video.cap_frame_get();
                    fmt::print("dmabuf get: ts={} ms, df={}, vaddr={}, size={}\n", te - ts, dbuf.getFd(), dbuf.getVa(),
                               dbuf.getSize());
                    video.cap_frame_put(std::move(dbuf));
                } */

        // cv::namedWindow("capture", cv::WINDOW_NORMAL);
        // while (1) {
        //     if (video.captrue_mp_dma_test() < 0) {
        //         break;
        //     }
        //     // std::this_thread::sleep_for(std::chrono::microseconds(100));
        // }
        // cv::destroyWindow("capture");

        //     MppEncoder encoder;
        //     encoder.init();
        //     auto hdr = encoder.getHdr();

        //     DmaBuf pkt_dbuf(1920 * 1088 * 2);

        //     // rtsp
        //     RtspPusher rtsp_client;
        //     rtsp_client.init("rtsp://10.110.240.25:8554/test", 1920, 1088, 30, hdr.data(), hdr.size());

        //     auto ts = get_now_ms();

        //     while (true) {
        //         for (int i = 0; i < 30; ++i) {
        //             auto t1 = get_now_ms();
        //             // img_uyvy_save(frm_dbuf.getVa(), 1920, 1088, i * 4, i * 4,
        //             //               "/home/rock/c_cpp/stream_infer/asset/test_" + std::to_string(i) + ".jpg");
        //             auto frm_dbuf = video.cap_frame_get();

        //             auto t2 = get_now_ms();

        // 			// rknn infer frame

        // 			// rga draw frame

        // 			// mpp encode frame
        //             int64_t t3, t4;
        //             auto pkt_len = encoder.encode(
        //                 frm_dbuf, pkt_dbuf, i == 0 ? 1 : 0, 0,
        //                 [&rtsp_client, &t3, &t4](const void *ptr, size_t len, RK_U32 is_keyframe, RK_U32 eos) {
        //                     t3 = get_now_ms();
        //                     rtsp_client.push_h264_packet((const uint8_t *)ptr, len, is_keyframe);
        //                     t4 = get_now_ms();
        //                 });

        //             video.cap_frame_put(std::move(frm_dbuf));
        // 			std::this_thread::sleep_for(std::chrono::milliseconds(30));

        //             fmt::print("t1(new loop)={}, t2(v4l2 frame ok)={}, t3(mpp encode ok)={}, "
        //                        "t4(rtsp send ok)={}\n",
        //                        t1 - ts, t2 - ts, t3 - ts, t4 - ts);
        //         }
        //     }
    }

    // rknn_destroy(ctx);
    return 0;
}