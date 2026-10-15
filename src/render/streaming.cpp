#include <bridger/render/streaming.hpp>

#include <bridger/core/parallel.hpp>
#include <bridger/render/lod_cache.hpp>
#include <bridger/render/mesher.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace bridger::render {
namespace {

u64 key_of(ChunkPos p) { return p.packed(); }

struct Column {
    LoadedColumn data;
    std::vector<const BakedBlock*> table; // registry id -> baked block (nullptr = air)
};

struct Job {
    ChunkPos pos;
    bool remesh = false;
    bool lod = false; // distant terrain: summary + LOD mesh
    int step = 2;     // LOD cell size in blocks
};

struct Result {
    ChunkPos pos;
    std::vector<std::pair<glm::ivec3, SectionMesh>> sections;
    bool missing = false; // the column does not exist
    bool lod = false;
};

// LOD meshes use one vertical frame for every world: sections from y -64 (the lowest build limit).
constexpr int kLodBaseY = -64;

ColumnSummary summarize(const Column& c) { return summarize_column(c.data.column, c.table, c.data.min_y); }

bool is_air(const std::string& id) {
    return id == "minecraft:air" || id == "minecraft:cave_air" || id == "minecraft:void_air" || id == "minecraft:structure_void";
}

} // namespace

struct StreamingScene::Impl {
    BlockModels& models;
    ColumnLoader loader;
    std::unordered_set<u64> available;
    usize available_count = 0;

    std::mutex models_lock; // BlockModels is not thread-safe

    mutable std::mutex lock; // columns, queue, results, scheduled
    std::condition_variable work_cv;
    std::deque<Job> queue;
    std::deque<Job> lod_queue; // taken only when `queue` is empty
    std::unordered_map<u64, std::shared_ptr<const Column>> columns;
    std::unordered_map<u64, std::shared_ptr<const ColumnSummary>> summaries; // distant terrain, also of full columns
    std::unordered_set<u64> lod_scheduled;
    int lod_radius = 0;
    std::shared_ptr<LodCache> cache;
    std::unordered_set<u64> scheduled; // queued or in flight (loads)
    std::unordered_set<u64> absent;    // listed but not loadable (partly generated, unreadable): not retried
    std::vector<Result> results;
    usize in_flight = 0;
    bool stop = false;
    std::vector<std::thread> workers;

    // Render-thread state.
    std::unordered_map<u64, std::vector<glm::ivec3>> uploaded; // column -> section origins on the GPU
    std::unordered_map<u64, u64> column_quads;
    std::unordered_set<u64> lod_shown; // columns whose uploaded sections are LOD meshes
    int min_y = INT32_MAX, max_y = INT32_MIN;
    usize uploaded_textures = 0; // texture layers on the GPU (baking adds layers as new blocks appear)

    Impl(BlockModels& m, ColumnLoader l, std::vector<ChunkPos> avail, usize threads) : models(m), loader(std::move(l)) {
        for (const ChunkPos p : avail) available.insert(key_of(p));
        available_count = available.size();
        const usize n = std::max<usize>(1, threads == 0 ? resolve_thread_count(0) - 1 : threads);
        for (usize i = 0; i < n; ++i) workers.emplace_back([this] { run(); });
    }

    ~Impl() {
        {
            std::lock_guard g(lock);
            stop = true;
        }
        work_cv.notify_all();
        for (std::thread& t : workers) t.join();
        if (cache) (void)cache->flush();
    }

    // Bakes the column's states (under the models lock) into its lookup table.
    void bake_table(Column& c) {
        const BlockStateRegistry& reg = c.data.column.block_registry();
        c.table.assign(reg.size(), nullptr);
        std::lock_guard g(models_lock);
        for (StateId id = 0; id < reg.size(); ++id)
            if (!is_air(reg.get(id).id())) c.table[id] = &models.bake(reg.get(id));
    }

    // Distant terrain job: the summary (memory, cache, or loading the column), then its LOD mesh.
    void run_lod(const Job& job, Result& r) {
        r.lod = true;
        const u64 k = key_of(job.pos);
        std::shared_ptr<const ColumnSummary> sum;
        {
            std::lock_guard g(lock);
            if (const auto it = summaries.find(k); it != summaries.end()) sum = it->second;
        }
        if (!sum && cache)
            if (auto c = cache->get(job.pos)) sum = std::make_shared<const ColumnSummary>(*c);
        if (!sum) {
            auto loaded = loader(job.pos);
            if (!loaded) {
                r.missing = true;
                std::lock_guard g(lock);
                absent.insert(k);
                return;
            }
            Column c;
            c.data = std::move(*loaded);
            bake_table(c);
            sum = std::make_shared<const ColumnSummary>(summarize(c));
            if (cache) cache->put(job.pos, *sum);
        }
        std::array<std::shared_ptr<const ColumnSummary>, 4> around;
        {
            std::lock_guard g(lock);
            summaries[k] = sum;
            const std::array<ChunkPos, 4> d{ChunkPos{1, 0}, ChunkPos{-1, 0}, ChunkPos{0, 1}, ChunkPos{0, -1}};
            for (usize i = 0; i < 4; ++i)
                if (const auto it = summaries.find(key_of({job.pos.x + d[i].x, job.pos.z + d[i].z})); it != summaries.end())
                    around[i] = it->second;
        }
        const glm::ivec3 origin(job.pos.x * 16, kLodBaseY, job.pos.z * 16);
        std::map<int, SectionMesh> meshes;
        mesh_lod(*sum, {around[0].get(), around[1].get(), around[2].get(), around[3].get()}, origin, job.step, models.white_texture(),
                 meshes);
        for (auto& [sy, m] : meshes)
            if (!m.empty()) r.sections.emplace_back(glm::ivec3(origin.x, kLodBaseY + sy * 16, origin.z), std::move(m));
    }

    std::shared_ptr<const Column> find(ChunkPos p) const {
        const auto it = columns.find(key_of(p));
        return it == columns.end() ? nullptr : it->second;
    }

    // Meshes every non-empty section of `col`, reading neighbour columns for the border.
    std::vector<std::pair<glm::ivec3, SectionMesh>> mesh_column(const Column& col) {
        std::array<std::shared_ptr<const Column>, 9> around; // [dz+1][dx+1]
        {
            std::lock_guard g(lock);
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx)
                    around[static_cast<usize>((dz + 1) * 3 + dx + 1)] =
                        (dx == 0 && dz == 0) ? nullptr : find({col.data.pos.x + dx, col.data.pos.z + dz});
        }
        const ChunkPos cp = col.data.pos;
        auto lookup = [&](int wx, int wy, int wz) -> const BakedBlock* {
            const int cx = wx >> 4, cz = wz >> 4;
            const Column* c = &col;
            if (cx != cp.x || cz != cp.z) {
                const int dx = cx - cp.x, dz = cz - cp.z;
                if (dx < -1 || dx > 1 || dz < -1 || dz > 1) return nullptr;
                c = around[static_cast<usize>((dz + 1) * 3 + dx + 1)].get();
                if (c == nullptr) return nullptr;
            }
            const BlockPos local{wx & 15, wy - c->data.min_y, wz & 15};
            if (!c->data.column.contains(local)) return nullptr;
            const StateId id = c->data.column.block(local);
            return id < c->table.size() ? c->table[id] : nullptr;
        };
        std::vector<std::pair<glm::ivec3, SectionMesh>> out;
        const Structure& s = col.data.column;
        auto blocks = std::make_unique<std::array<const BakedBlock*, kPaddedVolume>>();
        for (u32 sy = 0; sy < s.sections_y(); ++sy) {
            const Section& sec = s.section(0, sy, 0);
            // An all-air section has nothing to draw (its neighbours read it through `lookup`).
            if (sec.blocks().is_uniform()) {
                const u32 id = sec.blocks().palette()[0];
                if (id >= col.table.size() || col.table[id] == nullptr) continue;
            }
            const glm::ivec3 so(cp.x * 16, col.data.min_y + static_cast<int>(sy) * 16, cp.z * 16);
            for (int y = -1; y <= 16; ++y)
                for (int z = -1; z <= 16; ++z)
                    for (int x = -1; x <= 16; ++x) (*blocks)[padded_index(x, y, z)] = lookup(so.x + x, so.y + y, so.z + z);
            SectionMesh m;
            mesh_section(*blocks, so, m);
            if (!m.empty()) out.emplace_back(so, std::move(m));
        }
        return out;
    }

    void run() {
        for (;;) {
            Job job;
            {
                std::unique_lock g(lock);
                work_cv.wait(g, [&] { return stop || !queue.empty() || !lod_queue.empty(); });
                if (stop) return;
                std::deque<Job>& q = queue.empty() ? lod_queue : queue;
                job = q.front();
                q.pop_front();
                ++in_flight;
            }
            Result r;
            r.pos = job.pos;
            if (job.lod) {
                run_lod(job, r);
                std::lock_guard g(lock);
                lod_scheduled.erase(key_of(job.pos));
                --in_flight;
                results.push_back(std::move(r));
                continue;
            }
            std::shared_ptr<const Column> col;
            if (job.remesh) {
                std::lock_guard g(lock);
                col = find(job.pos);
            } else if (auto loaded = loader(job.pos)) {
                auto c = std::make_shared<Column>();
                c->data = std::move(*loaded);
                bake_table(*c);
                // Its summary too, so distant terrain next to it and later views of it are free.
                std::shared_ptr<const ColumnSummary> sum;
                if (lod_radius > 0 || cache) {
                    sum = std::make_shared<const ColumnSummary>(summarize(*c));
                    if (cache) cache->put(job.pos, *sum);
                }
                col = c;
                std::lock_guard g(lock);
                columns[key_of(job.pos)] = c;
                if (sum) summaries[key_of(job.pos)] = sum;
                // Neighbours meshed before this column existed get their border faces fixed.
                for (const ChunkPos d : {ChunkPos{1, 0}, ChunkPos{-1, 0}, ChunkPos{0, 1}, ChunkPos{0, -1}}) {
                    const ChunkPos n{job.pos.x + d.x, job.pos.z + d.z};
                    if (columns.contains(key_of(n))) queue.push_back({n, true});
                }
                work_cv.notify_all();
            } else {
                r.missing = true;
                std::lock_guard g(lock);
                absent.insert(key_of(job.pos));
            }
            if (col) r.sections = mesh_column(*col);
            std::lock_guard g(lock);
            if (!job.remesh) scheduled.erase(key_of(job.pos));
            --in_flight;
            results.push_back(std::move(r));
        }
    }

    bool update(const Camera& cam, Renderer& renderer, int radius, usize budget) {
        const int ccx = static_cast<int>(std::floor(cam.position.x / 16.0)), ccz = static_cast<int>(std::floor(cam.position.z / 16.0));
        std::vector<Result> ready;
        bool busy = false;
        {
            std::lock_guard g(lock);
            // Schedule missing columns in range, nearest first.
            std::vector<std::pair<int, ChunkPos>> want;
            for (int dz = -radius; dz <= radius; ++dz)
                for (int dx = -radius; dx <= radius; ++dx) {
                    const int d2 = dx * dx + dz * dz;
                    if (d2 > radius * radius) continue;
                    const ChunkPos p{ccx + dx, ccz + dz};
                    const u64 k = key_of(p);
                    if (!available.contains(k) || columns.contains(k) || scheduled.contains(k) || absent.contains(k)) continue;
                    want.emplace_back(d2, p);
                }
            std::sort(want.begin(), want.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            for (const auto& [d2, p] : want) {
                scheduled.insert(key_of(p));
                queue.push_back({p, false});
            }
            if (!want.empty()) work_cv.notify_all();
            // Distant terrain between the radius and the LOD radius: cells of 2 blocks, 4 beyond twice
            // the radius.
            const int lr = lod_radius;
            if (lr > radius) {
                std::vector<std::pair<int, ChunkPos>> far_want;
                for (int dz = -lr; dz <= lr; ++dz)
                    for (int dx = -lr; dx <= lr; ++dx) {
                        const int d2 = dx * dx + dz * dz;
                        if (d2 <= radius * radius || d2 > lr * lr) continue;
                        const ChunkPos p{ccx + dx, ccz + dz};
                        const u64 k = key_of(p);
                        if (!available.contains(k) || columns.contains(k) || scheduled.contains(k) || lod_scheduled.contains(k) ||
                            lod_shown.contains(k) || absent.contains(k))
                            continue;
                        far_want.emplace_back(d2, p);
                    }
                std::sort(far_want.begin(), far_want.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                for (const auto& [d2, p] : far_want) {
                    lod_scheduled.insert(key_of(p));
                    lod_queue.push_back({p, false, true, d2 <= 4 * radius * radius ? 2 : 4});
                }
                if (!far_want.empty()) work_cv.notify_all();
            }
            // Take finished results within the budget.
            const usize n = std::min(budget, results.size());
            ready.assign(std::make_move_iterator(results.begin()), std::make_move_iterator(results.begin() + static_cast<std::ptrdiff_t>(n)));
            results.erase(results.begin(), results.begin() + static_cast<std::ptrdiff_t>(n));
            // Unload far columns.
            const int far = radius + 2;
            for (auto it = columns.begin(); it != columns.end();) {
                const ChunkPos p = it->second->data.pos;
                const int dx = p.x - ccx, dz = p.z - ccz;
                if (dx * dx + dz * dz > far * far) {
                    if (const auto u = uploaded.find(it->first); u != uploaded.end()) {
                        for (const glm::ivec3& o : u->second) renderer.remove_section(o);
                        uploaded.erase(u);
                        column_quads.erase(it->first);
                    }
                    it = columns.erase(it);
                } else {
                    ++it;
                }
            }
            // Unload distant terrain beyond the LOD radius (summaries stay in the cache on disk).
            const int lod_far = std::max(lr, radius) + 2;
            for (auto it = lod_shown.begin(); it != lod_shown.end();) {
                const ChunkPos p = ChunkPos::unpack(*it);
                const int dx = p.x - ccx, dz = p.z - ccz;
                if (lr <= radius || dx * dx + dz * dz > lod_far * lod_far) {
                    if (const auto u = uploaded.find(*it); u != uploaded.end()) {
                        for (const glm::ivec3& o : u->second) renderer.remove_section(o);
                        uploaded.erase(u);
                    }
                    column_quads.erase(*it);
                    it = lod_shown.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = summaries.begin(); it != summaries.end();) {
                const ChunkPos p = ChunkPos::unpack(it->first);
                const int dx = p.x - ccx, dz = p.z - ccz;
                if (dx * dx + dz * dz > lod_far * lod_far && !columns.contains(it->first)) it = summaries.erase(it);
                else ++it;
            }
            busy = !queue.empty() || !lod_queue.empty() || in_flight > 0 || !results.empty();
        }
        for (Result& r : ready) {
            const u64 k = key_of(r.pos);
            {
                std::lock_guard g(lock);
                if (r.lod ? (r.missing || columns.contains(k)) : !columns.contains(k)) continue; // full detail won, or unloaded
            }
            auto& prev = uploaded[k];
            std::vector<glm::ivec3> now;
            u64 quads = 0;
            for (auto& [origin, mesh] : r.sections) {
                (void)renderer.upload_section(origin, mesh);
                now.push_back(origin);
                quads += (mesh.solid.size() + mesh.cutout.size() + mesh.translucent.size()) / 4;
                if (!r.lod) {
                    min_y = std::min(min_y, origin.y);
                    max_y = std::max(max_y, origin.y + 16);
                }
            }
            if (r.lod) lod_shown.insert(k);
            else lod_shown.erase(k);
            for (const glm::ivec3& o : prev)
                if (std::find(now.begin(), now.end(), o) == now.end()) renderer.remove_section(o);
            prev = std::move(now);
            column_quads[k] = quads;
        }
        if (!ready.empty()) {
            std::lock_guard g(models_lock);
            if (models.texture_count() != uploaded_textures) {
                (void)renderer.upload_textures(models.texture_pixels(), static_cast<u32>(models.texture_count()));
                uploaded_textures = models.texture_count();
            }
        }
        return busy || !ready.empty();
    }
};

StreamingScene::StreamingScene(BlockModels& models, ColumnLoader loader, std::vector<ChunkPos> available, usize threads)
    : impl_(std::make_unique<Impl>(models, std::move(loader), std::move(available), threads)) {}
StreamingScene::~StreamingScene() = default;

bool StreamingScene::update(const Camera& camera, Renderer& renderer, int radius_chunks, usize upload_budget) {
    return impl_->update(camera, renderer, radius_chunks, upload_budget);
}

void StreamingScene::load_all(const Camera& camera, Renderer& renderer, int radius_chunks) {
    while (impl_->update(camera, renderer, radius_chunks, 1u << 20)) std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

void StreamingScene::set_distant_terrain(int lod_radius_chunks, std::shared_ptr<LodCache> cache) {
    std::lock_guard g(impl_->lock);
    impl_->lod_radius = std::max(0, lod_radius_chunks);
    impl_->cache = std::move(cache);
}

Status StreamingScene::flush_cache() {
    std::shared_ptr<LodCache> c;
    {
        std::lock_guard g(impl_->lock);
        c = impl_->cache;
    }
    return c ? c->flush() : Status{};
}

StreamingStats StreamingScene::stats() const {
    StreamingStats s;
    std::lock_guard g(impl_->lock);
    s.available = impl_->available_count;
    s.distant = impl_->lod_shown.size();
    s.loaded = impl_->uploaded.size() - s.distant;
    s.queued = impl_->queue.size() + impl_->lod_queue.size();
    s.in_flight = impl_->in_flight;
    for (const auto& [k, q] : impl_->column_quads) s.quads += q;
    return s;
}

std::optional<std::pair<int, int>> StreamingScene::height_range() const {
    if (impl_->min_y == INT32_MAX) return std::nullopt;
    return std::pair{impl_->min_y, impl_->max_y};
}

} // namespace bridger::render
