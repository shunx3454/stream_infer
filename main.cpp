/*
 * Copyright (c) 2025-04-01 HeXiaotian
 *
 * This source code is licensed for learning and research purposes only.
 * Commercial use, redistribution, resale, and creation of derivative works
 * are strictly prohibited without prior written permission from the author.
 */

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <opencv2/highgui.hpp>
#include <ostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <im2d_buffer.h>
#include <im2d_common.h>
#include <im2d_single.h>
#include <rga.h>

#include <opencv2/core/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv4/opencv2/opencv.hpp>

#include <rknn_api.h>

#include "postprocess.h"
#include "preprocess.h"

#define W_ALIGN 16
#define H_ALIGN 2

int main(int argc, char *argv[]) {
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

    // 画框
    // dst = wrapbuffer_virtualaddr_t(img.data, img.cols, img.rows, img.step1(),
    //                                img.rows,
    //                                RK_FORMAT_RGB_888); // wstride, hstride,
    // im_rect rests[group.count];
    // for (int n = 0; n < group.count; n++) {
    //   rests[n].x = group.results[n].box.left;
    //   rests[n].y = group.results[n].box.top;
    //   rests[n].width = group.results[n].box.right - group.results[n].box.left;
    //   rests[n].height = group.results[n].box.bottom - group.results[n].box.top;
    // }
    // IM_STATUS status =
    //     imrectangleArray(dst, rests, group.count, 0x00FF0000, 0x00FF0000, 2);
    // if (IM_STATUS_SUCCESS != status) {
    //   std::cerr << "imrectangleArray error: " << status << std::endl;
    //   throw std::runtime_error("imrectangleArray error");
    // }
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
    cv::imshow("result", resized_img);
    cv::waitKey(0);

    // release output
    ret = rknn_outputs_release(ctx, io_num.n_output, outputs.data());
    if (ret < 0) {
      std::cerr << "rknn_outputs_release error: " << ret << std::endl;
      throw std::runtime_error("rknn_outputs_release error");
    }
  }

  rknn_destroy(ctx);
  return 0;
}