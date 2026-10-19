// kernel/idx/idx.cpp — Refactor Session 4
#include "kernel/idx/idx.h"
#include "kernel/idx/wal_hooks.h"

namespace idx {

static Rec* root = nullptr;
static volatile u64 count = 0;
static Spinlock lock {0};
static constexpr u32 RED=0, BLACK=1;
static inline bool red(Rec* n){ return n && n->color==RED; }

static Rec* rotl(Rec* h){ Rec* x=h->right; h->right=x->left; x->left=h; x->color=h->color; h->color=RED; return x; }
static Rec* rotr(Rec* h){ Rec* x=h->left;  h->left=x->right;  x->right=h; x->color=h->color; h->color=RED; return x; }
static void flipc(Rec* h){ h->color^=1; if(h->left) h->left->color^=1; if(h->right) h->right->color^=1; }
static Rec* fixup(Rec* h){
    if (red(h->right))                      h = rotl(h);
    if (red(h->left) && red(h->left->left)) h = rotr(h);
    if (red(h->left) && red(h->right))      flipc(h);
    return h;
}
static Rec* ins(Rec* h, Rec* n){
    if (!h) return n;
    if (n->key < h->key)      h->left  = ins(h->left, n);
    else if (n->key > h->key) h->right = ins(h->right, n);
    else { h->a=n->a; h->b=n->b; h->type=n->type; return h; }
    return fixup(h);
}
void put(u64 key, Type t, u64 a, u64 b){
    u64 rflags = spin_lock_irqsave(&lock);
    Rec* h = root;
    while (h) {
        if (key < h->key)      h = h->left;
        else if (key > h->key) h = h->right;
        else {
            h->a = a; h->b = b; h->type = (u32)t;
            spin_unlock_irqrestore(&lock, rflags);
            if (!nexus_wal_in_recovery()) nexus_wal_put(key, (u32)t, a, b);
            return;
        }
    }
    Rec* n = new Rec{};
    n->key=key; n->type=(u32)t; n->color=RED; n->a=a; n->b=b;
    n->left=n->right=nullptr;
    root = ins(root, n);
    if (root) root->color = BLACK;
    count = count + 1;
    spin_unlock_irqrestore(&lock, rflags);
    if (!nexus_wal_in_recovery()) nexus_wal_put(key, (u32)t, a, b);
}
Rec* find(u64 key){
    Rec* h = root;
    while(h){
        if (key < h->key) h = h->left;
        else if (key > h->key) h = h->right;
        else return h;
    }
    return nullptr;
}

// Sedgewick LLRB delete: Content-Swap. Der physisch unverlinkte Knoten ist
// immer ein Blatt-artiger Node, den wir nach dem rekursiven Delete als
// "logisch entfernt" markieren. Wir sammeln die zu freigebenden Nodes.
static Rec* min_node(Rec* h){ while(h && h->left) h=h->left; return h; }

static Rec* move_red_left(Rec* h){
    flipc(h);
    if (red(h->right->left)) {
        h->right = rotr(h->right);
        h = rotl(h);
        flipc(h);
    }
    return h;
}
static Rec* move_red_right(Rec* h){
    flipc(h);
    if (red(h->left->left)) {
        h = rotr(h);
        flipc(h);
    }
    return h;
}
// delete_min mit Freigabe des entfernten Knotens
static Rec* delete_min(Rec* h, Rec** freed){
    if (!h->left) { *freed = h; return nullptr; }
    if (!red(h->left) && !red(h->left->left))
        h = move_red_left(h);
    Rec* sub_freed = nullptr;
    h->left = delete_min(h->left, &sub_freed);
    if (sub_freed && !*freed) *freed = sub_freed;
    return fixup(h);
}
static Rec* del(Rec* h, u64 key, Rec** freed){
    if (key < h->key) {
        if (!h->left) return h;
        if (!red(h->left) && !red(h->left->left))
            h = move_red_left(h);
        Rec* sub = nullptr;
        h->left = del(h->left, key, &sub);
        if (sub && !*freed) *freed = sub;
    } else {
        if (red(h->left)) h = rotr(h);
        if (key == h->key && !h->right) {
            *freed = h;
            return nullptr;
        }
        if (!h->right) return h;
        if (!red(h->right) && !red(h->right->left))
            h = move_red_right(h);
        if (key == h->key) {
            Rec* m = min_node(h->right);
            // Content-Swap: nach diesem Schritt wird h zum "Node mit m's Inhalt"
            // und m ist der Knoten, der gleich unverlinkt wird.
            h->key  = m->key;
            h->a    = m->a;
            h->b    = m->b;
            h->type = m->type;
            Rec* sub = nullptr;
            h->right = delete_min(h->right, &sub);
            if (sub && !*freed) *freed = sub;
        } else {
            Rec* sub = nullptr;
            h->right = del(h->right, key, &sub);
            if (sub && !*freed) *freed = sub;
        }
    }
    return fixup(h);
}

bool remove(u64 key){
    u64 rflags = spin_lock_irqsave(&lock);
    bool found = false;
    { Rec* h = root; while(h){ if(key<h->key) h=h->left; else if(key>h->key) h=h->right; else { found=true; break; } } }
    if (found) {
        Rec* freed = nullptr;
        root = del(root, key, &freed);
        if (root) root->color = BLACK;
        if (freed) {
            // Knoten physisch freigeben
            ::operator delete((void*)freed);
        }
        count = count - 1;
        spin_unlock_irqrestore(&lock, rflags);
        if (!nexus_wal_in_recovery()) nexus_wal_remove(key);
        return true;
    }
    spin_unlock_irqrestore(&lock, rflags);
    return false;
}

using Visitor = void(*)(Rec*, void*);
static void walk(Rec* h, Type t, Visitor v, void* ctx){
    if (!h) return;
    walk(h->left, t, v, ctx);
    if (h->type == (u32)t) v(h, ctx);
    walk(h->right, t, v, ctx);
}
void scan(Type t, Visitor v, void* ctx){ walk(root, t, v, ctx); }
static void walk_all(Rec* h, Visitor v, void* ctx){
    if (!h) return;
    walk_all(h->left, v, ctx);
    v(h, ctx);
    walk_all(h->right, v, ctx);
}
void scan_all(Visitor v, void* ctx){ walk_all(root, v, ctx); }
u64 size(){ return count; }
Rec* debug_root() { return root; }

struct HashCtx {
    u64 h;
    u32 mode;      // 0 = persistent only, 1 = all types, 2 = specific
    u32 filter;
    u32 count;
};

// Version-Tag: bei Format-Aenderung erhoehen.
// Alte Checkpoints matchen dann nicht mehr, statt stumm falsch zu sein.
static constexpr u64 HASH_TAG = 0x4E4558555333ULL;  // "NEXUS2" (Extent-Chain-FS)

static void hash_mix(HashCtx* c, u64 x) {
    for (int i = 0; i < 8; i++) {
        c->h ^= (u8)((x >> (i * 8)) & 0xFF);
        c->h *= 0x100000001b3ULL;
    }
}

static bool type_is_persistent(u32 t) {
    return t == 1 || t == 4 || t == 5 || t == 6 || t == 7;   // CONFIG, SYS, FILE, NAME
}

static bool hash_include(u32 t, const HashCtx* c) {
    if (c->mode == 1) return true;
    if (c->mode == 2) return t == c->filter;
    return type_is_persistent(t);
}

static void hash_visit(Rec* r, void* ctx) {
    HashCtx* c = (HashCtx*)ctx;
    if (!hash_include(r->type, c)) return;
    c->count++;
    hash_mix(c, r->key);
    hash_mix(c, (u64)r->type);
    hash_mix(c, r->a);
    hash_mix(c, r->b);
}

u64 compute_hash(u32 mode, u32 filter, u32* out_count) {
    HashCtx c{0xcbf29ce484222325ULL, mode, filter, 0};
    hash_mix(&c, HASH_TAG);
    scan_all(hash_visit, &c);
    if (out_count) *out_count = c.count;
    return c.h;
}

// Kurzformen fuer die Shell:
u64 state_hash(u32* n) { return compute_hash(0, 0, n); }
u64 full_hash (u32* n) { return compute_hash(1, 0, n); }
u64 type_hash (u32 t, u32* n) { return compute_hash(2, t, n); }
}

// ---------------------------------------------------------------------------
// LLRB-Invarianten-Test
// ---------------------------------------------------------------------------