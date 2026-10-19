// kernel/idx/llrb_test.cpp — Refactor Session 4
#include "kernel/idx/idx.h"
#include "kernel/idx/llrb_test.h"

namespace llrb_test {

static u64 rng_state = 0x12345678;
static u64 rng() {
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return rng_state >> 16;
}

struct Stats { u64 black_viol; u64 red_red_left; u64 red_red_right; u64 sort_viol; u64 total_nodes; };

static u64 check_black(idx::Rec* h, Stats* s) {
    if (!h) return 1;
    u64 lb = check_black(h->left, s);
    u64 rb = check_black(h->right, s);
    if (lb != rb) s->black_viol++;
    return lb + (h->color == 1 ? 1 : 0);
}

static void check_colors(idx::Rec* h, Stats* s) {
    if (!h) return;
    s->total_nodes++;
    if (h->color == 0 && h->left && h->left->color == 0) s->red_red_left++;
    if (h->right && h->right->color == 0) s->red_red_right++;
    check_colors(h->left, s);
    check_colors(h->right, s);
}

static bool check_sorted(idx::Rec* h, u64* last, bool* first) {
    if (!h) return true;
    if (!check_sorted(h->left, last, first)) return false;
    if (!*first && h->key <= *last) return false;
    *last  = h->key;
    *first = false;
    return check_sorted(h->right, last, first);
}

void run() {
    hal::ps("[LLRB] invariant test (500 ins / 250 del)...\n");
    u64 before = idx::size();

    for (int i = 0; i < 500; i++) {
        u64 k = idx::NS_EVENT | (rng() & 0x00FFFFFFFFFFFFFFULL);
        idx::put(k, idx::Type::EVENT, (u64)i, 0);
    }
    u64 after_ins = idx::size();

    rng_state = 0x12345678;
    for (int i = 0; i < 250; i++) {
        u64 k = idx::NS_EVENT | (rng() & 0x00FFFFFFFFFFFFFFULL);
        idx::remove(k);
    }
    u64 after_del = idx::size();

    Stats s{0, 0, 0, 0, 0};
    idx::Rec* r = idx::debug_root();
    if (r) {
        check_black(r, &s);
        check_colors(r, &s);
        u64 last = 0; bool first = true;
        if (!check_sorted(r, &last, &first)) s.sort_viol = 1;
    }
    hal::pf("[LLRB] before=%u ins=%u del=%u nodes=%u black=%u redL=%u redR=%u sort=%u\n",
            (u32)before, (u32)after_ins, (u32)after_del,
            (u32)s.total_nodes,
            (u32)s.black_viol, (u32)s.red_red_left, (u32)s.red_red_right,
            (u32)s.sort_viol);

    rng_state = 0x12345678;
    for (int i = 0; i < 500; i++) {
        u64 k = idx::NS_EVENT | (rng() & 0x00FFFFFFFFFFFFFFULL);
        idx::remove(k);
    }
    hal::pf("[LLRB] cleanup done, size=%u\n", (u32)idx::size());
}

}  // namespace llrb_test

// Forward-Decl: fd_flush wird von task::reap gerufen, ist aber im
// Shell-Namespace definiert (nach task). Definition: in shell.