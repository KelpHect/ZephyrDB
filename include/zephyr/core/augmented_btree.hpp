#ifndef ZEPHYR_CORE_AUGMENTED_BTREE_HPP
#define ZEPHYR_CORE_AUGMENTED_BTREE_HPP

#include "zephyr/compat/cpp26.hpp"
#include "zephyr/core/rcu.hpp"

#include <array>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace zephyr::core {

/// Copy-on-Write (CoW) Augmented B+Tree (order 32) mapping `int64_t key -> row_id`
/// with per-node augmented aggregates for up to MAX_AGG_COLS numeric columns.
///
/// - Reads are completely lock-free using `cpp26::rcu::scoped_read_lock`.
/// - `total_count()`, `total_sum_i64()`, and `total_sum_f64()` run in O(1) time.
/// - `range_count()`, `range_sum_i64()`, and `range_sum_f64()` run in O(log N) time.
/// - Writes are serialized via an internal mutex, perform CoW path-copying with
///   atomic refcounting for shared subtrees, and retire replaced roots to RCU.
class AugmentedBTree {
public:
    static constexpr size_t ORDER = 32;
    static constexpr size_t MAX_AGG_COLS = 8;

    explicit AugmentedBTree(RcuReaper* reaper = nullptr,
                            cpp26::rcu::rcu_domain& dom = cpp26::rcu::default_domain());
    ~AugmentedBTree();

    AugmentedBTree(const AugmentedBTree&) = delete;
    AugmentedBTree& operator=(const AugmentedBTree&) = delete;

    // ------------------------------------------------------------------------
    // Lock-free RCU Read APIs
    // ------------------------------------------------------------------------

    bool find(int64_t key, uint64_t& out_row_id) const;

    uint64_t total_count() const;
    int64_t total_sum_i64(size_t col = 0) const;
    double total_sum_f64(size_t col = 0) const;

    uint64_t range_count(int64_t lo, int64_t hi) const;
    int64_t range_sum_i64(int64_t lo, int64_t hi, size_t col = 0) const;
    double range_sum_f64(int64_t lo, int64_t hi, size_t col = 0) const;

    void scan_all(std::vector<std::pair<int64_t, uint64_t>>& out) const;

    // ------------------------------------------------------------------------
    // Serialized CoW Write APIs
    // ------------------------------------------------------------------------

    struct BatchEntry {
        int64_t key = 0;
        uint64_t row_id = 0;
        std::array<int64_t, MAX_AGG_COLS> i64_vals{};
        std::array<double, MAX_AGG_COLS> f64_vals{};
    };

    void upsert(int64_t key,
                uint64_t row_id,
                const std::array<int64_t, MAX_AGG_COLS>& i64_vals = {},
                const std::array<double, MAX_AGG_COLS>& f64_vals = {});

    void upsert_batch(const std::vector<BatchEntry>& entries);

    bool erase(int64_t key);
    void clear();

private:
    static constexpr size_t INLINE_AGG_COLS = 2;
    static constexpr size_t EXTRA_AGG_COLS = MAX_AGG_COLS - INLINE_AGG_COLS;

    struct NodeHeader {
        std::atomic<uint32_t> refcount{1};
        bool is_leaf = true;
        uint16_t num_keys = 0;
        uint64_t batch_gen = 0;

        // Augmented aggregates over this subtree
        uint64_t count = 0;
        int64_t min_key = INT64_MAX;
        int64_t max_key = INT64_MIN;
        std::array<int64_t, MAX_AGG_COLS> col_sum_i64{};
        std::array<double, MAX_AGG_COLS> col_sum_f64{};

        // Separator keys (for internal) or entry keys (for leaf)
        std::array<int64_t, ORDER> keys{};
    };

    struct InternalNode : NodeHeader {
        std::array<NodeHeader*, ORDER + 1> children{};

        InternalNode() { is_leaf = false; }
    };

    struct LeafNode : NodeHeader {
        struct ExtraCols {
            std::array<std::array<int64_t, EXTRA_AGG_COLS>, ORDER> i64{};
            std::array<std::array<double, EXTRA_AGG_COLS>, ORDER> f64{};
        };

        std::array<uint64_t, ORDER> row_ids{};
        std::array<std::array<int64_t, INLINE_AGG_COLS>, ORDER> row_i64_inline{};
        std::array<std::array<double, INLINE_AGG_COLS>, ORDER> row_f64_inline{};
        ExtraCols* extra = nullptr;

        LeafNode() { is_leaf = true; }
        ~LeafNode() {
            delete extra;
            extra = nullptr;
        }

        LeafNode(const LeafNode&) = delete;
        LeafNode& operator=(const LeafNode&) = delete;

        int64_t get_row_i64(size_t row, size_t col) const noexcept {
            if (col < INLINE_AGG_COLS) return row_i64_inline[row][col];
            return extra ? extra->i64[row][col - INLINE_AGG_COLS] : 0;
        }

        double get_row_f64(size_t row, size_t col) const noexcept {
            if (col < INLINE_AGG_COLS) return row_f64_inline[row][col];
            return extra ? extra->f64[row][col - INLINE_AGG_COLS] : 0.0;
        }

        static bool has_extra_vals(const std::array<int64_t, MAX_AGG_COLS>& i64_vals,
                                   const std::array<double, MAX_AGG_COLS>& f64_vals) noexcept {
            for (size_t c = INLINE_AGG_COLS; c < MAX_AGG_COLS; ++c) {
                if (i64_vals[c] != 0 || f64_vals[c] != 0.0) return true;
            }
            return false;
        }

        void set_row_vals(size_t row,
                          const std::array<int64_t, MAX_AGG_COLS>& i64_vals,
                          const std::array<double, MAX_AGG_COLS>& f64_vals) {
            row_i64_inline[row][0] = i64_vals[0];
            row_i64_inline[row][1] = i64_vals[1];
            row_f64_inline[row][0] = f64_vals[0];
            row_f64_inline[row][1] = f64_vals[1];
            if (extra) {
                for (size_t c = 0; c < EXTRA_AGG_COLS; ++c) {
                    extra->i64[row][c] = i64_vals[INLINE_AGG_COLS + c];
                    extra->f64[row][c] = f64_vals[INLINE_AGG_COLS + c];
                }
            } else if (has_extra_vals(i64_vals, f64_vals)) {
                extra = new ExtraCols{};
                for (size_t c = 0; c < EXTRA_AGG_COLS; ++c) {
                    extra->i64[row][c] = i64_vals[INLINE_AGG_COLS + c];
                    extra->f64[row][c] = f64_vals[INLINE_AGG_COLS + c];
                }
            }
        }
    };

    struct InsertResult {
        NodeHeader* left = nullptr;
        NodeHeader* right = nullptr;
        int64_t split_key = 0;
        bool did_split = false;
        uint64_t delta_count = 0;
        std::array<int64_t, MAX_AGG_COLS> delta_i64{};
        std::array<double, MAX_AGG_COLS> delta_f64{};
    };

    struct InternalPool {
        static constexpr size_t MAX_POOLED_NODES = 4096;
        std::mutex pool_mu_;
        std::vector<InternalNode*> free_list_;

        InternalPool();
        ~InternalPool();

        InternalPool(const InternalPool&) = delete;
        InternalPool& operator=(const InternalPool&) = delete;

        InternalNode* alloc_internal();
        void recycle_internal(InternalNode* n) noexcept;
    };

    struct LeafPool {
        static constexpr size_t MAX_POOLED_NODES = 4096;
        std::mutex pool_mu_;
        std::vector<LeafNode*> free_list_;

        LeafPool();
        ~LeafPool();

        LeafPool(const LeafPool&) = delete;
        LeafPool& operator=(const LeafPool&) = delete;

        LeafNode* alloc_leaf();
        void recycle_leaf(LeafNode* n) noexcept;
    };

    static InternalPool& internal_pool() noexcept;
    static LeafPool& leaf_pool() noexcept;
    static InternalNode* alloc_internal();
    static LeafNode* alloc_leaf();
    static void recycle_internal(InternalNode* n) noexcept;
    static void recycle_leaf(LeafNode* n) noexcept;

    static void add_ref(NodeHeader* n) noexcept;
    static void release_node(NodeHeader* n) noexcept;
    static void rcu_node_deleter(void* p);

    static NodeHeader* clone_node(const NodeHeader* src, uint64_t batch_gen = 0);
    static void recompute_aggregates(NodeHeader* n) noexcept;

    static NodeHeader* bulk_load_empty(const std::vector<BatchEntry>& entries);

    static NodeHeader* upsert_into_root(
        NodeHeader* root,
        int64_t key,
        uint64_t row_id,
        const std::array<int64_t, MAX_AGG_COLS>& i64_vals,
        const std::array<double, MAX_AGG_COLS>& f64_vals,
        uint64_t batch_gen = 0);

    static InsertResult insert_recursive(
        NodeHeader* n,
        int64_t key,
        uint64_t row_id,
        const std::array<int64_t, MAX_AGG_COLS>& i64_vals,
        const std::array<double, MAX_AGG_COLS>& f64_vals,
        uint64_t batch_gen = 0);

    static NodeHeader* erase_recursive(const NodeHeader* n, int64_t key, bool& found);

    static uint64_t range_count_node(const NodeHeader* n, int64_t lo, int64_t hi) noexcept;
    static int64_t range_sum_i64_node(const NodeHeader* n, int64_t lo, int64_t hi, size_t col) noexcept;
    static double range_sum_f64_node(const NodeHeader* n, int64_t lo, int64_t hi, size_t col) noexcept;
    static void scan_node(const NodeHeader* n, std::vector<std::pair<int64_t, uint64_t>>& out);

    void retire_root(NodeHeader* old_root);

    std::atomic<NodeHeader*> root_{nullptr};
    std::mutex write_mu_;
    uint64_t next_batch_gen_ = 0;
    RcuReaper* reaper_ = nullptr;
    cpp26::rcu::rcu_domain& domain_;
};

/// 8-way hash-sharded AugmentedBTree for parallel concurrent writes across shards
/// while preserving lock-free RCU reads and full aggregate/range query semantics.
class ShardedAugmentedBTree {
public:
    static constexpr size_t NUM_SHARDS = 8;
    static constexpr size_t ORDER = AugmentedBTree::ORDER;
    static constexpr size_t MAX_AGG_COLS = AugmentedBTree::MAX_AGG_COLS;
    using BatchEntry = AugmentedBTree::BatchEntry;

    static size_t shard_for_key(int64_t key) noexcept {
        uint64_t x = static_cast<uint64_t>(key) + 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        x = x ^ (x >> 31);
        return static_cast<size_t>(x & (NUM_SHARDS - 1));
    }

    explicit ShardedAugmentedBTree(RcuReaper* reaper = nullptr,
                                   cpp26::rcu::rcu_domain& dom = cpp26::rcu::default_domain());
    ~ShardedAugmentedBTree() = default;

    ShardedAugmentedBTree(const ShardedAugmentedBTree&) = delete;
    ShardedAugmentedBTree& operator=(const ShardedAugmentedBTree&) = delete;

    // ------------------------------------------------------------------------
    // Lock-free RCU Read APIs
    // ------------------------------------------------------------------------

    bool find(int64_t key, uint64_t& out_row_id) const;

    uint64_t total_count() const;
    int64_t total_sum_i64(size_t col = 0) const;
    double total_sum_f64(size_t col = 0) const;

    uint64_t range_count(int64_t lo, int64_t hi) const;
    int64_t range_sum_i64(int64_t lo, int64_t hi, size_t col = 0) const;
    double range_sum_f64(int64_t lo, int64_t hi, size_t col = 0) const;

    void scan_all(std::vector<std::pair<int64_t, uint64_t>>& out) const;

    // ------------------------------------------------------------------------
    // Sharded CoW Write APIs
    // ------------------------------------------------------------------------

    void upsert(int64_t key,
                uint64_t row_id,
                const std::array<int64_t, MAX_AGG_COLS>& i64_vals = {},
                const std::array<double, MAX_AGG_COLS>& f64_vals = {});

    void upsert_batch(const std::vector<BatchEntry>& entries);

    bool erase(int64_t key);
    void clear();

private:
    std::array<std::unique_ptr<AugmentedBTree>, NUM_SHARDS> shards_;
};

} // namespace zephyr::core

#endif // ZEPHYR_CORE_AUGMENTED_BTREE_HPP
