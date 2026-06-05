#include <thread>

#include "mpp_enc.h"

#define MPP_ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
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

static RK_S32 get_mdinfo_size(RK_U32 w, RK_U32 h, MppCodingType type) {
    RK_S32 md_size;

    // if (soc_type == ROCKCHIP_SOC_RV1126B) {
    //     md_size = (MPP_VIDEO_CodingHEVC == type) ? (MPP_ALIGN(w, 32) >> 5) * (MPP_ALIGN(h, 32) >> 5) * 20
    //                                              : (MPP_ALIGN(w, 64) >> 6) * (MPP_ALIGN(h, 16) >> 4) * 16;
    // } else if (soc_type == ROCKCHIP_SOC_RK3588) {
    //     md_size = (MPP_ALIGN(w, 64) >> 6) * (MPP_ALIGN(h, 64) >> 6) * 32;
    // } else {
    //     md_size = (MPP_VIDEO_CodingHEVC == type) ? (MPP_ALIGN(w, 32) >> 5) * (MPP_ALIGN(h, 32) >> 5) * 16
    //                                              : (MPP_ALIGN(w, 64) >> 6) * (MPP_ALIGN(h, 16) >> 4) * 16;
    // }

    return (MPP_ALIGN(w, 64) >> 6) * (MPP_ALIGN(h, 64) >> 6) * 32;
}

MppEncoder::MppEncoder(RK_U32 w, RK_U32 h, MppFrameFormat pixfmt, RK_U32 w_stride, RK_U32 h_stride, RK_U32 fps,
                       RK_U32 gop)
    : width_(w), height_(h), pixfmt_(pixfmt), hor_stride_(w_stride), ver_stride_(h_stride), fps_(fps), gop_(gop),
      bps_(width_ * height_ / 8 * fps_) {}

MppEncoder::~MppEncoder() {
    for (auto &e : MppBufferMap) {
        if (e.second != NULL) {
            auto ret = mpp_buffer_put(e.second);
            if (ret != MPP_OK) {
                fmt::print("mpp_buffer_put error: {}\n", ret);
            }
        }
    }

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

int MppEncoder::init() {
    MppBufferInfo info{};

    if (!of.is_open()) {
        fmt::print("of file open error\n");
        return -1;
    }

    MPP_RET ret = mpp_create(&ctx, &mpi);
    if (ret) {
        fmt::print("mpp_creat error: {}\n", ret);
        goto INIT_FAIL;
    }

    // ret = mpi->control(ctx, MPP_SET_OUTPUT_TIMEOUT, &timeout);
    // if (MPP_OK != ret) {
    //     fmt::print("mpi->control MPP_SET_OUTPUT_TIMEOUT error: {}\n", ret);
    //     goto INIT_FAIL;
    // }

    ret = mpp_init(ctx, MPP_CTX_ENC, enc_type);
    if (ret) {
        fmt::print("mpp_init error: {}\n", ret);
        goto INIT_FAIL;
    }

    // show support
    mpp_show_support_format();
    mpp_show_color_format();

    // cfg
    {
        ret = mpp_enc_cfg_init(&cfg);
        if (ret) {
            fmt::print("mpp_enc_cfg_init error: {}\n", ret);
            goto INIT_FAIL;
        }

        ret = mpi->control(ctx, MPP_ENC_GET_CFG, cfg);
        if (ret) {
            fmt::print("mpi->control MPP_ENC_GET_CFG error: {}\n", ret);
            goto INIT_FAIL;
        }

        // for AVC && CBR
        // type
        mpp_enc_cfg_set_s32(cfg, "codec:type", enc_type);
        // resolution
        /* setup preprocess parameters */
        mpp_enc_cfg_set_s32(cfg, "prep:width", width_);
        mpp_enc_cfg_set_s32(cfg, "prep:height", height_);
        mpp_enc_cfg_set_s32(cfg, "prep:hor_stride", hor_stride_);
        mpp_enc_cfg_set_s32(cfg, "prep:ver_stride", ver_stride_);
        mpp_enc_cfg_set_s32(cfg, "prep:format", pixfmt_);
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
        mpp_enc_cfg_set_s32(cfg, "rc:fps_in_flex", 0);
        mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num", fps_);
        mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denom", 1);
        mpp_enc_cfg_set_s32(cfg, "rc:fps_out_flex", 0);
        mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num", fps_);
        mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denom", 1);
        // bps
        mpp_enc_cfg_set_s32(cfg, "rc:bps_target", bps_);
        /* default use VBR mode */
        mpp_enc_cfg_set_s32(cfg, "rc:bps_max", bps_ * 17 / 16);
        mpp_enc_cfg_set_s32(cfg, "rc:bps_min", bps_ / 16);
        // gop
        mpp_enc_cfg_set_s32(cfg, "rc:gop", gop_);

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
        mpp_enc_cfg_set_s32(cfg, "h264:level", 30);
        mpp_enc_cfg_set_s32(cfg, "h264:cabac_en", 1);
        mpp_enc_cfg_set_s32(cfg, "h264:cabac_idc", 0);
        mpp_enc_cfg_set_s32(cfg, "h264:trans8x8", 1);

        // hw specify
        mpp_enc_cfg_set_st(cfg, "hw:aq_thrd_i", aq_thd);
        mpp_enc_cfg_set_st(cfg, "hw:aq_thrd_p", aq_thd);
        mpp_enc_cfg_set_st(cfg, "hw:aq_step_i", aq_step_i_ipc);
        mpp_enc_cfg_set_st(cfg, "hw:aq_step_p", aq_step_p_ipc);
        mpp_enc_cfg_set_st(cfg, "hw:qbias_arr", qbias_arr_avc);

        ret = mpi->control(ctx, MPP_ENC_SET_CFG, cfg);
        if (ret != MPP_OK) {
            fmt::print("control MPP_ENC_SET_CFG error: {}\n", ret);
            goto INIT_FAIL;
        }

        // rc api
        if (enc_type == MPP_VIDEO_CodingAVC || enc_type == MPP_VIDEO_CodingHEVC) {
            RcApiBrief rc_api_brief;
            rc_api_brief.type = enc_type;
            rc_api_brief.name = "default";

            ret = mpi->control(ctx, MPP_ENC_SET_RC_API_CURRENT, &rc_api_brief);
            if (ret) {
                fmt::print("control MPP_ENC_SET_RC_API_CURRENT error: {}\n", ret);
                goto INIT_FAIL;
            }
        }

        // sei mode
        ret = mpi->control(ctx, MPP_ENC_SET_SEI_CFG, &sei_mode);
        if (ret) {
            fmt::print("control MPP_ENC_SET_SEI_CFG error: {}\n", ret);
            goto INIT_FAIL;
        }

        // header mode
        if (enc_type == MPP_VIDEO_CodingAVC || enc_type == MPP_VIDEO_CodingHEVC) {
            header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
            ret = mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);
            if (ret) {
                fmt::print("control MPP_ENC_SET_SEI_CFG error: {}\n", ret);
                goto INIT_FAIL;
            }
        }
    }

    return 0;

INIT_FAIL:
    if (cfg) {
        mpp_enc_cfg_deinit(cfg);
        cfg = NULL;
    }

    if (ctx) {
        mpp_destroy(ctx);
        ctx = NULL;
    }

    return -1;
}

std::vector<uint8_t> MppEncoder::getHdr() {
    std::vector<u_char> hdr{};
    MPP_RET ret{MPP_OK};
    // Get HDR
    auto uptr = std::make_unique<std::array<char, 512>>();
    MppPacket hdr_pkt{NULL};
    ret = mpp_packet_init(&hdr_pkt, uptr.get()->data(), 512);
    if (ret != MPP_OK) {
        fmt::print("mpp_packet_init error {}\n", ret);
        return hdr;
    }

    mpp_packet_set_length(hdr_pkt, 0);

    ret = mpi->control(ctx, MPP_ENC_GET_HDR_SYNC, hdr_pkt);
    if (ret) {
        fmt::print("mpi control enc get extra info failed\n");
        goto OUT;
    } else {
        /* get and write sps/pps for H.264 */

        void *ptr = mpp_packet_get_pos(hdr_pkt);
        size_t len = mpp_packet_get_length(hdr_pkt);
        fmt::print("Get Hdr pkt: va={}, len={}\n", ptr, len);

        auto ps = of.tellp();
        of.write((const char *)ptr, len);
        auto pe = of.tellp();
        fmt::print("write {} bytes HDR\n", pe - ps);

        hdr = std::vector<u_char>(len);
        memcpy(hdr.data(), ptr, len);
    }

OUT:
    if (hdr_pkt)
        mpp_packet_deinit(&hdr_pkt);

    return hdr;
}

int MppEncoder::encode(std::shared_ptr<ImgDMABuf> frm_dbuf, std::shared_ptr<ImgDMABuf> pkt_dbuf, RK_U32 iskeyFrame,
                       RK_U32 eos) {
    return this->encode(frm_dbuf, pkt_dbuf, iskeyFrame, eos,
                        [this](const void *ptr, size_t len, RK_U32 iskey, RK_U32 eos) {
                            of.write((const char *)ptr, len);
                            if (eos)
                                of.close();
                        });
}

int MppEncoder::encode(std::shared_ptr<ImgDMABuf> frm_dbuf, std::shared_ptr<ImgDMABuf> pkt_dbuf, RK_U32 iskeyFrame,
                       RK_U32 eos, const EncodedFrameWriter &writer) {
    MPP_RET ret{MPP_OK};
    MppBufferInfo info;
    MppMeta meta{NULL};

    MppBuffer frm_buf;
    MppBuffer pkt_buf;
    MppFrame frm{NULL};
    MppPacket pkt{NULL};
    int pkt_len = -1;

    if (frm_dbuf) {
        auto it = MppBufferMap.find(frm_dbuf->getFd());
        if (it == MppBufferMap.end()) {
            fmt::print("raw frm_dbuf: fd={}, addr={}, size={:#x}\n", frm_dbuf->getFd(), frm_dbuf->getVa(),
                       frm_dbuf->getSize());

            // wrap frm_dbuf -> frm_buf
            memset(&info, 0, sizeof(MppBufferInfo));
            info.index = n_buffers++;
            info.fd = frm_dbuf->getFd();
            info.size = frm_dbuf->getSize();
            info.type = (MppBufferType)(MPP_BUFFER_TYPE_EXT_DMA | MPP_BUFFER_FLAGS_CONTIG  | MPP_BUFFER_FLAGS_CACHABLE);
            ret = mpp_buffer_import(&frm_buf, &info);
            if (ret != MPP_OK) {
                fmt::print("mpp_buffer_import error {}\n", ret);
                goto OUT;
            }

            memset(&info, 0, sizeof(MppBufferInfo));
            ret = mpp_buffer_info_get(frm_buf, &info);
            if (ret != MPP_OK) {
                fmt::print("mpp_buffer_info_get error {}\n", ret);
                goto OUT;
            }
            fmt::print("mpp frm buf: index={}, fd={}, vaddr={}, size={:#x}, type={:#x}\n", info.index, info.fd,
                       info.ptr, info.size, info.type);

            MppBufferMap.emplace(info.fd, frm_buf);
        } else {
            frm_buf = it->second;
        }
    } else {
        fmt::print("frm_dbuf is NULL\n");
        goto OUT;
    }

    if (pkt_dbuf) {
        auto it = MppBufferMap.find(pkt_dbuf->getFd());
        if (it == MppBufferMap.end()) {
            fmt::print("raw pkt_dbuf: fd={}, addr={}, size={:#x}\n", pkt_dbuf->getFd(), pkt_dbuf->getVa(),
                       pkt_dbuf->getSize());

            // wrap pkt_dbuf -> pkt_buf
            memset(&info, 0, sizeof(MppBufferInfo));
            info.index = n_buffers++;
            info.fd = pkt_dbuf->getFd();
            info.size = pkt_dbuf->getSize();
            info.type = (MppBufferType)(MPP_BUFFER_TYPE_EXT_DMA | MPP_BUFFER_FLAGS_CONTIG  | MPP_BUFFER_FLAGS_CACHABLE);
            ret = mpp_buffer_import(&pkt_buf, &info);
            if (ret != MPP_OK) {
                fmt::print("mpp_buffer_import error {}\n", ret);
                return -1;
            }

            memset(&info, 0, sizeof(MppBufferInfo));
            ret = mpp_buffer_info_get(pkt_buf, &info);
            if (ret != MPP_OK) {
                fmt::print("mpp_buffer_info_get error {}\n", ret);
                goto OUT;
            }
            fmt::print("mpp pkt buf: index={}, fd={}, vaddr={}, size={:#x}, type={:#x}\n", info.index, info.fd,
                       info.ptr, info.size, info.type);

            MppBufferMap.emplace(info.fd, pkt_buf);
        } else {
            pkt_buf = it->second;
        }
    } else {
        fmt::print("frm_dbuf is NULL\n");
        goto OUT;
    }

    // frame <- buffer
    ret = mpp_frame_init(&frm);
    if (ret != MPP_OK) {
        fmt::print("mpp_frame_init error {}\n", ret);
        goto OUT;
    }

    mpp_frame_set_width(frm, width_);
    mpp_frame_set_height(frm, height_);
    mpp_frame_set_hor_stride(frm, hor_stride_);
    mpp_frame_set_ver_stride(frm, ver_stride_);
    mpp_frame_set_fmt(frm, pixfmt_);
    mpp_frame_set_eos(frm, eos);
    // link frm_buf
    mpp_frame_set_buffer(frm, frm_buf);

    // pkt init
    ret = mpp_packet_init_with_buffer(&pkt, pkt_buf);
    if (ret != MPP_OK) {
        fmt::print("mpp_packet_init_with_buffer error {}\n", ret);
        goto OUT;
    }
    /* NOTE: It is important to clear output packet length!! */
    mpp_packet_set_length(pkt, 0);

    // link frm and pkt
    meta = mpp_frame_get_meta(frm);
    ret = mpp_meta_set_packet(meta, KEY_OUTPUT_PACKET, pkt);
    if (ret != MPP_OK) {
        fmt::print("KEY_OUTPUT_PACKET error {}\n", ret);
        goto OUT;
    }

    ret = mpi->encode_put_frame(ctx, frm);
    if (ret != MPP_OK) {
        fmt::print("encode_put_frame error {}\n", ret);
        goto OUT;
    }

    ret = mpi->encode_get_packet(ctx, &pkt);
    if (ret != MPP_OK) {
        fmt::print("encode_get_packet error {}\n", ret);
        goto OUT;
    }

    if (pkt) {
        // write packet to file here
        void *ptr = mpp_packet_get_pos(pkt);
        size_t len = mpp_packet_get_length(pkt);
        pkt_len = len;

        eos = mpp_packet_get_eos(pkt);

        pkt_dbuf->syncDeviceToCpu();
        writer(ptr, len, iskeyFrame, eos);
    }

OUT:
    if (frm) {
        ret = mpp_frame_deinit(&frm);
        if (ret != MPP_OK) {
            fmt::print("mpp_frame_deinit error {}\n", ret);
        }
    }

    if (pkt) {
        ret = mpp_packet_deinit(&pkt);
        if (ret != MPP_OK) {
            fmt::print("mpp_packet_deinit error {}\n", ret);
        }
    }

    return pkt_len;
}
