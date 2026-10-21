// kernel/test_helpers/emit.h — Refactor Session 10b
// Die emit_*-Funktionen erzeugen minimale x86_64-Programme als
// Byte-Array fuer Kernel-interne Tests (ring3, a3e, r3bad).
// Produktiv-User-Code wird ueber ELF geladen (elf64::load_into).
#ifndef KERNEL_TEST_HELPERS_EMIT_H
#define KERNEL_TEST_HELPERS_EMIT_H

#include "kernel/hal/hal.h"


// ============================================================================
// TEST-ONLY CODE BELOW
// ============================================================================
// Die naechsten drei emit_*-Funktionen erzeugen minimale x86_64-Programme
// als Byte-Array. Sie sind ausschliesslich fuer Kernel-interne Tests
// gedacht (ring3, a3e, r3bad). Produktiv-User-Code wird ueber ELF geladen
// (siehe elf64::load_into und spawn_elf_task).
// Wenn ein Test nicht mehr gebraucht wird: Funktion, cmd_*-Wrapper und
// Dispatch-Eintrag gemeinsam entfernen.
// ============================================================================

void emit_user_program(u8* code, const char* msg, u32 msg_len);
void emit_kernptr_program(u8* code);
void emit_badptr_program(u8* code);
void emit_pipe_test_program(u8* code);
void emit_a3e_program(u8* code, u8 seed);

#endif
