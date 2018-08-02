#include "BVHSplit.h"

#include <algorithm>

namespace ray {
const float SAHOversplitThreshold = 0.95f;
const float NodeTraversalCost = 2.0f;

const float SpatialSplitAlpha = 0.00001f;
const int NumSpatialSplitBins = 256;

struct bbox_t {
    ref::simd_fvec3 min = { std::numeric_limits<float>::max() },
                    max = { std::numeric_limits<float>::lowest() };
    bbox_t() {}
    bbox_t(const ref::simd_fvec3 &_min, const ref::simd_fvec3 &_max) : min(_min), max(_max) {}

    float surface_area() const {
        return surface_area(min, max);
    }

    static float surface_area(const ref::simd_fvec3 &min, const ref::simd_fvec3 &max) {
        ref::simd_fvec3 d = max - min;
        return 2 * (d[0] + d[1] + d[2]);
        //return d[0] * d[1] + d[0] * d[2] + d[1] * d[2];
    }
};

// stolen from Mitsuba
static int sutherland_hodgman(const ref::simd_dvec3 *input, int in_count, ref::simd_dvec3 *output, int axis, double split_pos, bool is_minimum) {
    if (in_count < 3)
        return 0;

    ref::simd_dvec3 cur = input[0];
    double sign = is_minimum ? 1.0 : -1.0;
    double distance = sign * (cur[axis] - split_pos);
    bool cur_is_inside = (distance >= 0);
    int out_count = 0;

    for (int i = 0; i < in_count; ++i) {
        int nextIdx = i + 1;
        if (nextIdx == in_count)
            nextIdx = 0;
        ref::simd_dvec3 next = input[nextIdx];
        distance = sign * (next[axis] - split_pos);
        bool next_is_inside = (distance >= 0);

        if (cur_is_inside && next_is_inside) {
            // Both this and the next vertex are inside, add to the list
            output[out_count++] = next;
        } else if (cur_is_inside && !next_is_inside) {
            // Going outside -- add the intersection
            double t = (split_pos - cur[axis]) / (next[axis] - cur[axis]);
            ref::simd_dvec3 p = cur + (next - cur) * t;
            p[axis] = split_pos; // Avoid roundoff errors
            output[out_count++] = p;
        } else if (!cur_is_inside && next_is_inside) {
            // Coming back inside -- add the intersection + next vertex
            double t = (split_pos - cur[axis]) / (next[axis] - cur[axis]);
            ref::simd_dvec3 p = cur + (next - cur) * t;
            p[axis] = split_pos; // Avoid roundoff errors
            output[out_count++] = p;
            output[out_count++] = next;
        } else {
            // Entirely outside - do not add anything
        }
        cur = next;
        cur_is_inside = next_is_inside;
    }
    return out_count;
}

force_inline float castflt_down(double val) {
    int32_t b;
    float a = (float)val;

    memcpy(&b, &a, sizeof(float));

    if ((double)a > val)
        b += a > 0 ? -1 : 1;

    memcpy(&a, &b, sizeof(float));

    return a;
}

force_inline float castflt_up(double val) {
    int32_t b;
    float a = (float)val;

    memcpy(&b, &a, sizeof(float));

    if ((double)a < val)
        b += a < 0 ? -1 : 1;

    memcpy(&a, &b, sizeof(float));

    return a;
}

bbox_t GetClippedAABB(const ref::simd_fvec3 &_v0, const ref::simd_fvec3 &_v1, const ref::simd_fvec3 &_v2, const bbox_t &limits) {
    ref::simd_dvec3 vertices1[9], vertices2[9];
    int vertex_count = 3;

    vertices1[0] = { (double)_v0[0], (double)_v0[1], (double)_v0[2] };
    vertices1[1] = { (double)_v1[0], (double)_v1[1], (double)_v1[2] };
    vertices1[2] = { (double)_v2[0], (double)_v2[1], (double)_v2[2] };

    for (int axis = 0; axis < 3; axis++) {
        vertex_count = sutherland_hodgman(vertices1, vertex_count, vertices2, axis, limits.min[axis], true);
        vertex_count = sutherland_hodgman(vertices2, vertex_count, vertices1, axis, limits.max[axis], false);
    }

    bbox_t extends;

    for (int i = 0; i < vertex_count; ++i) {
        for (int j = 0; j < 3; ++j) {
            double pos = vertices1[i][j];
            extends.min[j] = std::min(extends.min[j], castflt_down(pos));
            extends.max[j] = std::max(extends.max[j], castflt_up(pos));
        }
    }

    return extends;
}
}

ray::split_data_t ray::SplitPrimitives_SAH(const prim_t *primitives, const std::vector<uint32_t> &tri_indices, const float *positions, size_t stride,
                                           const ref::simd_fvec3 &bbox_min, const ref::simd_fvec3 &bbox_max,
                                           const ref::simd_fvec3 &root_min, const ref::simd_fvec3 &root_max, bool use_spatial_splits) {
    size_t num_tris = tri_indices.size();
    bbox_t whole_box = { bbox_min, bbox_max };

    std::vector<uint32_t> axis_lists[3];
    for (int axis = 0; axis < 3; axis++) {
        axis_lists[axis].reserve(num_tris);
    }

    for (uint32_t i = 0; i < (uint32_t)num_tris; i++) {
        axis_lists[0].push_back(i);
        axis_lists[1].push_back(i);
        axis_lists[2].push_back(i);
    }

    std::vector<bbox_t> new_prim_bounds;

    if (true && use_spatial_splits && positions) {
        new_prim_bounds.resize(num_tris);

        for (size_t i = 0; i < tri_indices.size(); i++) {
            const auto &p = primitives[tri_indices[i]];

            ref::simd_fvec3 v0 = { &positions[p.i0 * stride] },
                            v1 = { &positions[p.i1 * stride] },
                            v2 = { &positions[p.i2 * stride] };

            new_prim_bounds[i] = GetClippedAABB(v0, v1, v2, whole_box);
        }
    }

    std::vector<bbox_t> right_bounds(num_tris);

    float res_sah = SAHOversplitThreshold * whole_box.surface_area() * num_tris;
    int div_axis = -1;
    uint32_t div_index = 0;
    bbox_t res_left_bounds, res_right_bounds;

    for (int axis = 0; axis < 3; axis++) {
        auto &list = axis_lists[axis];

        if (new_prim_bounds.empty()) {
            std::sort(list.begin(), list.end(),
                [axis, primitives, &tri_indices](uint32_t p1, uint32_t p2) -> bool {
                return primitives[tri_indices[p1]].bbox_max[axis] < primitives[tri_indices[p2]].bbox_max[axis];
            });
        } else {
            std::sort(list.begin(), list.end(),
                [axis, &new_prim_bounds](uint32_t p1, uint32_t p2) -> bool {
                return new_prim_bounds[p1].max[axis] < new_prim_bounds[p2].max[axis];
            });
        }

        bbox_t cur_right_bounds;
        if (new_prim_bounds.empty()) {
            for (size_t i = list.size() - 1; i > 0; i--) {
                cur_right_bounds.min = min(cur_right_bounds.min, primitives[tri_indices[list[i]]].bbox_min);
                cur_right_bounds.max = max(cur_right_bounds.max, primitives[tri_indices[list[i]]].bbox_max);
                right_bounds[i - 1] = cur_right_bounds;
            }
        } else {
            for (size_t i = list.size() - 1; i > 0; i--) {
                cur_right_bounds.min = min(cur_right_bounds.min, new_prim_bounds[list[i]].min);
                cur_right_bounds.max = max(cur_right_bounds.max, new_prim_bounds[list[i]].max);
                right_bounds[i - 1] = cur_right_bounds;
            }
        }

        bbox_t left_bounds;
        for (size_t i = 1; i < list.size(); i++) {
            if (new_prim_bounds.empty()) {
                left_bounds.min = min(left_bounds.min, primitives[tri_indices[list[i - 1]]].bbox_min);
                left_bounds.max = max(left_bounds.max, primitives[tri_indices[list[i - 1]]].bbox_max);
            } else {
                left_bounds.min = min(left_bounds.min, new_prim_bounds[list[i - 1]].min);
                left_bounds.max = max(left_bounds.max, new_prim_bounds[list[i - 1]].max);
            }

            float sah = NodeTraversalCost + left_bounds.surface_area() * i + right_bounds[i - 1].surface_area() * (list.size() - i);
            if (sah < res_sah) {
                res_sah = sah;
                div_axis = axis;
                div_index = (uint32_t)i;
                res_left_bounds = left_bounds;
                res_right_bounds = right_bounds[i - 1];
            }
        }
    }

    bbox_t overlap = { max(res_left_bounds.min, res_right_bounds.min),
                       min(res_left_bounds.max, res_right_bounds.max) };

    if (false && use_spatial_splits && (overlap.max <= overlap.min).all_zeros() &&
        overlap.surface_area() > SpatialSplitAlpha * bbox_t::surface_area(root_min, root_max)) {
        struct bin_t {
            bbox_t extends, limits;
            uint32_t enter_counter = 0, exit_counter = 0, prim_counter = 0;
        };

        int spatial_split = -1;

        for (int split_axis = 0; split_axis < 3; split_axis++) {
            bin_t bins[NumSpatialSplitBins];
            double bin_size = double(whole_box.max[split_axis] - whole_box.min[split_axis]) / NumSpatialSplitBins;
            
            // skip this axis if bbox is flat
            if (bin_size < FLT_EPS) continue;

            for (int i = 0; i < NumSpatialSplitBins; i++) {
                bins[i].limits.min = whole_box.min;
                bins[i].limits.max = whole_box.max;
                bins[i].limits.min[split_axis] = castflt_down(whole_box.min[split_axis] + i * bin_size);
                bins[i].limits.max[split_axis] = castflt_up(whole_box.min[split_axis] + (i + 1) * bin_size);
            }

            bins[NumSpatialSplitBins - 1].limits.max[split_axis] = whole_box.max[split_axis];

            const auto &list = axis_lists[split_axis];

            for (const auto i : list) {
                const auto &p = primitives[tri_indices[i]];

                float prim_min = p.bbox_min[split_axis],
                      prim_max = p.bbox_max[split_axis];

                if (!new_prim_bounds.empty()) {
                    prim_min = new_prim_bounds[i].min[split_axis];
                    prim_max = new_prim_bounds[i].max[split_axis];
                }

                int enter_index, exit_index;

                for (int j = 0; j < NumSpatialSplitBins; j++) {
                    if (prim_min >= bins[j].limits.min[split_axis]) {
                        enter_index = j;
                    } else {
                        break;
                    }
                }

                for (int j = enter_index; j < NumSpatialSplitBins; j++) {
                    if (prim_max >= bins[j].limits.min[split_axis]) {
                        exit_index = j;
                    } else {
                        break;
                    }
                }

                bins[enter_index].enter_counter++;
                bins[exit_index].exit_counter++;

                if (positions) {
                    ref::simd_fvec3 v0 = { &positions[p.i0 * stride] },
                                    v1 = { &positions[p.i1 * stride] },
                                    v2 = { &positions[p.i2 * stride] };

                    for (int j = enter_index; j <= exit_index; j++) {
                        bbox_t box = GetClippedAABB(v0, v1, v2, bins[j].limits);

                        bins[j].extends.min = min(bins[j].extends.min, box.min);
                        bins[j].extends.max = max(bins[j].extends.max, box.max);
                        bins[j].prim_counter++;
                    }
                } else {
                    for (int j = enter_index; j <= exit_index; j++) {
                        bins[j].extends.min = min(bins[j].extends.min, p.bbox_min);
                        bins[j].extends.max = max(bins[j].extends.max, p.bbox_max);
                        bins[j].prim_counter++;
                    }
                }
            }

            for (int i = 0; i < NumSpatialSplitBins; i++) {
                if (!bins[i].prim_counter) {
                    bins[i].extends = bins[i].limits;
                } else {
                    bins[i].extends.min = max(bins[i].extends.min, bins[i].limits.min);
                    bins[i].extends.max = min(bins[i].extends.max, bins[i].limits.max);
                }
            }

            for (int split = 1; split < NumSpatialSplitBins; split++) {
                bbox_t ext_left, ext_right;
                int num_left = 0, num_right = 0;
                for (int i = 0; i < split; i++) {
                    if (!bins[i].prim_counter) continue;

                    ext_left.min = min(ext_left.min, bins[i].extends.min);
                    ext_left.max = max(ext_left.max, bins[i].extends.max);
                    num_left += bins[i].enter_counter;
                }
                for (int i = split; i < NumSpatialSplitBins; i++) {
                    if (!bins[i].prim_counter) continue;

                    ext_right.min = min(ext_right.min, bins[i].extends.min);
                    ext_right.max = max(ext_right.max, bins[i].extends.max);
                    num_right += bins[i].exit_counter;
                }

                float split_sah = NodeTraversalCost + ext_left.surface_area() * num_left + ext_right.surface_area() * num_right;
                if (split_sah < res_sah) {
                    //__debugbreak();
                    res_sah = split_sah;
                    spatial_split = split;
                    div_axis = split_axis;

                    if (!num_left || !num_right) {
                        __debugbreak();
                    }

                    res_left_bounds = ext_left;
                    res_right_bounds = ext_right;

                    //printf("r l: %i %i\n", num_left, num_right);
                }
            }
        }

        if (spatial_split != -1) {
            std::vector<uint32_t> left_indices, right_indices;

            bbox_t over = { max(res_left_bounds.min, res_right_bounds.min),
                            min(res_left_bounds.max, res_right_bounds.max) };

            if ((over.min < over.max).not_all_zeros()) {
                if (over.surface_area() > 1.0f) {
                    //__debugbreak();
                }
            }

            /*res_left_bounds.min = max(res_left_bounds.min, whole_box.min);
            res_left_bounds.max = min(res_left_bounds.max, whole_box.max);
            res_right_bounds.min = max(res_right_bounds.min, whole_box.min);
            res_right_bounds.max = min(res_right_bounds.max, whole_box.max);*/

            int num_in_both = 0;

            const auto &list = axis_lists[div_axis];
            for (const auto i : list) {
                const auto &p = primitives[tri_indices[i]];

                bool b1 = false, b2 = false;
                if (new_prim_bounds[i].min[div_axis] <= res_left_bounds.max[div_axis]) {
                    left_indices.push_back(tri_indices[i]);
                    b1 = true;
                }

                if (new_prim_bounds[i].max[div_axis] >= res_right_bounds.min[div_axis]) {
                    right_indices.push_back(tri_indices[i]);
                    b2 = true;
                }

                if (b1 && b2) {
                    num_in_both++;
                }
            }

            printf("Spatial split: %i %i %i\n", (int)left_indices.size(), (int)right_indices.size(), num_in_both);
            printf("Extends: %f..%f %f..%f\n",
                res_left_bounds.min[div_axis], res_left_bounds.max[div_axis],
                res_right_bounds.min[div_axis], res_right_bounds.max[div_axis]);

            return{ std::move(left_indices), std::move(right_indices), { res_left_bounds.min, res_left_bounds.max }, { res_right_bounds.min, res_right_bounds.max } };
        } else {
            volatile int i = 0;
        }
    }

    std::vector<uint32_t> left_indices, right_indices;
    if (div_axis != -1) {
        left_indices.reserve((size_t)div_index);
        right_indices.reserve(tri_indices.size() - div_index);
        for (size_t i = 0; i < div_index; i++) {
            left_indices.push_back(tri_indices[axis_lists[div_axis][i]]);
        }
        for (size_t i = div_index; i < axis_lists[div_axis].size(); i++) {
            right_indices.push_back(tri_indices[axis_lists[div_axis][i]]);
        }
    } else {
        left_indices = tri_indices;
        res_left_bounds = whole_box;
    }

    return { std::move(left_indices), std::move(right_indices), { res_left_bounds.min, res_left_bounds.max }, { res_right_bounds.min, res_right_bounds.max } };
}





/* THIS IS FOR SPATIAL SPLITS, NEEDED LATER */

#if 0

//////////////////////////////////////////////////////////////////////////

//res_sah = 9999999999999.0f;

static int bbb = 0;

#if 0
bbox_t overlap = { max(res_left_bounds.min, res_right_bounds.min),
                   min(res_left_bounds.max, res_right_bounds.max)
                 };
if (div_axis != -1 /*&& bbb < 8*/ /*&&
    overlap.min.x < overlap.max.x && overlap.min.y < overlap.max.y && overlap.min.z < overlap.max.z &&
    (surface_area(overlap) / surface_area(whole_box)) > SpatialSplitAlpha*/) {
    bbb++;
    //whole_box = { bbox_min, bbox_max };

    struct bin_t {
        bbox_t extends;
        bbox_t limits;
        uint32_t enter_counter = 0, exit_counter = 0;
    };

    int spatial_split = -1;
    int g_num_left = 0, g_num_right = 0;

    //int split_axis = div_axis; {
    for (int split_axis = 0; split_axis < 3; split_axis++) {
        double bin_size = double(whole_box.max[split_axis] - whole_box.min[split_axis]) / NumSpatialSplitBins;
        bin_t bins[NumSpatialSplitBins];

        for (int i = 0; i < NumSpatialSplitBins; i++) {
            bins[i].limits.min = whole_box.min;
            bins[i].limits.max = whole_box.max;
            bins[i].limits.min[split_axis] = castflt_down(whole_box.min[split_axis] + i * bin_size);
            bins[i].limits.max[split_axis] = castflt_up(whole_box.min[split_axis] + (i + 1) * bin_size);
        }

        auto &list = axis_lists[0];
        for (const auto &p : list) {
            int enter_index = int(double(p.bbox_min[split_axis] - whole_box.min[split_axis]) / bin_size);
            int exit_index = int(double(p.bbox_max[split_axis] - whole_box.min[split_axis]) / bin_size);
            enter_index = clamp(enter_index, 0, NumSpatialSplitBins - 1);
            exit_index = clamp(exit_index, 0, NumSpatialSplitBins - 1);

            bins[enter_index].enter_counter++;
            bins[exit_index].exit_counter++;

            for (int j = enter_index; j <= exit_index; j++) {
                uint32_t i0 = vtx_indices[p.index * 3],
                         i1 = vtx_indices[p.index * 3 + 1],
                         i2 = vtx_indices[p.index * 3 + 2];

                vec3 v0 = make_vec3(&attrs[i0 * 8]),
                     v1 = make_vec3(&attrs[i1 * 8]),
                     v2 = make_vec3(&attrs[i2 * 8]);

                bbox_t box = GetClippedAABB(v0, v1, v2, bins[j].limits);

                bins[j].extends.min = min(bins[j].extends.min, box.min);
                bins[j].extends.max = max(bins[j].extends.max, box.max);

                /////////

                //bins[j].extends.min = min(bins[j].extends.min, p.bbox_min);
                //bins[j].extends.max = max(bins[j].extends.max, p.bbox_max);
            }
        }

        if (!bins[0].enter_counter || !bins[NumSpatialSplitBins - 1].exit_counter) {
            //__debugbreak();
        }

        for (int i = 0; i < NumSpatialSplitBins; i++) {
            bins[i].extends.min = max(bins[i].extends.min, bins[i].limits.min);
            bins[i].extends.max = min(bins[i].extends.max, bins[i].limits.max);
        }

        for (int split = 1; split < NumSpatialSplitBins; split++) {
            bbox_t ext_left, ext_right;
            int num_left = 0, num_right = 0;
            for (int i = 0; i < split; i++) {
                if (!bins[i].enter_counter && !bins[i].exit_counter) continue;

                ext_left.min = min(ext_left.min, bins[i].extends.min);
                ext_left.max = max(ext_left.max, bins[i].extends.max);
                num_left += bins[i].enter_counter;
            }
            for (int i = split; i < NumSpatialSplitBins; i++) {
                if (!bins[i].enter_counter && !bins[i].exit_counter) continue;

                ext_right.min = min(ext_right.min, bins[i].extends.min);
                ext_right.max = max(ext_right.max, bins[i].extends.max);
                num_right += bins[i].exit_counter;
            }

            if (!num_left || !num_right ||
                    ext_left.min[div_axis] > 9999999 ||
                    ext_left.max[div_axis] < -9999999 ||
                    ext_right.min[div_axis] > 9999999 ||
                    ext_right.max[div_axis] < -9999999) {
                //__debugbreak();
                continue;
            }

            float sah = NodeTraversalCost + surface_area(ext_left) * num_left + surface_area(ext_right) * num_right;
            if (sah < res_sah) {
                //__debugbreak();
                res_sah = sah;
                spatial_split = split;
                div_axis = split_axis;

                res_left_bounds = ext_left;
                res_right_bounds = ext_right;

                if (!num_left || !num_right) {
                    __debugbreak();
                }

                g_num_left = num_left;
                g_num_right = num_right;

                printf("r l: %i %i\n", num_left, num_right);
            }
        }
    }

    if (spatial_split != -1) {
        std::vector<uint32_t> left_indices, right_indices;

        res_left_bounds.min = max(res_left_bounds.min, whole_box.min);
        res_left_bounds.max = min(res_left_bounds.max, whole_box.max);
        res_right_bounds.min = max(res_right_bounds.min, whole_box.min);
        res_right_bounds.max = min(res_right_bounds.max, whole_box.max);

        //res_left_bounds.min -= vec3(10, 10, 10);
        //res_left_bounds.max[div_axis] += 0.00001f;
        //res_right_bounds.min[div_axis] -= 0.00001f;
        //res_right_bounds.max += vec3(10, 10, 10);

        int num_in_both = 0;

        auto &list = axis_lists[0];
        for (const auto &p : list) {
            bool b1 = false, b2 = false;
            if (p.bbox_min[div_axis] <= res_left_bounds.max[div_axis]) {
                left_indices.push_back(p.index);
                b1 = true;
            }

            if (p.bbox_max[div_axis] >= res_right_bounds.min[div_axis]) {
                right_indices.push_back(p.index);
                b2 = true;
            }

            if (b1 && b2) {
                num_in_both++;
            }

            if (!b1 && !b2) {
                __debugbreak();
            }

            /*b1 = p.bbox_min.x >= res_left_bounds.min.x &&
                 p.bbox_min.y >= res_left_bounds.min.y &&
                 p.bbox_min.z >= res_left_bounds.min.z;
            bool b3 = p.bbox_max.x <= res_left_bounds.max.x &&
                 p.bbox_max.y <= res_left_bounds.max.y &&
                 p.bbox_max.z <= res_left_bounds.max.z;

            b2 = p.bbox_min.x >= res_right_bounds.min.x &&
                 p.bbox_min.y >= res_right_bounds.min.y &&
                 p.bbox_min.z >= res_right_bounds.min.z;
            bool b4 = p.bbox_max.x <= res_right_bounds.max.x &&
                 p.bbox_max.y <= res_right_bounds.max.y &&
                 p.bbox_max.z <= res_right_bounds.max.z;

            if (!b1 && !b2 && !b3 && !b4) {
                __debugbreak();
            }*/
        }

        if (left_indices.empty() || right_indices.empty() ||
                res_left_bounds.min[div_axis] > 9999999 ||
                res_left_bounds.max[div_axis] < -9999999 ||
                res_right_bounds.min[div_axis] > 9999999 ||
                res_right_bounds.max[div_axis] < -9999999) {
            __debugbreak();
            volatile int i = 0;
        }

        //res_left_bounds.max += 10.0f;
        //res_right_bounds.min -= 10.0f;

        if (abs(res_left_bounds.min[div_axis] - whole_box.min[div_axis]) > 0.05f ||
                abs(res_right_bounds.max[div_axis] - whole_box.max[div_axis]) > 0.05f) {
            //__debugbreak();
        }

        printf("Spatial split: %i %i %i\n", (int)left_indices.size(), (int)right_indices.size(), num_in_both);
        //printf("Extends: %f %f\n", (float)res_left_bounds.max[div_axis], (float)res_right_bounds.min[div_axis]);

        if (left_indices.size() != g_num_left || right_indices.size() != g_num_right) {
            //__debugbreak();
        }

        return { std::move(left_indices), std::move(right_indices), { res_left_bounds.min, res_left_bounds.max }, { res_right_bounds.min, res_right_bounds.max } };
    } else {
        volatile int i = 0;
    }
}
#endif

//////////////////////////////////////////////////////////////////////////

#endif