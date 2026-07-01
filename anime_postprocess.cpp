#include "postprocess.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <vector>

namespace {

constexpr int kAnimeClassNum = 1;
constexpr int kAnchorNum = 3;
constexpr int kBoxSize = 5 + kAnimeClassNum;

constexpr int kAnchors0[kAnchorNum * 2] = {10, 13, 16, 30, 33, 23};
constexpr int kAnchors1[kAnchorNum * 2] = {30, 61, 62, 45, 59, 119};
constexpr int kAnchors2[kAnchorNum * 2] = {116, 90, 156, 198, 373, 326};

struct Candidate {
    float x;
    float y;
    float w;
    float h;
    float score;
};

static inline float clampf(float value, float low, float high) {
    return std::max(low, std::min(value, high));
}

static inline float sigmoid(float value) {
    return 1.0f / (1.0f + std::exp(-value));
}

static inline float dequant_i8(int8_t value, int32_t zp, float scale) {
    return (static_cast<float>(value) - static_cast<float>(zp)) * scale;
}

static inline bool looks_like_probability_quant(int32_t zp, float scale) {
    // RKNN models whose outputs are already sigmoid/probability commonly show
    // zp=-128 and scale close to 1/255. Raw-logit outputs usually have a wider
    // scale. This keeps the function compatible with both export styles.
    return zp == -128 && scale > 0.0f && scale <= (1.0f / 128.0f);
}

static inline float decode_value(int8_t value, int32_t zp, float scale, bool already_probability) {
    float decoded = dequant_i8(value, zp, scale);
    return already_probability ? decoded : sigmoid(decoded);
}

static float iou(const Candidate &a, const Candidate &b) {
    float ax0 = a.x;
    float ay0 = a.y;
    float ax1 = a.x + a.w;
    float ay1 = a.y + a.h;
    float bx0 = b.x;
    float by0 = b.y;
    float bx1 = b.x + b.w;
    float by1 = b.y + b.h;

    float inter_w = std::max(0.0f, std::min(ax1, bx1) - std::max(ax0, bx0));
    float inter_h = std::max(0.0f, std::min(ay1, by1) - std::max(ay0, by0));
    float inter = inter_w * inter_h;
    float area_a = std::max(0.0f, a.w) * std::max(0.0f, a.h);
    float area_b = std::max(0.0f, b.w) * std::max(0.0f, b.h);
    float denom = area_a + area_b - inter;
    return denom <= 0.0f ? 0.0f : inter / denom;
}

static void process_output(const int8_t *input, const int *anchors, int grid_h, int grid_w, int stride,
                           float conf_threshold, int32_t zp, float scale, std::vector<Candidate> &candidates) {
    const int grid_len = grid_h * grid_w;
    const bool already_probability = looks_like_probability_quant(zp, scale);

    for (int anchor = 0; anchor < kAnchorNum; ++anchor) {
        const int anchor_offset = kBoxSize * anchor * grid_len;
        for (int y = 0; y < grid_h; ++y) {
            for (int x = 0; x < grid_w; ++x) {
                const int cell = y * grid_w + x;
                const int offset = anchor_offset + cell;

                float obj = decode_value(input[offset + 4 * grid_len], zp, scale, already_probability);
                if (obj < conf_threshold) {
                    continue;
                }

                float cls = decode_value(input[offset + 5 * grid_len], zp, scale, already_probability);
                float score = obj * cls;
                if (score < conf_threshold) {
                    continue;
                }

                float bx = decode_value(input[offset + 0 * grid_len], zp, scale, already_probability);
                float by = decode_value(input[offset + 1 * grid_len], zp, scale, already_probability);
                float bw = decode_value(input[offset + 2 * grid_len], zp, scale, already_probability);
                float bh = decode_value(input[offset + 3 * grid_len], zp, scale, already_probability);

                bx = (bx * 2.0f - 0.5f + static_cast<float>(x)) * static_cast<float>(stride);
                by = (by * 2.0f - 0.5f + static_cast<float>(y)) * static_cast<float>(stride);
                bw = std::pow(bw * 2.0f, 2.0f) * static_cast<float>(anchors[anchor * 2 + 0]);
                bh = std::pow(bh * 2.0f, 2.0f) * static_cast<float>(anchors[anchor * 2 + 1]);

                candidates.push_back({bx - bw * 0.5f, by - bh * 0.5f, bw, bh, score});
            }
        }
    }
}

static std::vector<int> nms(const std::vector<Candidate> &candidates, float nms_threshold) {
    std::vector<int> order(candidates.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&candidates](int lhs, int rhs) {
        return candidates[lhs].score > candidates[rhs].score;
    });

    std::vector<int> keep;
    std::vector<uint8_t> suppressed(candidates.size(), 0);
    for (size_t i = 0; i < order.size(); ++i) {
        int idx = order[i];
        if (suppressed[idx]) {
            continue;
        }
        keep.push_back(idx);
        if (keep.size() >= OBJ_NUMB_MAX_SIZE) {
            break;
        }
        for (size_t j = i + 1; j < order.size(); ++j) {
            int other = order[j];
            if (!suppressed[other] && iou(candidates[idx], candidates[other]) > nms_threshold) {
                suppressed[other] = 1;
            }
        }
    }
    return keep;
}

} // namespace

int anime_post_process(int8_t *input0, int8_t *input1, int8_t *input2, int model_in_h, int model_in_w,
                       float conf_threshold, float nms_threshold, BOX_RECT pads, float scale_w, float scale_h,
                       const std::vector<int32_t> &qnt_zps, const std::vector<float> &qnt_scales,
                       detect_result_group_t *group) {
    if (group == nullptr || qnt_zps.size() < 3 || qnt_scales.size() < 3) {
        return -1;
    }

    std::memset(group, 0, sizeof(detect_result_group_t));

    std::vector<Candidate> candidates;
    process_output(input0, kAnchors0, model_in_h / 8, model_in_w / 8, 8, conf_threshold, qnt_zps[0], qnt_scales[0],
                   candidates);
    process_output(input1, kAnchors1, model_in_h / 16, model_in_w / 16, 16, conf_threshold, qnt_zps[1], qnt_scales[1],
                   candidates);
    process_output(input2, kAnchors2, model_in_h / 32, model_in_w / 32, 32, conf_threshold, qnt_zps[2], qnt_scales[2],
                   candidates);

    if (candidates.empty()) {
        return 0;
    }

    std::vector<int> keep = nms(candidates, nms_threshold);
    int count = 0;
    for (int idx : keep) {
        const Candidate &box = candidates[idx];

        float x0 = box.x - static_cast<float>(pads.left);
        float y0 = box.y - static_cast<float>(pads.top);
        float x1 = x0 + box.w;
        float y1 = y0 + box.h;

        detect_result_t &result = group->results[count];
        result.box.left = static_cast<int>(clampf(x0, 0.0f, static_cast<float>(model_in_w)) / scale_w);
        result.box.top = static_cast<int>(clampf(y0, 0.0f, static_cast<float>(model_in_h)) / scale_h);
        result.box.right = static_cast<int>(clampf(x1, 0.0f, static_cast<float>(model_in_w)) / scale_w);
        result.box.bottom = static_cast<int>(clampf(y1, 0.0f, static_cast<float>(model_in_h)) / scale_h);
        result.prop = box.score;
        std::strncpy(result.name, "face", OBJ_NAME_MAX_SIZE - 1);
        result.name[OBJ_NAME_MAX_SIZE - 1] = '\0';

        ++count;
    }

    group->count = count;
    return 0;
}

