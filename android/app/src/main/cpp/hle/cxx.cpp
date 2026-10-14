// libstdc++.6 shims: the handful of out-of-line GNU libstdc++ helpers the game
// imports (red-black tree for std::map/std::set, static-init guards, new/delete).
#include <cstdlib>
#include <mutex>

#include "hle/common.h"

namespace {

enum Color : int { kRed = 0, kBlack = 1 };
struct RbNode {
    Color color;
    RbNode* parent;
    RbNode* left;
    RbNode* right;
};

RbNode* rb_increment(RbNode* x) {
    if (x->right) {
        x = x->right;
        while (x->left) x = x->left;
    } else {
        RbNode* y = x->parent;
        while (x == y->right) {
            x = y;
            y = y->parent;
        }
        if (x->right != y) x = y;
    }
    return x;
}

RbNode* rb_decrement(RbNode* x) {
    if (x->color == kRed && x->parent->parent == x) {
        x = x->right;  // header -> rightmost
    } else if (x->left) {
        RbNode* y = x->left;
        while (y->right) y = y->right;
        x = y;
    } else {
        RbNode* y = x->parent;
        while (x == y->left) {
            x = y;
            y = y->parent;
        }
        x = y;
    }
    return x;
}

void rotate_left(RbNode* x, RbNode*& root) {
    RbNode* y = x->right;
    x->right = y->left;
    if (y->left) y->left->parent = x;
    y->parent = x->parent;
    if (x == root) root = y;
    else if (x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    y->left = x;
    x->parent = y;
}

void rotate_right(RbNode* x, RbNode*& root) {
    RbNode* y = x->left;
    x->left = y->right;
    if (y->right) y->right->parent = x;
    y->parent = x->parent;
    if (x == root) root = y;
    else if (x == x->parent->right) x->parent->right = y;
    else x->parent->left = y;
    y->right = x;
    x->parent = y;
}

void rb_insert_and_rebalance(bool insert_left, RbNode* x, RbNode* p, RbNode& header) {
    RbNode*& root = header.parent;
    x->parent = p;
    x->left = nullptr;
    x->right = nullptr;
    x->color = kRed;
    if (insert_left) {
        p->left = x;
        if (p == &header) {
            header.parent = x;
            header.right = x;
        } else if (p == header.left) {
            header.left = x;
        }
    } else {
        p->right = x;
        if (p == header.right) header.right = x;
    }
    while (x != root && x->parent->color == kRed) {
        RbNode* xpp = x->parent->parent;
        if (x->parent == xpp->left) {
            RbNode* y = xpp->right;
            if (y && y->color == kRed) {
                x->parent->color = kBlack;
                y->color = kBlack;
                xpp->color = kRed;
                x = xpp;
            } else {
                if (x == x->parent->right) {
                    x = x->parent;
                    rotate_left(x, root);
                }
                x->parent->color = kBlack;
                xpp->color = kRed;
                rotate_right(xpp, root);
            }
        } else {
            RbNode* y = xpp->left;
            if (y && y->color == kRed) {
                x->parent->color = kBlack;
                y->color = kBlack;
                xpp->color = kRed;
                x = xpp;
            } else {
                if (x == x->parent->left) {
                    x = x->parent;
                    rotate_right(x, root);
                }
                x->parent->color = kBlack;
                xpp->color = kRed;
                rotate_left(xpp, root);
            }
        }
    }
    root->color = kBlack;
}

// std::allocator<char> constructors/destructor are trivial.
void alloc_noop(void*) {}
void alloc_copy_noop(void*, const void*) {}

[[noreturn]] void throw_bad_alloc() {
    LOGE("std::__throw_bad_alloc");
    abort();
}
[[noreturn]] void throw_length_error(const char* what) {
    LOGE("std::__throw_length_error: %s", what ? what : "");
    abort();
}
[[noreturn]] void terminate_() {
    LOGE("std::terminate");
    abort();
}
[[noreturn]] void pure_virtual() {
    LOGE("pure virtual method called");
    abort();
}
void* begin_catch(void* e) {
    LOGE("__cxa_begin_catch(%p): unexpected C++ exception", e);
    return e;
}
int personality(...) {
    LOGE("__gxx_personality_v0 called");
    abort();
}

void* op_new(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) throw_bad_alloc();
    return p;
}
void op_delete(void* p) { free(p); }

// Itanium static-local guards. One recursive mutex serialises all guarded
// initialisations (nested ones on the same thread are fine).
std::recursive_mutex g_guard_mutex;
int guard_acquire(uint64_t* g) {
    if (__atomic_load_n((uint8_t*)g, __ATOMIC_ACQUIRE)) return 0;
    g_guard_mutex.lock();
    if (*(uint8_t*)g) {
        g_guard_mutex.unlock();
        return 0;
    }
    return 1;
}
void guard_release(uint64_t* g) {
    __atomic_store_n((uint8_t*)g, 1, __ATOMIC_RELEASE);
    g_guard_mutex.unlock();
}
void guard_abort(uint64_t*) { g_guard_mutex.unlock(); }

}  // namespace

HLE_EXPORT_AS("__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base", rb_increment);
HLE_EXPORT_AS("__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base", rb_decrement);
HLE_EXPORT_AS("__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_", rb_insert_and_rebalance);
HLE_EXPORT_AS("__ZNSaIcEC1Ev", alloc_noop);
HLE_EXPORT_AS("__ZNSaIcEC2Ev", alloc_noop);
HLE_EXPORT_AS("__ZNSaIcEC1ERKS_", alloc_copy_noop);
HLE_EXPORT_AS("__ZNSaIcEC2ERKS_", alloc_copy_noop);
HLE_EXPORT_AS("__ZNSaIcED1Ev", alloc_noop);
HLE_EXPORT_AS("__ZNSaIcED2Ev", alloc_noop);
HLE_EXPORT_AS("__ZSt17__throw_bad_allocv", throw_bad_alloc);
HLE_EXPORT_AS("__ZSt20__throw_length_errorPKc", throw_length_error);
HLE_EXPORT_AS("__ZSt9terminatev", terminate_);
HLE_EXPORT_AS("___cxa_pure_virtual", pure_virtual);
HLE_EXPORT_AS("___cxa_begin_catch", begin_catch);
HLE_EXPORT_AS("___gxx_personality_v0", personality);
HLE_EXPORT_AS("__Znwm", op_new);
HLE_EXPORT_AS("__Znam", op_new);
HLE_EXPORT_AS("__ZdlPv", op_delete);
HLE_EXPORT_AS("__ZdaPv", op_delete);
HLE_EXPORT_AS("___cxa_guard_acquire", guard_acquire);
HLE_EXPORT_AS("___cxa_guard_release", guard_release);
HLE_EXPORT_AS("___cxa_guard_abort", guard_abort);
