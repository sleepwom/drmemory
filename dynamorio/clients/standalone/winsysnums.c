/* **********************************************************
 * Copyright (c) 2011 Google, Inc.  All rights reserved.
 * Copyright (c) 2003-2008 VMware, Inc.  All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * 
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * 
 * * Neither the name of VMware, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL VMWARE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* Copyright (c) 2003-2007 Determina Corp. */

/* winsysnums.c: analyzes a dll's exported routines, looking for system call
 * numbers or Ki* routines -- typically pointed at a new ntdll.dll
 *
 * now additionally uses drsyms to analyze all symbols and thus locate
 * system call wrappers that are not exported.
 */

/* Uses the DR CLIENT_INTERFACE API, using DR as a standalone library, rather than
 * being a client library working with DR on a target program.
 *
 * To build, use cmake.  Something like:
 *   % mkdir ~/dr/git/build_winsysnums
 *   % cd !$
 *   % cmake -DDynamoRIO_DIR=e:/src/dr/git/exports/cmake ../src/clients/standalone
 *   % cmake --build .
 *
 * To run, you need to put dynamorio.dll, drsyms.dll, and dbghelp.dll into the
 * same directory as winsysnums.exe.  (If you build drsyms statically you don't
 * need to copy it of course.)
 */

#include "dr_api.h"
#include "drsyms.h"
#include <assert.h>
#include <imagehlp.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef LINUX
# define EXPORT
#else
# define EXPORT __declspec(dllexport)
#endif

#define BUFFER_SIZE_BYTES(buf)      sizeof(buf)
#define BUFFER_SIZE_ELEMENTS(buf)   (BUFFER_SIZE_BYTES(buf) / sizeof((buf)[0]))
#define BUFFER_LAST_ELEMENT(buf)    (buf)[BUFFER_SIZE_ELEMENTS(buf) - 1]
#define NULL_TERMINATE_BUFFER(buf)  BUFFER_LAST_ELEMENT(buf) = 0

/* global params */
static bool expect_int2e = false;
static bool expect_sysenter = false;
static bool expect_wow = false;
static bool expect_x64 = false;
static bool verbose = false;
static bool list_exports = false;
static bool list_forwards = false;
static bool list_Ki = false;
static bool list_syscalls = false;
static bool ignore_Zw = false;

static void
common_print(char *fmt, va_list ap)
{
    vfprintf(stdout, fmt, ap);
    fflush(stdout);
}

static void
verbose_print(char *fmt, ...)
{
    if (verbose) {
        va_list ap;
        va_start(ap, fmt);
        common_print(fmt, ap);
        va_end(ap);
    }
}

static void
print(char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    common_print(fmt, ap);
    va_end(ap);
}

#define MAX_INSTRS_BEFORE_SYSCALL 16
#define MAX_INSTRS_IN_FUNCTION 256

typedef struct {
    int sysnum;
    int num_args;
    int fixup_index; /* WOW dlls only */
} syscall_info_t;

/* returns false on failure */
static bool
decode_function(void *dcontext, byte *entry)
{
    byte *pc;
    int num_instr = 0;
    bool found_ret = false;
    instr_t *instr;
    if (entry == NULL)
        return false;
    instr = instr_create(dcontext);
    pc = entry;
    while (true) {
        instr_reset(dcontext, instr);
        pc = decode(dcontext, pc, instr);
        dr_print_instr(dcontext, STDOUT, instr, "");
        if (instr_is_return(instr)) {
            found_ret = true;
            break;
        }
        num_instr++;
        if (num_instr > MAX_INSTRS_IN_FUNCTION) {
            print("ERROR: hit max instr limit %d\n", MAX_INSTRS_IN_FUNCTION);
            break;
        }
    }
    instr_destroy(dcontext, instr);
    return found_ret;
}

static void
check_Ki(const char *name)
{
    /* FIXME: eventually we should automatically analyze these, but
     * not worth the time at this point.  Once we have automatic
     * analysis code, should put it into DR debug build init too!  For
     * now we issue manual instructions about verifying our
     * assumptions, and look for unknown Ki routines.
     */
    if (strcmp(name, "KiUserApcDispatcher") == 0) {
        print("verify that:\n"
              "\t1) *esp == call* target (not relied on)\n"
              "\t2) *(esp+16) == CONTEXT\n");
    } else if (strcmp(name, "KiUserExceptionDispatcher") == 0) {
        print("verify that:\n"
              "\t1) *esp = EXCEPTION_RECORD*\n"
              "\t2) *(esp+4) == CONTEXT*\n");
    } else if (strcmp(name, "KiRaiseUserExceptionDispatcher") == 0) {
        print("we've never seen this guy invoked\n");
    } else if (strcmp(name, "KiUserCallbackDispatcher") == 0) {
        print("verify that:\n"
              "\t1) peb->KernelCallbackTable[*(esp+4)] == call* target (not relied on)\n");
    } else if (strcmp(name, "KiFastSystemCall") == 0) {
        print("should be simply \"mov esp,edx; sysenter; ret\"\n");
    } else if (strcmp(name, "KiFastSystemCallRet") == 0) {
        print("should be simply \"ret\"\n");
    } else if (strcmp(name, "KiIntSystemCall") == 0) {
        print("should be simply \"lea 0x8(esp),edx; int 2e; ret\"\n");
    } else {
        print("WARNING!  UNKNOWN Ki ROUTINE!\n");
    }
}

static void
process_ret(instr_t *instr, syscall_info_t *info)
{
    assert(instr_is_return(instr));
    if (opnd_is_immed_int(instr_get_src(instr, 0)))
        info->num_args = (int) opnd_get_immed_int(instr_get_src(instr, 0));
    else
        info->num_args = 0;
}

/* returns false on failure */
static bool
decode_syscall_num(void *dcontext, byte *entry, syscall_info_t *info)
{
    /* FIXME: would like to fail gracefully rather than have a DR assertion
     * on non-code! => use DEBUG=0 INTERNAL=1 DR build!
     */
    bool found_syscall = false, found_eax = false, found_edx = false, found_ecx = false;
    bool found_ret = false;
    byte *pc;
    int num_instr = 0;
    instr_t *instr;
    if (entry == NULL)
        return false;
    info->num_args = -1; /* if find sysnum but not args */
    info->sysnum = -1;
    info->fixup_index = -1;
    instr = instr_create(dcontext);
    pc = entry;
    /* FIXME - we don't support decoding 64bit instructions in 32bit mode, but I want
     * this to work on 32bit machines.  Hack fix based on the wrapper pattern, we skip
     * the first instruction (mov r10, rcx) here, the rest should decode ok.
     * Xref PR 236203. */
    if (expect_x64 && *pc == 0x4c && *(pc+1) == 0x8b && *(pc+2) == 0xd1)
        pc += 3;
    while (true) {
        instr_reset(dcontext, instr);
        pc = decode(dcontext, pc, instr);
        if (verbose)
            dr_print_instr(dcontext, STDOUT, instr, "");
        if (pc == NULL || !instr_valid(instr))
            break;
        /* ASSUMPTION: a mov imm of 0x7ffe0300 into edx followed by an
         * indirect call via edx is a system call on XP and later
         * On XP SP1 it's call *edx, while on XP SP2 it's call *(edx)
         * For wow it's a call through fs.
         * FIXME - core exports various is_*_syscall routines (such as
         * instr_is_wow64_syscall()) which we could use here instead of
         * duplicating if they were more flexible about when they could
         * be called (instr_is_wow64_syscall() for ex. asserts if not
         * in a wow process).
         */
        if (/* int 2e or x64 or win8 sysenter */
            (instr_is_syscall(instr) && found_eax && (expect_int2e || expect_x64 || expect_sysenter)) ||
            /* sysenter case */
            (expect_sysenter && found_edx && found_eax &&
             instr_is_call_indirect(instr) &&
             /* XP SP{0,1}, 2003 SP0: call *edx */
             ((opnd_is_reg(instr_get_target(instr)) &&
               opnd_get_reg(instr_get_target(instr)) == REG_EDX) ||
              /* XP SP2, 2003 SP1: call *(edx) */
              (opnd_is_base_disp(instr_get_target(instr)) &&
               opnd_get_base(instr_get_target(instr)) == REG_EDX &&
               opnd_get_index(instr_get_target(instr)) == REG_NULL &&
               opnd_get_disp(instr_get_target(instr)) == 0))) ||
            /* wow case 
             * we don't require found_ecx b/c win8 does not use ecx
             */
            (expect_wow && found_eax &&
             instr_is_call_indirect(instr) &&
             opnd_is_far_base_disp(instr_get_target(instr)) &&
             opnd_get_base(instr_get_target(instr)) == REG_NULL &&
             opnd_get_index(instr_get_target(instr)) == REG_NULL &&
             opnd_get_segment(instr_get_target(instr)) == SEG_FS)) {
            found_syscall = true;
        } else if (instr_is_return(instr)) {
            if (!found_ret) {
                process_ret(instr, info);
                found_ret = true;
            }
            break;
        } else if (instr_is_cti(instr)) {
            if (instr_get_opcode(instr) == OP_call) {
                /* handle win8 x86 which has sysenter callee adjacent-"inlined"
                 *     ntdll!NtYieldExecution:
                 *     77d7422c b801000000      mov     eax,1
                 *     77d74231 e801000000      call    ntdll!NtYieldExecution+0xb (77d74237)
                 *     77d74236 c3              ret
                 *     77d74237 8bd4            mov     edx,esp
                 *     77d74239 0f34            sysenter
                 *     77d7423b c3              ret
                 */
                byte *tgt;
                assert(opnd_is_pc(instr_get_target(instr)));
                tgt = opnd_get_pc(instr_get_target(instr));
                /* we expect only ret or ret imm, and possibly some nops (in gdi32).
                 * XXX: what about jmp to shared ret (seen in the past on some syscalls)?
                 */
                if (tgt > pc && tgt <= pc + 16) {
                    bool ok = false;
                    do {
                        if (pc == tgt) {
                            ok = true;
                            break;
                        }
                        instr_reset(dcontext, instr);
                        pc = decode(dcontext, pc, instr);
                        if (verbose)
                            dr_print_instr(dcontext, STDOUT, instr, "");
                        if (instr_is_return(instr)) {
                            process_ret(instr, info);
                            found_ret = true;
                        } else if (!instr_is_nop(instr))
                            break;
                        num_instr++;
                    } while (num_instr <= MAX_INSTRS_BEFORE_SYSCALL);
                    if (ok)
                        continue;
                }
            }
            /* assume not a syscall wrapper if we hit a cti */
            break; /* give up gracefully */
        } else if ((!found_eax || !found_edx || !found_ecx) &&
            instr_get_opcode(instr) == OP_mov_imm &&
            opnd_is_reg(instr_get_dst(instr, 0))) {
            if (!found_eax && opnd_get_reg(instr_get_dst(instr, 0)) == REG_EAX) {
                info->sysnum = (int) opnd_get_immed_int(instr_get_src(instr, 0));
                found_eax = true;
            } else if (!found_edx && opnd_get_reg(instr_get_dst(instr, 0)) == REG_EDX) {
                int imm = (int) opnd_get_immed_int(instr_get_src(instr, 0));
                if (imm == 0x7ffe0300)
                    found_edx = true;
            } else if (!found_ecx && opnd_get_reg(instr_get_dst(instr, 0)) == REG_ECX) {
                found_ecx = true;
                info->fixup_index = (int) opnd_get_immed_int(instr_get_src(instr, 0));
            }
        } else if (instr_get_opcode(instr) == OP_xor &&
                   opnd_is_reg(instr_get_src(instr, 0)) &&
                   opnd_get_reg(instr_get_src(instr, 0)) == REG_ECX &&
                   opnd_is_reg(instr_get_dst(instr, 0)) &&
                   opnd_get_reg(instr_get_dst(instr, 0)) == REG_ECX) {
            /* xor to 0 */
            found_ecx = true;
            info->fixup_index = 0;
        }
        num_instr++;
        if (num_instr > MAX_INSTRS_BEFORE_SYSCALL) /* wrappers should be short! */
            break; /* avoid weird cases like NPXEMULATORTABLE */
    }
    instr_destroy(dcontext, instr);
    return found_syscall;
}

static void
process_syscall_wrapper(void *dcontext, byte *addr, const char *string,
                        const char *type)
{
    syscall_info_t sysinfo;
    if (ignore_Zw && string[0] == 'Z' && string[1] == 'w')
        return;
    if (decode_syscall_num(dcontext, addr, &sysinfo)) {
        if (sysinfo.sysnum == -1) {
            /* we expect this sometimes */
            if (strcmp(string, "KiFastSystemCall") != 0 &&
                strcmp(string, "KiIntSystemCall") != 0) {
                print("ERROR: unknown syscall #: %s\n", string);
            }
        } else {
            /* be sure to print all digits b/c win8 now uses the top 16 bits for wow64 */
            if (expect_wow) {
                print("syscall # 0x%08x %-6s %2d args fixup 0x%02x = %s\n",
                      sysinfo.sysnum, type, sysinfo.num_args,
                      sysinfo.fixup_index, string);
            } else if (expect_x64) {
                print("syscall # 0x%08x %-6s = %s\n",
                      sysinfo.sysnum, type, string);
            } else {
                print("syscall # 0x%08x %-6s %2d args = %s\n",
                      sysinfo.sysnum, type, sysinfo.num_args, string);
            }
        }
    }
}

typedef struct _search_data_t {
    void *dcontext;
    LOADED_IMAGE *img;
} search_data_t;

/* not only do we have NtUser*, NtWow64*, etc., but also user32!UserContectToServer,
 * so we go through all symbols
 */
#define SYM_PATTERN "*"

static bool
search_syms_cb(const char *name, size_t modoffs, void *data)
{
    search_data_t *sd = (search_data_t *) data;
    byte *addr = ImageRvaToVa(sd->img->FileHeader, sd->img->MappedAddress,
                              (ULONG) modoffs, NULL);
    verbose_print("Found symbol \"%s\" at offs "PIFX" => "PFX"\n", name, modoffs, addr);
    process_syscall_wrapper(sd->dcontext, addr, name, "pdb");
    return true; /* keep iterating */
}

static void
process_symbols(void *dcontext, char *dllname, LOADED_IMAGE *img)
{
    /* We have to specify the module via "modname!symname".
     * We must use the same modname as in full_path.
     */
# define MAX_SYM_WITH_MOD_LEN 256
    char sym_with_mod[MAX_SYM_WITH_MOD_LEN];
    size_t modoffs;
    drsym_error_t symres;
    char *fname = NULL, *c;
    search_data_t sd;

    if (drsym_init(NULL) != DRSYM_SUCCESS) {
        print("WARNING: unable to initialize symbol engine\n");
        return;
    }

    if (dllname == NULL)
        return;
    for (c = dllname; *c != '\0'; c++) {
        if (*c == '/' || *c == '\\')
            fname = c + 1;
    }
    assert(fname != NULL && "unable to get fname for module");
    if (fname == NULL)
        return;
    /* now get rid of extension */
    for (; c > fname && *c != '.'; c--)
        ; /* nothing */

    assert(c - fname < BUFFER_SIZE_ELEMENTS(sym_with_mod) && "sizes way off");
    modoffs = dr_snprintf(sym_with_mod, c - fname, "%s", fname);
    assert(modoffs > 0 && "error printing modname!symname");
    modoffs = dr_snprintf(sym_with_mod + modoffs,
                          BUFFER_SIZE_ELEMENTS(sym_with_mod) - modoffs,
                          "!%s", SYM_PATTERN);
    assert(modoffs > 0 && "error printing modname!symname");

    sd.dcontext = dcontext;
    sd.img = img;
    verbose_print("Searching \"%s\" for \"%s\"\n", dllname, sym_with_mod);
    symres = drsym_search_symbols(dllname, sym_with_mod, true, search_syms_cb, &sd);
    if (symres != DRSYM_SUCCESS)
        print("Error %d searching \"%s\" for \"%s\"\n", dllname, sym_with_mod);
    drsym_exit();
}

static void
process_exports(void *dcontext, char *dllname)
{
    LOADED_IMAGE img;
    IMAGE_EXPORT_DIRECTORY *dir;
    IMAGE_SECTION_HEADER *sec;
    DWORD *name, *code;
    WORD *ordinal;
    const char *string;
    uint size;
    BOOL res;
    uint i;
    byte *addr, *start_exports, *end_exports;

    verbose_print("Processing exports of \"%s\"\n", dllname);
    res = MapAndLoad(dllname, NULL, &img, FALSE, TRUE);
    if (!res) {
        print("Error loading %s\n", dllname);
        return;
    }
    dir = (IMAGE_EXPORT_DIRECTORY *)
        ImageDirectoryEntryToData(img.MappedAddress, FALSE,
                                  IMAGE_DIRECTORY_ENTRY_EXPORT, &size);
    verbose_print("mapped at 0x%08x, exports is at 0x%08x, size is 0x%x\n",
                  img.MappedAddress, dir, size);
    start_exports = (byte *) dir;
    end_exports = start_exports + size;
    verbose_print("name=%s, ord base=0x%08x, names=%d 0x%08x\n",
                  (char *) ImageRvaToVa(img.FileHeader, img.MappedAddress,
                                        dir->Name, NULL),
                  dir->Base, dir->NumberOfNames, dir->AddressOfNames);

    /* don't limit functions to lie in .text -- 
     * for ntdll, some exported routines have their code after .text, inside
     * ECODE section!
     */
    sec = img.Sections;
    for (i = 0; i < img.NumberOfSections; i++) {
        verbose_print("Section %d %s: 0x%x + 0x%x == 0x%08x through 0x%08x\n",
                      i, sec->Name, sec->VirtualAddress, sec->SizeOfRawData,
                      ImageRvaToVa(img.FileHeader, img.MappedAddress,
                                   sec->VirtualAddress, NULL),
                      (uint) ImageRvaToVa(img.FileHeader, img.MappedAddress,
                                          sec->VirtualAddress, NULL) +
                      sec->SizeOfRawData);
        sec++;
    }

    name = (DWORD *) ImageRvaToVa(img.FileHeader, img.MappedAddress,
                                  dir->AddressOfNames, NULL);
    code = (DWORD *) ImageRvaToVa(img.FileHeader, img.MappedAddress,
                                  dir->AddressOfFunctions, NULL);
    ordinal = (WORD *) ImageRvaToVa(img.FileHeader, img.MappedAddress,
                                    dir->AddressOfNameOrdinals, NULL);
    verbose_print("names: from 0x%08x to 0x%08x\n",
                  ImageRvaToVa(img.FileHeader, img.MappedAddress, name[0], NULL),
                  ImageRvaToVa(img.FileHeader, img.MappedAddress,
                               name[dir->NumberOfNames-1], NULL));

    for (i = 0; i < dir->NumberOfNames; i++) {
        string = (char *) ImageRvaToVa(img.FileHeader, img.MappedAddress, name[i], NULL);
        /* ordinal is biased (dir->Base), but don't add base when using as index */
        assert(dir->NumberOfFunctions > ordinal[i]);
        /* I don't understand why have to do RVA to VA here, when dumpbin /exports
         * seems to give the same offsets but by simply adding them to base we
         * get the appropriate code location -- but that doesn't work here...
         */
        addr = ImageRvaToVa(img.FileHeader, img.MappedAddress,
                            code[ordinal[i]], NULL);
        verbose_print("name=%s 0x%08x, ord=%d, code=0x%x -> 0x%08x\n",
                      string, string, ordinal[i], code[ordinal[i]], addr);
        if (list_exports) {
            print("ord %3d offs 0x%08x %s\n", ordinal[i],
                  addr - img.MappedAddress, string);
        }
        if (list_Ki && string[0] == 'K' && string[1] == 'i') {
            print("\n==================================================\n");
            print("%s\n\n", string);
            check_Ki(string);
            print("\ndisassembly:\n");
            decode_function(dcontext, addr);
            print(  "==================================================\n");
        }
        /* forwarded export points inside exports section */
        if (addr >= start_exports && addr < end_exports) {
            if (list_forwards || verbose) {
                /* I've had issues w/ forwards before, so avoid printing crap */
                if (addr[0] > 0 && addr[0] < 127)
                    print("%s is forwarded to %.128s\n", string, addr);
                else
                    print("ERROR identifying forwarded entry for %s\n", string);
            }
        } else if (list_syscalls) {
            process_syscall_wrapper(dcontext, addr, string, "export");
        }
    }

    if (list_syscalls)
        process_symbols(dcontext, dllname, &img);

    UnMapAndLoad(&img);
}

static void
usage(char *pgm)
{
    print("Usage: %s [-syscalls <-sysenter | -int2e | -wow | -x64> [-ignore_Zw]] | "
          "-Ki | -exports | -forwards | -v] <dll>\n", pgm);
    exit(-1);
}

int
main(int argc, char *argv[])
{
    void *dcontext = dr_standalone_init();
    int res;
    char *dll;
    bool forced = false;

#ifdef X64
    set_x86_mode(dcontext, true/*x86*/);
#endif

    for (res=1; res < argc; res++) {
        if (strcmp(argv[res], "-sysenter") == 0) {
            expect_sysenter = true;
            forced = true;
        } else if (strcmp(argv[res], "-int2e") == 0) {
            expect_int2e = true;
            forced = true;
        } else if (strcmp(argv[res], "-wow") == 0) {
            expect_wow = true;
            forced = true;
        } else if (strcmp(argv[res], "-x64") == 0) {
            expect_x64 = true;
#ifdef X64
            set_x86_mode(dcontext, false/*x64*/);
#else
            /* For 32-bit builds we hack a fix for -syscalls (see
             * decode_syscall_num()) but -Ki won't work.
             */
#endif
            forced = true;
        } else if (strcmp(argv[res], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[res], "-exports") == 0) {
            list_exports = true;
            list_forwards = true; /* implied */
        } else if (strcmp(argv[res], "-forwards") == 0) {
            list_forwards = true;
        } else if (strcmp(argv[res], "-Ki") == 0) {
            list_Ki = true;
        } else if (strcmp(argv[res], "-syscalls") == 0) {
            list_syscalls = true;
        } else if (strcmp(argv[res], "-ignore_Zw") == 0) {
            ignore_Zw = true;
        } else if (argv[res][0] == '-') {
            usage(argv[0]);
            assert(false); /* not reached */
        } else {
            break;
        }
    }
    if (res >= argc ||
        (!list_syscalls && !list_Ki && !list_forwards && !verbose)) {
        usage(argv[0]);
        assert(false); /* not reached */
    }
    dll = argv[res];

    if (!forced && list_syscalls) {
        usage(argv[0]);
        assert(false); /* not reached */
    }

    process_exports(dcontext, dll);
    return 0;
}
