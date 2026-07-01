#include "rknn.h"

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






// 获取图片数据
cv::Mat img = cv::imread("/home/rock/c_cpp/stream_infer/asset/person_cropped.png");
if (img.empty()) {
    throw std::runtime_error("cv::imread error");
}
std::cout << "image color type: " << img.type() << std::endl;
std::cout << "image size: " << img.cols << "x" << img.rows << ", channels: " << img.channels() << std::endl;

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
cv::imwrite("/home/rock/c_cpp/stream_infer/asset/person_resized.png", resized_img);

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
    int64_t elapse_us = std::chrono::duration_cast<std::chrono::microseconds>(end_t - start_t).count();
    if (ret < 0) {
        printf("rknn run error %d\n", ret);
        return -1;
    }
    printf("%4d: Elapse Time = %.2fms, FPS = %.2f\n", i, elapse_us / 1000.f, 1000.f * 1000.f / elapse_us);

    // get output
    ret = rknn_outputs_get(ctx, io_num.n_output, outputs.data(), NULL);
    if (ret < 0) {
        std::cerr << "rknn_outputs_get error: " << ret << std::endl;
        throw std::runtime_error("rknn_outputs_get error");
    }

    // post process
    ret = post_process((int8_t *)outputs[0].buf, (int8_t *)outputs[1].buf, (int8_t *)outputs[2].buf, model_in_height,
                       model_in_width, BOX_THRESH, NMS_THRESH, {0, 0, 0, 0}, (float)resized_img.cols / model_in_width,
                       (float)resized_img.rows / model_in_height, {out_attrs[0].zp, out_attrs[1].zp, out_attrs[2].zp},
                       {out_attrs[0].scale, out_attrs[1].scale, out_attrs[2].scale}, &group);
    if (ret < 0) {
        std::cerr << "post_process error: " << ret << std::endl;
        throw std::runtime_error("post_process error");
    }
    printf("detect result: count=%d\n", group.count);
    for (int n = 0; n < group.count; n++) {
        printf("result %2d: (%4d, %4d, %4d, %4d), %s, prop=%.2f\n", n, group.results[n].box.left,
               group.results[n].box.top, group.results[n].box.right, group.results[n].box.bottom, group.results[n].name,
               group.results[n].prop);
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
        cv::rectangle(resized_img, cv::Point(group.results[n].box.left, group.results[n].box.top),
                      cv::Point(group.results[n].box.right, group.results[n].box.bottom),
                      cv::Scalar(0, 255, 0), // BGR绿色
                      2,                     // 线宽
                      cv::LINE_8);           // 线型
    }
    cv::imwrite("/home/rock/c_cpp/stream_infer/asset/person_detected.png", resized_img);
}

#endif

RKNN::RKNN(std::string model_name, rknn_core_mask cm) {

    // data
    std::ifstream model_file(model_name, std::ios::binary);
    if (!model_file.is_open()) {
        throw std::runtime_error("file can not open\n");
    }

    model_file.seekg(0, std::ios::end);
    std::streamsize size = model_file.tellg();
    std::cout << "model size=" << size << " bytes." << std::endl;
    model_file.seekg(0, std::ios::beg);

    auto model_data = std::vector<char>(size);
    model_file.read(model_data.data(), size);

    // init
    int ret = rknn_init(&ctx, model_data.data(), size, 0, nullptr);
    if (ret < 0) {
        std::cerr << "rknn_init failed: " << ret << std::endl;
        return;
    }
    std::cout << "RKNN init success!" << std::endl;

    // set core nu
    ret = rknn_set_core_mask(ctx, cm);
    if (ret < 0) {
        throw std::runtime_error("rknn core mask set error");
    }

    // quary version
    ret = rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &rknn_ver, sizeof(rknn_sdk_version));
    if (ret < 0) {
        throw std::runtime_error("rknn quary error");
    }
    std::cout << "rknn sdk version: " << rknn_ver.api_version << ", driver version: " << rknn_ver.drv_version
              << std::endl;

    // quary io num
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(rknn_input_output_num));
    if (ret < 0) {
        throw std::runtime_error("rknn quary io num error");
    }

    // quary in attr
    in_attrs = std::vector<rknn_tensor_attr>(io_num.n_input);
    for (int i = 0; i < io_num.n_input; i++) {
        in_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(in_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret < 0) {
            throw std::runtime_error("rknn quary in attrs error");
        }
    }
    for (int i = 0; i < io_num.n_input; i++) {
        printf("input %d: name=%s, type=%d, qnt_type=%d, zp=%d, scale=%f, fmt=%d "
               "n_dims=%d, ",
               i, in_attrs[i].name, in_attrs[i].type, in_attrs[i].qnt_type, in_attrs[i].zp, in_attrs[i].scale,
               in_attrs[i].fmt, in_attrs[i].n_dims);

        std::cout << "dims=[";
        for (int j = 0; j < in_attrs[i].n_dims; j++) {
            std::cout << in_attrs[i].dims[j];
            if (j != in_attrs[i].n_dims - 1) {
                std::cout << ",";
            }
        }
        std::cout << "], n_elems=" << in_attrs[i].n_elems << ", size=" << in_attrs[i].size << " bytes." << std::endl;
    }

    // quary out attr
    out_attrs = std::vector<rknn_tensor_attr>(io_num.n_output);
    for (int i = 0; i < io_num.n_output; i++) {
        out_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(out_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret < 0) {
            throw std::runtime_error("rknn quary out attrs error");
        }
    }

    for (int i = 0; i < io_num.n_output; i++) {
        printf("output %d: name=%s, type=%d, qnt_type=%d, zp=%d, scale=%f, fmt=%d "
               "n_dims=%d, ",
               i, out_attrs[i].name, out_attrs[i].type, out_attrs[i].qnt_type, out_attrs[i].zp, out_attrs[i].scale,
               out_attrs[i].fmt, out_attrs[i].n_dims);

        std::cout << "dims=[";
        for (int j = 0; j < out_attrs[i].n_dims; j++) {
            std::cout << out_attrs[i].dims[j];
            if (j != out_attrs[i].n_dims - 1) {
                std::cout << ",";
            }
        }
        std::cout << "], n_elems=" << out_attrs[i].n_elems << ", size=" << out_attrs[i].size << " bytes." << std::endl;
    }

    // mem alloc
    rkmem_out = std::vector<rknn_tensor_mem *>(io_num.n_output);
    for (int i = 0; i < io_num.n_output; ++i) {
        rkmem_out[i] = rknn_create_mem(ctx, out_attrs[i].size_with_stride);
    }

    // resize imgd
    dstImgd = std::make_shared<ImgDMABuf>(in_attrs[0].dims[2], in_attrs[0].dims[1], in_attrs[0].dims[2] * 3,
                                          in_attrs[0].dims[1], V4L2_PIX_FMT_RGB24);
}

int RKNN::infer(std::shared_ptr<ImgDMABuf> imgd) {
    int ret = 0;

    if (imgd->isValid())
        return -1;

    // resize
    RGA::resizeAndCvtColor(imgd, dstImgd);
    dstImgd->syncCpuToDevice();

    // in
    rknn_tensor_mem *rkmem_in = rknn_create_mem_from_fd(ctx, dstImgd->getFd(), dstImgd->getVa(), dstImgd->getSize(), 0);
    in_attrs[0].type = RKNN_TENSOR_UINT8;
    in_attrs[0].fmt = RKNN_TENSOR_NHWC;
    ret = rknn_set_io_mem(ctx, rkmem_in, &in_attrs[0]);
    if (ret < 0) {
        fmt::print("rknn_set_io_mem rkmem_in error: {}\n", ret);
        goto OUT;
    }

    // out
    for (int i = 0; i < io_num.n_output; ++i) {
        out_attrs[i].type = RKNN_TENSOR_INT8;
        ret = rknn_set_io_mem(ctx, rkmem_out[i], &out_attrs[i]);
        if (ret < 0) {
            fmt::print("rknn_set_io_mem rkmem_out error: {}\n", ret);
            goto OUT;
        }
    }

    ret = rknn_run(ctx, NULL);
    if (ret < 0) {
        fmt::print("rknn_run error: {}\n", ret);
        goto OUT;
    }

    // post process
    // ret = post_process((int8_t *)rkmem_out[0]->virt_addr, (int8_t *)rkmem_out[1]->virt_addr,
    //                    (int8_t *)rkmem_out[2]->virt_addr, in_attrs[0].dims[1], in_attrs[0].dims[2], BOX_THRESH,
    //                    NMS_THRESH, {0, 0, 0, 0}, (float)in_attrs[0].dims[2] / imgd->img_get_width(),
    //                    (float)in_attrs[0].dims[1] / imgd->img_get_height(),
    //                    {out_attrs[0].zp, out_attrs[1].zp, out_attrs[2].zp},
    //                    {out_attrs[0].scale, out_attrs[1].scale, out_attrs[2].scale}, &group);

    ret = anime_post_process((int8_t *)rkmem_out[0]->virt_addr, (int8_t *)rkmem_out[1]->virt_addr,
                             (int8_t *)rkmem_out[2]->virt_addr, in_attrs[0].dims[1], in_attrs[0].dims[2], BOX_THRESH,
                             NMS_THRESH, {0, 0, 0, 0}, (float)in_attrs[0].dims[2] / imgd->img_get_width(),
                             (float)in_attrs[0].dims[1] / imgd->img_get_height(),
                             {out_attrs[0].zp, out_attrs[1].zp, out_attrs[2].zp},
                             {out_attrs[0].scale, out_attrs[1].scale, out_attrs[2].scale}, &group);
    if (ret < 0) {
        std::cerr << "post_process error: " << ret << std::endl;
        goto OUT;
    }
    printf("detect result: count=%d\n", group.count);
    for (int n = 0; n < group.count; n++) {
        printf("result %2d: (%4d, %4d, %4d, %4d), %s, prop=%.2f\n", n, group.results[n].box.left,
               group.results[n].box.top, group.results[n].box.right, group.results[n].box.bottom, group.results[n].name,
               group.results[n].prop);
    }

    // rga draw rectangle
    RGA::drawRect(imgd, group);

OUT:
    if (rkmem_in) {
        rknn_destroy_mem(ctx, rkmem_in);
        rkmem_in = NULL;
    }

    return ret;
}

RKNN::~RKNN() {
    for (int i = 0; i < io_num.n_output; ++i) {
        if (rkmem_out[i]) {
            rknn_destroy_mem(ctx, rkmem_out[i]);
            rkmem_out[i] = NULL;
        }
    }

    rknn_destroy(ctx);
}
