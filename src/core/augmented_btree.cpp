#include "zephyr/core/augmented_btree.hpp"

#include <algorithm>

namespace zephyr::core {

namespace {

// Branchless binary search for small sorted arrays (up to ORDER = 32 keys).
// Compiles to cmp + cmov with zero branch mispredictions.
inline size_t fast_lower_bound(const int64_t* keys, size_t n, int64_t key) noexcept {
    size_t base = 0;
    size_t len = n;
    while (len > 1) {
        size_t half = len >> 1;
        base += (keys[base + half] < key) ? half : 0;
        len -= half;
    }
    return base + (len > 0 && keys[base] < key ? 1 : 0);
}

inline size_t fast_upper_bound(const int64_t* keys, size_t n, int64_t key) noexcept {
    size_t base = 0;
    size_t len = n;
    while (len > 1) {
        size_t half = len >> 1;
        base += (keys[base + half] <= key) ? half : 0;
        len -= half;
    }
    return base + (len > 0 && keys[base] <= key ? 1 : 0);
}

} // namespace

// ----------------------------------------------------------------------------
// InternalPool & LeafPool
// ----------------------------------------------------------------------------

AugmentedBTree::InternalPool::InternalPool() {
    free_list_.reserve(256);
}

AugmentedBTree::InternalPool::~InternalPool() {
    std::lock_guard<std::mutex> lk(pool_mu_);
    for (InternalNode* n : free_list_) {
        delete n;
    }
    free_list_.clear();
}

AugmentedBTree::InternalNode* AugmentedBTree::InternalPool::alloc_internal() {
    InternalNode* n = nullptr;
    {
        std::lock_guard<std::mutex> lk(pool_mu_);
        if (!free_list_.empty()) {
            n = free_list_.back();
            free_list_.pop_back();
        }
    }
    if (!n) {
        n = new InternalNode();
    }
    return n;
}

void AugmentedBTree::InternalPool::recycle_internal(InternalNode* n) noexcept {
    if (!n) return;
    n->refcount.store(1, std::memory_order_relaxed);
    n->is_leaf = false;
    n->num_keys = 0;
    n->batch_gen = 0;
    n->count = 0;
    n->min_key = INT64_MAX;
    n->max_key = INT64_MIN;
    n->col_sum_i64.fill(0);
    n->col_sum_f64.fill(0.0);
    n->children.fill(nullptr);
    {
        std::lock_guard<std::mutex> lk(pool_mu_);
        if (free_list_.size() < MAX_POOLED_NODES) {
            free_list_.push_back(n);
            return;
        }
    }
    delete n;
}

AugmentedBTree::LeafPool::LeafPool() {
    free_list_.reserve(256);
}

AugmentedBTree::LeafPool::~LeafPool() {
    std::lock_guard<std::mutex> lk(pool_mu_);
    for (LeafNode* n : free_list_) {
        delete n;
    }
    free_list_.clear();
}

AugmentedBTree::LeafNode* AugmentedBTree::LeafPool::alloc_leaf() {
    LeafNode* n = nullptr;
    {
        std::lock_guard<std::mutex> lk(pool_mu_);
        if (!free_list_.empty()) {
            n = free_list_.back();
            free_list_.pop_back();
        }
    }
    if (!n) {
        n = new LeafNode();
    }
    return n;
}

void AugmentedBTree::LeafPool::recycle_leaf(LeafNode* n) noexcept {
    if (!n) return;
    delete n->extra;
    n->extra = nullptr;
    n->refcount.store(1, std::memory_order_relaxed);
    n->is_leaf = true;
    n->num_keys = 0;
    n->batch_gen = 0;
    n->count = 0;
    n->min_key = INT64_MAX;
    n->max_key = INT64_MIN;
    n->col_sum_i64.fill(0);
    n->col_sum_f64.fill(0.0);
    {
        std::lock_guard<std::mutex> lk(pool_mu_);
        if (free_list_.size() < MAX_POOLED_NODES) {
            free_list_.push_back(n);
            return;
        }
    }
    delete n;
}

AugmentedBTree::InternalPool& AugmentedBTree::internal_pool() noexcept {
    static InternalPool pool;
    return pool;
}

AugmentedBTree::LeafPool& AugmentedBTree::leaf_pool() noexcept {
    static LeafPool pool;
    return pool;
}

AugmentedBTree::InternalNode* AugmentedBTree::alloc_internal() {
    return internal_pool().alloc_internal();
}

AugmentedBTree::LeafNode* AugmentedBTree::alloc_leaf() {
    return leaf_pool().alloc_leaf();
}

void AugmentedBTree::recycle_internal(InternalNode* n) noexcept {
    internal_pool().recycle_internal(n);
}

void AugmentedBTree::recycle_leaf(LeafNode* n) noexcept {
    leaf_pool().recycle_leaf(n);
}

// ----------------------------------------------------------------------------
// Lifecycle & Refcounting
// ----------------------------------------------------------------------------

AugmentedBTree::AugmentedBTree(RcuReaper* reaper, cpp26::rcu::rcu_domain& dom)
    : reaper_(reaper), domain_(dom) {}

AugmentedBTree::~AugmentedBTree() {
    NodeHeader* r = root_.exchange(nullptr, std::memory_order_acq_rel);
    if (r) {
        release_node(r);
    }
}

void AugmentedBTree::add_ref(NodeHeader* n) noexcept {
    if (n) {
        n->refcount.fetch_add(1, std::memory_order_relaxed);
    }
}

void AugmentedBTree::release_node(NodeHeader* n) noexcept {
    if (!n) return;
    if (n->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (n->is_leaf) {
            recycle_leaf(static_cast<LeafNode*>(n));
        } else {
            auto* in = static_cast<InternalNode*>(n);
            for (uint16_t i = 0; i <= in->num_keys; ++i) {
                release_node(in->children[i]);
            }
            recycle_internal(in);
        }
    }
}

void AugmentedBTree::rcu_node_deleter(void* p) {
    release_node(static_cast<NodeHeader*>(p));
}

void AugmentedBTree::retire_root(NodeHeader* old_root) {
    if (!old_root) return;
    if (reaper_) {
        reaper_->retire(old_root, &rcu_node_deleter);
    } else {
        domain_.retire(old_root, &rcu_node_deleter);
    }
}

AugmentedBTree::NodeHeader* AugmentedBTree::clone_node(const NodeHeader* src, uint64_t batch_gen) {
    if (src->is_leaf) {
        const auto* s = static_cast<const LeafNode*>(src);
        LeafNode* dst = alloc_leaf();
        dst->refcount.store(1, std::memory_order_relaxed);
        dst->is_leaf = true;
        dst->num_keys = s->num_keys;
        dst->batch_gen = batch_gen;
        dst->count = s->count;
        dst->min_key = s->min_key;
        dst->max_key = s->max_key;
        dst->col_sum_i64 = s->col_sum_i64;
        dst->col_sum_f64 = s->col_sum_f64;
        dst->keys = s->keys;
        dst->row_ids = s->row_ids;
        dst->row_i64_inline = s->row_i64_inline;
        dst->row_f64_inline = s->row_f64_inline;
        if (s->extra) {
            dst->extra = new LeafNode::ExtraCols(*s->extra);
        }
        return dst;
    }

    const auto* s = static_cast<const InternalNode*>(src);
    InternalNode* dst = alloc_internal();
    dst->refcount.store(1, std::memory_order_relaxed);
    dst->is_leaf = false;
    dst->num_keys = s->num_keys;
    dst->batch_gen = batch_gen;
    dst->count = s->count;
    dst->min_key = s->min_key;
    dst->max_key = s->max_key;
    dst->col_sum_i64 = s->col_sum_i64;
    dst->col_sum_f64 = s->col_sum_f64;
    dst->keys = s->keys;
    for (uint16_t i = 0; i <= s->num_keys; ++i) {
        dst->children[i] = s->children[i];
        add_ref(dst->children[i]);
    }
    return dst;
}

void AugmentedBTree::recompute_aggregates(NodeHeader* n) noexcept {
    n->count = 0;
    n->min_key = INT64_MAX;
    n->max_key = INT64_MIN;
    n->col_sum_i64.fill(0);
    n->col_sum_f64.fill(0.0);

    if (n->is_leaf) {
        auto* leaf = static_cast<LeafNode*>(n);
        leaf->count = leaf->num_keys;
        if (leaf->num_keys > 0) {
            leaf->min_key = leaf->keys[0];
            leaf->max_key = leaf->keys[leaf->num_keys - 1];
            for (uint16_t i = 0; i < leaf->num_keys; ++i) {
                leaf->col_sum_i64[0] += leaf->row_i64_inline[i][0];
                leaf->col_sum_i64[1] += leaf->row_i64_inline[i][1];
                leaf->col_sum_f64[0] += leaf->row_f64_inline[i][0];
                leaf->col_sum_f64[1] += leaf->row_f64_inline[i][1];
            }
            if (leaf->extra) {
                for (uint16_t i = 0; i < leaf->num_keys; ++i) {
                    for (size_t c = 0; c < EXTRA_AGG_COLS; ++c) {
                        leaf->col_sum_i64[INLINE_AGG_COLS + c] += leaf->extra->i64[i][c];
                        leaf->col_sum_f64[INLINE_AGG_COLS + c] += leaf->extra->f64[i][c];
                    }
                }
            }
        }
    } else {
        auto* in = static_cast<InternalNode*>(n);
        for (uint16_t i = 0; i <= in->num_keys; ++i) {
            const NodeHeader* child = in->children[i];
            if (!child || child->count == 0) continue;
            in->count += child->count;
            if (child->min_key < in->min_key) in->min_key = child->min_key;
            if (child->max_key > in->max_key) in->max_key = child->max_key;
            for (size_t c = 0; c < MAX_AGG_COLS; ++c) {
                in->col_sum_i64[c] += child->col_sum_i64[c];
                in->col_sum_f64[c] += child->col_sum_f64[c];
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Lock-free RCU Reads
// ----------------------------------------------------------------------------

bool AugmentedBTree::find(int64_t key, uint64_t& out_row_id) const {
    cpp26::rcu::scoped_read_lock guard(domain_);
    const NodeHeader* cur = root_.load(std::memory_order_acquire);
    if (!cur) return false;

    while (!cur->is_leaf) {
        const auto* in = static_cast<const InternalNode*>(cur);
        const size_t idx = fast_upper_bound(in->keys.data(), in->num_keys, key);
        cur = in->children[idx];
        if (!cur) return false;
    }

    const auto* leaf = static_cast<const LeafNode*>(cur);
    const size_t pos = fast_lower_bound(leaf->keys.data(), leaf->num_keys, key);
    if (pos < leaf->num_keys && leaf->keys[pos] == key) {
        out_row_id = leaf->row_ids[pos];
        return true;
    }
    return false;
}

uint64_t AugmentedBTree::total_count() const {
    cpp26::rcu::scoped_read_lock guard(domain_);
    const NodeHeader* r = root_.load(std::memory_order_acquire);
    return r ? r->count : 0;
}

int64_t AugmentedBTree::total_sum_i64(size_t col) const {
    if (col >= MAX_AGG_COLS) return 0;
    cpp26::rcu::scoped_read_lock guard(domain_);
    const NodeHeader* r = root_.load(std::memory_order_acquire);
    return r ? r->col_sum_i64[col] : 0;
}

double AugmentedBTree::total_sum_f64(size_t col) const {
    if (col >= MAX_AGG_COLS) return 0.0;
    cpp26::rcu::scoped_read_lock guard(domain_);
    const NodeHeader* r = root_.load(std::memory_order_acquire);
    return r ? r->col_sum_f64[col] : 0.0;
}

uint64_t AugmentedBTree::range_count_node(const NodeHeader* n, int64_t lo, int64_t hi) noexcept {
    if (!n || n->count == 0 || n->max_key < lo || n->min_key > hi) {
        return 0;
    }
    if (n->min_key >= lo && n->max_key <= hi) {
        return n->count;
    }
    if (n->is_leaf) {
        const size_t lo_idx = fast_lower_bound(n->keys.data(), n->num_keys, lo);
        const size_t hi_idx = lo_idx + fast_upper_bound(n->keys.data() + lo_idx, n->num_keys - lo_idx, hi);
        return static_cast<uint64_t>(hi_idx - lo_idx);
    }
    const auto* in = static_cast<const InternalNode*>(n);
    uint64_t sum = 0;
    for (uint16_t i = 0; i <= in->num_keys; ++i) {
        if (i < in->num_keys && in->keys[i] < lo) continue;
        if (i > 0 && in->keys[i - 1] > hi) break;
        sum += range_count_node(in->children[i], lo, hi);
    }
    return sum;
}

int64_t AugmentedBTree::range_sum_i64_node(const NodeHeader* n, int64_t lo, int64_t hi, size_t col) noexcept {
    if (!n || n->count == 0 || n->max_key < lo || n->min_key > hi) {
        return 0;
    }
    if (n->min_key >= lo && n->max_key <= hi) {
        return n->col_sum_i64[col];
    }
    if (n->is_leaf) {
        const auto* leaf = static_cast<const LeafNode*>(n);
        if (col >= INLINE_AGG_COLS && !leaf->extra) {
            return 0;
        }
        const size_t start = fast_lower_bound(leaf->keys.data(), leaf->num_keys, lo);
        int64_t sum = 0;
        if (col < INLINE_AGG_COLS) {
            for (size_t i = start; i < leaf->num_keys && leaf->keys[i] <= hi; ++i) {
                sum += leaf->row_i64_inline[i][col];
            }
        } else {
            const size_t ec = col - INLINE_AGG_COLS;
            for (size_t i = start; i < leaf->num_keys && leaf->keys[i] <= hi; ++i) {
                sum += leaf->extra->i64[i][ec];
            }
        }
        return sum;
    }
    const auto* in = static_cast<const InternalNode*>(n);
    int64_t sum = 0;
    for (uint16_t i = 0; i <= in->num_keys; ++i) {
        if (i < in->num_keys && in->keys[i] < lo) continue;
        if (i > 0 && in->keys[i - 1] > hi) break;
        sum += range_sum_i64_node(in->children[i], lo, hi, col);
    }
    return sum;
}

double AugmentedBTree::range_sum_f64_node(const NodeHeader* n, int64_t lo, int64_t hi, size_t col) noexcept {
    if (!n || n->count == 0 || n->max_key < lo || n->min_key > hi) {
        return 0.0;
    }
    if (n->min_key >= lo && n->max_key <= hi) {
        return n->col_sum_f64[col];
    }
    if (n->is_leaf) {
        const auto* leaf = static_cast<const LeafNode*>(n);
        if (col >= INLINE_AGG_COLS && !leaf->extra) {
            return 0.0;
        }
        const size_t start = fast_lower_bound(leaf->keys.data(), leaf->num_keys, lo);
        double sum = 0.0;
        if (col < INLINE_AGG_COLS) {
            for (size_t i = start; i < leaf->num_keys && leaf->keys[i] <= hi; ++i) {
                sum += leaf->row_f64_inline[i][col];
            }
        } else {
            const size_t ec = col - INLINE_AGG_COLS;
            for (size_t i = start; i < leaf->num_keys && leaf->keys[i] <= hi; ++i) {
                sum += leaf->extra->f64[i][ec];
            }
        }
        return sum;
    }
    const auto* in = static_cast<const InternalNode*>(n);
    double sum = 0.0;
    for (uint16_t i = 0; i <= in->num_keys; ++i) {
        if (i < in->num_keys && in->keys[i] < lo) continue;
        if (i > 0 && in->keys[i - 1] > hi) break;
        sum += range_sum_f64_node(in->children[i], lo, hi, col);
    }
    return sum;
}

uint64_t AugmentedBTree::range_count(int64_t lo, int64_t hi) const {
    if (lo > hi) return 0;
    cpp26::rcu::scoped_read_lock guard(domain_);
    return range_count_node(root_.load(std::memory_order_acquire), lo, hi);
}

int64_t AugmentedBTree::range_sum_i64(int64_t lo, int64_t hi, size_t col) const {
    if (lo > hi || col >= MAX_AGG_COLS) return 0;
    cpp26::rcu::scoped_read_lock guard(domain_);
    return range_sum_i64_node(root_.load(std::memory_order_acquire), lo, hi, col);
}

double AugmentedBTree::range_sum_f64(int64_t lo, int64_t hi, size_t col) const {
    if (lo > hi || col >= MAX_AGG_COLS) return 0.0;
    cpp26::rcu::scoped_read_lock guard(domain_);
    return range_sum_f64_node(root_.load(std::memory_order_acquire), lo, hi, col);
}

void AugmentedBTree::scan_node(const NodeHeader* n, std::vector<std::pair<int64_t, uint64_t>>& out) {
    if (!n) return;
    if (n->is_leaf) {
        const auto* leaf = static_cast<const LeafNode*>(n);
        for (uint16_t i = 0; i < leaf->num_keys; ++i) {
            out.emplace_back(leaf->keys[i], leaf->row_ids[i]);
        }
    } else {
        const auto* in = static_cast<const InternalNode*>(n);
        for (uint16_t i = 0; i <= in->num_keys; ++i) {
            scan_node(in->children[i], out);
        }
    }
}

void AugmentedBTree::scan_all(std::vector<std::pair<int64_t, uint64_t>>& out) const {
    out.clear();
    cpp26::rcu::scoped_read_lock guard(domain_);
    const NodeHeader* r = root_.load(std::memory_order_acquire);
    if (r) {
        out.reserve(r->count);
        scan_node(r, out);
    }
}

// ----------------------------------------------------------------------------
// Serialized CoW Writes
// ----------------------------------------------------------------------------

AugmentedBTree::InsertResult AugmentedBTree::insert_recursive(
    NodeHeader* n,
    int64_t key,
    uint64_t row_id,
    const std::array<int64_t, MAX_AGG_COLS>& i64_vals,
    const std::array<double, MAX_AGG_COLS>& f64_vals,
    uint64_t batch_gen) {

    const bool can_mutate =
        (batch_gen != 0 && n->batch_gen == batch_gen &&
         n->refcount.load(std::memory_order_relaxed) == 1);

    if (n->is_leaf) {
        auto* leaf = static_cast<LeafNode*>(n);
        const size_t pos = fast_lower_bound(leaf->keys.data(), leaf->num_keys, key);

        // Update existing key with O(1) incremental aggregate delta
        if (pos < leaf->num_keys && leaf->keys[pos] == key) {
            LeafNode* target = can_mutate ? leaf : static_cast<LeafNode*>(clone_node(leaf, batch_gen));
            InsertResult res{};
            res.left = target;
            res.delta_count = 0;
            for (size_t c = 0; c < MAX_AGG_COLS; ++c) {
                const int64_t di = i64_vals[c] - target->get_row_i64(pos, c);
                const double df = f64_vals[c] - target->get_row_f64(pos, c);
                res.delta_i64[c] = di;
                res.delta_f64[c] = df;
                target->col_sum_i64[c] += di;
                target->col_sum_f64[c] += df;
            }
            target->row_ids[pos] = row_id;
            target->set_row_vals(pos, i64_vals, f64_vals);
            return res;
        }

        // Insert into non-full leaf with O(1) incremental aggregate delta
        if (leaf->num_keys < ORDER) {
            LeafNode* target = can_mutate ? leaf : static_cast<LeafNode*>(clone_node(leaf, batch_gen));
            for (size_t i = target->num_keys; i > pos; --i) {
                target->keys[i] = target->keys[i - 1];
                target->row_ids[i] = target->row_ids[i - 1];
                target->row_i64_inline[i] = target->row_i64_inline[i - 1];
                target->row_f64_inline[i] = target->row_f64_inline[i - 1];
            }
            if (target->extra) {
                for (size_t i = target->num_keys; i > pos; --i) {
                    target->extra->i64[i] = target->extra->i64[i - 1];
                    target->extra->f64[i] = target->extra->f64[i - 1];
                }
            }
            target->keys[pos] = key;
            target->row_ids[pos] = row_id;
            target->set_row_vals(pos, i64_vals, f64_vals);
            ++target->num_keys;
            ++target->count;
            if (key < target->min_key) target->min_key = key;
            if (key > target->max_key) target->max_key = key;
            for (size_t c = 0; c < MAX_AGG_COLS; ++c) {
                target->col_sum_i64[c] += i64_vals[c];
                target->col_sum_f64[c] += f64_vals[c];
            }
            return {target, nullptr, 0, false, 1, i64_vals, f64_vals};
        }

        // Split full leaf directly into freshly allocated left and right leaves.
        // `leaf` is never mutated here, so we avoid temporary stack buffers.
        constexpr size_t mid = (ORDER + 1) / 2;

        auto populate_split_leaf = [&](LeafNode* dst, size_t merged_begin, size_t merged_end) {
            dst->is_leaf = true;
            dst->batch_gen = batch_gen;
            dst->num_keys = static_cast<uint16_t>(merged_end - merged_begin);
            for (size_t m = merged_begin; m < merged_end; ++m) {
                const size_t d = m - merged_begin;
                if (m == pos) {
                    dst->keys[d] = key;
                    dst->row_ids[d] = row_id;
                    dst->set_row_vals(d, i64_vals, f64_vals);
                } else {
                    const size_t s = (m < pos) ? m : (m - 1);
                    dst->keys[d] = leaf->keys[s];
                    dst->row_ids[d] = leaf->row_ids[s];
                    dst->row_i64_inline[d] = leaf->row_i64_inline[s];
                    dst->row_f64_inline[d] = leaf->row_f64_inline[s];
                    if (leaf->extra) {
                        bool row_nonzero = false;
                        if (!dst->extra) {
                            for (size_t c = 0; c < EXTRA_AGG_COLS; ++c) {
                                if (leaf->extra->i64[s][c] != 0 || leaf->extra->f64[s][c] != 0.0) {
                                    row_nonzero = true;
                                    break;
                                }
                            }
                            if (row_nonzero) {
                                dst->extra = new LeafNode::ExtraCols{};
                            }
                        }
                        if (dst->extra) {
                            dst->extra->i64[d] = leaf->extra->i64[s];
                            dst->extra->f64[d] = leaf->extra->f64[s];
                        }
                    }
                }
            }
        };

        LeafNode* left = alloc_leaf();
        LeafNode* right = alloc_leaf();
        populate_split_leaf(left, 0, mid);
        populate_split_leaf(right, mid, ORDER + 1);

        recompute_aggregates(left);
        recompute_aggregates(right);
        return {left, right, right->keys[0], true, 1, i64_vals, f64_vals};
    }

    // Internal node
    auto* in = static_cast<InternalNode*>(n);
    const size_t idx = fast_upper_bound(in->keys.data(), in->num_keys, key);

    InsertResult child_res = insert_recursive(
        in->children[idx], key, row_id, i64_vals, f64_vals, batch_gen);

    if (!child_res.did_split) {
        InternalNode* target = can_mutate ? in : static_cast<InternalNode*>(clone_node(in, batch_gen));
        if (target->children[idx] != child_res.left) {
            release_node(target->children[idx]);
            target->children[idx] = child_res.left;
        }
        target->count += child_res.delta_count;
        if (key < target->min_key) target->min_key = key;
        if (key > target->max_key) target->max_key = key;
        for (size_t c = 0; c < MAX_AGG_COLS; ++c) {
            target->col_sum_i64[c] += child_res.delta_i64[c];
            target->col_sum_f64[c] += child_res.delta_f64[c];
        }
        return {target, nullptr, 0, false,
                child_res.delta_count, child_res.delta_i64, child_res.delta_f64};
    }

    if (in->num_keys < ORDER) {
        InternalNode* target = can_mutate ? in : static_cast<InternalNode*>(clone_node(in, batch_gen));
        release_node(target->children[idx]);

        for (size_t i = target->num_keys; i > idx; --i) {
            target->keys[i] = target->keys[i - 1];
            target->children[i + 1] = target->children[i];
        }
        target->keys[idx] = child_res.split_key;
        target->children[idx] = child_res.left;
        target->children[idx + 1] = child_res.right;
        ++target->num_keys;
        target->count += child_res.delta_count;
        if (key < target->min_key) target->min_key = key;
        if (key > target->max_key) target->max_key = key;
        for (size_t c = 0; c < MAX_AGG_COLS; ++c) {
            target->col_sum_i64[c] += child_res.delta_i64[c];
            target->col_sum_f64[c] += child_res.delta_f64[c];
        }
        return {target, nullptr, 0, false,
                child_res.delta_count, child_res.delta_i64, child_res.delta_f64};
    }

    // Split full internal node
    std::array<int64_t, ORDER + 1> tmp_keys{};
    std::array<NodeHeader*, ORDER + 2> tmp_children{};

    for (size_t i = 0; i < idx; ++i) {
        tmp_keys[i] = in->keys[i];
    }
    tmp_keys[idx] = child_res.split_key;
    for (size_t i = idx; i < ORDER; ++i) {
        tmp_keys[i + 1] = in->keys[i];
    }

    for (size_t i = 0; i <= ORDER; ++i) {
        if (i < idx) {
            tmp_children[i] = in->children[i];
            add_ref(tmp_children[i]);
        } else if (i == idx) {
            tmp_children[idx] = child_res.left;
            tmp_children[idx + 1] = child_res.right;
        } else {
            tmp_children[i + 1] = in->children[i];
            add_ref(tmp_children[i + 1]);
        }
    }

    constexpr size_t mid = ORDER / 2;
    const int64_t promoted_key = tmp_keys[mid];

    InternalNode* left = alloc_internal();
    left->is_leaf = false;
    left->batch_gen = batch_gen;
    left->num_keys = static_cast<uint16_t>(mid);
    for (size_t i = 0; i < mid; ++i) {
        left->keys[i] = tmp_keys[i];
    }
    for (size_t i = 0; i <= mid; ++i) {
        left->children[i] = tmp_children[i];
    }

    InternalNode* right = alloc_internal();
    right->is_leaf = false;
    right->batch_gen = batch_gen;
    right->num_keys = static_cast<uint16_t>(ORDER - mid);
    for (size_t i = mid + 1; i <= ORDER; ++i) {
        right->keys[i - (mid + 1)] = tmp_keys[i];
    }
    for (size_t i = mid + 1; i <= ORDER + 1; ++i) {
        right->children[i - (mid + 1)] = tmp_children[i];
    }

    recompute_aggregates(left);
    recompute_aggregates(right);
    return {left, right, promoted_key, true,
            child_res.delta_count, child_res.delta_i64, child_res.delta_f64};
}

AugmentedBTree::NodeHeader* AugmentedBTree::bulk_load_empty(const std::vector<BatchEntry>& entries) {
    if (entries.empty()) return nullptr;

    // Fast path: check if entries are already strictly sorted (no duplicates).
    bool strictly_sorted = true;
    for (size_t i = 1; i < entries.size(); ++i) {
        if (entries[i - 1].key >= entries[i].key) {
            strictly_sorted = false;
            break;
        }
    }

    std::vector<const BatchEntry*> sorted_ptrs;
    size_t n_unique = entries.size();
    if (!strictly_sorted) {
        sorted_ptrs.resize(entries.size());
        for (size_t i = 0; i < entries.size(); ++i) {
            sorted_ptrs[i] = &entries[i];
        }
        std::stable_sort(sorted_ptrs.begin(), sorted_ptrs.end(),
                         [](const BatchEntry* a, const BatchEntry* b) {
                             return a->key < b->key;
                         });
        // Deduplicate: last-wins for identical keys.
        size_t write_idx = 0;
        for (size_t i = 0; i < sorted_ptrs.size(); ++i) {
            if (i + 1 < sorted_ptrs.size() && sorted_ptrs[i]->key == sorted_ptrs[i + 1]->key) {
                continue;
            }
            sorted_ptrs[write_idx++] = sorted_ptrs[i];
        }
        sorted_ptrs.resize(write_idx);
        n_unique = write_idx;
    }

    auto get_entry = [&](size_t idx) -> const BatchEntry& {
        return strictly_sorted ? entries[idx] : *sorted_ptrs[idx];
    };

    // Pack leaf nodes directly (28/32 fill factor leaves headroom for future inserts).
    constexpr size_t LEAF_FILL = 28;
    const size_t num_leaves = (n_unique + LEAF_FILL - 1) / LEAF_FILL;
    std::vector<NodeHeader*> level;
    level.reserve(num_leaves);

    for (size_t l = 0; l < num_leaves; ++l) {
        const size_t start = l * LEAF_FILL;
        const size_t end = std::min(start + LEAF_FILL, n_unique);
        const size_t cnt = end - start;

        LeafNode* leaf = alloc_leaf();
        leaf->is_leaf = true;
        leaf->batch_gen = 0;
        leaf->num_keys = static_cast<uint16_t>(cnt);
        leaf->count = cnt;
        leaf->min_key = get_entry(start).key;
        leaf->max_key = get_entry(end - 1).key;
        leaf->col_sum_i64.fill(0);
        leaf->col_sum_f64.fill(0.0);

        for (size_t j = 0; j < cnt; ++j) {
            const BatchEntry& e = get_entry(start + j);
            leaf->keys[j] = e.key;
            leaf->row_ids[j] = e.row_id;
            leaf->set_row_vals(j, e.i64_vals, e.f64_vals);
            for (size_t c = 0; c < MAX_AGG_COLS; ++c) {
                leaf->col_sum_i64[c] += e.i64_vals[c];
                leaf->col_sum_f64[c] += e.f64_vals[c];
            }
        }
        level.push_back(leaf);
    }

    // Build internal levels bottom-up in O(N) time with zero path-copying.
    constexpr size_t INTERNAL_FILL_CHILDREN = 29; // 28 separator keys <= ORDER (32)
    while (level.size() > 1) {
        const size_t total = level.size();
        const size_t num_parents = (total + INTERNAL_FILL_CHILDREN - 1) / INTERNAL_FILL_CHILDREN;
        std::vector<NodeHeader*> next_level;
        next_level.reserve(num_parents);

        size_t offset = 0;
        for (size_t p = 0; p < num_parents; ++p) {
            const size_t remaining_children = total - offset;
            const size_t remaining_parents = num_parents - p;
            // Avoid a 1-child trailing internal node by balancing the final two parents.
            size_t cnt = std::min(INTERNAL_FILL_CHILDREN, remaining_children);
            if (remaining_parents == 2 && remaining_children == INTERNAL_FILL_CHILDREN + 1) {
                cnt = remaining_children / 2;
            }

            InternalNode* parent = alloc_internal();
            parent->is_leaf = false;
            parent->batch_gen = 0;
            parent->num_keys = static_cast<uint16_t>(cnt - 1);
            parent->count = 0;
            parent->min_key = level[offset]->min_key;
            parent->max_key = level[offset + cnt - 1]->max_key;
            parent->col_sum_i64.fill(0);
            parent->col_sum_f64.fill(0.0);

            for (size_t j = 0; j < cnt; ++j) {
                NodeHeader* child = level[offset + j];
                parent->children[j] = child;
                if (j > 0) {
                    parent->keys[j - 1] = child->min_key;
                }
                parent->count += child->count;
                for (size_t c = 0; c < MAX_AGG_COLS; ++c) {
                    parent->col_sum_i64[c] += child->col_sum_i64[c];
                    parent->col_sum_f64[c] += child->col_sum_f64[c];
                }
            }
            offset += cnt;
            next_level.push_back(parent);
        }
        level = std::move(next_level);
    }

    return level[0];
}

AugmentedBTree::NodeHeader* AugmentedBTree::upsert_into_root(
    NodeHeader* root,
    int64_t key,
    uint64_t row_id,
    const std::array<int64_t, MAX_AGG_COLS>& i64_vals,
    const std::array<double, MAX_AGG_COLS>& f64_vals,
    uint64_t batch_gen) {
    if (!root) {
        LeafNode* leaf = alloc_leaf();
        leaf->is_leaf = true;
        leaf->batch_gen = batch_gen;
        leaf->num_keys = 1;
        leaf->keys[0] = key;
        leaf->row_ids[0] = row_id;
        leaf->set_row_vals(0, i64_vals, f64_vals);
        leaf->count = 1;
        leaf->min_key = key;
        leaf->max_key = key;
        leaf->col_sum_i64 = i64_vals;
        leaf->col_sum_f64 = f64_vals;
        return leaf;
    }

    InsertResult res = insert_recursive(root, key, row_id, i64_vals, f64_vals, batch_gen);
    if (!res.did_split) {
        return res.left;
    }

    InternalNode* new_root = alloc_internal();
    new_root->is_leaf = false;
    new_root->batch_gen = batch_gen;
    new_root->num_keys = 1;
    new_root->keys[0] = res.split_key;
    new_root->children[0] = res.left;
    new_root->children[1] = res.right;
    recompute_aggregates(new_root);
    return new_root;
}

void AugmentedBTree::upsert(int64_t key,
                            uint64_t row_id,
                            const std::array<int64_t, MAX_AGG_COLS>& i64_vals,
                            const std::array<double, MAX_AGG_COLS>& f64_vals) {
    std::lock_guard<std::mutex> lk(write_mu_);
    NodeHeader* old_root = root_.load(std::memory_order_acquire);
    NodeHeader* new_root = upsert_into_root(old_root, key, row_id, i64_vals, f64_vals, 0);
    root_.store(new_root, std::memory_order_release);
    retire_root(old_root);
}

void AugmentedBTree::upsert_batch(const std::vector<BatchEntry>& entries) {
    if (entries.empty()) return;

    std::lock_guard<std::mutex> lk(write_mu_);
    NodeHeader* old_root = root_.load(std::memory_order_acquire);

    // O(N) bottom-up bulk load when inserting into an empty tree.
    if (old_root == nullptr) {
        NodeHeader* new_root = bulk_load_empty(entries);
        root_.store(new_root, std::memory_order_release);
        return;
    }

    // In-place batch mutation for non-empty trees: nodes created with cur_gen
    // are mutated in-place without re-cloning until the batch root is published.
    const uint64_t cur_gen = ++next_batch_gen_;
    NodeHeader* working_root = old_root;

    for (const auto& entry : entries) {
        NodeHeader* next_root = upsert_into_root(
            working_root, entry.key, entry.row_id, entry.i64_vals, entry.f64_vals, cur_gen);
        if (next_root != working_root && working_root != old_root) {
            release_node(working_root);
        }
        working_root = next_root;
    }

    root_.store(working_root, std::memory_order_release);
    retire_root(old_root);
}

AugmentedBTree::NodeHeader* AugmentedBTree::erase_recursive(const NodeHeader* n, int64_t key, bool& found) {
    if (n->is_leaf) {
        const auto* leaf = static_cast<const LeafNode*>(n);
        const size_t pos = fast_lower_bound(leaf->keys.data(), leaf->num_keys, key);
        if (pos >= leaf->num_keys || leaf->keys[pos] != key) {
            found = false;
            return nullptr;
        }
        found = true;
        if (leaf->num_keys == 1) {
            return nullptr;
        }
        auto* clone = static_cast<LeafNode*>(clone_node(leaf));
        for (size_t i = pos; i + 1 < clone->num_keys; ++i) {
            clone->keys[i] = clone->keys[i + 1];
            clone->row_ids[i] = clone->row_ids[i + 1];
            clone->row_i64_inline[i] = clone->row_i64_inline[i + 1];
            clone->row_f64_inline[i] = clone->row_f64_inline[i + 1];
        }
        if (clone->extra) {
            for (size_t i = pos; i + 1 < clone->num_keys; ++i) {
                clone->extra->i64[i] = clone->extra->i64[i + 1];
                clone->extra->f64[i] = clone->extra->f64[i + 1];
            }
            clone->extra->i64[clone->num_keys - 1].fill(0);
            clone->extra->f64[clone->num_keys - 1].fill(0.0);
        }
        --clone->num_keys;
        recompute_aggregates(clone);
        return clone;
    }

    const auto* in = static_cast<const InternalNode*>(n);
    const size_t idx = fast_upper_bound(in->keys.data(), in->num_keys, key);

    NodeHeader* new_child = erase_recursive(in->children[idx], key, found);
    if (!found) {
        return nullptr;
    }

    if (new_child != nullptr) {
        auto* clone = static_cast<InternalNode*>(clone_node(in));
        release_node(clone->children[idx]);
        clone->children[idx] = new_child;
        recompute_aggregates(clone);
        return clone;
    }

    // Child became empty
    if (in->num_keys == 0) {
        return nullptr;
    }
    if (in->num_keys == 1) {
        NodeHeader* survivor = in->children[1 - idx];
        add_ref(survivor);
        return survivor;
    }

    auto* clone = static_cast<InternalNode*>(clone_node(in));
    release_node(clone->children[idx]);

    const size_t key_remove_idx = (idx == 0) ? 0 : (idx - 1);
    for (size_t i = key_remove_idx; i + 1 < clone->num_keys; ++i) {
        clone->keys[i] = clone->keys[i + 1];
    }
    for (size_t i = idx; i < clone->num_keys; ++i) {
        clone->children[i] = clone->children[i + 1];
    }
    clone->children[clone->num_keys] = nullptr;
    --clone->num_keys;
    recompute_aggregates(clone);
    return clone;
}

bool AugmentedBTree::erase(int64_t key) {
    std::lock_guard<std::mutex> lk(write_mu_);
    NodeHeader* old_root = root_.load(std::memory_order_acquire);
    if (!old_root) return false;

    bool found = false;
    NodeHeader* new_root = erase_recursive(old_root, key, found);
    if (!found) return false;

    root_.store(new_root, std::memory_order_release);
    retire_root(old_root);
    return true;
}

void AugmentedBTree::clear() {
    std::lock_guard<std::mutex> lk(write_mu_);
    NodeHeader* old_root = root_.exchange(nullptr, std::memory_order_acq_rel);
    if (old_root) {
        retire_root(old_root);
    }
}

// ----------------------------------------------------------------------------
// ShardedAugmentedBTree
// ----------------------------------------------------------------------------

ShardedAugmentedBTree::ShardedAugmentedBTree(RcuReaper* reaper, cpp26::rcu::rcu_domain& dom) {
    for (size_t i = 0; i < NUM_SHARDS; ++i) {
        shards_[i] = std::make_unique<AugmentedBTree>(reaper, dom);
    }
}

bool ShardedAugmentedBTree::find(int64_t key, uint64_t& out_row_id) const {
    return shards_[shard_for_key(key)]->find(key, out_row_id);
}

uint64_t ShardedAugmentedBTree::total_count() const {
    uint64_t sum = 0;
    for (size_t i = 0; i < NUM_SHARDS; ++i) {
        sum += shards_[i]->total_count();
    }
    return sum;
}

int64_t ShardedAugmentedBTree::total_sum_i64(size_t col) const {
    int64_t sum = 0;
    for (size_t i = 0; i < NUM_SHARDS; ++i) {
        sum += shards_[i]->total_sum_i64(col);
    }
    return sum;
}

double ShardedAugmentedBTree::total_sum_f64(size_t col) const {
    double sum = 0.0;
    for (size_t i = 0; i < NUM_SHARDS; ++i) {
        sum += shards_[i]->total_sum_f64(col);
    }
    return sum;
}

uint64_t ShardedAugmentedBTree::range_count(int64_t lo, int64_t hi) const {
    if (lo > hi) return 0;
    uint64_t sum = 0;
    for (size_t i = 0; i < NUM_SHARDS; ++i) {
        sum += shards_[i]->range_count(lo, hi);
    }
    return sum;
}

int64_t ShardedAugmentedBTree::range_sum_i64(int64_t lo, int64_t hi, size_t col) const {
    if (lo > hi || col >= MAX_AGG_COLS) return 0;
    int64_t sum = 0;
    for (size_t i = 0; i < NUM_SHARDS; ++i) {
        sum += shards_[i]->range_sum_i64(lo, hi, col);
    }
    return sum;
}

double ShardedAugmentedBTree::range_sum_f64(int64_t lo, int64_t hi, size_t col) const {
    if (lo > hi || col >= MAX_AGG_COLS) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < NUM_SHARDS; ++i) {
        sum += shards_[i]->range_sum_f64(lo, hi, col);
    }
    return sum;
}

void ShardedAugmentedBTree::scan_all(std::vector<std::pair<int64_t, uint64_t>>& out) const {
    out.clear();
    out.reserve(total_count());
    std::vector<std::pair<int64_t, uint64_t>> tmp;
    for (size_t i = 0; i < NUM_SHARDS; ++i) {
        shards_[i]->scan_all(tmp);
        out.insert(out.end(), tmp.begin(), tmp.end());
    }
    std::sort(out.begin(), out.end(),
              [](const std::pair<int64_t, uint64_t>& a, const std::pair<int64_t, uint64_t>& b) {
                  return a.first < b.first;
              });
}

void ShardedAugmentedBTree::upsert(int64_t key,
                                   uint64_t row_id,
                                   const std::array<int64_t, MAX_AGG_COLS>& i64_vals,
                                   const std::array<double, MAX_AGG_COLS>& f64_vals) {
    shards_[shard_for_key(key)]->upsert(key, row_id, i64_vals, f64_vals);
}

void ShardedAugmentedBTree::upsert_batch(const std::vector<BatchEntry>& entries) {
    if (entries.empty()) return;
    std::array<std::vector<BatchEntry>, NUM_SHARDS> buckets;
    const size_t approx = (entries.size() / NUM_SHARDS) + 1;
    for (size_t i = 0; i < NUM_SHARDS; ++i) {
        buckets[i].reserve(approx);
    }
    for (const auto& entry : entries) {
        buckets[shard_for_key(entry.key)].push_back(entry);
    }
    for (size_t i = 0; i < NUM_SHARDS; ++i) {
        if (!buckets[i].empty()) {
            shards_[i]->upsert_batch(buckets[i]);
        }
    }
}

bool ShardedAugmentedBTree::erase(int64_t key) {
    return shards_[shard_for_key(key)]->erase(key);
}

void ShardedAugmentedBTree::clear() {
    for (size_t i = 0; i < NUM_SHARDS; ++i) {
        shards_[i]->clear();
    }
}

} // namespace zephyr::core
