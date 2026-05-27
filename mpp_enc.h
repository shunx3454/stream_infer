#pragma once

#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_rc_api.h>
#include <rockchip/mpp_task.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_type.h>
#include <rockchip/rk_venc_cfg.h>
#include <rockchip/rk_venc_cmd.h>

#include <cstddef>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>

#include <fmt/core.h>

#include "dmabuf.h"

// 定义回调函数的别名
using EncodedFrameWriter = std::function<void(const void *ptr,    // 编码后的 H264/H265 数据指针
                                              size_t len,         // 数据长度
                                              RK_U32 is_keyframe, // 是否为关键帧 (I帧)
                                              RK_U32 eos          // 是否为最后一帧 (End of Stream)
                                              )>;

class MppEncoder {
  private:
    MppCtx ctx{NULL};
    MppApi *mpi{NULL};
    MppPollType timeout{MPP_POLL_BLOCK};
    MppCodingType enc_type{MPP_VIDEO_CodingAVC};
    MppEncRcMode rc_mode{MPP_ENC_RC_MODE_VBR};
    MppEncSeiMode sei_mode{MPP_ENC_SEI_MODE_ONE_SEQ};
    MppEncHeaderMode header_mode{MPP_ENC_HEADER_MODE_EACH_IDR};
    MppFrameFormat fmt_type{MPP_FMT_YUV422_UYVY};
    MppEncRcDropFrmMode drop_mode{MPP_ENC_RC_DROP_FRM_NORMAL};
    MppEncCfg cfg{NULL};

    MppBuffer frm_buf{NULL};
    MppBuffer pkt_buf{NULL};
    MppBuffer hdr_buf{NULL};

    RK_U32 height{1088}, width{1920}; // UYVY : 1920 * 2 (UYVY) * 1088
    RK_U32 hor_stride{width * 2}, ver_stride{height};
    RK_U32 fps{30};
    RK_U32 bps{height * width / 8 * fps};
    RK_U32 gop{30};

    // io
    std::ofstream of{"/home/rock/c_cpp/stream_infer/asset/out.h264",
                     std::ios::binary | std::ios::out | std::ios::trunc};

  public:
    MppEncoder() {}

    int init();
    int encode(DmaBuf &frm_dbuf, DmaBuf &pkt_dbuf, RK_U32 iskeyFrame, RK_U32 eos, const EncodedFrameWriter &writer);
    std::vector<uint8_t> getHdr();

    ~MppEncoder();
};