#include "rga.h"
// im draw rect
// {
//
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
RGA::RGA() {

    imconfig(IM_CONFIG_SCHEDULER_CORE, IM_SCHEDULER_RGA3_CORE0);
    std::cout << querystring(RGA_ALL) << std::endl;
}

void RGA::resizeAndCvtColor(std::shared_ptr<ImgDMABuf> imgd_i, std::shared_ptr<ImgDMABuf> imgd_o) {

    rga_buffer_t src;
    rga_buffer_t dst;
    src = wrapbuffer_fd(imgd_i->getFd(), imgd_i->img_get_width(), imgd_i->img_get_height(),
                        v4l2FmtToRga(imgd_i->img_get_fmt()));
    dst = wrapbuffer_fd(imgd_o->getFd(), imgd_o->img_get_width(), imgd_o->img_get_height(),
                        v4l2FmtToRga(imgd_o->img_get_fmt()));

    IM_STATUS STATUS = imresize(src, dst);
    if (IM_STATUS_SUCCESS != STATUS) {
        fmt::print(stderr, "rga resize error! {}", imStrError(STATUS));
    }
}

void RGA::drawRect(std::shared_ptr<ImgDMABuf> imgd, detect_result_group_t group) {

    auto dst =
        wrapbuffer_fd(imgd->getFd(), imgd->img_get_width(), imgd->img_get_height(), v4l2FmtToRga(imgd->img_get_fmt()));

    for (int i = 0; i < group.count; ++i) {

        int thickness = 5;
        im_rect rect{group.results[i].box.left, group.results[i].box.top,
                     group.results[i].box.right - group.results[i].box.left,
                     group.results[i].box.bottom - group.results[i].box.top};

        if (rect.x + rect.width >= imgd->img_get_width())
            rect.width = imgd->img_get_width() - rect.x;
        if (rect.y + rect.height >= imgd->img_get_height())
            rect.height = imgd->img_get_height() - rect.y;

        rect.width &= ~0x1;
        rect.height &= ~0x1;
        rect.x &= ~0x1;
        rect.y &= ~0x1;

        // im_rect rect{11, 10, 190, 108};

        fmt::print("rect: {},{} {}x{}\n", rect.x, rect.y, rect.width, rect.height);

        auto status = imrectangle(dst, rect, 0x00000000ff, 4);
        if (status != IM_STATUS_SUCCESS) {
            fmt::print("imrectangle error, {}\n", status);
            return;
        }
    }
}

RGA::~RGA() {}
