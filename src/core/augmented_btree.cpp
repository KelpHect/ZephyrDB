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

AugmentedBTree::NodePool::NodePool() {
    free_list_.reserve(256);
}

AugmentedBTree::NodePool::~NodePool() {
    std::lock_guard<std::mutex> lk(pool_mu_);
    for (Node* n : free_list_) {
        delete n;
    }
    free_list_.clear();
}

AugmentedBTree::Node* AugmentedBTree::NodePool::alloc_node() {
    Node* n = nullptr;
    {
        std::lock_guard<std::mutex> lk(pool_mu_);
        if (!free_list_.empty()) {
            n = free_list_.back();
            free_list_.pop_back();
        }
    }
    if (!n) {
        n = new Node();
    }
    return n;
}

void AugmentedBTree::NodePool::recycle_node(Node* n) noexcept {
    if (!n) return;
    n->refcount.store(1, std::memory_order_relaxed);
    n->is_leaf = true;
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

AugmentedBTree::NodePool& AugmentedBTree::node_pool() noexcept {
    static NodePool pool;
    return pool;
}

AugmentedBTree::Node* AugmentedBTree::alloc_node() {
    return node_pool().alloc_node();
}

void AugmentedBTree::recycle_node(Node* n) noexcept {
    node_pool().recycle_node(n);
}

AugmentedBTree::AugmentedBTree(RcuReaper* reaper, cpp26::rcu::rcu_domain& dom)
    : reaper_(reaper), domain_(dom) {}

AugmentedBTree::~AugmentedBTree() {
    Node* r = root_.exchange(nullptr, std::memory_order_acq_rel);
    if (r) {
        release_node(r);
    }
}

void AugmentedBTree::add_ref(Node* n) noexcept {
    if (n) {
        n->refcount.fetch_add(1, std::memory_order_relaxed);
    }
}

void AugmentedBTree::release_node(Node* n) noexcept {
    if (!n) return;
    if (n->refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (!n->is_leaf) {
            for (uint16_t i = 0; i <= n->num_keys; ++i) {
                release_node(n->children[i]);
            }
        }
        recycle_node(n);
    }
}

void AugmentedBTree::rcu_node_deleter(void* p) {
    release_node(static_cast<Node*>(p));
}

void AugmentedBTree::retire_root(Node* old_root) {
    if (!old_root) return;
    if (reaper_) {
        reaper_->retire(old_root, &rcu_node_deleter);
    } else {
        domain_.retire(old_root, &rcu_node_deleter);
    }
}

AugmentedBTree::Node* AugmentedBTree::clone_node(const Node* src, uint64_t batch_gen) {
    Node* dst = alloc_node();
    dst->refcount.store(1, std::memory_order_relaxed);
    dst->is_leaf = src->is_leaf;
    dst->num_keys = src->num_keys;
    dst->batch_gen = batch_gen;
    dst->count = src->count;
    dst->min_key = src->min_key;
    dst->max_key = src->max_key;
    dst->col_sum_i64 = src->col_sum_i64;
    dst->col_sum_f64 = src->col_sum_f64;
    dst->keys = src->keys;

    if (src->is_leaf) {
        dst->row_ids = src->row_ids;
        dst->row_i64 = src->row_i64;
        dst->row_f64 = src->row_f64;
    } else {
        for (uint16_t i = 0; i <= src->num_keys; ++i) {
            dst->children[i] = src->children[i];
            add_ref(dst->children[i]);
        }
    }
    return dst;
}

void AugmentedBTree::recompute_aggregates(Node* n) noexcept {
    n->count = 0;
    n->min_key = INT64_MAX;
    n->max_key = INT64_MIN;
    n->col_sum_i64.fill(0);
    n->col_sum_f64.fill(0.0);

    if (n->is_leaf) {
        n->count = n->num_keys;
        if (n->num_keys > 0) {
            n->min_key = n->keys[0];
            n->max_key = n->keys[n->num_keys - 1];
            for (uint16_t i = 0; i < n->num_keys; ++i) {
                for (size_t c = 0; c < MAX_AGG_COLS; ++c) {
                    n->col_sum_i64[c] += n->row_i64[i][c];
                    n->col_sum_f64[c] += n->row_f64[i][c];
                }
            }
        }
    } else {
        for (uint16_t i = 0; i <= n->num_keys; ++i) {
            const Node* child = n->children[i];
            if (!child || child->count == 0) continue;
            n->count += child->count;
            if (child->min_key < n->min_key) n->min_key = child->min_key;
            if (child->max_key > n->max_key) n->max_key = child->max_key;
            for (size_t c = 0; c < MAX_AGG_COLS; ++c) {
                n->col_sum_i64[c] += child->col_sum_i64[c];
                n->col_sum_f64[c] += child->col_sum_f64[c];
            }
        }
    }
}

// ----------------------------------------------------------------------------
// Lock-free RCU Reads
// ----------------------------------------------------------------------------

bool AugmentedBTree::find(int64_t key, uint64_t& out_row_id) const {
    cpp26::rcu::scoped_read_lock guard(domain_);
    const Node* cur = root_.load(std::memory_order_acquire);
    if (!cur) return false;

    while (!cur->is_leaf) {
        const size_t idx = fast_upper_bound(cur->keys.data(), cur->num_keys, key);
        cur = cur->children[idx];
        if (!cur) return false;
    }

    const size_t pos = fast_lower_bound(cur->keys.data(), cur->num_keys, key);
    if (pos < cur->num_keys && cur->keys[pos] == key) {
        out_row_id = cur->row_ids[pos];
        return true;
    }
    return false;
}

uint64_t AugmentedBTree::total_count() const {
    cpp26::rcu::scoped_read_lock guard(domain_);
    const Node* r = root_.load(std::memory_order_acquire);
    return r ? r->count : 0;
}

int64_t AugmentedBTree::total_sum_i64(size_t col) const {
    if (col >= MAX_AGG_COLS) return 0;
    cpp26::rcu::scoped_read_lock guard(domain_);
    const Node* r = root_.load(std::memory_order_acquire);
    return r ? r->col_sum_i64[col] : 0;
}

double AugmentedBTree::total_sum_f64(size_t col) const {
    if (col >= MAX_AGG_COLS) return 0.0;
    cpp26::rcu::scoped_read_lock guard(domain_);
    const Node* r = root_.load(std::memory_order_acquire);
    return r ? r->col_sum_f64[col] : 0.0;
}

uint64_t AugmentedBTree::range_count_node(const Node* n, int64_t lo, int64_t hi) noexcept {
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
    uint64_t sum = 0;
    for (uint16_t i = 0; i <= n->num_keys; ++i) {
        if (i < n->num_keys && n->keys[i] < lo) continue;
        if (i > 0 && n->keys[i - 1] > hi) break;
        sum += range_count_node(n->children[i], lo, hi);
    }
    return sum;
}

int64_t AugmentedBTree::range_sum_i64_node(const Node* n, int64_t lo, int64_t hi, size_t col) noexcept {
    if (!n || n->count == 0 || n->max_key < lo || n->min_key > hi) {
        return 0;
    }
    if (n->min_key >= lo && n->max_key <= hi) {
        return n->col_sum_i64[col];
    }
    if (n->is_leaf) {
        const size_t start = fast_lower_bound(n->keys.data(), n->num_keys, lo);
        int64_t sum = 0;
        for (size_t i = start; i < n->num_keys && n->keys[i] <= hi; ++i) {
            sum += n->row_i64[i][col];
        }
        return sum;
    }
    int64_t sum = 0;
    for (uint16_t i = 0; i <= n->num_keys; ++i) {
        if (i < n->num_keys && n->keys[i] < lo) continue;
        if (i > 0 && n->keys[i - 1] > hi) break;
        sum += range_sum_i64_node(n->children[i], lo, hi, col);
    }
    return sum;
}

double AugmentedBTree::range_sum_f64_node(const Node* n, int64_t lo, int64_t hi, size_t col) noexcept {
    if (!n || n->count == 0 || n->max_key < lo || n->min_key > hi) {
        return 0.0;
    }
    if (n->min_key >= lo && n->max_key <= hi) {
        return n->col_sum_f64[col];
    }
    if (n->is_leaf) {
        const size_t start = fast_lower_bound(n->keys.data(), n->num_keys, lo);
        double sum = 0.0;
        for (size_t i = start; i < n->num_keys && n->keys[i] <= hi; ++i) {
            sum += n->row_f64[i][col];
        }
        return sum;
    }
    double sum = 0.0;
    for (uint16_t i = 0; i <= n->num_keys; ++i) {
        if (i < n->num_keys && n->keys[i] < lo) continue;
        if (i > 0 && n->keys[i - 1] > hi) break;
        sum += range_sum_f64_node(n->children[i], lo, hi, col);
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

void AugmentedBTree::scan_node(const Node* n, std::vector<std::pair<int64_t, uint64_t>>& out) {
    if (!n) return;
    if (n->is_leaf) {
        for (uint16_t i = 0; i < n->num_keys; ++i) {
            out.emplace_back(n->keys[i], n->row_ids[i]);
        }
    } else {
        for (uint16_t i = 0; i <= n->num_keys; ++i) {
            scan_node(n->children[i], out);
        }
    }
}

void AugmentedBTree::scan_all(std::vector<std::pair<int64_t, uint64_t>>& out) const {
    out.clear();
    cpp26::rcu::scoped_read_lock guard(domain_);
    const Node* r = root_.load(std::memory_order_acquire);
    if (r) {
        out.reserve(r->count);
        scan_node(r, out);
    }
}

// ----------------------------------------------------------------------------
// Serialized CoW Writes
// ----------------------------------------------------------------------------

AugmentedBTree::InsertResult AugmentedBTree::insert_recursive(
    Node* n,
    int64_t key,
    uint64_t row_id,
    const std::array<int64_t, MAX_AGG_COLS>& i64_vals,
    const std::array<double, MAX_AGG_COLS>& f64_vals,
    uint64_t batch_gen) {

    const bool can_mutate =
        (batch_gen != 0 && n->batch_gen == batch_gen &&
         n->refcount.load(std::memory_order_relaxed) == 1);

    if (n->is_leaf) {
        const size_t pos = fast_lower_bound(n->keys.data(), n->num_keys, key);

        // Update existing key with O(1) incremental aggregate delta
        if (pos < n->num_keys && n->keys[pos] == key) {
            Node* target = can_mutate ? n : clone_node(n, batch_gen);
            InsertResult res{};
            res.left = target;
            res.delta_count = 0;
            for (size_t c = 0; c < MAX_AGG_COLS; ++c) {
                const int64_t di = i64_vals[c] - target->row_i64[pos][c];
                const double df = f64_vals[c] - target->row_f64[pos][c];
                res.delta_i64[c] = di;
                res.delta_f64[c] = df;
                target->col_sum_i64[c] += di;
                target->col_sum_f64[c] += df;
            }
            target->row_ids[pos] = row_id;
            target->row_i64[pos] = i64_vals;
            target->row_f64[pos] = f64_vals;
            return res;
        }

        // Insert into non-full leaf with O(1) incremental aggregate delta
        if (n->num_keys < ORDER) {
            Node* target = can_mutate ? n : clone_node(n, batch_gen);
            for (size_t i = target->num_keys; i > pos; --i) {
                target->keys[i] = target->keys[i - 1];
                target->row_ids[i] = target->row_ids[i - 1];
                target->row_i64[i] = target->row_i64[i - 1];
                target->row_f64[i] = target->row_f64[i - 1];
            }
            target->keys[pos] = key;
            target->row_ids[pos] = row_id;
            target->row_i64[pos] = i64_vals;
            target->row_f64[pos] = f64_vals;
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

        // Split full leaf
        std::array<int64_t, ORDER + 1> tmp_keys{};
        std::array<uint64_t, ORDER + 1> tmp_ids{};
        std::array<std::array<int64_t, MAX_AGG_COLS>, ORDER + 1> tmp_i64{};
        std::array<std::array<double, MAX_AGG_COLS>, ORDER + 1> tmp_f64{};

        for (size_t i = 0; i < pos; ++i) {
            tmp_keys[i] = n->keys[i];
            tmp_ids[i] = n->row_ids[i];
            tmp_i64[i] = n->row_i64[i];
            tmp_f64[i] = n->row_f64[i];
        }
        tmp_keys[pos] = key;
        tmp_ids[pos] = row_id;
        tmp_i64[pos] = i64_vals;
        tmp_f64[pos] = f64_vals;
        for (size_t i = pos; i < ORDER; ++i) {
            tmp_keys[i + 1] = n->keys[i];
            tmp_ids[i + 1] = n->row_ids[i];
            tmp_i64[i + 1] = n->row_i64[i];
            tmp_f64[i + 1] = n->row_f64[i];
        }

        constexpr size_t mid = (ORDER + 1) / 2;
        Node* left = alloc_node();
        left->is_leaf = true;
        left->batch_gen = batch_gen;
        left->num_keys = static_cast<uint16_t>(mid);
        for (size_t i = 0; i < mid; ++i) {
            left->keys[i] = tmp_keys[i];
            left->row_ids[i] = tmp_ids[i];
            left->row_i64[i] = tmp_i64[i];
            left->row_f64[i] = tmp_f64[i];
        }

        Node* right = alloc_node();
        right->is_leaf = true;
        right->batch_gen = batch_gen;
        right->num_keys = static_cast<uint16_t>((ORDER + 1) - mid);
        for (size_t i = mid; i < ORDER + 1; ++i) {
            right->keys[i - mid] = tmp_keys[i];
            right->row_ids[i - mid] = tmp_ids[i];
            right->row_i64[i - mid] = tmp_i64[i];
            right->row_f64[i - mid] = tmp_f64[i];
        }

        recompute_aggregates(left);
        recompute_aggregates(right);
        return {left, right, right->keys[0], true, 1, i64_vals, f64_vals};
    }

    // Internal node
    const size_t idx = fast_upper_bound(n->keys.data(), n->num_keys, key);

    InsertResult child_res = insert_recursive(
        n->children[idx], key, row_id, i64_vals, f64_vals, batch_gen);

    if (!child_res.did_split) {
        Node* target = can_mutate ? n : clone_node(n, batch_gen);
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

    if (n->num_keys < ORDER) {
        Node* target = can_mutate ? n : clone_node(n, batch_gen);
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
    std::array<Node*, ORDER + 2> tmp_children{};

    for (size_t i = 0; i < idx; ++i) {
        tmp_keys[i] = n->keys[i];
    }
    tmp_keys[idx] = child_res.split_key;
    for (size_t i = idx; i < ORDER; ++i) {
        tmp_keys[i + 1] = n->keys[i];
    }

    for (size_t i = 0; i <= ORDER; ++i) {
        if (i < idx) {
            tmp_children[i] = n->children[i];
            add_ref(tmp_children[i]);
        } else if (i == idx) {
            tmp_children[idx] = child_res.left;
            tmp_children[idx + 1] = child_res.right;
        } else {
            tmp_children[i + 1] = n->children[i];
            add_ref(tmp_children[i + 1]);
        }
    }

    constexpr size_t mid = ORDER / 2;
    const int64_t promoted_key = tmp_keys[mid];

    Node* left = alloc_node();
    left->is_leaf = false;
    left->batch_gen = batch_gen;
    left->num_keys = static_cast<uint16_t>(mid);
    for (size_t i = 0; i < mid; ++i) {
        left->keys[i] = tmp_keys[i];
    }
    for (size_t i = 0; i <= mid; ++i) {
        left->children[i] = tmp_children[i];
    }

    Node* right = alloc_node();
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

AugmentedBTree::Node* AugmentedBTree::bulk_load_empty(const std::vector<BatchEntry>& entries) {
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
    std::vector<Node*> level;
    level.reserve(num_leaves);

    for (size_t l = 0; l < num_leaves; ++l) {
        const size_t start = l * LEAF_FILL;
        const size_t end = std::min(start + LEAF_FILL, n_unique);
        const size_t cnt = end - start;

        Node* leaf = alloc_node();
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
            leaf->row_i64[j] = e.i64_vals;
            leaf->row_f64[j] = e.f64_vals;
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
        std::vector<Node*> next_level;
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

            Node* parent = alloc_node();
            parent->is_leaf = false;
            parent->batch_gen = 0;
            parent->num_keys = static_cast<uint16_t>(cnt - 1);
            parent->count = 0;
            parent->min_key = level[offset]->min_key;
            parent->max_key = level[offset + cnt - 1]->max_key;
            parent->col_sum_i64.fill(0);
            parent->col_sum_f64.fill(0.0);

            for (size_t j = 0; j < cnt; ++j) {
                Node* child = level[offset + j];
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

AugmentedBTree::Node* AugmentedBTree::upsert_into_root(
    Node* root,
    int64_t key,
    uint64_t row_id,
    const std::array<int64_t, MAX_AGG_COLS>& i64_vals,
    const std::array<double, MAX_AGG_COLS>& f64_vals,
    uint64_t batch_gen) {
    if (!root) {
        Node* leaf = alloc_node();
        leaf->is_leaf = true;
        leaf->batch_gen = batch_gen;
        leaf->num_keys = 1;
        leaf->keys[0] = key;
        leaf->row_ids[0] = row_id;
        leaf->row_i64[0] = i64_vals;
        leaf->row_f64[0] = f64_vals;
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

    Node* new_root = alloc_node();
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
    Node* old_root = root_.load(std::memory_order_acquire);
    Node* new_root = upsert_into_root(old_root, key, row_id, i64_vals, f64_vals, 0);
    root_.store(new_root, std::memory_order_release);
    retire_root(old_root);
}

void AugmentedBTree::upsert_batch(const std::vector<BatchEntry>& entries) {
    if (entries.empty()) return;

    std::lock_guard<std::mutex> lk(write_mu_);
    Node* old_root = root_.load(std::memory_order_acquire);

    // O(N) bottom-up bulk load when inserting into an empty tree.
    if (old_root == nullptr) {
        Node* new_root = bulk_load_empty(entries);
        root_.store(new_root, std::memory_order_release);
        return;
    }

    // In-place batch mutation for non-empty trees: nodes created with cur_gen
    // are mutated in-place without re-cloning until the batch root is published.
    const uint64_t cur_gen = ++next_batch_gen_;
    Node* working_root = old_root;

    for (const auto& entry : entries) {
        Node* next_root = upsert_into_root(
            working_root, entry.key, entry.row_id, entry.i64_vals, entry.f64_vals, cur_gen);
        if (next_root != working_root && working_root != old_root) {
            release_node(working_root);
        }
        working_root = next_root;
    }

    root_.store(working_root, std::memory_order_release);
    retire_root(old_root);
}

AugmentedBTree::Node* AugmentedBTree::erase_recursive(const Node* n, int64_t key, bool& found) {
    if (n->is_leaf) {
        const size_t pos = fast_lower_bound(n->keys.data(), n->num_keys, key);
        if (pos >= n->num_keys || n->keys[pos] != key) {
            found = false;
            return nullptr;
        }
        found = true;
        if (n->num_keys == 1) {
            return nullptr;
        }
        Node* clone = clone_node(n);
        for (size_t i = pos; i + 1 < clone->num_keys; ++i) {
            clone->keys[i] = clone->keys[i + 1];
            clone->row_ids[i] = clone->row_ids[i + 1];
            clone->row_i64[i] = clone->row_i64[i + 1];
            clone->row_f64[i] = clone->row_f64[i + 1];
        }
        --clone->num_keys;
        recompute_aggregates(clone);
        return clone;
    }

    const size_t idx = fast_upper_bound(n->keys.data(), n->num_keys, key);

    Node* new_child = erase_recursive(n->children[idx], key, found);
    if (!found) {
        return nullptr;
    }

    if (new_child != nullptr) {
        Node* clone = clone_node(n);
        release_node(clone->children[idx]);
        clone->children[idx] = new_child;
        recompute_aggregates(clone);
        return clone;
    }

    // Child became empty
    if (n->num_keys == 0) {
        return nullptr;
    }
    if (n->num_keys == 1) {
        Node* survivor = n->children[1 - idx];
        add_ref(survivor);
        return survivor;
    }

    Node* clone = clone_node(n);
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
    Node* old_root = root_.load(std::memory_order_acquire);
    if (!old_root) return false;

    bool found = false;
    Node* new_root = erase_recursive(old_root, key, found);
    if (!found) return false;

    root_.store(new_root, std::memory_order_release);
    retire_root(old_root);
    return true;
}

void AugmentedBTree::clear() {
    std::lock_guard<std::mutex> lk(write_mu_);
    Node* old_root = root_.exchange(nullptr, std::memory_order_acq_rel);
    if (old_root) {
        retire_root(old_root);
    }
}

} // namespace zephyr::core
