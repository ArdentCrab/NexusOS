// kernel/shell/shell.cpp — Refactor Session 11b
#include "kernel/shell/shell.h"
#include "kernel/ipc/pipe.h"
#include "kernel/cpu/gdt.h"
#include "kernel/idx/idx.h"
#include "kernel/fs/fs.h"
#include "kernel/mm/mm.h"
#include "kernel/task/task.h"
#include "kernel/task/fd.h"
#include "kernel/elf64/elf64.h"
#include "kernel/test_helpers/layout.h"
#include "kernel/test_helpers/emit.h"
#include "kernel/dev/kbd.h"
#include "kernel/dev/pci.h"
#include "kernel/dev/virtio_blk.h"
#include "kernel/dev/blockcache.h"
#include "kernel/persist/persist.h"
#include "kernel/editor/editor.h"
#include "kernel/trig/trig.h"
#include "kernel/dev/virtio_net.h"
#include "kernel/dev/net.h"
#include "kernel/dev/tcp.h"
#include "kernel/dev/http.h"
#include "kernel/dev/dns.h"
#include "kernel/ipc/msg.h"

// Symbole aus nexus.cpp (S11c: nach main/ oder test_helpers/ verschieben)
extern void user_ring3_fn(void* arg);
extern void user_a3e_fn(void* arg);

namespace shell {

static char  line_buf[128];
static int   line_len = 0;
static char* argv[32];
static int   argc = 0;

static bool streq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}
static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static int  hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// Alias fuer cmd_bench, das parse_num aus dem Shell-Namespace braucht.
static bool shell_parse_num(const char* s, u64* out);

static bool parse_num(const char* s, u64* out) {
    if (!s || !*s) return false;
    u64 v = 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (!*s) return false;
        while (*s) { int d = hex_val(*s); if (d < 0) return false; v = (v << 4) | d; s++; }
    } else {
        while (*s) { if (!is_digit(*s)) return false; v = v * 10 + (*s - '0'); s++; }
    }
    *out = v;
    return true;
}
static bool shell_parse_num(const char* s, u64* out) {
    return parse_num(s, out);
}

static void print_padded(const char* s, int width) {
    int n = 0;
    while (s[n] && n < width) { hal::pc(s[n]); n++; }
    while (n < width) { hal::pc(' '); n++; }
}

// Forward-Decl: Definition steht spaeter in diesem Namespace (bei cmd_run).
static task::TCB* spawn_elf_task(const char* name,
                                 const u8* elf, u64 elf_len,
                                 const char* source,
                                 const char* arg);

// Baut argc/argv/arg-String auf der obersten Stack-Page.
static u64 setup_argc_argv(u64 stack_top, u64 stack_last_phys,
                           const char* arg) {
    if (!arg) return stack_top - 16;
    usize alen = 0; while (arg[alen] && alen < 256) alen++;
    u64 total = 32 + alen + 1;
    if (total > 0x1000 - 16) return 0;
    u64 rsp_user = stack_top - ((total + 15) & ~15ULL);
    u64 last_kva = (u64)mem::p2v(stack_last_phys);
    u64 off_in_page = rsp_user - (stack_top - 0x1000);

    u64* w = (u64*)(last_kva + off_in_page);
    w[0] = 1;                    // argc
    w[1] = rsp_user + 32;        // argv[0]
    w[2] = 0;                    // argv-Ende
    w[3] = 0;                    // envp-Ende
    char* strp = (char*)(last_kva + off_in_page + 32);
    for (usize i = 0; i < alen; i++) strp[i] = arg[i];
    strp[alen] = 0;
    return rsp_user;
}

// ============================================================================
// A6b-3a: isolierter Arg-Stack-Test
// ----------------------------------------------------------------------------
// Baut einen User-Task mit handgebautem Mini-Code, der nur argc und
// argv[0] aus dem User-Stack liest und den ersten Char von argv[0] als
// Exit-Code zurueckgibt. Kein ELF, kein sys_open. Nur: funktioniert der
// Kernel -> User-Stack-Aufbau mit argc/argv?
// ============================================================================
static void user_elf_fn(void*);

static void emit_argtest_program(u8* code) {
    auto w8 = [](u8* p, u8 v){ *p = v; };

    u32 off = 0;
    // mov rax, [rsp]        ; argc
    w8(code+off++, 0x48); w8(code+off++, 0x8B); w8(code+off++, 0x04);
    w8(code+off++, 0x24);
    // test rax, rax         ; argc == 0 ?
    w8(code+off++, 0x48); w8(code+off++, 0x85); w8(code+off++, 0xC0);
    // jz .err (disp patchen) -- exit(0xFF)
    u32 patch_jz1 = off;
    w8(code+off++, 0x74); w8(code+off++, 0x00);

    // mov rbx, [rsp+8]      ; argv[0]
    w8(code+off++, 0x48); w8(code+off++, 0x8B); w8(code+off++, 0x5C);
    w8(code+off++, 0x24); w8(code+off++, 0x08);
    // test rbx, rbx
    w8(code+off++, 0x48); w8(code+off++, 0x85); w8(code+off++, 0xDB);
    // jz .err
    u32 patch_jz2 = off;
    w8(code+off++, 0x74); w8(code+off++, 0x00);

    // movzx rdi, byte [rbx] ; erster Char
    w8(code+off++, 0x0F); w8(code+off++, 0xB6); w8(code+off++, 0x3B);
    // test rdi, rdi
    w8(code+off++, 0x48); w8(code+off++, 0x85); w8(code+off++, 0xFF);
    // jz .err
    u32 patch_jz3 = off;
    w8(code+off++, 0x74); w8(code+off++, 0x00);

    // mov rax, 60           ; sys_exit
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w8(code+off++, 60); w8(code+off++, 0); w8(code+off++, 0); w8(code+off++, 0);
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);
    // jmp $-2
    w8(code+off++, 0xEB); w8(code+off++, 0xFE);

    // .err:
    u32 err_off = off;
    // mov rax, 60
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC0);
    w8(code+off++, 60); w8(code+off++, 0); w8(code+off++, 0); w8(code+off++, 0);
    // mov rdi, 0xFF
    w8(code+off++, 0x48); w8(code+off++, 0xC7); w8(code+off++, 0xC7);
    w8(code+off++, 0xFF); w8(code+off++, 0); w8(code+off++, 0); w8(code+off++, 0);
    // syscall
    w8(code+off++, 0x0F); w8(code+off++, 0x05);
    // jmp $-2
    w8(code+off++, 0xEB); w8(code+off++, 0xFE);

    // Displacements patchen (rel8, alle zum .err)
    auto patch_jcc = [&](u32 at) {
        i32 d = (i32)err_off - (i32)(at + 2);
        code[at + 1] = (u8)(i8)d;
    };
    patch_jcc(patch_jz1);
    patch_jcc(patch_jz2);
    patch_jcc(patch_jz3);
}

// Spawnt Arg-Test-Task. Baut:
//   1 Page User-Code bei USER_CODE_VA (mit emit_argtest_program)
//   4 Pages User-Stack bei USER_STACK_VA..+0x4000
//   argc/argv[0] auf der hoechsten Stack-Page
// Dann iretq nach USER_CODE_VA.

// A9e: pipe_test aus Ring-3
static task::TCB* spawn_pipe_test_task() {
    u64 code_phys  = mem::pmm::alloc();
    if (!code_phys) return nullptr;
    u64 pml4 = vmm::alloc_pml4();
    if (!pml4) { mem::pmm::free_n(code_phys, 1); return nullptr; }

    u8* code = (u8*)mem::p2v(code_phys);
    for (int i = 0; i < 4096; i++) code[i] = 0;
    emit_pipe_test_program(code);

    const u64 stack_top = USER_STACK_VA + 0x4000;
    const u64 stack_lo  = stack_top - 0x4000;
    u64 last_page_phys = 0;
    for (u64 va = stack_lo; va < stack_top; va += 0x1000) {
        u64 sp = mem::pmm::alloc();
        if (!sp) { vmm::free_user_all(pml4); return nullptr; }
        u8* sk = (u8*)mem::p2v(sp);
        for (int k = 0; k < 4096; k++) sk[k] = 0;
        if (!vmm::map_user_page_in(pml4, va, sp, vmm::P_WRITE)) {
            mem::pmm::free_n(sp, 1);
            vmm::free_user_all(pml4);
            return nullptr;
        }
        if (va == stack_top - 0x1000) last_page_phys = sp;
    }
    (void)last_page_phys;

    if (!vmm::map_user_page_in(pml4, USER_CODE_VA, code_phys, 0)) {
        vmm::free_user_all(pml4);
        return nullptr;
    }

    u64 rsp_user = stack_top - 16;
    return task::spawn_full("pipetest", user_elf_fn, nullptr,
                            pml4, USER_CODE_VA, rsp_user);
}

static void cmd_pipe_test(int ac, char** av) {
    (void)ac; (void)av;
    task::TCB* t = spawn_pipe_test_task();
    if (t) hal::pf("[PIPETEST] spawned pid=%u\n", (u32)t->pid);
    else   hal::ps("[PIPETEST] spawn failed\n");
}

static task::TCB* spawn_argtest_task(const char* arg) {
    u64 code_phys  = mem::pmm::alloc();
    if (!code_phys) return nullptr;
    u64 pml4 = vmm::alloc_pml4();
    if (!pml4) { mem::pmm::free_n(code_phys, 1); return nullptr; }

    u8* code = (u8*)mem::p2v(code_phys);
    for (int i = 0; i < 4096; i++) code[i] = 0;
    emit_argtest_program(code);
    hal::pf("[ARG ] code emitted, pml4=%p code_phys=%p\n", pml4, code_phys);

    // Stack: 4 Pages, Top bei USER_STACK_VA + 0x4000
    const u64 stack_top = USER_STACK_VA + 0x4000;
    const u64 stack_lo  = stack_top - 0x4000;
    u64 last_page_phys = 0;
    for (u64 va = stack_lo; va < stack_top; va += 0x1000) {
        u64 sp = mem::pmm::alloc();
        if (!sp) { vmm::free_user_all(pml4); return nullptr; }
        u8* sk = (u8*)mem::p2v(sp);
        for (int k = 0; k < 4096; k++) sk[k] = 0;
        if (!vmm::map_user_page_in(pml4, va, sp, vmm::P_WRITE)) {
            mem::pmm::free_n(sp, 1);
            vmm::free_user_all(pml4);
            return nullptr;
        }
        if (va == stack_top - 0x1000) last_page_phys = sp;
    }
    if (!vmm::map_user_page_in(pml4, USER_CODE_VA, code_phys, 0)) {
        vmm::free_user_all(pml4);
        return nullptr;
    }

    u64 rsp_user = setup_argc_argv(stack_top, last_page_phys, arg);
    if (!rsp_user) { vmm::free_user_all(pml4); return nullptr; }

    hal::pf("[ARG ] rsp_user=%p argv[0]=%p arg='%s'\n",
            rsp_user, rsp_user + 32, arg);

    task::TCB* t = task::spawn_full("argtest", user_elf_fn, nullptr,
                                    pml4, USER_CODE_VA, rsp_user);
    return t;
}

static void cmd_argtest(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: argtest <string>\n"); return; }
    task::TCB* t = spawn_argtest_task(av[1]);
    if (t) hal::pf("[ARG ] spawned pid=%u arg='%s'\n", (u32)t->pid, av[1]);
    else   hal::ps("[ARG ] spawn failed\n");
}

// A6b-3b: cat2 <name>. Spawnt cat.elf mit <name> als argv[0].
static void cmd_cat2(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: cat2 <name>\n"); return; }
    static char buf[16 * 1024];
    int n = fs::cat_file("cat.elf", buf, sizeof(buf));
    if (n <= 0 || n >= (int)sizeof(buf)) {
        hal::pf("[CAT2] cat.elf load failed (%d)\n", n);
        return;
    }
    task::TCB* t = spawn_elf_task("cat", (const u8*)buf, (u64)n,
                                  "cat.elf", av[1]);
    if (t) hal::pf("[CAT2] spawned pid=%u reading '%s'\n",
                   (u32)t->pid, av[1]);
    else   hal::ps("[CAT2] spawn failed\n");
}

// A6c: write2 <name>. Spawnt write.elf, das eine Datei erstellt und beschreibt.
static void cmd_write2(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: write2 <name>\n"); return; }
    static char buf[16 * 1024];
    int n = fs::cat_file("write.elf", buf, sizeof(buf));
    if (n <= 0 || n >= (int)sizeof(buf)) {
        hal::pf("[WRITE2] write.elf load failed (%d)\n", n);
        return;
    }
    task::TCB* t = spawn_elf_task("write", (const u8*)buf, (u64)n,
                                  "write.elf", av[1]);
    if (t) hal::pf("[WRITE2] spawned pid=%u writing '%s'\n",
                   (u32)t->pid, av[1]);
    else   hal::ps("[WRITE2] spawn failed\n");
}

// A6b-2 Test: ruft sys_open/sys_close im Kernel-Kontext (nicht aus
// Ring 3). Prueft die Dispatch-Logik isoliert. Der echte User-Test
// kommt in A6b-3 mit einem C-Programm.
static void cmd_fdtest(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: fdtest <name>\n"); return; }
    const char* name = av[1];

    // Kernel-Kontext: ruft die interne Variante ohne user_range_ok.
    // Ein aus Ring 3 kommender Aufruf wuerde stattdessen sys_open
    // treffen, das zusaetzlich copy_user_str macht.
    i64 fd = sys_open_kernel(name, O_RDONLY);
    if (fd < 0) {
        hal::pf("[FDT ] open('%s') -> %d\n", name, (i32)fd);
        return;
    }
    task::TCB* t = task::current;
    hal::pf("[FDT ] open('%s') -> fd=%d size=%u buf=%p\n",
            name, (i32)fd, (u32)t->fds[fd].size, t->fds[fd].ram_buf);

    // Datei-Inhalt kurz zeigen (max 32 Bytes)
    if (t->fds[fd].ram_buf) {
        char* p = (char*)t->fds[fd].ram_buf;
        u64 n = t->fds[fd].size;
        u64 show = n > 32 ? 32 : n;
        hal::ps("[FDT ] first bytes: ");
        for (u64 i = 0; i < show; i++) {
            char c = p[i];
            if (c == '\n') hal::ps("\\n");
            else if (c >= 32 && c < 127) hal::pc(c);
            else hal::pc('.');
        }
        if (n > 32) hal::ps("...");
        hal::pc('\n');
    }

    // sys_read-Test: lies die Datei ueber sys_read_kernel in einen
    // Kernel-Puffer. Vergleiche FNV mit dem direkten Puffer-FNV.
    // Damit ist sys_read im Kern-Pfad verifiziert.
    u64 size = t->fds[fd].size;
    if (size > 0 && size <= 8192) {
        char kbuf[8192];
        i64 total = 0;
        for (;;) {
            i64 n = sys_read_kernel((int)fd, kbuf + total, (u64)(size - total));
            if (n < 0) { hal::pf("[FDT ] read error %d\n", (i32)n); break; }
            if (n == 0) break;
            total += n;
            if ((u64)total >= size) break;
        }
        if ((u64)total != size) {
            hal::pf("[FDT ] read size mismatch: got %d want %u\n",
                    (i32)total, (u32)size);
        } else {
            u64 h = 0xcbf29ce484222325ULL;
            for (u64 i = 0; i < size; i++) {
                h ^= (u8)kbuf[i];
                h *= 0x100000001b3ULL;
            }
            hal::pf("[FDT ] read via sys_read_kernel: %d bytes fnv=%p\n",
                    (i32)total, h);
        }

        // Zwei reads hintereinander: der zweite muss 0 (EOF) geben,
        // weil offset am Ende ist.
        i64 after = sys_read_kernel((int)fd, kbuf, 1);
        hal::pf("[FDT ] read-after-EOF -> %d (expect 0)\n", (i32)after);
    }

    i64 r = sys_close((u64)fd);
    hal::pf("[FDT ] close(%d) -> %d\n", (i32)fd, (i32)r);

    // Nach dem Close darf der Puffer nicht mehr belegt sein.
    // Doppelclose sollte E_BADF geben.
    i64 r2 = sys_close((u64)fd);
    hal::pf("[FDT ] double-close(%d) -> %d (expect -9)\n", (i32)fd, (i32)r2);
}

// Diagnose-Befehl: prueft die va_arg-Semantik von hal::pf. Nach dem
// B1-Fix liest pf %u/%d/%x als 32-Bit und %lu/%ld/%lx als 64-Bit.
static void cmd_pf_test() {
    hal::ps("pf_test:\n");
    hal::pf("  u32 0xFFFFFFFF  %%u:  %u\n", (u32)0xFFFFFFFFu);
    hal::pf("  u64 0xFFFFFFFFFFFFFFFF %%lx: %lx\n", (u64)0xFFFFFFFFFFFFFFFFULL);
    hal::pf("  i32 -14         %%d:  %d\n", (i32)-14);
    hal::pf("  i64 -14         %%ld: %ld\n", (i64)-14);
    hal::pf("  ptr 0xdeadbeef  %%p:  %p\n", (u64)0xdeadbeefULL);
    hal::pf("  u32 0           %%u:  %u\n", (u32)0);
    hal::pf("  u64 0           %%lu: %lu\n", (u64)0);
    hal::pf("  str %%s: %s   char %%c: %c\n", "hello", 'X');
    hal::ps("pf_test done\n");
}

static void cmd_help() {
    hal::ps(
        "commands:\n"
        "  help                   this list\n"
        "  get <key>              read a record (hex or decimal key)\n"
        "  put <key> <a> <b>      write a CONFIG record\n"
        "  scan <type>            list: task config trigger event sys\n"
        "  tasks                  list all tasks with state\n"        "  prio <pid> [<p>]       get/set task priority (0-10, default 5)\n"
        "  mem                    pmm / heap / slab stats\n"
        "  clear                  clear screen (ANSI)\n"
        "  pci                    list PCI devices\n"
        "  sync                   persist index to disk\n"
        "  wal                    show WAL state\n"
        "  write <name> <text>    create/overwrite file (fs-level)\n"
        "  append <name> <text>   append to file (creates if missing)\n"
        "  trunc <name>           truncate file to 0 bytes\n"
        "  writetrunc <name> <text>  sys_open(O_TRUNC) + write + close\n"
        "  cat <name>             read file\n"
        "  lseek <name> <o> <w>   set file offset (w: 0=SET 1=CUR 2=END)\n"
        "  pathtest <path>        parse/dirname/basename (A7a)\n"
        "  pub <key> <a> [<b>]    publish event (A9)\n"
        "  piper                  pipe roundtrip test (A9c)\n"
        "  piper-async            producer-task + blocking read (A9d)\n"
        "  pipe-test              sys_pipe from ring-3 (A9e)\n"
        "  sub <type> [<ms>]      subscribe + wait (A9)\n"
        "  sub-reset              reset event cursor (A9)\n"
        "  mkdir <path>           create directory (A7b-1)\n"
        "  rmdir <path>           remove directory (A7b-1)\n"
        "  pwd-test <path>        check dir_exists (A7b-1)\n"
        "  cd [<path>]            change cwd (A7b-2)\n"
        "  pwd                    print cwd (A7b-2)\n"
        "  sleep <ms>             sleep N milliseconds (cap 30s)\n"
        "  ls                     list files\n"
        "  rm <name>              remove file\n"
        "  edit <name>            edit a text file\n"
        "  ring3                  spawn user task (syscall test)\n"
        "  a3e                    spawn 2 concurrent isolated user tasks\n"
        "  r3bad                  ring3 with invalid user pointer (E_FAULT test)\n"
        "  r3kern                 ring3 with kernel-space pointer (P_USER test)\n"
        "  bench [N]              spawn N ring3 tasks, verify no PMM leak\n"
        "  run <name>             load ELF from FS and execute in ring 3\n"
        "  hello                  run embedded hello ELF\n"
        "  fib                    run embedded fib ELF (recursion test)\n"
        "  int3                   direct int3-from-ring3 test\n"
        "  gen <name> <bytes>     generate file with pattern (extent test)\n"
        "  wc <name>              report size + content hash\n"
        "  bmtest                 block-bitmap alloc/free stress\n"
        "  factory-reset          restore the factory-default checkpoint\n"
        "  hash [type]            system hash (or per-type)\n"
        "  history [N]            last N WAL entries\n"
        "  trace <key>            WAL entries for a key\n"
        "  watch [type]           live stream of index changes\n"
        "  blk [read <s>|write <s> <t>]  virtio block device\n"
        "  log [N]                show last N ring-buffer lines\n"
        "  lvl d|i|w|e            set log level (default i)\n"
        "  pf_test                verify printf %%u/%%lu/%%p semantics\n"
        "  fdtest <name>          open/close file, show first bytes\n"
        "  argtest <string>       user-task: read argc/argv[0], exit(ch)\n"
        "  cat2 <name>            cat.elf: open+read+write+close from ring3\n"
        "  write2 <name>          write.elf: open+write+close from ring3\n"
        "\nshortcuts:\n"
        "  s t | s c | s e | s r  scan task/config/event/trigger\n"
        "  g c 1                  get  NS_CONFIG|1\n"
        "  g t 5                  get  NS_TASK|5\n"
        "  p c 1 999 0            put  NS_CONFIG|1  a=999  b=0\n"
        "  t | ps                 tasks\n"
        "  m                      mem\n");
}
static bool resolve_key(int ac, char** av, int idx_arg, u64* out) {
    if (idx_arg >= ac) return false;
    const char* first = av[idx_arg];
    if ((first[0] == 'c' || first[0] == 't' || first[0] == 'e' || first[0] == 'r')
        && first[1] == 0 && idx_arg + 1 < ac) {
        u64 id;
        if (!parse_num(av[idx_arg + 1], &id)) return false;
        u64 ns;
        switch (first[0]) {
            case 'c': ns = idx::NS_CONFIG;  break;
            case 't': ns = idx::NS_TASK;    break;
            case 'e': ns = idx::NS_EVENT;   break;
            case 'r': ns = idx::NS_TRIGGER; break;
            default:  return false;
        }
        *out = ns | id;
        return true;
    }
    return parse_num(first, out);
}

static void cmd_get(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: get <key> | get <c|t|e|r> <id>\n"); return; }
    u64 key;
    if (!resolve_key(ac, av, 1, &key)) { hal::ps("bad key\n"); return; }
    idx::Rec* r = idx::find(key);
    if (!r) { hal::pf("not found: %p\n", key); return; }
    const char* tname = "?";
    switch ((idx::Type)r->type) {
        case idx::Type::TASK:    tname = "TASK";    break;
        case idx::Type::CONFIG:  tname = "CONFIG";  break;
        case idx::Type::EVENT:   tname = "EVENT";   break;
        case idx::Type::TRIGGER: tname = "TRIGGER"; break;
        case idx::Type::SYS:     tname = "SYS";     break;
        case idx::Type::FILE:    tname = "FILE";    break;
        case idx::Type::NAME:    tname = "NAME";    break;
        case idx::Type::DIR:     tname = "DIR";     break;
        case idx::Type::PATH:    tname = "PATH";    break;
        case idx::Type::MSG:     tname = "MSG";     break;
    }
    hal::pf("%p  type=%s  a=%p  b=%p\n", key, tname, r->a, r->b);
}
static void cmd_put(int ac, char** av) {
    // Formen: put <key> <a> <b>   |   put c <id> <a> <b>
    if (ac < 4) { hal::ps("usage: put <key> <a> <b> | put c <id> <a> <b>\n"); return; }

    u64 k;
    int a_idx, b_idx;
    if (av[1][0] == 'c' && av[1][1] == 0 && ac >= 5) {
        // put c <id> <a> <b>
        u64 id;
        if (!parse_num(av[2], &id)) { hal::ps("bad id\n"); return; }
        k = idx::NS_CONFIG | id;
        a_idx = 3; b_idx = 4;
    } else {
        if (!resolve_key(ac, av, 1, &k)) { hal::ps("bad key\n"); return; }
        a_idx = 2; b_idx = 3;
    }
    u64 a, b;
    if (!parse_num(av[a_idx], &a) || !parse_num(av[b_idx], &b)) {
        hal::ps("bad number\n"); return;
    }
    idx::put(k, idx::Type::CONFIG, a, b);
    hal::ps("OK\n");
}
static void print_task_visit(idx::Rec* r, void*) {
    task::TCB* t = (task::TCB*)r->a;
    const char* st = "?";
    switch (r->b) {
        case task::ST_READY:   st = "READY";   break;
        case task::ST_RUNNING: st = "RUNNING"; break;
        case task::ST_DEAD:    st = "DEAD";    break;
    }
    hal::pf("  pid=%u  ", (u32)t->pid);
    print_padded(t->name, 10);
    hal::ps("  ");
    print_padded(st, 8);
    hal::pf("  prio=%u  vrt=%u\n", (u32)t->priority, (u32)t->vruntime);
}
static void print_rec_visit(idx::Rec* r, void*) {
    hal::pf("  %p  type=%u  a=%p  b=%p\n", r->key, r->type, r->a, r->b);
}
static void cmd_tasks() {
    hal::ps("tasks:\n");
    idx::scan(idx::Type::TASK, print_task_visit, nullptr);
}
static void cmd_scan(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: scan <type>\n"); return; }
    idx::Type t;
    const char* n = av[1];
    if      (streq(n, "task")    || streq(n, "t")) t = idx::Type::TASK;
    else if (streq(n, "config")  || streq(n, "c")) t = idx::Type::CONFIG;
    else if (streq(n, "event")   || streq(n, "e")) t = idx::Type::EVENT;
    else if (streq(n, "trigger") || streq(n, "r")) t = idx::Type::TRIGGER;
    else if (streq(n, "sys")     || streq(n, "s")) t = idx::Type::SYS;
    else { hal::ps("unknown type (t/c/e/r/s)\n"); return; }
    idx::scan(t, print_rec_visit, nullptr);
}
static void cmd_mem() {
    hal::pf("pmm:  total=%u  used=%u  free=%u pages\n",
            (u32)mem::pmm::total(),
            (u32)mem::pmm::used_count(),
            (u32)mem::pmm::free_count());
    hal::pf("heap: %u KiB total, %u KiB used\n",
            (u32)(heap::total()/1024), (u32)(heap::used()/1024));
    hal::pf("slab: %u objects in use, %u KiB\n",
            (u32)slab::used(), (u32)(slab::bytes_used()/1024));
    hal::pf("cache: %u/64 slots, %u dirty, hits=%u misses=%u evicted=%u\n",
            (u32)blockcache::valid_count(),
            (u32)blockcache::dirty_count(),
            (u32)blockcache::hits(),
            (u32)blockcache::misses(),
            (u32)blockcache::evicted());
    hal::pf("bitmap: %u sectors dirty, %u total writes\n",
            (u32)fs::bm_dirty_count(), (u32)fs::bm_write_count());
}
static void cmd_clear() { hal::ps("\x1b[2J\x1b[H"); }

// ---- PCI helpers + command ----
static void hex2(u8 v) {
    const char* h = "0123456789abcdef";
    hal::pc(h[(v >> 4) & 0xF]);
    hal::pc(h[v & 0xF]);
}
static void hex4(u16 v) {
    hex2((u8)(v >> 8));
    hex2((u8)(v & 0xFF));
}
static void pci_visit(const pci::Device* d, void*) {
    hal::ps("  ");
    hex2(d->bus); hal::pc(':'); hex2(d->dev); hal::pc('.');
    hal::pc((char)('0' + (d->func & 7)));
    hal::ps("  ");
    hex4(d->vendor); hal::pc(':'); hex4(d->device);
    hal::ps("  ");
    hex2(d->class_code); hal::pc(':'); hex2(d->subclass);
    hal::ps("  ");
    print_padded(pci::class_name(d->class_code), 14);
    if (d->bar_size[0] != 0) {
        hal::ps(" bar0=");
        hex4((u16)((d->bar[0] >> 16) & 0xFFFF));
        hex4((u16)(d->bar[0] & 0xFFFF));
        hal::pf("/%uK", d->bar_size[0] / 1024);
    }
    if (d->bar_size[5] != 0) {
        hal::ps(" bar5=");
        hex4((u16)((d->bar[5] >> 16) & 0xFFFF));
        hex4((u16)(d->bar[5] & 0xFFFF));
        hal::pf("/%uK", d->bar_size[5] / 1024);
    }
    hal::pc('\n');
}
static void cmd_log(int ac, char** av) {
    int n = 20;
    if (ac >= 2) { u64 v; if (parse_num(av[1], &v)) n = (int)v; }
    hal::log_dump(n);
}
static void cmd_lvl(int ac, char** av) {
    if (ac < 2) {
        hal::pf("log level: %u (0/d=debug 1/i=info 2/w=warn 3/e=error)\n",
                (u32)hal::g_log_level);
        return;
    }
    char c = av[1][0];
    if      (c == '0' || c == 'd') hal::g_log_level = hal::LOG_DEBUG;
    else if (c == '1' || c == 'i') hal::g_log_level = hal::LOG_INFO;
    else if (c == '2' || c == 'w') hal::g_log_level = hal::LOG_WARN;
    else if (c == '3' || c == 'e') hal::g_log_level = hal::LOG_ERROR;
    else { hal::ps("usage: lvl d|i|w|e\n"); return; }
    hal::pf("log level = %u\n", (u32)hal::g_log_level);
}

static void cmd_sync() {
    if (!virtio_blk::is_ready()) { hal::ps("no block device\n"); return; }
    hal::ps("sync...\n");
    int r = persist::save();
    if (r < 0) { hal::pf("sync failed (%d)\n", r); return; }
    int flushed = blockcache::flush();
    hal::pf("snapshot: %u records, seq=%u, flushed %d sectors\n",
            (u32)r, (u32)persist::snap_seq(), flushed);
}
static void cmd_wal() {
    hal::pf("wal: start=%u head=%u count=%u\n",
            (u32)persist::WAL_START,
            (u32)persist::wal_head(),
            (u32)persist::wal_count());
    hal::pf("snap_seq=%u  puts_since_sync=%u\n",
            (u32)persist::snap_seq(),
            (u32)persist::puts_since_sync());
    hal::pf("autosync_pending=%u  threshold=%u\n",
            (u32)persist::autosync_pending(), 500);
    hal::pf("cache dirty: %d sectors\n", blockcache::dirty_count());
}
static void cmd_blk(int ac, char** av) {
    if (!virtio_blk::is_ready()) {
        hal::ps("no virtio-blk device\n");
        return;
    }
    if (ac < 2) {
        hal::pf("virtio-blk: %u sectors (%u MiB)\n",
                (u32)virtio_blk::capacity(),
                (u32)(virtio_blk::capacity() * 512 / 1024 / 1024));
        return;
    }
    if (streq(av[1], "read")) {
        u64 sec = 0;
        if (ac >= 3) parse_num(av[2], &sec);
        u8 buf[512];
        int r = virtio_blk::read_sector(sec, buf);
        if (r != 0) { hal::pf("read error %d\n", r); return; }
        hal::pf("sector %u:\n", (u32)sec);
        for (int row = 0; row < 16; row++) {
            hal::pf("  %x: ", row * 32);
            for (int i = 0; i < 32; i++) {
                u8 b = buf[row * 32 + i];
                const char* hx = "0123456789abcdef";
                hal::pc(hx[b >> 4]);
                hal::pc(hx[b & 0xF]);
                hal::pc(' ');
            }
            hal::ps(" |");
            for (int i = 0; i < 32; i++) {
                u8 b = buf[row * 32 + i];
                hal::pc((b >= 32 && b < 127) ? (char)b : '.');
            }
            hal::ps("|\n");
        }
    } else if (streq(av[1], "write")) {
        u64 sec = 0;
        if (ac < 3) { hal::ps("usage: blk write <sec> <text>\n"); return; }
        parse_num(av[2], &sec);
        u8 buf[512];
        for (int i = 0; i < 512; i++) buf[i] = 0;
        int pos = 0;
        for (int t = 3; t < ac && pos < 511; t++) {
            const char* txt = av[t];
            if (txt[0] == '"' || txt[0] == '\'') txt++;
            while (*txt && pos < 511) {
                if (*txt == '"' || *txt == '\'') { txt++; continue; }
                buf[pos++] = (u8)*txt++;
            }
            if (t + 1 < ac && pos < 511) buf[pos++] = ' ';
        }
        int r = virtio_blk::write_sector(sec, buf);
        hal::pf("write %s (%d), %u bytes\n", r == 0 ? "OK" : "ERROR", r, (u32)pos);
    } else {
        hal::ps("usage: blk [read <sec> | write <sec> <text>]\n");
    }
}

static void cmd_write(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: write <name> [text...]\n"); return; }
    const char* name = av[1];
    // Content = alle Tokens 2..ac, mit Space verbunden
    static char content[512];
    int pos = 0;
    for (int t = 2; t < ac && pos < 511; t++) {
        const char* txt = av[t];
        if (txt[0] == '"' || txt[0] == '\'') txt++;
        while (*txt && pos < 511) {
            if (*txt == '"' || *txt == '\'') { txt++; continue; }
            content[pos++] = *txt++;
        }
        if (t + 1 < ac && pos < 511) content[pos++] = ' ';
    }
    content[pos] = 0;
    int r = fs::write_file(name, content, pos);
    if (r < 0) hal::pf("write error %d\n", r);
    else hal::pf("OK, wrote %u bytes to '%s'\n", (u32)r, name);
}
static void cmd_append(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: append <name> [text...]\n"); return; }
    const char* name = av[1];
    static char add_buf[512];
    int pos = 0;
    for (int t = 2; t < ac && pos < 511; t++) {
        const char* txt = av[t];
        while (*txt && pos < 511) add_buf[pos++] = *txt++;
        if (t + 1 < ac && pos < 511) add_buf[pos++] = ' ';
    }
    add_buf[pos] = 0;
    u64 h = fs::hash_name(name);
    idx::Rec* r = idx::find(idx::NS_FILE | h);
    u64 old_size = (r && r->type == (u32)idx::Type::FILE) ? r->a : 0;
    if (old_size + pos > 1024*1024) { hal::ps("append: too large\n"); return; }
    char* buf = (char*)heap::alloc((usize)(old_size + pos + 1));
    if (!buf) { hal::ps("append: oom\n"); return; }
    if (old_size > 0) fs::cat_file(name, buf, (usize)old_size + 1);
    for (int i = 0; i < pos; i++) buf[old_size + i] = add_buf[i];
    buf[old_size + pos] = 0;
    int r2 = fs::write_file(name, buf, (usize)(old_size + pos));
    heap::free(buf);
    if (r2 < 0) hal::pf("append error %d\n", r2);
    else        hal::pf("OK, appended %u bytes (total %u)\n",
                        (u32)pos, (u32)(old_size + pos));
}

static void cmd_writetrunc(int ac, char** av) {
    if (ac < 3) { hal::ps("usage: writetrunc <name> <text...>\n"); return; }
    const char* name = av[1];
    static char buf[512];
    int n = 0;
    for (int t = 2; t < ac && n < 511; t++) {
        const char* p = av[t];
        while (*p && n < 511) buf[n++] = *p++;
        if (t + 1 < ac && n < 511) buf[n++] = ' ';
    }
    buf[n] = 0;

    // O_TRUNC: existierende Datei sofort leeren
    i64 fd = sys_open_kernel(name, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { hal::pf("writetrunc: open -> %d\n", (i32)fd); return; }

    // Schreibe buf an offset 0 (durch O_TRUNC ist size=0, offset=0)
    task::TCB* t = task::current;
    FileDesc* d = &t->fds[fd];
    if (!d->ram_buf) {
        u64 cap = (n < 64) ? 64 : (u64)n;
        d->ram_buf = (u64)heap::alloc((usize)cap);
        if (!d->ram_buf) { sys_close((u64)fd); hal::ps("writetrunc: oom\n"); return; }
        d->capacity = cap;
    }
    u8* dst = (u8*)d->ram_buf;
    for (int i = 0; i < n; i++) dst[i] = (u8)buf[i];
    d->size = (u64)n;
    d->offset = (u64)n;
    d->dirty = 1;

    i64 r = sys_close((u64)fd);
    if (r < 0) hal::pf("writetrunc: close -> %d\n", (i32)r);
    else       hal::pf("OK, writetrunc '%s' (%d bytes)\n", name, n);
}

static void cmd_trunc(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: trunc <name>\n"); return; }
    int r = fs::write_file(av[1], "", 0);
    if (r < 0) hal::pf("trunc error %d\n", r);
    else       hal::ps("OK\n");
}


static void cmd_pathtest(int ac, char** av) {
  if (ac < 2) { hal::ps("usage: pathtest <path>\n"); return; }
  fs::Path p; i64 r = fs::parse_path(av[1], &p);
  if (r < 0) { hal::pf("parse -> %ld\n", r); return; }
  hal::pf("parts=%u\n", (u32)p.n);
  for (u32 i = 0; i < p.n; i++) hal::pf("  [%u] '%s'\n", i, p.parts[i]);
  char b[256];
  i64 n = fs::build_path(&p, b, sizeof(b));
  if (n < 0) hal::pf("build -> %ld\n", n); else hal::pf("norm: %s (%ld)\n", b, n);
  n = fs::dirname_of(av[1], b, sizeof(b));
  if (n < 0) hal::pf("dirname -> %ld\n", n); else hal::pf("dirname: %s\n", b);
  n = fs::basename_of(av[1], b, sizeof(b));
  if (n < 0) hal::pf("basename -> %ld\n", n); else hal::pf("basename: %s\n", b);
}

static void cmd_mkdir(int ac, char** av) {
  if (ac < 2) { hal::ps("usage: mkdir <path>\n"); return; }
  char abs[128];
  i64 r = fs::fs_resolve(av[1], task::current->cwd, abs, sizeof(abs));
  if (r < 0) { hal::pf("mkdir: resolve -> %ld\n", r); return; }
  r = fs::fs_mkdir(abs);
  if (r < 0) hal::pf("mkdir %s -> %ld\n", abs, r);
  else       hal::pf("OK, mkdir %s\n", abs);
}
static void cmd_rmdir(int ac, char** av) {
  if (ac < 2) { hal::ps("usage: rmdir <path>\n"); return; }
  char abs[128];
  i64 r = fs::fs_resolve(av[1], task::current->cwd, abs, sizeof(abs));
  if (r < 0) { hal::pf("rmdir: resolve -> %ld\n", r); return; }
  r = fs::fs_rmdir(abs);
  if (r < 0) hal::pf("rmdir %s -> %ld\n", abs, r);
  else       hal::pf("OK, rmdir %s\n", abs);
}
static void cmd_pwdtest(int ac, char** av) {
  if (ac < 2) { hal::ps("usage: pwd-test <path>\n"); return; }
  char abs[128];
  i64 r = fs::fs_resolve(av[1], task::current->cwd, abs, sizeof(abs));
  if (r < 0) { hal::pf("resolve -> %ld\n", r); return; }
  bool e = fs::dir_exists(abs);
  hal::pf("%s: %s\n", abs, e ? "EXISTS" : "NOT FOUND");
}
static void cmd_lsdirs() {
  fs::fs_lsdirs();
}
static void cmd_cd(int ac, char** av) {
  if (ac < 2) {
    task::current->cwd[0] = '/'; task::current->cwd[1] = 0;
    return;
  }
  char abs[128];
  i64 r = fs::fs_resolve(av[1], task::current->cwd, abs, sizeof(abs));
  if (r < 0) { hal::pf("cd: resolve -> %ld\n", r); return; }
  if (!fs::dir_exists(abs)) { hal::pf("cd: %s: not found\n", abs); return; }
  for (u32 i = 0; i <= (u32)r; i++) task::current->cwd[i] = abs[i];
}
static void cmd_pwd() {
  hal::pf("%s\n", task::current->cwd);
}

static void cmd_pub(int ac, char** av) {
    if (ac < 3) { hal::ps("usage: pub <key> <a> [<b>]\n"); return; }
    u64 k = 0, a = 0, b = 0;
    if (!parse_num(av[1], &k) || !parse_num(av[2], &a)) { hal::ps("bad number\n"); return; }
    if (ac >= 4 && !parse_num(av[3], &b)) { hal::ps("bad number\n"); return; }
    ipc::publish(k, 0, a, b);
    hal::pf("OK, published key=0x%lx a=0x%lx seq=%lu\n", k, a, ipc::next_seq() - 1);
}

static u64 g_shell_last_seq = 0;  // A9: persistent event cursor

static void cmd_sub(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: sub <type_filter> [<timeout_ms>]\n"); return; }
    u64 tf = 0, tout = 1000;
    if (!parse_num(av[1], &tf)) { hal::ps("bad number\n"); return; }
    if (ac >= 3 && !parse_num(av[2], &tout)) { hal::ps("bad number\n"); return; }
    task::TCB* t = task::current;
    i64 h = ipc::sub_alloc(t->subs, tf);
    if (h < 0) { hal::pf("subscribe -> %ld\n", h); return; }
    t->subs[h].last_seq = g_shell_last_seq;
    hal::pf("waiting %lu ms (from seq %lu)...\n", tout, g_shell_last_seq);
    u64 k = 0, a = 0;
    i64 r = ipc::wait_event(t->subs, (u64)h, tout, &k, &a);
    if (r == 0) {
        g_shell_last_seq = t->subs[h].last_seq;
        hal::pf("EVENT: key=0x%lx a=0x%lx seq=%lu\n", k, a, g_shell_last_seq);
    }
    else if (r == -11) hal::ps("no event (EAGAIN)\n");
    else if (r == -110)hal::ps("timeout\n");
    else               hal::pf("wait -> %ld\n", r);
    ipc::sub_unsubscribe(t->subs, (u64)h);
}

static void cmd_sub_reset() {
    g_shell_last_seq = 0;
    hal::ps("OK, event cursor reset to 0\n");
}

static void cmd_piper(int ac, char** av) {
    (void)ac; (void)av;
    int r_idx = -1, w_idx = -1;
    if (ipc::pipe_alloc(&r_idx, &w_idx) < 0) { hal::ps("pipe_alloc fail\n"); return; }
    const char* msg = "hello through pipe";
    u64 len = 18;
    i64 w = ipc::pipe_write(w_idx, (const u8*)msg, len);
    hal::pf("wrote %ld bytes\n", w);
    char buf[64];
    i64 r = ipc::pipe_read(r_idx, (u8*)buf, sizeof(buf));
    hal::pf("read %ld bytes: '", r);
    for (i64 i = 0; i < r; i++) hal::pc(buf[i]);
    hal::ps("'\n");
    ipc::pipe_close(r_idx, true);
    ipc::pipe_close(w_idx, false);
}

static void producer_fn(void* arg) {
    int w_idx = (int)(u64)arg;
    task::sys_sleep(200);  // 200 ms warten
    const char* msg = "async through pipe";
    ipc::pipe_write(w_idx, (const u8*)msg, 19);
    ipc::pipe_close(w_idx, false);
    for (;;) __asm__ volatile("sti; hlt");
}

static void cmd_piper_async(int ac, char** av) {
    (void)ac; (void)av;
    int r_idx = -1, w_idx = -1;
    if (ipc::pipe_alloc(&r_idx, &w_idx) < 0) {
        hal::ps("pipe_alloc fail\n"); return;
    }
    task::spawn("producer", producer_fn, (void*)(u64)w_idx);
    hal::ps("producer started, blocking read...\n");
    char buf[64];
    for (;;) {
        i64 r = ipc::pipe_read(r_idx, (u8*)buf, sizeof(buf));
        if (r == -11) { __asm__ volatile("sti; hlt"); continue; }
        hal::pf("read %ld bytes: '", r);
        for (i64 i = 0; i < r; i++) hal::pc(buf[i]);
        hal::ps("'\n");
        break;
    }
    ipc::pipe_close(r_idx, true);
}

static void cmd_lseek(int ac, char** av) {
    if (ac < 4) { hal::ps("usage: lseek <name> <offset> <whence 0=SET 1=CUR 2=END>\n"); return; }
    const char* name = av[1];
    u64 off_u = 0, whence_u = 0;
    if (!parse_num(av[2], &off_u) || !parse_num(av[3], &whence_u)) {
        hal::ps("bad number\n"); return;
    }
    if (whence_u > 2) { hal::ps("whence must be 0, 1, or 2\n"); return; }

    i64 fd = sys_open_kernel(name, O_RDONLY);
    if (fd < 0) { hal::pf("open('%s') -> %d\n", name, (i32)fd); return; }

    i64 r = sys_lseek((u64)fd, (i64)off_u, (u32)whence_u);
    if (r < 0) {
        hal::pf("lseek -> %d (E_INVAL)\n", (i32)r);
        sys_close((u64)fd);
        return;
    }
    hal::pf("new offset = %ld\n", r);

    char buf[33];
    i64 n = sys_read_kernel((int)fd, buf, 32);
    if (n > 0) {
        buf[n] = 0;
        // Newlines fuer Ausgabe entschaerfen
        for (i64 i = 0; i < n; i++) if (buf[i] == '\n') buf[i] = ' ';
        hal::pf("read 32 bytes @ %ld: '%s'\n", r, buf);
    } else if (n == 0) {
        hal::pf("EOF at %ld\n", r);
    } else {
        hal::pf("read -> %d\n", (i32)n);
    }
    sys_close((u64)fd);
}


static void cmd_prio(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: prio <pid> [<new_prio 0-10>] | prio list\n"); return; }
    if (streq(av[1], "list")) { cmd_tasks(); return; }
    u64 pid = 0;
    if (!parse_num(av[1], &pid)) { hal::ps("bad pid\n"); return; }
    if (ac < 3) {
        i64 p = task::sys_getpriority(pid);
        if (p < 0) { hal::pf("getpriority(%lu) -> %ld (no such task)\n", pid, p); return; }
        hal::pf("pid=%lu priority=%ld\n", pid, p);
        return;
    }
    u64 newp = 0;
    if (!parse_num(av[2], &newp)) { hal::ps("bad prio\n"); return; }
    if (newp > 10) { hal::ps("priority must be 0..10\n"); return; }
    i64 r = task::sys_setpriority(pid, newp);
    if (r < 0) { hal::pf("setpriority(%lu, %lu) -> %ld\n", pid, newp, r); return; }
    hal::pf("OK, pid=%lu priority=%lu\n", pid, newp);
    if (pid == task::current->pid && newp < 3)
        hal::ps("WARN: current task demoted to <3; may starve\n");
}


static void cmd_sleep(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: sleep <ms>  (cap 30000)\n"); return; }
    u64 ms = 0;
    if (!parse_num(av[1], &ms)) { hal::ps("bad number\n"); return; }
    hal::pf("sleeping %u ms...\n", (u32)ms);
    i64 r = task::sys_sleep(ms);
    if (r < 0) hal::pf("sleep -> %ld\n", r);
    else       hal::ps("woke up\n");
}

static void cmd_cat(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: cat <name>\n"); return; }
    static char buf[8192];   // bis ~16 Extent-Bloecke
    int n = fs::cat_file(av[1], buf, sizeof(buf));
    if (n < 0) { hal::pf("not found: %s\n", av[1]); return; }
    hal::pf("%s\n", buf);
}
static const char* type_name(u32 t) {
    switch (t) {
        case 0: return "TASK";  case 1: return "CONFIG"; case 2: return "EVENT";
        case 3: return "TRIGGER"; case 4: return "SYS"; case 5: return "FILE";
        case 6: return "NAME";
        case 8: return "PATH"; case 9: return "MSG"; default: return "?";
    }
}
static bool parse_type_arg(const char* t, u32* out) {
    if      (streq(t, "task") || streq(t, "t"))    *out = 0;
    else if (streq(t, "config") || streq(t, "c"))  *out = 1;
    else if (streq(t, "event") || streq(t, "e"))   *out = 2;
    else if (streq(t, "trigger") || streq(t, "r")) *out = 3;
    else if (streq(t, "sys") || streq(t, "s"))     *out = 4;
    else if (streq(t, "file") || streq(t, "f"))    *out = 5;
    else if (streq(t, "name") || streq(t, "n"))    *out = 6;
    else return false;
    return true;
}
static void cmd_hash(int ac, char** av) {
    if (ac >= 2) {
        const char* arg = av[1];
        if (streq(arg, "all")) {
            u32 n = 0;
            u64 h = idx::compute_hash(1, 0, &n);
            hal::pf("full hash: %p  (%u records)\n", h, n);
            return;
        }
        u32 t;
        if (parse_type_arg(arg, &t)) {
            u32 n = 0;
            u64 h = idx::compute_hash(2, t, &n);
            hal::pf("hash(%s): %p  (%u records)\n", type_name(t), h, n);
            return;
        }
        hal::ps("usage: hash [all|t|c|e|r|s|f|n]\n");
        return;
    }
    u32 n = 0;
    u64 h = idx::compute_hash(0, 0, &n);
    hal::pf("state hash: %p  (%u persistent records)\n", h, n);
}

static constexpr int HIST_MAX = 64;
struct HistEntry { u8 op; u8 type; u64 key; u64 a; u64 b; };
static HistEntry g_hist_buf[HIST_MAX];
static int g_hist_n, g_hist_max;
static u64 g_hist_total;
static void hist_visit(u64 idx, u8 op, u8 type, u64 key, u64 a, u64 b, void*) {
    if (g_hist_n < g_hist_max) g_hist_buf[g_hist_n++] = {op, type, key, a, b};
    else {
        for (int i = 0; i < g_hist_max - 1; i++) g_hist_buf[i] = g_hist_buf[i+1];
        g_hist_buf[g_hist_max - 1] = {op, type, key, a, b};
    }
    g_hist_total = idx + 1;
}
static void cmd_history(int ac, char** av) {
    int n = 20;
    if (ac >= 2) { u64 v; if (parse_num(av[1], &v)) n = (int)v; }
    if (n < 1) n = 1;
    if (n > HIST_MAX) n = HIST_MAX;
    g_hist_n = 0; g_hist_max = n; g_hist_total = 0;
    persist::wal_iterate_indexed(hist_visit, nullptr);
    if (g_hist_n == 0) { hal::ps("(no WAL entries)\n"); return; }
    u64 start = g_hist_total - g_hist_n;
    hal::pf("history (%u of %u entries):\n", (u32)g_hist_n, (u32)g_hist_total);
    for (int i = 0; i < g_hist_n; i++) {
        HistEntry* e = &g_hist_buf[i];
        const char* opstr = (e->op == 0) ? "put" : "rm ";
        hal::pf("  [%u] %s %-7s key=%p a=%p b=%p\n",
                (u32)(start + i), opstr, type_name(e->type), e->key, e->a, e->b);
    }
}

static u64 g_trace_key; static int g_trace_count;
static void trace_visit(u64 idx, u8 op, u8 type, u64 key, u64 a, u64 b, void*) {
    if (key != g_trace_key) return;
    g_trace_count++;
    const char* opstr = (op == 0) ? "put" : "rm ";
    hal::pf("  [%u] %s %-7s a=%p b=%p\n", (u32)idx, opstr, type_name(type), a, b);
}
static void cmd_trace(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: trace <key> | trace <c|t|e|r> <id>\n"); return; }
    u64 key;
    if (!resolve_key(ac, av, 1, &key)) { hal::ps("bad key\n"); return; }
    g_trace_key = key; g_trace_count = 0;
    hal::pf("trace %p:\n", key);
    persist::wal_iterate_indexed(trace_visit, nullptr);
    hal::pf("  total: %u\n", (u32)g_trace_count);
}

static u32 g_watch_filter; static u64 g_watch_start, g_watch_max;
static void watch_visit(u64 idx, u8 op, u8 type, u64 key, u64 a, u64 b, void*) {
    if (idx + 1 > g_watch_max) g_watch_max = idx + 1;
    if (idx < g_watch_start) return;
    if (g_watch_filter != 255 && type != g_watch_filter) return;
    const char* opstr = (op == 0) ? "put" : "rm ";
    hal::pf("  [%u] %s %-7s key=%p a=%p b=%p\n",
            (u32)idx, opstr, type_name(type), key, a, b);
}
static void cmd_watch(int ac, char** av) {
    u32 filter = 255;
    if (ac >= 2) {
        if (!parse_type_arg(av[1], &filter)) { hal::ps("unknown type\n"); return; }
    }
    hal::ps("[watch] any key to stop\n");
    g_watch_filter = filter; g_watch_max = 0; g_watch_start = (u64)-1;
    persist::wal_iterate_indexed(watch_visit, nullptr);
    g_watch_start = g_watch_max;
    for (;;) {
        int ch = kbd::getchar_nb();
        if (ch >= 0) break;
        persist::wal_iterate_indexed(watch_visit, nullptr);
        g_watch_start = g_watch_max;
        for (int i = 0; i < 500000; i++) __asm__ volatile("" ::: "memory");
    }
    hal::ps("[watch] stopped\n");
}

static void cmd_checkpoint(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: checkpoint <name>\n"); return; }
    int r = persist::checkpoint_create(av[1]);
    if (r == 0) hal::pf("checkpoint '%s' saved\n", av[1]);
    else        hal::pf("checkpoint failed (%d)\n", r);
}

static void cmd_snapshots() {
    u8 buf[512];
    virtio_blk::read_sector(8190, buf);
    u32 magic = *(u32*)(buf + 0);
    u32 count = *(u32*)(buf + 4);
    if (magic != 0x43484B50 || count > 12) { hal::ps("(no catalog)\n"); return; }
    hal::ps("checkpoints:\n");
    for (u32 i = 0; i < count; i++) {
        u8* p = buf + 8 + i * 40;
        char nm[17];
        for (int j = 0; j < 16; j++) nm[j] = (char)p[j];
        nm[16] = 0;
        u64 h = *(u64*)(p + 16);
        u32 st = *(u32*)(p + 24);
        u32 ns = *(u32*)(p + 28);
        u32 ts = *(u32*)(p + 32);
        hal::pf("  %-16s  hash=%p  sect=%u+%u  tick=%u\n", nm, h, st, ns, ts);
    }
    hal::pf("  total: %u\n", count);
}

static void cmd_restore(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: restore <name>\n"); return; }
    hal::ps("restoring...\n");
    int r = persist::checkpoint_restore(av[1]);
    if (r == 0)      hal::pf("restored to '%s'\n", av[1]);
    else if (r == -2) hal::ps("restore: verification failed\n");
    else              hal::pf("restore failed (%d)\n", r);
}

static void cmd_factory_reset() {
    int r = persist::checkpoint_restore("factory-default");
    if (r == 0) hal::ps("factory reset OK.\n");
    else        hal::pf("factory reset failed (%d)\n", r);
}

static void cmd_bmtest() {
    static u64 allocs[5000];
    u64 start_dirty  = fs::bm_dirty_count();
    u64 start_writes = fs::bm_write_count();
    int n_ok = 0;
    for (int i = 0; i < 5000; i++) {
        u64 sec = fs::blk_alloc_nosave();
        if (!sec) break;
        allocs[n_ok++] = sec;
    }
    u64 dirty_after_alloc = fs::bm_dirty_count();
    hal::pf("allocated %u blocks, dirty sectors: %u -> %u\n",
            (u32)n_ok, (u32)start_dirty, (u32)dirty_after_alloc);
    fs::bitmap_save();
    u64 dirty_after_save = fs::bm_dirty_count();
    u64 writes_saved = fs::bm_write_count() - start_writes;
    hal::pf("after save: dirty=%u, writes +%u\n",
            (u32)dirty_after_save, (u32)writes_saved);
    for (int i = 0; i < n_ok; i++) fs::blk_free(allocs[i]);
    fs::bitmap_save();
    hal::pf("after free+save: dirty=%u, writes +%u total\n",
            (u32)fs::bm_dirty_count(),
            (u32)(fs::bm_write_count() - start_writes));
}

static void cmd_ls(int ac, char** av) {
  if (ac >= 2) {
    fs::fs_ls_dir(av[1]);
  } else {
    fs::fs_ls_dir(task::current->cwd);
  }
}
static void cmd_rm(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: rm <name>\n"); return; }
    if (fs::rm_file(av[1]) == 0) hal::pf("removed: %s\n", av[1]);
    else hal::pf("not found: %s\n", av[1]);
}

static void cmd_edit(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: edit <name>\n"); return; }
    editor::run(av[1]);
    hal::ps("NEXUS> ");
}

// A3e-Task: eigener Address-Space, eigener Seed aus der PID.
// Zwei gleichzeitig laufende a3e-Tasks schreiben unterschiedliche Seeds
// an dieselbe VA in ihren User-Stacks und pruefen die Unversehrtheit.

// Generischer User-ELF-Task. Der TCB traegt pml4/entry/stack_top.
static void user_elf_fn(void*) {
    task::TCB* self = task::current;
    if (!self || !self->pml4_phys || !self->user_entry_va) {
        hal::ps("[ELF] invalid TCB\n");
        for(;;) __asm__ volatile("hlt");
    }

    __asm__ volatile("cli");
    __asm__ volatile("movq %0, %%cr3" :: "r"(self->pml4_phys) : "memory");

    // user_stack_top_va traegt immer den fertigen rsp.
    u64 ursp = self->user_stack_top_va;
    u64 urip = self->user_entry_va;

    hal::pf("[ELF] pid=%u entry=%p stack=%p pml4=%p iretq\n",
            (u32)self->pid, urip, self->user_stack_top_va, self->pml4_phys);

    __asm__ volatile(
        "cli\n"
        "swapgs\n"
        "pushq $0x23\n"
        "pushq %[ursp]\n"
        "pushq $0x202\n"
        "pushq $0x1B\n"
        "pushq %[urip]\n"
        "iretq\n"
        :: [ursp] "r"(ursp), [urip] "r"(urip)
        : "memory"
    );
    for(;;) __asm__ volatile("hlt");
}

// Baut PML4 + User-Stack, laedt das ELF, spawnt den Task.
// arg: optionaler String, wird als argv[0] uebergeben (Linux-Konvention).
//      nullptr = argc=0.
static task::TCB* spawn_elf_task(const char* name,
                                 const u8* elf, u64 elf_len,
                                 const char* source,
                                 const char* arg) {
    u64 pml4 = vmm::alloc_pml4();
    if (!pml4) { hal::ps("[ELF] alloc_pml4 fail\n"); return nullptr; }

    u64 entry = 0;
    int r = elf64::load_into(pml4, elf, elf_len, &entry);
    if (r != 0) {
        hal::pf("[ELF] load failed (%d) for %s\n", r, source);
        vmm::free_user_all(pml4);
        return nullptr;
    }

    // 16 KiB User-Stack, Top bei USER_STACK_VA + 16 KiB
    // (PML4[0], sicher unterhalb HHDM/Kernel)
    const u64 stack_top = USER_STACK_VA + 0x4000;
    const u64 stack_lo  = stack_top - 0x4000;
    u64 last_page_phys = 0;
    for (u64 va = stack_lo; va < stack_top; va += 0x1000) {
        u64 sp = mem::pmm::alloc();
        if (!sp) { vmm::free_user_all(pml4); return nullptr; }
        u8* s = (u8*)mem::p2v(sp);
        for (int k = 0; k < 4096; k++) s[k] = 0;
        if (!vmm::map_user_page_in(pml4, va, sp, vmm::P_WRITE)) {
            mem::pmm::free_n(sp, 1);
            vmm::free_user_all(pml4);
            return nullptr;
        }
        if (va == stack_top - 0x1000) last_page_phys = sp;
    }

    u64 rsp_user = setup_argc_argv(stack_top, last_page_phys, arg);
    if (!rsp_user) { vmm::free_user_all(pml4); return nullptr; }

    return task::spawn_full(name, user_elf_fn, nullptr, pml4, entry, rsp_user);
}

// --- Shell-Kommandos ---
static void cmd_run(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: run <name>\n"); return; }
    static char buf[256 * 1024];
    int n = fs::cat_file(av[1], buf, sizeof(buf));
    if (n <= 0) { hal::pf("run: not found or empty: %s\n", av[1]); return; }
    task::TCB* t = spawn_elf_task("elf", (const u8*)buf, (u64)n, av[1], nullptr);
    if (t) hal::pf("[RUN] '%s' spawned pid=%u\n", av[1], (u32)t->pid);
}

static void cmd_hello() {
    static char buf[16 * 1024];
    int n = fs::cat_file("hello.elf", buf, sizeof(buf));
    if (n <= 0 || n >= (int)sizeof(buf)) {
        hal::pf("[RUN] hello.elf load failed (%d)\n", n);
        return;
    }
    task::TCB* t = spawn_elf_task("hello", (const u8*)buf, (u64)n,
                                  "hello.elf", nullptr);
    if (t) hal::pf("[RUN] hello pid=%u\n", (u32)t->pid);
}
static void cmd_fib() {
    static char buf[16 * 1024];
    int n = fs::cat_file("fib.elf", buf, sizeof(buf));
    if (n <= 0 || n >= (int)sizeof(buf)) {
        hal::pf("[RUN] fib.elf load failed (%d)\n", n);
        return;
    }
    task::TCB* t = spawn_elf_task("fib", (const u8*)buf, (u64)n,
                                  "fib.elf", nullptr);
    if (t) hal::pf("[RUN] fib pid=%u\n", (u32)t->pid);
}

// Spawnt einen User-Task. Dessen Entry springt per iretq nach Ring 3,
// fuehrt sys_write + sys_exit aus, und der Scheduler raeumt ihn ab.
static void cmd_ring3() {
    task::spawn("ring3", user_ring3_fn, nullptr);
    hal::ps("[R3 ] spawned user task\n");
}
// A6a-Test: gleicher Task, aber mit 0xdeadbeef als sys_write-Puffer.
// user_range_ok muss das abweisen, sonst kernel-panic.
static void cmd_r3bad() {
    task::spawn("r3bad", user_ring3_fn, (void*)1);
    hal::ps("[R3 ] spawned BADPTR user task\n");
}
static void cmd_r3kern() {
    task::spawn("r3kern", user_ring3_fn, (void*)2);
    hal::ps("[R3 ] spawned KERNPTR user task\n");
}

// Leak-Test: spawnt N ring3-Tasks und kehrt sofort zurueck (NICHT
// blockieren — der Shell-Task teilt sich die CPU mit den neuen Tasks,
// eine Busy-Loop wuerde minutenlang das Prompt blockieren).
// Vorgehen: bench N -> warten -> mem, mem. Beide used-Werte muessen
// gleich sein.
static void cmd_bench(int ac, char** av) {
    u64 n = 20;
    if (ac >= 2) { u64 v; if (shell_parse_num(av[1], &v)) n = v; }
    if (n > 100) n = 100;

    u64 start_used = mem::pmm::used_count();
    u64 slab_u0 = slab::used(), slab_t0 = slab::total();
    hal::pf("[BENCH] pre: pmm_used=%u slab_used=%u slab_total=%u\n",
            (u32)start_used, (u32)slab_u0, (u32)slab_t0);
    for (u64 i = 0; i < n; i++)
        task::spawn("bench", user_ring3_fn, nullptr);
    u64 slab_u1 = slab::used(), slab_t1 = slab::total();
    hal::pf("[BENCH] post: pmm_used=%u slab_used=%u slab_total=%u\n",
            (u32)mem::pmm::used_count(), (u32)slab_u1, (u32)slab_t1);
    hal::pf("[BENCH] delta: pmm=%+d slab_used=%+d slab_total=%+d\n",
            (int)(mem::pmm::used_count() - start_used),
            (int)(slab_u1 - slab_u0),
            (int)(slab_t1 - slab_t0));
}

// A3e: spawnt zwei Test-Tasks, die unterschiedliche Seeds schreiben und lesen.
static void cmd_a3e() {
    task::spawn("a3e-a", user_a3e_fn, nullptr);
    task::spawn("a3e-b", user_a3e_fn, nullptr);
    hal::ps("[A3E] spawned 2 tasks\n");
}

// Ring-3-Test: mappt Code- und Stack-Page, springt per iretq nach CPL=3.
// User-Code ist `int3` (0xCC) gefolgt von `hlt`. Der #BP-Dump zeigt
// cs=0x1b, was beweist, dass wir wirklich in Ring 3 waren.
static void cmd_int3_test() {
    hal::ps("[R3 ] alloc user code + stack + kernel stack...\n");
    u64 code_phys   = mem::pmm::alloc();
    u64 stack_phys  = mem::pmm::alloc();
    u64 kstack_phys = mem::pmm::alloc();
    if (!code_phys || !stack_phys || !kstack_phys) {
        hal::ps("[R3 ] oom\n"); return;
    }

    u64 code_va  = 0x0000008000000000ULL;
    u64 stack_va = 0x0000008000100000ULL;

    __asm__ volatile("cli");

    u8* code = (u8*)mem::p2v(code_phys);
    code[0] = 0xCC;                 // int3
    code[1] = 0xF4;                 // hlt
    for (int i = 2; i < 4096; i++) code[i] = 0;

    u8* stk = (u8*)mem::p2v(stack_phys);
    for (int i = 0; i < 4096; i++) stk[i] = 0;

    if (!vmm::map_user_page(code_va,  code_phys,  0)) {
        hal::ps("[R3 ] map code fail\n"); for(;;) __asm__ volatile("hlt");
    }
    if (!vmm::map_user_page(stack_va, stack_phys, vmm::P_WRITE)) {
        hal::ps("[R3 ] map stack fail\n"); for(;;) __asm__ volatile("hlt");
    }

    gdt::tss_set_rsp0((u64)mem::p2v(kstack_phys) + 4096);

    hal::pf("[R3 ] code @%p phys=%p\n", code_va, code_phys);
    hal::pf("[R3 ] stack@%p phys=%p\n", stack_va, stack_phys);
    hal::pf("[R3 ] kstack top=%p\n", (u64)mem::p2v(kstack_phys) + 4096);
    hal::ps("[R3 ] iretq to ring 3...\n");

    u64 user_rsp = stack_va + 4096 - 16;
    u64 user_rip = code_va;

    __asm__ volatile(
        "pushq $0x23\n"
        "pushq %[ursp]\n"
        "pushq $0x202\n"
        "pushq $0x1B\n"
        "pushq %[urip]\n"
        "iretq\n"
        :: [ursp] "r"(user_rsp), [urip] "r"(user_rip)
        : "memory"
    );
    for(;;) __asm__ volatile("hlt");
}

// Datei mit wiederholendem Muster fuellen. Newline alle 64 Bytes,
// sonst 'A'..'Z' rotierend. Optionaler Seed variiert den Inhalt,
// damit zwei Files unterschiedliche Hashes liefern (fuer Restore-Tests).
static void cmd_gen(int ac, char** av) {
    if (ac < 3) { hal::ps("usage: gen <name> <bytes> [seed]\n"); return; }
    u64 n = 0, seed = 0;
    if (!parse_num(av[2], &n)) { hal::ps("bad size\n"); return; }
    if (ac >= 4 && !parse_num(av[3], &seed)) { hal::ps("bad seed\n"); return; }
    if (n == 0)            { hal::ps("size 0\n"); return; }
    if (n > 1024 * 1024)   { hal::ps("max 1 MiB\n"); return; }

    u8* buf = (u8*)heap::alloc((usize)n);
    if (!buf) { hal::ps("oom\n"); return; }
    for (u64 i = 0; i < n; i++) {
        if ((i % 64) == 63) buf[i] = '\n';
        else                buf[i] = (u8)('A' + ((i + seed) % 26));
    }

    int r = fs::write_file(av[1], (const char*)buf, (usize)n);
    heap::free(buf);
    if (r < 0) hal::pf("gen error %d\n", r);
    else       hal::pf("OK, generated %u bytes to '%s'\n", (u32)r, av[1]);
}

// Groesse + FNV-Hash des Datei-Inhalts. Fuer Content-Integritaets-Check.
static void cmd_wc(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: wc <name>\n"); return; }
    static char buf[1024 * 1024];
    int n = fs::cat_file(av[1], buf, sizeof(buf));
    if (n < 0) { hal::pf("not found: %s\n", av[1]); return; }
    u64 h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < n; i++) {
        h ^= (u8)buf[i];
        h *= 0x100000001b3ULL;
    }
    hal::pf("%u bytes, fnv=%p\n", (u32)n, h);
}

static void cmd_pci() {
    hal::ps("PCI devices:\n");
    pci::scan(pci_visit, nullptr);
    hal::pf("  total: %u\n", (u32)pci::count());
}

static void cmd_netinfo() {
    if (!virtio_net::is_ready()) {
        hal::ps("virtio-net not present\n");
        return;
    }
    u8 mac[6];
    virtio_net::get_mac(mac);
    hal::ps("virtio-net: ready\n");
    hal::pf("  MAC:    %x:%x:%x:%x:%x:%x\n",
            (u32)mac[0], (u32)mac[1], (u32)mac[2],
            (u32)mac[3], (u32)mac[4], (u32)mac[5]);
    hal::pf("  queues: rx_qsz=%u tx_qsz=%u\n",
            virtio_net::rx_qsize(), virtio_net::tx_qsize());
    u64 rx=0, tx=0, rxb=0, txb=0;
    virtio_net::stats(&rx, &tx, &rxb, &txb);
    hal::pf("  RX:     %lu pkts, %lu bytes\n", rx, rxb);
    hal::pf("  TX:     %lu pkts, %lu bytes\n", tx, txb);
    net::Iface* f = net::default_iface();
    if (f) {
        hal::pf("  iface %s: ip=%u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u\n",
                f->name,
                f->ip[0], f->ip[1], f->ip[2], f->ip[3],
                f->mask[0], f->mask[1], f->mask[2], f->mask[3],
                f->gw[0], f->gw[1], f->gw[2], f->gw[3]);
        hal::pf("  IP:     rx=%lu tx=%lu pkts\n",
                net::stat_rx_ip(), net::stat_tx_ip());
    }
}

// A11.3b: Ring-3-Programm, das sys_net_ping (nr=200) aufruft.
// Bytes: mov rax,200; mov rdi,ip_be; mov rsi,3; mov rdx,500;
//        syscall; mov rdi,rax; mov rax,60; syscall
// Ergebnis: exit-Code = Anzahl erfolgreicher Pings (0..3).
static void emit_r3ping_program(u8* code, u32 ip_be) {
    for (int i = 0; i < 256; i++) code[i] = 0;
    u32 p = 0;
    // mov rax, 200  (48 B8 imm64)
    code[p++] = 0x48; code[p++] = 0xB8;
    u64 v = 200; for (int i = 0; i < 8; i++) code[p++] = (u8)(v >> (8*i));
    // mov rdi, ip_be  (48 BF imm64)
    code[p++] = 0x48; code[p++] = 0xBF;
    v = ip_be; for (int i = 0; i < 8; i++) code[p++] = (u8)(v >> (8*i));
    // mov rsi, 3  (48 BE imm64)
    code[p++] = 0x48; code[p++] = 0xBE;
    v = 3; for (int i = 0; i < 8; i++) code[p++] = (u8)(v >> (8*i));
    // mov rdx, 500  (48 BA imm64)
    code[p++] = 0x48; code[p++] = 0xBA;
    v = 500; for (int i = 0; i < 8; i++) code[p++] = (u8)(v >> (8*i));
    // syscall (0F 05)
    code[p++] = 0x0F; code[p++] = 0x05;
    // mov rdi, rax  (48 89 C7)
    code[p++] = 0x48; code[p++] = 0x89; code[p++] = 0xC7;
    // mov rax, 60  (48 B8 imm64)
    code[p++] = 0x48; code[p++] = 0xB8;
    v = 60; for (int i = 0; i < 8; i++) code[p++] = (u8)(v >> (8*i));
    // syscall
    code[p++] = 0x0F; code[p++] = 0x05;
    // (unreachable) hlt
    code[p++] = 0xF4;
}

static void cmd_r3ping(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: r3ping <ip>\n"); return; }
    u8 ip[4] = {0,0,0,0};
    int dots = 0, cur = 0;
    for (const char* p = av[1]; *p && dots < 4; p++) {
        if (*p == '.') { ip[dots++] = (u8)cur; cur = 0; }
        else if (*p >= '0' && *p <= '9') cur = cur * 10 + (*p - '0');
        else break;
    }
    if (dots == 3) ip[3] = (u8)cur;
    else { hal::ps("bad ip\n"); return; }

    u32 ip_be = ((u32)ip[0] << 24) | ((u32)ip[1] << 16) |
                ((u32)ip[2] <<  8) |  (u32)ip[3];

    // Physikalische Pages fuer Code + Stack
    u64 code_phys  = mem::pmm::alloc();
    u64 stack_phys = mem::pmm::alloc();
    if (!code_phys || !stack_phys) { hal::ps("pmm fail\n"); return; }

    u8* code = (u8*)mem::p2v(code_phys);
    emit_r3ping_program(code, ip_be);

    u8* stk = (u8*)mem::p2v(stack_phys);
    for (int i = 0; i < 4096; i++) stk[i] = 0;

    u64 my_pml4 = vmm::alloc_pml4();
    if (!my_pml4) { hal::ps("alloc_pml4 fail\n"); return; }

    u64 code_va  = 0x0000000040000000ULL;
    u64 stack_va = 0x0000000050000000ULL;
    if (!vmm::map_user_page_in(my_pml4, code_va,  code_phys,  1)) {
        hal::ps("map code fail\n"); return;
    }
    if (!vmm::map_user_page_in(my_pml4, stack_va, stack_phys, 1)) {
        hal::ps("map stack fail\n"); return;
    }

    task::TCB* t = task::spawn_full("r3ping", user_elf_fn, nullptr,
                                    my_pml4, code_va, stack_va + 0x3ff0);
    if (!t) { hal::ps("spawn fail\n"); return; }
    hal::pf("[R3PING] spawned pid=%u dst=%u.%u.%u.%u (nr=200)\n",
            (u32)t->pid, ip[0], ip[1], ip[2], ip[3]);
}

static bool _parse_ip4(const char* str, u8 out[4]) {
    int dots = 0, cur = 0;
    for (const char* p = str; *p && dots < 4; p++) {
        if (*p == '.') { out[dots++] = (u8)cur; cur = 0; }
        else if (*p >= '0' && *p <= '9') cur = cur * 10 + (*p - '0');
        else return false;
    }
    if (dots == 3) { out[3] = (u8)cur; return true; }
    return false;
}

static void cmd_dns(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: dns <hostname>\n"); return; }
    u8 ip[4] = {0};
    int r = net::dns_resolve(av[1], ip);
    if (r == 0) {
        hal::pf("%s -> %u.%u.%u.%u\n",
                av[1], ip[0], ip[1], ip[2], ip[3]);
    } else {
        hal::pf("dns fail (err %d)\n", r);
    }
}

static void cmd_httpget(int ac, char** av) {
    if (ac < 4) { hal::ps("usage: httpget <ip-or-host> <port> <path>\n"); return; }

    u8 ip[4];
    if (!_parse_ip4(av[1], ip)) {
        if (net::dns_resolve(av[1], ip) != 0) {
            hal::ps("[HTTP] bad ip and dns failed\n");
            return;
        }
    }

    u32 port = 0;
    for (const char* q = av[2]; *q >= '0' && *q <= '9'; q++)
        port = port * 10 + (u32)(*q - '0');
    if (port == 0 || port > 65535) { hal::ps("bad port\n"); return; }

    const char* path = av[3];
    u8 body[8192];
    u32 body_len = 0;

    hal::pf("[HTTP] GET %s (dst=%u.%u.%u.%u:%u)\n",
            path, ip[0], ip[1], ip[2], ip[3], port);

    int status = net::http_get(ip, av[1], (u16)port, path,
                               body, sizeof(body), &body_len,
                               true, 5000);

    if (status < 0) {
        hal::pf("\n[HTTP] transport error %d\n", status);
        return;
    }

    hal::pf("\n[HTTP] status=%d, body=%u bytes\n", status, body_len);
    if (body_len > 0) {
        hal::ps("--- body ---\n");
        for (u32 k = 0; k < body_len; k++) hal::pc((char)body[k]);
        hal::ps("\n--- end ---\n");
    } else {
        hal::ps("[HTTP] empty body\n");
    }
}

static void cmd_msgsend(int ac, char** av) {
    if (ac < 3) { hal::ps("usage: msgsend <ch> <text>\n"); return; }
    u32 ch = 0;
    for (const char* p = av[1]; *p >= '0' && *p <= '9'; p++)
        ch = ch * 10 + (u32)(*p - '0');
    if (ch >= 16) { hal::ps("bad channel (0-15)\n"); return; }
    u32 len = 0;
    while (av[2][len] && len < 255) len++;
    int r = ipc::msg_send((u8)ch, (const u8*)av[2], len);
    if (r < 0) hal::pf("msgsend: err %d\n", r);
    else       hal::pf("[MSG] sent %u bytes to ch=%u\n", len, ch);
}

static void cmd_msgrecv(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: msgrecv <ch> [timeout_ms]\n"); return; }
    u32 ch = 0;
    for (const char* p = av[1]; *p >= '0' && *p <= '9'; p++)
        ch = ch * 10 + (u32)(*p - '0');
    if (ch >= 16) { hal::ps("bad channel\n"); return; }
    u32 to = 1000;
    if (ac >= 3) {
        u32 v = 0;
        for (const char* p = av[2]; *p >= '0' && *p <= '9'; p++)
            v = v * 10 + (u32)(*p - '0');
        if (v > 0 && v <= 30000) to = v;
    }
    u8 buf[256];
    int r = ipc::msg_recv((u8)ch, buf, sizeof(buf), to);
    if (r < 0) {
        if (r == -110) hal::ps("[MSG] timeout\n");
        else           hal::pf("[MSG] err %d\n", r);
        return;
    }
    hal::pf("[MSG] rx %d bytes from ch=%u: '", r, ch);
    for (int i = 0; i < r; i++) hal::pc((char)buf[i]);
    hal::ps("'\n");
}

static void cmd_msgs() {
    u32 used = 0, total = 0;
    u64 seq = 0;
    ipc::msg_stats(&used, &total, &seq);
    hal::pf("msg pool: %u/%u slots, seq=%lu\n", used, total, seq);
    // Zaehle MSG-Records im Index
    struct C { u32 n; } c = {0};
    idx::scan(idx::Type::MSG,
              [](idx::Rec*, void* vp) { ((C*)vp)->n++; }, &c);
    hal::pf("idx: %u MSG-Records\n", c.n);
}

static void cmd_udpsend(int ac, char** av) {
    if (ac < 4) { hal::ps("usage: udpsend <ip> <port> <text>\n"); return; }
    u8 ip[4];
    if (!_parse_ip4(av[1], ip)) { hal::ps("bad ip\n"); return; }
    u32 port = 0;
    for (const char* p = av[2]; *p >= '0' && *p <= '9'; p++) port = port * 10 + (u32)(*p - '0');
    if (port == 0 || port > 65535) { hal::ps("bad port\n"); return; }
    u32 tlen = 0;
    while (av[3][tlen]) tlen++;
    net::udp_send(5001, ip, (u16)port, (const u8*)av[3], tlen);
}

static void cmd_udprecv(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: udprecv <port> [timeout_ms]\n"); return; }
    u32 port = 0;
    for (const char* p = av[1]; *p >= '0' && *p <= '9'; p++) port = port * 10 + (u32)(*p - '0');
    if (port == 0 || port > 65535) { hal::ps("bad port\n"); return; }
    u32 to = 3000;
    if (ac >= 3) {
        u32 v = 0;
        for (const char* p = av[2]; *p >= '0' && *p <= '9'; p++) v = v * 10 + (u32)(*p - '0');
        if (v > 0 && v <= 30000) to = v;
    }
    int idx = net::udp_open((u16)port);
    if (idx < 0) { hal::ps("no free udp slot\n"); return; }
    hal::pf("[UDP] listening on port %u (timeout %u ms)\n", port, to);
    u8 buf[512];
    u8 src_ip[4]; u16 src_port = 0;
    int n = net::udp_recv((u32)idx, buf, sizeof(buf), src_ip, &src_port, to);
    net::udp_close((u32)idx);
    if (n < 0) {
        hal::ps("no UDP packet received\n");
        return;
    }
    hal::pf("[UDP] rx %d bytes from %u.%u.%u.%u:%u: '",
            n, src_ip[0], src_ip[1], src_ip[2], src_ip[3], (u32)src_port);
    for (int i = 0; i < n; i++) hal::pc((char)buf[i]);
    hal::ps("'\n");
}

// A11.5b: Ring-3 HTTP-Client via Syscalls 210-213
static void emit_r3httpget_program(u8* code, u32 ip_be, u16 port,
                                    const char* req, u32 req_len) {
    for (int i = 0; i < 4096; i++) code[i] = 0;

    u32 p = 0;
    auto w8 = [&](u8 v) { code[p++] = v; };
    auto w32 = [&](u32 v) {
        code[p++] = (u8)v; code[p++] = (u8)(v>>8);
        code[p++] = (u8)(v>>16); code[p++] = (u8)(v>>24);
    };
    auto w64 = [&](u64 v) {
        for (int i = 0; i < 8; i++) code[p++] = (u8)(v >> (8*i));
    };
    auto mov_rax = [&](u64 v) { w8(0x48); w8(0xB8); w64(v); };
    auto mov_rdi = [&](u64 v) { w8(0x48); w8(0xBF); w64(v); };
    auto mov_rsi = [&](u64 v) { w8(0x48); w8(0xBE); w64(v); };
    auto mov_rdx = [&](u64 v) { w8(0x48); w8(0xBA); w64(v); };
    auto mov_r10 = [&](u64 v) { w8(0x49); w8(0xBA); w64(v); };
    auto sysc    = [&]() { w8(0x0F); w8(0x05); };
    auto mov_r12_rax = [&]() { w8(0x49); w8(0x89); w8(0xC4); };
    auto mov_rdi_r12 = [&]() { w8(0x4C); w8(0x89); w8(0xE7); };
    auto mov_r15_rax = [&]() { w8(0x49); w8(0x89); w8(0xC7); };
    auto mov_rdx_r15 = [&]() { w8(0x4C); w8(0x89); w8(0xFA); };
    auto xor_r14_r14 = [&]() { w8(0x4D); w8(0x31); w8(0xF6); };
    auto add_r14_rax = [&]() { w8(0x49); w8(0x01); w8(0xC6); };
    auto mov_rdi_r14 = [&]() { w8(0x4C); w8(0x89); w8(0xF7); };
    auto test_rax_rax = [&]() { w8(0x48); w8(0x85); w8(0xC0); };
    auto jne_rel32 = [&]() -> u32 { u32 pos = p; w8(0x0F); w8(0x85); w32(0); return pos; };
    auto jle_rel32 = [&]() -> u32 { u32 pos = p; w8(0x0F); w8(0x8E); w32(0); return pos; };
    auto jmp_rel32 = [&]() -> u32 { u32 pos = p; w8(0xE9); w32(0); return pos; };
    auto patch_jcc = [&](u32 pos, u32 target) {
        i32 rel = (i32)target - (i32)(pos + 6);
        code[pos+2] = (u8)(rel & 0xFF);
        code[pos+3] = (u8)((rel >> 8) & 0xFF);
        code[pos+4] = (u8)((rel >> 16) & 0xFF);
        code[pos+5] = (u8)((rel >> 24) & 0xFF);
    };

    const u64 CODE_VA = 0x40000000ULL;
    const u64 BUF_VA  = 0x50000100ULL;
    u64 req_addr = CODE_VA + 0x100;

    // tcp_open() -> idx
    mov_rax(210); sysc();
    mov_r12_rax();

    // tcp_connect(idx, ip_be, port)
    mov_rax(211);
    mov_rdi_r12();
    mov_rsi(ip_be);
    mov_rdx(port);
    sysc();
    test_rax_rax();
    u32 jne_fail = jne_rel32();

    // tcp_send(idx, req_addr, req_len)
    mov_rax(212);
    mov_rdi_r12();
    mov_rsi(req_addr);
    mov_rdx(req_len);
    sysc();

    // r14 = 0
    xor_r14_r14();

    // .loop: tcp_recv
    u32 loop_pos = p;
    mov_rax(213);
    mov_rdi_r12();
    mov_rsi(BUF_VA);
    mov_rdx(512);
    mov_r10(3000);
    sysc();
    test_rax_rax();
    u32 jle_done = jle_rel32();

    // total += n
    add_r14_rax();

    // write(1, BUF_VA, n)
    mov_r15_rax();
    mov_rax(1);
    mov_rdi(1);
    mov_rsi(BUF_VA);
    mov_rdx_r15();
    sysc();

    // jmp .loop
    u32 jmp_loop = jmp_rel32();

    // .done: exit(total)
    u32 done_pos = p;
    mov_rax(60);
    mov_rdi_r14();
    sysc();

    // .fail: exit(0xFF)
    u32 fail_pos = p;
    mov_rax(60);
    mov_rdi(0xFF);
    sysc();

    patch_jcc(jne_fail, fail_pos);
    patch_jcc(jle_done, done_pos);

    i32 back = (i32)loop_pos - (i32)(jmp_loop + 5);
    code[jmp_loop+1] = (u8)(back & 0xFF);
    code[jmp_loop+2] = (u8)((back >> 8) & 0xFF);
    code[jmp_loop+3] = (u8)((back >> 16) & 0xFF);
    code[jmp_loop+4] = (u8)((back >> 24) & 0xFF);

    for (u32 i = 0; i < req_len && (0x100 + i) < 0x200; i++) {
        code[0x100 + i] = (u8)req[i];
    }
}

static void cmd_r3httpget(int ac, char** av) {
    if (ac < 3) { hal::ps("usage: r3httpget <ip> <port>\n"); return; }
    u8 ip[4];
    if (!_parse_ip4(av[1], ip)) { hal::ps("bad ip\n"); return; }
    u32 port = 0;
    for (const char* p = av[2]; *p >= '0' && *p <= '9'; p++) port = port * 10 + (u32)(*p - '0');
    if (port == 0 || port > 65535) { hal::ps("bad port\n"); return; }

    u32 ip_be = ((u32)ip[0] << 24) | ((u32)ip[1] << 16) |
                ((u32)ip[2] <<  8) |  (u32)ip[3];

    char req[200];
    u32 rp = 0;
    const char* pre = "GET / HTTP/1.1\r\nHost: ";
    while (*pre) req[rp++] = *pre++;
    for (int i = 0; i < 4; i++) {
        u32 v = ip[i];
        if (v >= 100) req[rp++] = (char)('0' + (v / 100));
        if (v >= 10)  req[rp++] = (char)('0' + ((v / 10) % 10));
        req[rp++] = (char)('0' + (v % 10));
        if (i < 3) req[rp++] = '.';
    }
    const char* tail = "\r\nConnection: close\r\n\r\n";
    while (*tail) req[rp++] = *tail++;

    u64 code_phys  = mem::pmm::alloc();
    u64 stack_phys = mem::pmm::alloc();
    if (!code_phys || !stack_phys) { hal::ps("pmm fail\n"); return; }

    u8* code = (u8*)mem::p2v(code_phys);
    emit_r3httpget_program(code, ip_be, (u16)port, req, rp);

    u8* stk = (u8*)mem::p2v(stack_phys);
    for (int i = 0; i < 4096; i++) stk[i] = 0;

    u64 my_pml4 = vmm::alloc_pml4();
    if (!my_pml4) { hal::ps("alloc_pml4 fail\n"); return; }

    u64 code_va  = 0x40000000ULL;
    u64 stack_va = 0x50000000ULL;
    if (!vmm::map_user_page_in(my_pml4, code_va,  code_phys,  0)) {
        hal::ps("map code fail\n"); return;
    }
    if (!vmm::map_user_page_in(my_pml4, stack_va, stack_phys, vmm::P_WRITE)) {
        hal::ps("map stack fail\n"); return;
    }

    task::TCB* t = task::spawn_full("r3httpget", user_elf_fn, nullptr,
                                    my_pml4, code_va, stack_va + 0x3ff0);
    if (!t) { hal::ps("spawn fail\n"); return; }
    hal::pf("[R3HTTP] spawned pid=%u dst=%u.%u.%u.%u:%u req_len=%u\n",
            (u32)t->pid, ip[0], ip[1], ip[2], ip[3], port, rp);
}

static void cmd_ping(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: ping <ip> [count]\n"); return; }
    u32 count = 3;
    if (ac >= 3) {
        u32 v = 0;
        for (const char* q = av[2]; *q >= '0' && *q <= '9'; q++) v = v * 10 + (u32)(*q - '0');
        if (v > 0) count = v;
    }
    if (count == 0 || count > 10) count = 3;

    u8 ip[4] = {0,0,0,0};
    int dots = 0, cur = 0;
    for (const char* p = av[1]; *p && dots < 4; p++) {
        if (*p == '.') { ip[dots++] = (u8)cur; cur = 0; }
        else if (*p >= '0' && *p <= '9') cur = cur * 10 + (*p - '0');
        else break;
    }
    if (dots == 3) ip[3] = (u8)cur;
    else { hal::ps("bad ip\n"); return; }

    if (!net::default_iface()) { hal::ps("no network interface\n"); return; }

    u32 recv = 0;
    for (u32 i = 0; i < count; i++) {
        int r = net::icmp_ping(ip, 500);
        if (r == 0) recv++;
    }
    hal::pf("%u transmitted, %u received, %u%% loss\n",
            count, recv, (count - recv) * 100 / count);
}

static void cmd_arp(int ac, char** av) {
    if (ac < 2) { hal::ps("usage: arp <ip>\n"); return; }
    u8 ip[4] = {0,0,0,0};
    int dots = 0, cur = 0;
    for (const char* p = av[1]; *p && dots < 4; p++) {
        if (*p == '.') { ip[dots++] = (u8)cur; cur = 0; }
        else if (*p >= '0' && *p <= '9') cur = cur * 10 + (*p - '0');
        else break;
    }
    if (dots == 3) ip[3] = (u8)cur;
    else { hal::ps("bad ip (expected a.b.c.d)\n"); return; }
    u8 mac[6] = {0};
    int r = virtio_net::arp_resolve(ip, mac);
    if (r == 0) {
        hal::pf("%u.%u.%u.%u is at %x:%x:%x:%x:%x:%x\n",
                (u32)ip[0], (u32)ip[1], (u32)ip[2], (u32)ip[3],
                (u32)mac[0], (u32)mac[1], (u32)mac[2],
                (u32)mac[3], (u32)mac[4], (u32)mac[5]);
    } else {
        hal::pf("no ARP reply (err %d)\n", r);
    }
}

static void dispatch() {
    if (argc == 0) return;
    const char* c = argv[0];
    if (streq(c, "help") || streq(c, "?"))     { cmd_help(); return; }
    if (streq(c, "get")  || streq(c, "g"))     { cmd_get(argc, argv); return; }
    if (streq(c, "put")  || streq(c, "p"))     { cmd_put(argc, argv); return; }
    if (streq(c, "scan") || streq(c, "s"))     { cmd_scan(argc, argv); return; }
    if (streq(c, "prio"))                      { cmd_prio(argc, argv); return; }
    if (streq(c, "tasks")|| streq(c, "t") || streq(c, "ps"))
                                               { cmd_tasks(); return; }
    if (streq(c, "mem")  || streq(c, "m"))     { cmd_mem(); return; }
    if (streq(c, "clear")|| streq(c, "cls"))   { cmd_clear(); return; }
    if (streq(c, "pci"))                       { cmd_pci(); return; }
    if (streq(c, "netinfo"))                   { cmd_netinfo(); return; }
    if (streq(c, "arp"))                       { cmd_arp(argc, argv); return; }
    if (streq(c, "ping"))                      { cmd_ping(argc, argv); return; }
    if (streq(c, "r3ping"))                     { cmd_r3ping(argc, argv); return; }
    if (streq(c, "r3httpget"))                  { cmd_r3httpget(argc, argv); return; }
    if (streq(c, "udpsend"))                    { cmd_udpsend(argc, argv); return; }
    if (streq(c, "httpget"))                    { cmd_httpget(argc, argv); return; }
    if (streq(c, "dns"))                        { cmd_dns(argc, argv); return; }
    if (streq(c, "udprecv"))                    { cmd_udprecv(argc, argv); return; }
    if (streq(c, "blk"))                       { cmd_blk(argc, argv); return; }
    if (streq(c, "sync"))                      { cmd_sync(); return; }
    if (streq(c, "wal"))                       { cmd_wal(); return; }
    if (streq(c, "write"))                     { cmd_write(argc, argv); return; }
    if (streq(c, "append"))                    { cmd_append(argc, argv); return; }
    if (streq(c, "trunc"))                     { cmd_trunc(argc, argv); return; }
    if (streq(c, "writetrunc"))                 { cmd_writetrunc(argc, argv); return; }
    if (streq(c, "cat"))                       { cmd_cat(argc, argv); return; }
    if (streq(c, "lseek"))                     { cmd_lseek(argc, argv); return; }
    if (streq(c, "pub"))                       { cmd_pub(argc, argv); return; }
    if (streq(c, "piper"))                     { cmd_piper(argc, argv); return; }
    if (streq(c, "piper-async"))               { cmd_piper_async(argc, argv); return; }
    if (streq(c, "pipe-test"))                 { cmd_pipe_test(argc, argv); return; }
    if (streq(c, "sub"))                       { cmd_sub(argc, argv); return; }
    if (streq(c, "msgsend"))                    { cmd_msgsend(argc, argv); return; }
    if (streq(c, "msgrecv"))                    { cmd_msgrecv(argc, argv); return; }
    if (streq(c, "msgs"))                       { cmd_msgs(); return; }
    if (streq(c, "sub-reset"))                 { cmd_sub_reset(); return; }
    if (streq(c, "mkdir"))                     { cmd_mkdir(argc, argv); return; }
    if (streq(c, "rmdir"))                     { cmd_rmdir(argc, argv); return; }
    if (streq(c, "lsdirs"))                    { cmd_lsdirs(); return; }
    if (streq(c, "pwd-test"))                  { cmd_pwdtest(argc, argv); return; }
    if (streq(c, "cd"))                       { cmd_cd(argc, argv); return; }
    if (streq(c, "pwd"))                      { cmd_pwd(); return; }
    if (streq(c, "pathtest"))                 { cmd_pathtest(argc, argv); return; }
    if (streq(c, "sleep"))                     { cmd_sleep(argc, argv); return; }
    if (streq(c, "ls"))                        { cmd_ls(argc, argv); return; }
    if (streq(c, "rm"))                        { cmd_rm(argc, argv); return; }
    if (streq(c, "bmtest"))                    { cmd_bmtest(); return; }
    if (streq(c, "hash"))                      { cmd_hash(argc, argv); return; }
    if (streq(c, "history"))                   { cmd_history(argc, argv); return; }
    if (streq(c, "trace"))                     { cmd_trace(argc, argv); return; }
    if (streq(c, "watch"))                     { cmd_watch(argc, argv); return; }
    if (streq(c, "checkpoint"))                { cmd_checkpoint(argc, argv); return; }
    if (streq(c, "snapshots"))                 { cmd_snapshots(); return; }
    if (streq(c, "restore"))                   { cmd_restore(argc, argv); return; }
    if (streq(c, "factory-reset"))             { cmd_factory_reset(); return; }
    if (streq(c, "edit"))                      { cmd_edit(argc, argv); return; }
    if (streq(c, "int3"))                      { cmd_int3_test(); return; }
    if (streq(c, "ring3"))                     { cmd_ring3(); return; }
    if (streq(c, "r3bad"))                     { cmd_r3bad(); return; }
    if (streq(c, "r3kern"))                    { cmd_r3kern(); return; }
    if (streq(c, "bench"))                     { cmd_bench(argc, argv); return; }
    if (streq(c, "a3e"))                       { cmd_a3e(); return; }
    if (streq(c, "run"))                       { cmd_run(argc, argv); return; }
    if (streq(c, "hello"))                     { cmd_hello(); return; }
    if (streq(c, "fib"))                       { cmd_fib(); return; }
    if (streq(c, "gen"))                       { cmd_gen(argc, argv); return; }
    if (streq(c, "wc"))                        { cmd_wc(argc, argv); return; }
    if (streq(c, "log"))                       { cmd_log(argc, argv); return; }
    if (streq(c, "lvl"))                       { cmd_lvl(argc, argv); return; }
    if (streq(c, "pf_test"))                   { cmd_pf_test(); return; }
    if (streq(c, "fdtest"))                    { cmd_fdtest(argc, argv); return; }
    if (streq(c, "argtest"))                   { cmd_argtest(argc, argv); return; }
    if (streq(c, "cat2"))                      { cmd_cat2(argc, argv); return; }
    if (streq(c, "write2"))                    { cmd_write2(argc, argv); return; }
    hal::pf("unknown command: %s (try 'help')\n", c);
}

static void tokenize() {
    argc = 0;
    char* p = line_buf;
    while (*p && argc < 32) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = 0; p++; }
    }
}

void task_fn(void*) {
    hal::ps("\nNEXUS> ");
    for (;;) {
        int ch = kbd::getchar_nb();
        if (ch < 0) {
            for (int i = 0; i < 2000; i++) __asm__ volatile("" ::: "memory");
            continue;
        }
        char c = (char)ch;
        if (c == '\n' || c == '\r') {
            hal::pc('\n');
            line_buf[line_len] = 0;
            tokenize();
            dispatch();
            line_len = 0;
            hal::ps("NEXUS> ");
        } else if (c == '\b' || c == 127) {
            if (line_len > 0) {
                line_len--;
                hal::ps("\b \b");
            }
        } else if (c >= 32 && c < 127) {
            if (line_len < 127) {
                line_buf[line_len++] = c;
                hal::pc(c);
            }
        }
    }
}

} // namespace shell
