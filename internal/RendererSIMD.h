
#include <chrono>
#include <functional>
#include <mutex>
#include <random>

#include "CoreSIMD.h"
#include "FramebufferRef.h"
#include "Halton.h"
#include "../RendererBase.h"

namespace Ray {
namespace NS {
template <int S>
struct PassData {
    aligned_vector<ray_packet_t<S>> primary_rays;
    aligned_vector<simd_ivec<S>> primary_masks;
    aligned_vector<ray_packet_t<S>> secondary_rays;
    aligned_vector<simd_ivec<S>> secondary_masks;
    aligned_vector<hit_data_t<S>> intersections;

    aligned_vector<simd_ivec<S>> hash_values;
    std::vector<int> head_flags;
    std::vector<uint32_t> scan_values;
    std::vector<ray_chunk_t> chunks, chunks_temp;
    std::vector<uint32_t> skeleton;

    PassData() = default;

    PassData(const PassData &rhs) = delete;
    PassData(PassData &&rhs) { *this = std::move(rhs); }

    PassData &operator=(const PassData &rhs) = delete;
    PassData &operator=(PassData &&rhs) {
        primary_rays = std::move(rhs.primary_rays);
        primary_masks = std::move(rhs.primary_masks);
        secondary_rays = std::move(rhs.secondary_rays);
        intersections = std::move(rhs.intersections);
        hash_values = std::move(rhs.hash_values);
        head_flags = std::move(rhs.head_flags);
        scan_values = std::move(rhs.scan_values);
        chunks = std::move(rhs.chunks);
        chunks_temp = std::move(rhs.chunks_temp);
        skeleton = std::move(rhs.skeleton);
        return *this;
    }
};

template <int DimX, int DimY>
class RendererSIMD : public RendererBase {
    Ref::Framebuffer clean_buf_, final_buf_, temp_buf_;

    std::mutex pass_cache_mtx_;
    std::vector<PassData<DimX * DimY>> pass_cache_;

    stats_t stats_ = { 0 };
    int w_ = 0, h_ = 0;

    std::vector<uint16_t> permutations_;
    void UpdateHaltonSequence(int iteration, std::unique_ptr<float[]> &seq);
public:
    RendererSIMD(int w, int h);

    std::pair<int, int> size() const override {
        return std::make_pair(final_buf_.w(), final_buf_.h());
    }

    const pixel_color_t *get_pixels_ref() const override {
        return final_buf_.get_pixels_ref();
    }

    const SHL1_data *get_sh_data_ref() const override {
        return final_buf_.get_sh_data_ref();
    }

    void Resize(int w, int h) override {
        if (w_ != w || h_ != h) {
            clean_buf_.Resize(w, h, false);
            final_buf_.Resize(w, h, false);
            temp_buf_.Resize(w, h, false);

            w_ = w; h_ = h;
        }
    }
    void Clear(const pixel_color_t &c) override {
        clean_buf_.Clear(c);
    }

    std::shared_ptr<SceneBase> CreateScene() override;
    void RenderScene(const std::shared_ptr<SceneBase> &s, RegionContext &region) override;

    virtual void GetStats(stats_t &st) override { st = stats_; }
    virtual void ResetStats() override { stats_ = { 0 }; }
};
}
}

////////////////////////////////////////////////////////////////////////////////////////////

#include "SceneRef.h"

template <int DimX, int DimY>
Ray::NS::RendererSIMD<DimX, DimY>::RendererSIMD(int w, int h) : clean_buf_(w, h), final_buf_(w, h), temp_buf_(w, h) {
    auto rand_func = std::bind(std::uniform_int_distribution<int>(), std::mt19937(0));
    permutations_ = Ray::ComputeRadicalInversePermutations(g_primes, PrimesCount, rand_func);
}

template <int DimX, int DimY>
std::shared_ptr<Ray::SceneBase> Ray::NS::RendererSIMD<DimX, DimY>::CreateScene() {
    return std::make_shared<Ref::Scene>();
}

template <int DimX, int DimY>
void Ray::NS::RendererSIMD<DimX, DimY>::RenderScene(const std::shared_ptr<SceneBase> &_s, RegionContext &region) {
    const int S = DimX * DimY;

    auto s = std::dynamic_pointer_cast<Ref::Scene>(_s);
    if (!s) return;

    const auto &cam = s->cams_[s->current_cam()].cam;

    const auto num_tris = (uint32_t)s->tris_.size();
    const auto *tris = num_tris ? &s->tris_[0] : nullptr;

    const auto num_indices = (uint32_t)s->tri_indices_.size();
    const auto *tri_indices = num_indices ? &s->tri_indices_[0] : nullptr;

    const auto num_nodes = (uint32_t)s->nodes_.size();
    const auto *nodes = num_nodes ? &s->nodes_[0] : nullptr;

    const auto macro_tree_root = s->macro_nodes_start_;
    const auto light_tree_root = s->light_nodes_start_;

    const auto num_meshes = (uint32_t)s->meshes_.size();
    const auto *meshes = num_meshes ? &s->meshes_[0] : nullptr;

    const auto num_transforms = (uint32_t)s->transforms_.size();
    const auto *transforms = num_transforms ? &s->transforms_[0] : nullptr;

    const auto num_mesh_instances = (uint32_t)s->mesh_instances_.size();
    const auto *mesh_instances = num_mesh_instances ? &s->mesh_instances_[0] : nullptr;

    const auto num_mi_indices = (uint32_t)s->mi_indices_.size();
    const auto *mi_indices = num_mi_indices ? &s->mi_indices_[0] : nullptr;

    const auto num_vertices = (uint32_t)s->vertices_.size();
    const auto *vertices = num_vertices ? &s->vertices_[0] : nullptr;

    const auto num_vtx_indices = (uint32_t)s->vtx_indices_.size();
    const auto *vtx_indices = num_vtx_indices ? &s->vtx_indices_[0] : nullptr;

    const auto num_textures = (uint32_t)s->textures_.size();
    const auto *textures = num_textures ? &s->textures_[0] : nullptr;

    const auto num_materials = (uint32_t)s->materials_.size();
    const auto *materials = num_materials ? &s->materials_[0] : nullptr;

    const auto num_lights = (uint32_t)s->lights_.size();
    const auto *lights = num_lights ? &s->lights_[0] : nullptr;

    const auto num_li_indices = (uint32_t)s->li_indices_.size();
    const auto *li_indices = num_li_indices ? &s->li_indices_[0] : nullptr;

    const auto &tex_atlas = s->texture_atlas_;
    //const auto &env = s->env_;

    const float *root_min = nodes[macro_tree_root].bbox[0], *root_max = nodes[macro_tree_root].bbox[1];
    float cell_size[3] = { (root_max[0] - root_min[0]) / 255, (root_max[1] - root_min[1]) / 255, (root_max[2] - root_min[2]) / 255 };

    NS::environment_t env;
    memcpy(&env.env_col[0], &s->env_.env_col[0], 3 * sizeof(float));
    env.env_clamp = s->env_.env_clamp;
    env.env_map = s->env_.env_map;

    const auto w = final_buf_.w(), h = final_buf_.h();

    auto rect = region.rect();
    if (rect.w == 0 || rect.h == 0) {
        rect = { 0, 0, w, h };
    }

    region.iteration++;
    if (!region.halton_seq || region.iteration % HALTON_SEQ_LEN == 0) {
        UpdateHaltonSequence(region.iteration, region.halton_seq);
    }

    PassData<S> p;

    {
        std::lock_guard<std::mutex> _(pass_cache_mtx_);
        if (!pass_cache_.empty()) {
            p = std::move(pass_cache_.back());
            pass_cache_.pop_back();
        }
    }

    pass_info_t pass_info;

    pass_info.iteration = region.iteration;
    pass_info.bounce = 2;
    pass_info.flags = cam.pass_flags;

    const auto time_start = std::chrono::high_resolution_clock::now();
    std::chrono::time_point<std::chrono::high_resolution_clock> time_after_ray_gen;

    if (cam.type != Geo) {
        GeneratePrimaryRays<DimX, DimY>(region.iteration, cam, rect, w, h, &region.halton_seq[0], p.primary_rays);

        time_after_ray_gen = std::chrono::high_resolution_clock::now();

        p.primary_masks.resize(p.primary_rays.size());
        p.intersections.resize(p.primary_rays.size());

        for (size_t i = 0; i < p.primary_rays.size(); i++) {
            const auto &r = p.primary_rays[i];
            auto &inter = p.intersections[i];

            inter = {};
            inter.xy = r.xy;

            if (macro_tree_root != 0xffffffff) {
                NS::Traverse_MacroTree_WithStack(r, { -1 }, nodes, macro_tree_root, mesh_instances, mi_indices, meshes, transforms, tris, tri_indices, inter);
            }
        }
    } else {
        const auto &mi = mesh_instances[cam.mi_index];
        SampleMeshInTextureSpace<DimX, DimY>(region.iteration, cam.mi_index, cam.uv_index,
                                             meshes[mi.mesh_index], transforms[mi.tr_index], vtx_indices, vertices,
                                             rect, w, h, &region.halton_seq[0], p.primary_rays, p.intersections);

        p.primary_masks.resize(p.primary_rays.size());

        time_after_ray_gen = std::chrono::high_resolution_clock::now();
    }

    const auto time_after_prim_trace = std::chrono::high_resolution_clock::now();

    p.secondary_rays.resize(p.intersections.size());
    p.secondary_masks.resize(p.intersections.size());
    int secondary_rays_count = 0;

    for (size_t i = 0; i < p.intersections.size(); i++) {
        const auto &r = p.primary_rays[i];
        const auto &inter = p.intersections[i];

        simd_ivec<S> x = inter.xy >> 16,
                     y = inter.xy & 0x0000FFFF;

        simd_ivec<S> index = { y * w + x };

        p.secondary_masks[i] = { 0 };

        simd_fvec<S> out_rgba[4] = { 0.0f };
        NS::ShadeSurface(index, pass_info, &region.halton_seq[0], inter, r, env, mesh_instances,
                         mi_indices, meshes, transforms, vtx_indices, vertices, nodes, macro_tree_root,
                         tris, tri_indices, materials, textures, tex_atlas, lights, li_indices, light_tree_root,
                         out_rgba, &p.secondary_masks[0], &p.secondary_rays[0], &secondary_rays_count);

        for (int j = 0; j < S; j++) {
            temp_buf_.SetPixel(x[j], y[j], { out_rgba[0][j], out_rgba[1][j], out_rgba[2][j], out_rgba[3][j] });
        }
    }

    const auto time_after_prim_shade = std::chrono::high_resolution_clock::now();
    std::chrono::duration<double, std::micro> secondary_sort_time{}, secondary_trace_time{}, secondary_shade_time{};

    p.hash_values.resize(secondary_rays_count);
    p.head_flags.resize(secondary_rays_count * S);
    p.scan_values.resize(secondary_rays_count * S);
    p.chunks.resize(secondary_rays_count * S);
    p.chunks_temp.resize(secondary_rays_count * S);
    p.skeleton.resize(secondary_rays_count * S);

    for (int bounce = 0; bounce < MAX_BOUNCES && secondary_rays_count && !(pass_info.flags & SkipIndirectLight); bounce++) {
        auto time_secondary_sort_start = std::chrono::high_resolution_clock::now();

        SortRays(&p.secondary_rays[0], &p.secondary_masks[0], secondary_rays_count, root_min, cell_size,
                 &p.hash_values[0], &p.head_flags[0], &p.scan_values[0], &p.chunks[0], &p.chunks_temp[0], &p.skeleton[0]);

        auto time_secondary_trace_start = std::chrono::high_resolution_clock::now();

        for (int i = 0; i < secondary_rays_count; i++) {
            const auto &r = p.secondary_rays[i];
            auto &inter = p.intersections[i];

            inter = {};
            inter.xy = r.xy;

            NS::Traverse_MacroTree_WithStack(r, p.secondary_masks[i], nodes, macro_tree_root, mesh_instances, mi_indices, meshes, transforms, tris, tri_indices, inter);
        }

        auto time_secondary_shade_start = std::chrono::high_resolution_clock::now();

        int rays_count = secondary_rays_count;
        secondary_rays_count = 0;
        std::swap(p.primary_rays, p.secondary_rays);
        std::swap(p.primary_masks, p.secondary_masks);

        pass_info.bounce = bounce + 3;

        for (int i = 0; i < rays_count; i++) {
            const auto &r = p.primary_rays[i];
            const auto &inter = p.intersections[i];

            simd_ivec<S> x = inter.xy >> 16,
                         y = inter.xy & 0x0000FFFF;

            simd_ivec<S> index = { y * w + x };

            simd_fvec<S> out_rgba[4] = { 0.0f };
            NS::ShadeSurface(index, pass_info, &region.halton_seq[0], inter, r, env, mesh_instances,
                             mi_indices, meshes, transforms, vtx_indices, vertices, nodes, macro_tree_root,
                             tris, tri_indices, materials, textures, tex_atlas, lights, li_indices, light_tree_root, 
                             out_rgba, &p.secondary_masks[0], &p.secondary_rays[0], &secondary_rays_count);
            out_rgba[3] = 0.0f;

            for (int j = 0; j < S; j++) {
                if (!p.primary_masks[i][j]) continue;

                temp_buf_.AddPixel(x[j], y[j], { out_rgba[0][j], out_rgba[1][j], out_rgba[2][j], out_rgba[3][j] });
            }
        }

        auto time_secondary_shade_end = std::chrono::high_resolution_clock::now();
        secondary_sort_time += std::chrono::duration<double, std::micro>{ time_secondary_trace_start - time_secondary_sort_start };
        secondary_trace_time += std::chrono::duration<double, std::micro>{ time_secondary_shade_start - time_secondary_trace_start };
        secondary_shade_time += std::chrono::duration<double, std::micro>{ time_secondary_shade_end - time_secondary_shade_start };
    }

    {
        std::lock_guard<std::mutex> _(pass_cache_mtx_);
        pass_cache_.emplace_back(std::move(p));

        stats_.time_primary_ray_gen_us += (unsigned long long)std::chrono::duration<double, std::micro>{ time_after_ray_gen - time_start }.count();
        stats_.time_primary_trace_us += (unsigned long long)std::chrono::duration<double, std::micro>{ time_after_prim_trace - time_after_ray_gen }.count();
        stats_.time_primary_shade_us += (unsigned long long)std::chrono::duration<double, std::micro>{ time_after_prim_shade - time_after_prim_trace }.count();
        stats_.time_secondary_sort_us += (unsigned long long)secondary_sort_time.count();
        stats_.time_secondary_trace_us += (unsigned long long)secondary_trace_time.count();
        stats_.time_secondary_shade_us += (unsigned long long)secondary_shade_time.count();
    }

    clean_buf_.MixWith(temp_buf_, rect, 1.0f / region.iteration);

    auto clamp_and_gamma_correct = [&cam](const pixel_color_t &p) {
        auto c = simd_fvec4(&p.r);
        c = pow(c, simd_fvec4{ 1.0f / cam.gamma });
        if (cam.pass_flags & Clamp) {
            c = clamp(c, 0.0f, 1.0f);
        }
        return pixel_color_t{ c[0], c[1], c[2], c[3] };
    };

    final_buf_.CopyFrom(clean_buf_, rect, clamp_and_gamma_correct);
}

template <int DimX, int DimY>
void Ray::NS::RendererSIMD<DimX, DimY>::UpdateHaltonSequence(int iteration, std::unique_ptr<float[]> &seq) {
    if (!seq) {
        seq.reset(new float[HALTON_COUNT * HALTON_SEQ_LEN]);
    }

    for (int i = 0; i < HALTON_SEQ_LEN; i++) {
        seq[2 * (i * HALTON_2D_COUNT + 0 ) + 0] = Ray::ScrambledRadicalInverse<2 >(&permutations_[0  ], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 0 ) + 1] = Ray::ScrambledRadicalInverse<3 >(&permutations_[2  ], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 1 ) + 0] = Ray::ScrambledRadicalInverse<5 >(&permutations_[5  ], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 1 ) + 1] = Ray::ScrambledRadicalInverse<7 >(&permutations_[10 ], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 2 ) + 0] = Ray::ScrambledRadicalInverse<11>(&permutations_[17 ], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 2 ) + 1] = Ray::ScrambledRadicalInverse<13>(&permutations_[28 ], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 3 ) + 0] = Ray::ScrambledRadicalInverse<17>(&permutations_[41 ], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 3 ) + 1] = Ray::ScrambledRadicalInverse<19>(&permutations_[58 ], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 4 ) + 0] = Ray::ScrambledRadicalInverse<23>(&permutations_[77 ], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 4 ) + 1] = Ray::ScrambledRadicalInverse<29>(&permutations_[100], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 5 ) + 0] = Ray::ScrambledRadicalInverse<31>(&permutations_[129], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 5 ) + 1] = Ray::ScrambledRadicalInverse<37>(&permutations_[160], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 6 ) + 0] = Ray::ScrambledRadicalInverse<41>(&permutations_[197], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 6 ) + 1] = Ray::ScrambledRadicalInverse<43>(&permutations_[238], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 7 ) + 0] = Ray::ScrambledRadicalInverse<47>(&permutations_[281], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 7 ) + 1] = Ray::ScrambledRadicalInverse<53>(&permutations_[328], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 8 ) + 0] = Ray::ScrambledRadicalInverse<59>(&permutations_[381], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 8 ) + 1] = Ray::ScrambledRadicalInverse<61>(&permutations_[440], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 9 ) + 0] = Ray::ScrambledRadicalInverse<67>(&permutations_[501], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 9 ) + 1] = Ray::ScrambledRadicalInverse<71>(&permutations_[568], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 10) + 0] = Ray::ScrambledRadicalInverse<73>(&permutations_[639], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 10) + 1] = Ray::ScrambledRadicalInverse<79>(&permutations_[712], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 11) + 0] = Ray::ScrambledRadicalInverse<83>(&permutations_[791], (uint64_t)(iteration + i));
        seq[2 * (i * HALTON_2D_COUNT + 11) + 1] = Ray::ScrambledRadicalInverse<89>(&permutations_[874], (uint64_t)(iteration + i));
    }
}
