// kernel/trig/trig.cpp — Refactor Session 11a
#include "kernel/trig/trig.h"
#include "kernel/idx/idx.h"

namespace trig {
using Fn = void(*)(u64, u64, u64);
static u64 next_trig = 1;
void on(u64 kind, Fn fn){
    idx::put(idx::NS_TRIGGER | next_trig++, idx::Type::TRIGGER, (u64)fn, kind);
}
struct FireCtx { u64 kind, ea, eb; };
static void fire_visit(idx::Rec* r, void* v){
    FireCtx* c = (FireCtx*)v;
    if (r->b != c->kind) return;
    ((Fn)r->a)(0, c->ea, c->eb);
}
// Kein Index-Put mehr -> kein Leak
void emit(u64 kind, u64 a, u64 b){
    FireCtx c{kind, a, b};
    idx::scan(idx::Type::TRIGGER, fire_visit, &c);
}
}
