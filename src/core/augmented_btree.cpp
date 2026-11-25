#include "zephyr/core/augmented_btree.hpp"

#include <algorithm>

namespace zephyr::core {

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
        delete n;
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

AugmentedBTree::Node* AugmentedBTree::clone_node(const Node* src) {
    Node* dst = new Node();
    dst->refcount.store(1, std::memory_order_relaxed);
    dst->is_leaf = src->is_leaf;
    dst->num_keys = src->num_keys;
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
        const auto* begin = cur->keys.data();
        const auto* end = begin + cur->num_keys;
        const size_t idx = static_cast<size_t>(std::upper_bound(begin, end, key) - begin);
        cur = cur->children[idx];
        if (!cur) return false;
    }

    const auto* begin = cur->keys.data();
    const auto* end = begin + cur->num_keys;
    const auto* it = std::lower_bound(begin, end, key);
    if (it != end && *it == key) {
        out_row_id = cur->row_ids[static_cast<size_t>(it - begin)];
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
        const auto* begin = n->keys.data();
        const auto* end = begin + n->num_keys;
        const auto* it_lo = std::lower_bound(begin, end, lo);
        const auto* it_hi = std::upper_bound(it_lo, end, hi);
        return static_cast<uint64_t>(it_hi - it_lo);
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
        const auto* begin = n->keys.data();
        const auto* end = begin + n->num_keys;
        const size_t start = static_cast<size_t>(std::lower_bound(begin, end, lo) - begin);
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
        const auto* begin = n->keys.data();
        const auto* end = begin + n->num_keys;
        const size_t start = static_cast<size_t>(std::lower_bound(begin, end, lo) - begin);
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
    const Node* n,
    int64_t key,
    uint64_t row_id,
    const std::array<int64_t, MAX_AGG_COLS>& i64_vals,
    const std::array<double, MAX_AGG_COLS>& f64_vals) {

    if (n->is_leaf) {
        const auto* begin = n->keys.data();
        const auto* end = begin + n->num_keys;
        const size_t pos = static_cast<size_t>(std::lower_bound(begin, end, key) - begin);

        // Update existing key in cloned leaf
        if (pos < n->num_keys && n->keys[pos] == key) {
            Node* clone = clone_node(n);
            clone->row_ids[pos] = row_id;
            clone->row_i64[pos] = i64_vals;
            clone->row_f64[pos] = f64_vals;
            recompute_aggregates(clone);
            return {clone, nullptr, 0, false};
        }

        // Insert into non-full leaf
        if (n->num_keys < ORDER) {
            Node* clone = clone_node(n);
            for (size_t i = clone->num_keys; i > pos; --i) {
                clone->keys[i] = clone->keys[i - 1];
                clone->row_ids[i] = clone->row_ids[i - 1];
                clone->row_i64[i] = clone->row_i64[i - 1];
                clone->row_f64[i] = clone->row_f64[i - 1];
            }
            clone->keys[pos] = key;
            clone->row_ids[pos] = row_id;
            clone->row_i64[pos] = i64_vals;
            clone->row_f64[pos] = f64_vals;
            ++clone->num_keys;
            recompute_aggregates(clone);
            return {clone, nullptr, 0, false};
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
        Node* left = new Node();
        left->is_leaf = true;
        left->num_keys = static_cast<uint16_t>(mid);
        for (size_t i = 0; i < mid; ++i) {
            left->keys[i] = tmp_keys[i];
            left->row_ids[i] = tmp_ids[i];
            left->row_i64[i] = tmp_i64[i];
            left->row_f64[i] = tmp_f64[i];
        }

        Node* right = new Node();
        right->is_leaf = true;
        right->num_keys = static_cast<uint16_t>((ORDER + 1) - mid);
        for (size_t i = mid; i < ORDER + 1; ++i) {
            right->keys[i - mid] = tmp_keys[i];
            right->row_ids[i - mid] = tmp_ids[i];
            right->row_i64[i - mid] = tmp_i64[i];
            right->row_f64[i - mid] = tmp_f64[i];
        }

        recompute_aggregates(left);
        recompute_aggregates(right);
        return {left, right, right->keys[0], true};
    }

    // Internal node
    const auto* begin = n->keys.data();
    const auto* end = begin + n->num_keys;
    const size_t idx = static_cast<size_t>(std::upper_bound(begin, end, key) - begin);

    InsertResult child_res = insert_recursive(n->children[idx], key, row_id, i64_vals, f64_vals);

    if (!child_res.did_split) {
        Node* clone = clone_node(n);
        release_node(clone->children[idx]);
        clone->children[idx] = child_res.left;
        recompute_aggregates(clone);
        return {clone, nullptr, 0, false};
    }

    if (n->num_keys < ORDER) {
        Node* clone = clone_node(n);
        release_node(clone->children[idx]);

        for (size_t i = clone->num_keys; i > idx; --i) {
            clone->keys[i] = clone->keys[i - 1];
            clone->children[i + 1] = clone->children[i];
        }
        clone->keys[idx] = child_res.split_key;
        clone->children[idx] = child_res.left;
        clone->children[idx + 1] = child_res.right;
        ++clone->num_keys;
        recompute_aggregates(clone);
        return {clone, nullptr, 0, false};
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

    Node* left = new Node();
    left->is_leaf = false;
    left->num_keys = static_cast<uint16_t>(mid);
    for (size_t i = 0; i < mid; ++i) {
        left->keys[i] = tmp_keys[i];
    }
    for (size_t i = 0; i <= mid; ++i) {
        left->children[i] = tmp_children[i];
    }

    Node* right = new Node();
    right->is_leaf = false;
    right->num_keys = static_cast<uint16_t>(ORDER - mid);
    for (size_t i = mid + 1; i <= ORDER; ++i) {
        right->keys[i - (mid + 1)] = tmp_keys[i];
    }
    for (size_t i = mid + 1; i <= ORDER + 1; ++i) {
        right->children[i - (mid + 1)] = tmp_children[i];
    }

    recompute_aggregates(left);
    recompute_aggregates(right);
    return {left, right, promoted_key, true};
}

void AugmentedBTree::upsert(int64_t key,
                            uint64_t row_id,
                            const std::array<int64_t, MAX_AGG_COLS>& i64_vals,
                            const std::array<double, MAX_AGG_COLS>& f64_vals) {
    std::lock_guard<std::mutex> lk(write_mu_);
    Node* old_root = root_.load(std::memory_order_acquire);

    if (!old_root) {
        Node* leaf = new Node();
        leaf->is_leaf = true;
        leaf->num_keys = 1;
        leaf->keys[0] = key;
        leaf->row_ids[0] = row_id;
        leaf->row_i64[0] = i64_vals;
        leaf->row_f64[0] = f64_vals;
        recompute_aggregates(leaf);
        root_.store(leaf, std::memory_order_release);
        return;
    }

    InsertResult res = insert_recursive(old_root, key, row_id, i64_vals, f64_vals);
    Node* new_root = nullptr;
    if (!res.did_split) {
        new_root = res.left;
    } else {
        new_root = new Node();
        new_root->is_leaf = false;
        new_root->num_keys = 1;
        new_root->keys[0] = res.split_key;
        new_root->children[0] = res.left;
        new_root->children[1] = res.right;
        recompute_aggregates(new_root);
    }

    root_.store(new_root, std::memory_order_release);
    retire_root(old_root);
}

AugmentedBTree::Node* AugmentedBTree::erase_recursive(const Node* n, int64_t key, bool& found) {
    if (n->is_leaf) {
        const auto* begin = n->keys.data();
        const auto* end = begin + n->num_keys;
        const auto* it = std::lower_bound(begin, end, key);
        if (it == end || *it != key) {
            found = false;
            return nullptr;
        }
        found = true;
        if (n->num_keys == 1) {
            return nullptr;
        }
        const size_t pos = static_cast<size_t>(it - begin);
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

    const auto* begin = n->keys.data();
    const auto* end = begin + n->num_keys;
    const size_t idx = static_cast<size_t>(std::upper_bound(begin, end, key) - begin);

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
