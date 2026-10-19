// kernel/test_helpers/emit.cpp — Refactor Session 10b
#include "kernel/test_helpers/emit.h"

void emit_user_program(u8* code, const char* msg, u32 msg_len) {
    auto w8  = [](u8* p, u8 v){ *p = v; };
    auto w32 = [](u8* p, u32 v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; };

    u32 off = 0;
    // mov rax, 39  (sys_getpid)
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w32(code+off, 39); off += 4;
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);
    // rax = pid -> in rbx speichern (callee-saved, bleibt im syscall erhalten)
    // mov rbx, rax
    w8(code+off++, 0x48); w8(code+off++, 0x89); w8(code+off++, 0xC3);

    // mov rax, 1   (sys_write)
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w32(code+off, 1); off += 4;
    // mov rdi, 1   (fd=1)
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC7);
    w32(code+off, 1); off += 4;
    // lea rsi, [rip+disp]  -> msg @ offset 256
    w8(code+off++, 0x48); w8(code+off++, 0x8D); w8(code+off++, 0x35);
    i32 disp = (i32)256 - (i32)(off + 4);
    w32(code+off, (u32)disp); off += 4;
    // mov rdx, msg_len
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC2);
    w32(code+off, msg_len); off += 4;
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);

    // mov rax, 24  (sys_yield)
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w32(code+off, 24); off += 4;
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);

    // mov rax, 60  (sys_exit)
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w32(code+off, 60); off += 4;
    // mov rdi, rbx  (pid als exit-code)
    w8(code+off++, 0x48); w8(code+off++, 0x89); w8(code+off++, 0xDF);
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);
    // jmp $-2
    w8(code+off++, 0xEB); w8(code+off++, 0xFE);

    for (u32 i = 0; i < msg_len && (256+i) < 4096; i++)
        code[256 + i] = (u8)msg[i];
}

// A6a-Test: absichtlich ungueltiger User-Pointer (0xdeadbeef) an
// sys_write. Wenn user_range_ok korrekt arbeitet, liefert der Syscall
// E_FAULT und der User exit't mit 0xFFFFFFF2 (=-14 als u32). Ohne
// den Check wuerde der Kernel bei der Dereferenzierung crashen.
// A6a-Nachtrag: prueft, dass user_range_ok auch Kernel-Bereich
// zurueckweist (P_USER fehlt auf Zwischenebenen). Pointer 0xffff800000000000
// landet in PML4[256], das kein P_USER hat.
void emit_kernptr_program(u8* code) {
    auto w8  = [](u8* p, u8 v){ *p = v; };
    auto w32 = [](u8* p, u32 v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; };
    u32 off = 0;
    // mov rax, 1
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w32(code+off, 1); off += 4;
    // mov rdi, 1
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC7);
    w32(code+off, 1); off += 4;
    // mov rsi, 0xffff800000000000 (10 48 BE imm64)
    w8(code+off++, 0x48); w8(code+off++, 0xBE);
    // little-endian imm64: 00 00 00 00 00 80 ff ff
    w8(code+off++, 0x00); w8(code+off++, 0x00); w8(code+off++, 0x00); w8(code+off++, 0x00);
    w8(code+off++, 0x00); w8(code+off++, 0x80); w8(code+off++, 0xFF); w8(code+off++, 0xFF);
    // mov rdx, 8
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC2);
    w32(code+off, 8); off += 4;
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);
    // cmp rax, E_FAULT (-14 als u64) -> wenn ja, exit(0xEE), sonst exit(rax)
    // mov rdi, 0xEE
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC7);
    w32(code+off, 0xEE); off += 4;
    // mov rax, 60
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w32(code+off, 60); off += 4;
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);
    // jmp $-2
    w8(code+off++, 0xEB); w8(code+off++, 0xFE);
}

void emit_badptr_program(u8* code) {
    auto w8  = [](u8* p, u8 v){ *p = v; };
    auto w32 = [](u8* p, u32 v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; };
    u32 off = 0;
    // mov rax, 1
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w32(code+off, 1); off += 4;
    // mov rdi, 1
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC7);
    w32(code+off, 1); off += 4;
    // mov esi, 0xdeadbeef  (zero-extended to rsi)
    w8(code+off++, 0xBE);
    w32(code+off, 0xDEADBEEF); off += 4;
    // mov rdx, 16
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC2);
    w32(code+off, 16); off += 4;
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);
    // mov rdi, rax
    w8(code+off++, 0x48); w8(code+off++, 0x89); w8(code+off++, 0xC7);
    // mov rax, 60
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w32(code+off, 60); off += 4;
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);
    // jmp $-2
    w8(code+off++, 0xEB); w8(code+off++, 0xFE);
}

// A3e-Testprogramm (62 Bytes):
//   lea rdi, [rip+0x179]      ; &seed @ offset 0x180
//   movzx eax, byte [rdi]     ; eax = seed
//   mov r12, rax              ; r12 = seed (callee-saved)
//   mov [rsp-8], rax          ; write seed to user stack
//   mov ecx, 0x08000000       ; ~200ms busy loop
// spin: dec ecx; jnz spin
//   mov rax, [rsp-8]          ; read back
//   cmp r12, rax              ; ZF=1 wenn unveraendert
//   mov rdi, 0xEE             ; default: fail
//   jnz +3                    ; wenn nicht gleich, 0xEE behalten
//   mov rdi, r12              ; sonst: exit mit seed
//   mov rax, 60; syscall      ; exit
//   jmp $-2
//
// Wenn ein anderer Task den Stack ueberschrieben hat, liest dieser Task
// einen fremden Wert und exit't mit 0xEE. Wenn Isolation korrekt ist,
// exit't jeder Task mit seinem eigenen Seed.
void emit_a3e_program(u8* code, u8 seed) {
    auto w8  = [](u8* p, u8 v){ *p = v; };
    auto w32 = [](u8* p, u32 v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; };

    u32 off = 0;
    // lea rdi, [rip+0x179]
    w8(code+off++, 0x48); w8(code+off++, 0x8D); w8(code+off++, 0x3D);
    w32(code+off, 0x179); off += 4;
    // movzx eax, byte [rdi]
    w8(code+off++, 0x0F); w8(code+off++, 0xB6); w8(code+off++, 0x07);
    // mov r12, rax
    w8(code+off++, 0x49); w8(code+off++, 0x89); w8(code+off++, 0xC4);
    // mov [rsp-8], rax
    w8(code+off++, 0x48); w8(code+off++, 0x89); w8(code+off++, 0x44);
    w8(code+off++, 0x24); w8(code+off++, 0xF8);
    // mov ecx, 0x08000000
    w8(code+off++, 0xB9);
    w32(code+off, 0x08000000); off += 4;
    // spin: dec ecx (FF C9); jnz spin (75 FC)
    w8(code+off++, 0xFF); w8(code+off++, 0xC9);
    w8(code+off++, 0x75); w8(code+off++, 0xFC);
    // mov rax, [rsp-8]
    w8(code+off++, 0x48); w8(code+off++, 0x8B); w8(code+off++, 0x44);
    w8(code+off++, 0x24); w8(code+off++, 0xF8);
    // cmp r12, rax
    w8(code+off++, 0x49); w8(code+off++, 0x39); w8(code+off++, 0xC4);
    // mov rdi, 0xEE
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC7);
    w32(code+off, 0xEE); off += 4;
    // jnz +3
    w8(code+off++, 0x75); w8(code+off++, 0x03);
    // mov rdi, r12  (4C 89 E7)
    w8(code+off++, 0x4C); w8(code+off++, 0x89); w8(code+off++, 0xE7);
    // mov rax, 60
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w32(code+off, 60); off += 4;
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);
    // jmp $-2
    w8(code+off++, 0xEB); w8(code+off++, 0xFE);

    // Seed byte bei Offset 0x180
    code[0x180] = seed;
}
