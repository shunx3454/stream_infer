#pragma once

#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <fmt/core.h>
#include <rknn_api.h>

#include "img_dmabuf.h"
#include "postprocess.h"
#include "rga.h"

class RKNN {
  private:
    rknn_context ctx;
    rknn_sdk_version rknn_ver;
    rknn_input_output_num io_num;
    std::vector<rknn_tensor_attr> in_attrs;
    std::vector<rknn_tensor_attr> out_attrs;
    std::vector<rknn_tensor_mem *> rkmem_out;

    detect_result_group_t group;

    RGA rga;
    std::shared_ptr<ImgDMABuf> dstImgd;

  public:
    RKNN(std::string model_name, rknn_core_mask cm = RKNN_NPU_CORE_0);

    int infer(std::shared_ptr<ImgDMABuf> imgd);

    ~RKNN();
};
