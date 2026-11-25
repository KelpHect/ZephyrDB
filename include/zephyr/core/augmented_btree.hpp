#ifndef ZEPHYR_CORE_AUGMENTED_BTREE_HPP
#define ZEPHYR_CORE_AUGMENTED_BTREE_HPP

#include "zephyr/compat/cpp26.hpp"
#include "zephyr/core/rcu.hpp"

#include <array>
#include <atomic>
#include <climits>
#include <cstddef>
#include <cstdint>
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
    struct Node {
        std::atomic<uint32_t> refcount{1};
        bool is_leaf = true;
        uint16_t num_keys = 0;

        // Augmented aggregates over this subtree
        uint64_t count = 0;
        int64_t min_key = INT64_MAX;
        int64_t max_key = INT64_MIN;
        std::array<int64_t, MAX_AGG_COLS> col_sum_i64{};
        std::array<double, MAX_AGG_COLS> col_sum_f64{};

        // Separator keys (for internal) or entry keys (for leaf)
        std::array<int64_t, ORDER> keys{};

        // Leaf payload
        std::array<uint64_t, ORDER> row_ids{};
        std::array<std::array<int64_t, MAX_AGG_COLS>, ORDER> row_i64{};
        std::array<std::array<double, MAX_AGG_COLS>, ORDER> row_f64{};

        // Internal children
        std::array<Node*, ORDER + 1> children{};
    };

    struct InsertResult {
        Node* left = nullptr;
        Node* right = nullptr;
        int64_t split_key = 0;
        bool did_split = false;
    };

    struct NodePool {
        static constexpr size_t MAX_POOLED_NODES = 4096;
        std::mutex pool_mu_;
        std::vector<Node*> free_list_;

        NodePool();
        ~NodePool();

        NodePool(const NodePool&) = delete;
        NodePool& operator=(const NodePool&) = delete;

        Node* alloc_node();
        void recycle_node(Node* n) noexcept;
    };

    static NodePool& node_pool() noexcept;
    static Node* alloc_node();
    static void recycle_node(Node* n) noexcept;

    static void add_ref(Node* n) noexcept;
    static void release_node(Node* n) noexcept;
    static void rcu_node_deleter(void* p);

    static Node* clone_node(const Node* src);
    static void recompute_aggregates(Node* n) noexcept;

    static Node* upsert_into_root(
        const Node* root,
        int64_t key,
        uint64_t row_id,
        const std::array<int64_t, MAX_AGG_COLS>& i64_vals,
        const std::array<double, MAX_AGG_COLS>& f64_vals);

    static InsertResult insert_recursive(
        const Node* n,
        int64_t key,
        uint64_t row_id,
        const std::array<int64_t, MAX_AGG_COLS>& i64_vals,
        const std::array<double, MAX_AGG_COLS>& f64_vals);

    static Node* erase_recursive(const Node* n, int64_t key, bool& found);

    static uint64_t range_count_node(const Node* n, int64_t lo, int64_t hi) noexcept;
    static int64_t range_sum_i64_node(const Node* n, int64_t lo, int64_t hi, size_t col) noexcept;
    static double range_sum_f64_node(const Node* n, int64_t lo, int64_t hi, size_t col) noexcept;
    static void scan_node(const Node* n, std::vector<std::pair<int64_t, uint64_t>>& out);

    void retire_root(Node* old_root);

    std::atomic<Node*> root_{nullptr};
    std::mutex write_mu_;
    RcuReaper* reaper_ = nullptr;
    cpp26::rcu::rcu_domain& domain_;
};

} // namespace zephyr::core

#endif // ZEPHYR_CORE_AUGMENTED_BTREE_HPP
