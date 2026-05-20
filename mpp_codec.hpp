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
#include <functional>
#include <mutex>
#include <stdexcept>

#include <fmt/core.h>

#define MAX_FILE_NAME_LENGTH 256

static RK_S32 qbias_arr_hevc[18] = {3, 6, 13, 171, 171, 171, 171, 3, 6, 13, 171, 171, 220, 171, 85, 85, 85, 85};

static RK_S32 qbias_arr_avc[18] = {3, 6, 13, 683, 683, 683, 683, 3, 6, 13, 683, 683, 683, 683, 341, 341, 341, 341};

static RK_S32 aq_rnge_arr[10] = {5, 5, 10, 12, 12, 5, 5, 10, 12, 12};

static RK_S32 aq_thd_smart[16] = {1, 3, 3, 3, 3, 3, 5, 5, 8, 8, 8, 15, 15, 20, 25, 28};

static RK_S32 aq_step_smart[16] = {-8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 4, 6, 8, 10};

static RK_S32 aq_thd[16] = {0, 0, 0, 0, 3, 3, 5, 5, 8, 8, 8, 15, 15, 20, 25, 25};

static RK_S32 aq_step_i_ipc[16] = {
    -8, -7, -6, -5, -4, -3, -2, -1, 0, 1, 2, 3, 5, 7, 7, 8,
};

static RK_S32 aq_step_p_ipc[16] = {
    -8, -7, -6, -5, -4, -2, -1, -1, 0, 2, 3, 4, 6, 8, 9, 10,
};

class MppEncoder {
  private:
    MppCtx ctx{NULL};
    MppApi *mpi{NULL};
    MppPollType timeout{MPP_POLL_BLOCK};
    MppCodingType enc_type{MPP_VIDEO_CodingAVC};
    MppEncRcMode rc_mode{MPP_ENC_RC_MODE_CBR};
    MppEncSeiMode sei_mode{MPP_ENC_SEI_MODE_ONE_SEQ};
    MppEncHeaderMode header_mode{MPP_ENC_HEADER_MODE_EACH_IDR};
    MppFrameFormat fmt_type{MPP_FMT_YUV422_YVYU};
    MppEncRcDropFrmMode drop_mode{MPP_ENC_RC_DROP_FRM_NORMAL};
    MppEncCfg cfg{NULL};

    RK_U32 height{1088}, width{1920};
    RK_U32 fps{30};
    RK_U32 bps{height * width / 8 * fps};
    RK_U32 gop{30};

  public:
    MppEncoder() {}

    int init() {
        MPP_RET ret = mpp_create(&ctx, &mpi);
        if (ret) {
            fmt::print("mpp_creat error: {}\n", ret);
            return -1;
        }

        ret = mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout);
        if (MPP_OK != ret) {
            fmt::print("mpi->control MPP_SET_OUTPUT_TIMEOUT error: {}\n", ret);
            return -1;
        }

        ret = mpp_init(ctx, MPP_CTX_ENC, enc_type);
        if (ret) {
            fmt::print("mpp_init error: {}\n", ret);
            return -1;
        }

        // show support
        mpp_show_support_format();
        mpp_show_color_format();

        // cfg
        {
            ret = mpp_enc_cfg_init(&cfg);
            if (ret) {
                fmt::print("mpp_enc_cfg_init error: {}\n", ret);
                return -1;
            }

            ret = mpi->control(ctx, MPP_ENC_GET_CFG, cfg);
            if (ret) {
                fmt::print("mpi->control MPP_ENC_GET_CFG error: {}\n", ret);
            }

            // for AVC && CBR
            // type
            mpp_enc_cfg_set_s32(cfg, "codec:type", enc_type);
            // resolution
            /* setup preprocess parameters */
            mpp_enc_cfg_set_s32(cfg, "prep:width", width);
            mpp_enc_cfg_set_s32(cfg, "prep:height", height);
            mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", width);
            mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", height);
            mpp_enc_cfg_set_s32(cfg, "prep:format", fmt_type);
            // mpp_enc_cfg_set_s32(cfg, "prep:range", MPP_FRAME_RANGE_JPEG);

            // /* setup rate control parameters */
            mpp_enc_cfg_set_s32(cfg, "rc:mode", rc_mode);
            mpp_enc_cfg_set_u32(cfg, "rc:max_reenc_times", 0);
            mpp_enc_cfg_set_u32(cfg, "rc:super_mode", 0);

            // /* drop frame or not when bitrate overflow */
            mpp_enc_cfg_set_u32(cfg, "rc:drop_mode", drop_mode);
            mpp_enc_cfg_set_u32(cfg, "rc:drop_thd", 20); /* 20% of max bps */
            mpp_enc_cfg_set_u32(cfg, "rc:drop_gap", 1);  /* Do not continuous drop frame */

            // /* setup fine tuning paramters */
            // mpp_enc_cfg_set_s32(cfg, "tune:anti_flicker_str", cmd->anti_flicker_str);
            // mpp_enc_cfg_set_s32(cfg, "tune:atf_str", cmd->atf_str);
            // mpp_enc_cfg_set_s32(cfg, "tune:atr_str_i", cmd->atr_str_i);
            // mpp_enc_cfg_set_s32(cfg, "tune:atr_str_p", cmd->atr_str_p);
            // mpp_enc_cfg_set_s32(cfg, "tune:atl_str", cmd->atl_str);
            // mpp_enc_cfg_set_s32(cfg, "tune:deblur_en", cmd->deblur_en);
            // mpp_enc_cfg_set_s32(cfg, "tune:deblur_str", cmd->deblur_str);
            // mpp_enc_cfg_set_s32(cfg, "tune:sao_str_i", cmd->sao_str_i);
            // mpp_enc_cfg_set_s32(cfg, "tune:sao_str_p", cmd->sao_str_p);
            // mpp_enc_cfg_set_s32(cfg, "tune:lambda_idx_p", cmd->lambda_idx_p);
            // mpp_enc_cfg_set_s32(cfg, "tune:lambda_idx_i", cmd->lambda_idx_i);
            // mpp_enc_cfg_set_s32(cfg, "tune:rc_container", cmd->rc_container);
            // mpp_enc_cfg_set_s32(cfg, "tune:scene_mode", cmd->scene_mode);
            // mpp_enc_cfg_set_s32(cfg, "tune:speed", cmd->speed);
            // mpp_enc_cfg_set_s32(cfg, "tune:vmaf_opt", 0);

            // mpp_enc_cfg_set_s32(cfg, "hw:qbias_en", 1);
            // mpp_enc_cfg_set_s32(cfg, "hw:qbias_i", cmd->bias_i);
            // mpp_enc_cfg_set_s32(cfg, "hw:qbias_p", cmd->bias_p);
            // mpp_enc_cfg_set_s32(cfg, "hw:skip_bias_en", 0);
            // mpp_enc_cfg_set_s32(cfg, "hw:skip_bias", 4);
            // mpp_enc_cfg_set_s32(cfg, "hw:skip_sad", 8);

            //  fps
            mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 1);
            mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps);
            mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
            mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 1);
            mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps);
            mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
            // bps
            mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bps);
            /* default use CBR mode */
            mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bps * 17 / 16);
            mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bps * 15 / 16);

            // qp
            mpp_enc_cfg_set_s32(cfg, "rc:qp_init", -1);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_max", 51);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_min", 10);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_max_i", 51);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_min_i", 10);
            mpp_enc_cfg_set_s32(cfg, "rc:qp_ip", 2);
            mpp_enc_cfg_set_s32(cfg, "rc:fqp_min_i", 10);
            mpp_enc_cfg_set_s32(cfg, "rc:fqp_max_i", 45);
            mpp_enc_cfg_set_s32(cfg, "rc:fqp_min_p", 10);
            mpp_enc_cfg_set_s32(cfg, "rc:fqp_max_p", 45);
            // h264
            /*
             * H.264 profile_idc parameter
             * 66  - Baseline profile
             * 77  - Main profile
             * 100 - High profile
             */
            mpp_enc_cfg_set_s32(cfg, "h264:profile", 100);
            /*
             * H.264 level_idc parameter
             * 10 / 11 / 12 / 13    - qcif@15fps / cif@7.5fps / cif@15fps / cif@30fps
             * 20 / 21 / 22         - cif@30fps / half-D1@@25fps / D1@12.5fps
             * 30 / 31 / 32         - D1@25fps / 720p@30fps / 720p@60fps
             * 40 / 41 / 42         - 1080p@30fps / 1080p@30fps / 1080p@60fps
             * 50 / 51 / 52         - 4K@30fps
             */
            mpp_enc_cfg_set_s32(cfg, "h264:level", 40);
            mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
            mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
            mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", 1);
            // gop
            mpp_enc_cfg_set_s32(cfg, "rc:gop", gop);
            // hw specify
            mpp_enc_cfg_set_st(cfg, "hw:aq_thrd_i", aq_thd);
            mpp_enc_cfg_set_st(cfg, "hw:aq_thrd_p", aq_thd);
            mpp_enc_cfg_set_st(cfg, "hw:aq_step_i", aq_step_i_ipc);
            mpp_enc_cfg_set_st(cfg, "hw:aq_step_p", aq_step_p_ipc);
            mpp_enc_cfg_set_st(cfg, "hw:qbias_arr", qbias_arr_avc);

            ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
            if (ret) {
                fmt::print("control MPP_ENC_SET_CFG error: {}\n", ret);
                return -1;
            }

            // rc api
            if (enc_type == MPP_VIDEO_CodingAVC || enc_type == MPP_VIDEO_CodingHEVC) {
                RcApiBrief rc_api_brief;
                rc_api_brief.type = enc_type;
                rc_api_brief.name = "default";

                ret = mpi->control(ctx, MPP_ENC_SET_RC_API_CURRENT, &rc_api_brief);
                if (ret) {
                    fmt::print("control MPP_ENC_SET_RC_API_CURRENT error: {}\n", ret);
                    return -1;
                }
            }

            // sei mode
            ret = mpi->control(ctx, MPP_ENC_SET_SEI_CFG, &sei_mode);
            if (ret) {
                fmt::print("control MPP_ENC_SET_SEI_CFG error: {}\n", ret);
                return -1;
            }

            // header mode
            if (enc_type == MPP_VIDEO_CodingAVC || enc_type == MPP_VIDEO_CodingHEVC) {
                header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
                ret = mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);
                if (ret) {
                    fmt::print("control MPP_ENC_SET_SEI_CFG error: {}\n", ret);
                    return -1;
                }
            }
        }

        return 0;
    }

    ~MppEncoder() {
        if (cfg) {
            MPP_RET ret = mpp_enc_cfg_deinit(cfg);
            if (ret) {
                fmt::print("mpp_enc_cfg_deinit error: {}\n", ret);
            }
        }

        if (ctx) {
            MPP_RET ret = mpi->reset(ctx);
            if (ret) {
                fmt::print("mpi->reset error: {}\n", ret);
            }

            ret = mpp_destroy(ctx);
            if (ret) {
                fmt::print("mpp_destroy error: {}\n", ret);
            }
        }
    }
};