/* **********************************************************
 * Copyright (c) 2011-2012 Google, Inc.  All rights reserved.
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
 * * Neither the name of Google, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL GOOGLE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* Tests the drsyms extension.  Relies on the drwrap extension. */

#include "dr_api.h"
#include "drsyms.h"
#include "drwrap.h"
#include "client_tools.h"

#include <limits.h>
#include <string.h>

/* DR's build system usually disables warnings we're not interested in, but the
 * flags don't seem to make it to the compiler for this file, maybe because
 * it's C++.  Disable them with pragmas instead.
 */
#ifdef WINDOWS
# pragma warning( disable : 4100 )  /* Unreferenced formal parameter. */
#endif

static void event_exit(void);
static void lookup_exe_syms(void);
static void lookup_dll_syms(void *dc, const module_data_t *dll_data,
                            bool loaded);
static void check_enumerate_dll_syms(const char *dll_path);
#ifdef LINUX
static void lookup_glibc_syms(void *dc, const module_data_t *dll_data);
#endif
static void test_demangle(void);
#ifdef WINDOWS
static void lookup_overloads(const char *exe_path);
#endif

extern "C" DR_EXPORT void
dr_init(client_id_t id)
{
    drsym_init(0);
    drwrap_init();
    dr_register_exit_event(event_exit);

    lookup_exe_syms();
    dr_register_module_load_event(lookup_dll_syms);
    test_demangle();
}

/* Count intercepted calls. */
static int call_count = 0;

static void
pre_func(void *wrapcxt, void **user_data)
{
    call_count++;
}

/* Assuming prologue has "push xbp, mov xsp -> xbp", this struct is at the base
 * of every frame.
 */
typedef struct _frame_base_t {
    struct _frame_base_t *parent;
    app_pc ret_addr;
} frame_base_t;

#ifdef WINDOWS
# define FULL_PDB_DEBUG_KIND \
        (DRSYM_SYMBOLS | DRSYM_LINE_NUMS | DRSYM_PDB)
# define FULL_PECOFF_DEBUG_KIND \
        (DRSYM_SYMBOLS | DRSYM_LINE_NUMS | \
         DRSYM_PECOFF_SYMTAB | DRSYM_DWARF_LINE)
#else
# define FULL_DEBUG_KIND \
        (DRSYM_SYMBOLS | DRSYM_LINE_NUMS | \
         DRSYM_ELF_SYMTAB | DRSYM_DWARF_LINE)
#endif

static bool
debug_kind_is_full(drsym_debug_kind_t debug_kind)
{
    return 
#ifdef WINDOWS
        TESTALL(FULL_PDB_DEBUG_KIND, debug_kind) ||
        TESTALL(FULL_PECOFF_DEBUG_KIND, debug_kind);
#else
        TESTALL(FULL_DEBUG_KIND, debug_kind);
#endif
}

#define MAX_FUNC_LEN 1024
/* Take and symbolize a stack trace.  Assumes no frame pointer omission.
 */
static void
pre_stack_trace(void *wrapcxt, void **user_data)
{
    dr_mcontext_t *mc = drwrap_get_mcontext(wrapcxt);
    frame_base_t inner_frame;
    frame_base_t *frame;
    int depth;

    /* This should use safe_read and all that, but this is a test case. */
    dr_fprintf(STDERR, "stack trace:\n");
    frame = &inner_frame;
    /* It's impossible to get frame pointers on Win x64, so we only print one
     * frame.
     */
#if defined(WINDOWS) && defined(X64)
    frame->parent = NULL;
#else
    frame->parent = (frame_base_t*)mc->xbp;
#endif
    frame->ret_addr = *(app_pc*)mc->xsp;
    depth = 0;
    while (frame != NULL) {
        drsym_error_t r;
        module_data_t *mod;
        drsym_info_t *sym_info;
        char sbuf[sizeof(*sym_info) + MAX_FUNC_LEN];
        size_t modoffs;
        const char *basename;

        sym_info = (drsym_info_t *)sbuf;
        sym_info->struct_size = sizeof(*sym_info);
        sym_info->name_size = MAX_FUNC_LEN;

        mod = dr_lookup_module(frame->ret_addr);
        modoffs = frame->ret_addr - mod->start;
        /* gcc says the next line starts at the return address.  Back up one to
         * get the line that the call was on.
         */
        modoffs--;
        r = drsym_lookup_address(mod->full_path, modoffs, sym_info,
                                 DRSYM_DEMANGLE);
        dr_free_module_data(mod);
        ASSERT(r == DRSYM_SUCCESS);
        if (!debug_kind_is_full(sym_info->debug_kind)) {
            dr_fprintf(STDERR, "unexpected debug_kind: %x\n",
                       sym_info->debug_kind);
        }
        if (sym_info->file == NULL)
            basename = "/<unknown>";
        else {
            basename = strrchr(sym_info->file, IF_WINDOWS_ELSE('\\', '/'));
#ifdef WINDOWS
            if (basename == NULL)
                basename = strrchr(sym_info->file, '/');
#endif
        }
        ASSERT(basename != NULL);
        basename++;
        dr_fprintf(STDERR, "%s:%d!%s\n",
                   basename, (int)sym_info->line, sym_info->name);

        /* Stop after main. */
        if (strstr(sym_info->name, "main"))
            break;

        frame = frame->parent;
        depth++;
        if (depth > 20) {
            dr_fprintf(STDERR, "20 frames deep, stopping trace.\n");
            break;
        }
    }
}

static void
post_func(void *wrapcxt, void *user_data)
{
}

/* Use dr_get_proc_addr to get the exported address of a symbol.  Attempt to
 * look through any export table jumps so that we get the address for the
 * symbol that would be returned by looking at debug information.
 */
static app_pc
get_real_proc_addr(module_handle_t mod_handle, const char *symbol)
{
    instr_t instr;
    app_pc next_pc = NULL;
    app_pc export_addr = (app_pc)dr_get_proc_address(mod_handle, symbol);
    void *dc = dr_get_current_drcontext();

#ifdef WINDOWS
    /* use a symbol forwarded by DR to ntdll that's not an intrinsic to test
     * duplicate link issues vs libcmt
     */
    if (isdigit(symbol[0]))
        next_pc = next_pc; /* avoid warning about empty statement */
#endif

    instr_init(dc, &instr);
    if (export_addr != NULL)
        next_pc = decode(dc, export_addr, &instr);
    if (next_pc != NULL && instr_is_ubr(&instr)) {
        /* This is a jump to the real function entry point. */
        export_addr = opnd_get_pc(instr_get_target(&instr));
    }
    instr_reset(dc, &instr);

    return export_addr;
}

static size_t
lookup_and_wrap(const char *modpath, app_pc modbase, const char *modname, const
                char *symbol, int flags)
{
    drsym_error_t r;
    size_t modoffs = 0;
    bool ok;
    char lookup_str[256];

    dr_snprintf(lookup_str, sizeof(lookup_str), "%s!%s", modname, symbol);
    r = drsym_lookup_symbol(modpath, lookup_str, &modoffs, flags);
    if (r != DRSYM_SUCCESS || modoffs == 0) {
        dr_fprintf(STDERR, "Failed to lookup %s => %d\n", lookup_str, r);
    } else {
        ok = drwrap_wrap(modbase + modoffs, pre_func, post_func);
        ASSERT(ok);
    }
    return modoffs;
}

/* Lookup symbols in the exe and wrap them. */
static void
lookup_exe_syms(void)
{
    module_data_t *exe_data;
    const char *exe_path;
    app_pc exe_base;
    app_pc exe_export_addr;
    size_t exe_export_offs;
    size_t exe_public_offs;
    drsym_info_t unused_info;
    drsym_error_t r;
    drsym_debug_kind_t debug_kind;
    const char *appname = dr_get_application_name();
    char appbase[MAXIMUM_PATH];
    size_t len;

    len = dr_snprintf(appbase, BUFFER_SIZE_ELEMENTS(appbase), "%s", appname);
    ASSERT(len > 0);
#ifdef WINDOWS
    /* blindly assuming ends in .exe */
    appbase[strlen(appname) - 4/*subtract .exe*/] = '\0';
#endif

    exe_data = dr_lookup_module_by_name(appname);
    ASSERT(exe_data != NULL);
    exe_path = exe_data->full_path;
    exe_base = exe_data->start;

    /* We expect to have full debug info for this module. */
    r = drsym_get_module_debug_kind(exe_path, &debug_kind);
    ASSERT(r == DRSYM_SUCCESS);
    if (!debug_kind_is_full(debug_kind)) {
        dr_fprintf(STDERR, "unexpected debug_kind: %x\n", debug_kind);
    }

    exe_export_addr = get_real_proc_addr(exe_data->handle, "exe_export");
    exe_export_offs = lookup_and_wrap(exe_path, exe_base, appbase,
                                      "exe_export", DRSYM_DEFAULT_FLAGS);
    ASSERT(exe_export_addr == exe_base + exe_export_offs);

    /* exe_public is a function in the exe we wouldn't be able to find without
     * drsyms and debug info.
     */
    (void)lookup_and_wrap(exe_path, exe_base, appbase,
                          "exe_public", DRSYM_DEFAULT_FLAGS);

    /* Test symbol not found error handling. */
    r = drsym_lookup_symbol(exe_path, "nonexistent_sym", &exe_public_offs,
                            DRSYM_DEFAULT_FLAGS);
    ASSERT(r == DRSYM_ERROR_SYMBOL_NOT_FOUND);

    /* Test invalid parameter errors. */
    r = drsym_lookup_symbol(NULL, "malloc", &exe_public_offs, DRSYM_DEFAULT_FLAGS);
    ASSERT(r == DRSYM_ERROR_INVALID_PARAMETER);
    r = drsym_lookup_symbol(exe_path, NULL, &exe_public_offs, DRSYM_DEFAULT_FLAGS);
    ASSERT(r == DRSYM_ERROR_INVALID_PARAMETER);
    r = drsym_enumerate_symbols(exe_path, NULL, NULL, DRSYM_DEFAULT_FLAGS);
    ASSERT(r == DRSYM_ERROR_INVALID_PARAMETER);
    r = drsym_lookup_address(NULL, 0xDEADBEEFUL, &unused_info, DRSYM_DEFAULT_FLAGS);
    ASSERT(r == DRSYM_ERROR_INVALID_PARAMETER);

#ifdef WINDOWS
    if (TEST(DRSYM_PDB, debug_kind)) /* else NYI */
        lookup_overloads(exe_path);
#endif

    dr_free_module_data(exe_data);
}

#ifdef WINDOWS
# define NUM_OVERLOADED_CLASS 3
typedef struct _overloaded_params_t {
    const char *exe_path;
    bool overloaded_char;
    bool overloaded_wchar;
    bool overloaded_int;
    bool overloaded_void_ptr;
    bool overloaded_void;
    uint overloaded_class;
} overloaded_params_t;

static bool
overloaded_cb(const char *name, size_t modoffs, void *data)
{
    overloaded_params_t *p = (overloaded_params_t*)data;
    drsym_func_type_t *func_type;
    static char buf[4096];
    drsym_error_t r;

    if (strcmp(name, "overloaded") != 0)
        return true;
    r = drsym_get_func_type(p->exe_path, modoffs, buf, sizeof(buf), &func_type);
    if (r != DRSYM_SUCCESS) {
        dr_fprintf(STDERR, "drsym_get_func_type failed: %d\n", (int)r);
        return true;
    }
    if (func_type->num_args == 1 &&
        func_type->arg_types[0]->kind == DRSYM_TYPE_PTR) {
        drsym_ptr_type_t *arg_type = (drsym_ptr_type_t*)func_type->arg_types[0];
        size_t arg_int_size = arg_type->elt_type->size;
        if (arg_type->elt_type->kind == DRSYM_TYPE_INT) {
            switch (arg_int_size) {
            case 1: p->overloaded_char = true;  break;
            case 2: p->overloaded_wchar = true; break;
            case 4: p->overloaded_int = true;   break;
            default: break;
            }
        } else if (arg_type->elt_type->kind == DRSYM_TYPE_VOID) {
            p->overloaded_void_ptr = true;
        } else if (arg_type->elt_type->kind == DRSYM_TYPE_COMPOUND) {
            drsym_compound_type_t *ctype = (drsym_compound_type_t *) arg_type->elt_type;
            int i;
            ASSERT(ctype->field_types == NULL); /* drsym_get_func_type does not expand */
            p->overloaded_class++;
            dr_fprintf(STDERR, "compound arg %s has %d field(s), size %d\n",
                       ctype->name, ctype->num_fields, ctype->type.size);
            r = drsym_expand_type(p->exe_path, ctype->type.id, UINT_MAX,
                                  buf, sizeof(buf), (drsym_type_t **)&ctype);
            if (r != DRSYM_SUCCESS) {
                dr_fprintf(STDERR, "drsym_expand_type failed: %d\n", (int)r);
            } else {
                ASSERT(ctype->type.kind == DRSYM_TYPE_COMPOUND);
                for (i=0; i < ctype->num_fields; i++) {
                    dr_fprintf(STDERR, "  class field %d is type %d and size %d\n",
                               i, ctype->field_types[i]->kind,
                               ctype->field_types[i]->size);
                    if (ctype->field_types[i]->kind == DRSYM_TYPE_FUNC) {
                        drsym_func_type_t *ftype = (drsym_func_type_t *)
                            ctype->field_types[i];
                        dr_fprintf(STDERR, "    func has %d args\n", ftype->num_args);
                        for (i=0; i < ftype->num_args; i++) {
                            dr_fprintf(STDERR, "      arg %d is type %d and size %d\n",
                                       i, ftype->arg_types[i]->kind,
                                       ftype->arg_types[i]->size);
                        }
                    }
                }
            }
        } else {
            dr_fprintf(STDERR, "overloaded() arg has unexpected type!\n");
        }
    } else if (func_type->num_args == 0) {
        /* no arg so not really an overload, but we need to test no-arg func */
        p->overloaded_void = true;
    } else {
        dr_fprintf(STDERR, "overloaded() has unexpected args\n");
    }

    return true;
}

static void
lookup_overloads(const char *exe_path)
{
    overloaded_params_t p;
    drsym_error_t r;

    p.exe_path = exe_path;
    p.overloaded_char = false;
    p.overloaded_wchar = false;
    p.overloaded_int = false;
    p.overloaded_void = false;
    p.overloaded_void_ptr = false;
    p.overloaded_class = 0;
    r = drsym_enumerate_symbols(exe_path, overloaded_cb, &p,
                                DRSYM_DEFAULT_FLAGS);
    ASSERT(r == DRSYM_SUCCESS);
    if (!p.overloaded_char)  dr_fprintf(STDERR, "overloaded_char missing!\n");
    if (!p.overloaded_wchar) dr_fprintf(STDERR, "overloaded_wchar missing!\n");
    if (!p.overloaded_int)   dr_fprintf(STDERR, "overloaded_int missing!\n");
    if (!p.overloaded_void)  dr_fprintf(STDERR, "overloaded_void missing!\n");
    if (!p.overloaded_void_ptr)  dr_fprintf(STDERR, "overloaded_void_ptr missing!\n");
    if (p.overloaded_class != NUM_OVERLOADED_CLASS)
        dr_fprintf(STDERR, "overloaded_class missing!\n");
    if (p.overloaded_char &&
        p.overloaded_wchar &&
        p.overloaded_int &&
        p.overloaded_void &&
        p.overloaded_void_ptr &&
        p.overloaded_class == NUM_OVERLOADED_CLASS) {
        dr_fprintf(STDERR, "found all overloads\n");
    }
}
#endif /* WINDOWS */

/* Lookup symbols in the appdll and wrap them. */
static void
lookup_dll_syms(void *dc, const module_data_t *dll_data, bool loaded)
{
    const char *dll_path;
    app_pc dll_base;
    app_pc dll_export_addr;
    size_t dll_export_offs;
    size_t stack_trace_offs;
    drsym_error_t r;
    bool ok;
    drsym_debug_kind_t debug_kind;
    const char *dll_name = dr_module_preferred_name(dll_data);
    char base_name[MAXIMUM_PATH];
    size_t len;

    dll_path = dll_data->full_path;
    dll_base = dll_data->start;

#ifdef LINUX
    if (strstr(dll_path, "/libc-")) {
        lookup_glibc_syms(dc, dll_data);
        return;
    }
#endif

    /* Avoid running on any module other than the appdll. */
    if (!strstr(dll_path, "appdll"))
        return;

    len = dr_snprintf(base_name, BUFFER_SIZE_ELEMENTS(base_name), "%s", dll_name);
    ASSERT(len > 0);
#ifdef WINDOWS
    /* blindly assuming ends in .dll */
    base_name[strlen(dll_name) - 4/*subtract .dll*/] = '\0';
#else
    /* blindly assuming ends in .so */
    base_name[strlen(dll_name) - 3/*subtract .so*/] = '\0';
#endif

    /* We expect to have full debug info for this module. */
    r = drsym_get_module_debug_kind(dll_path, &debug_kind);
    ASSERT(r == DRSYM_SUCCESS);
    if (!debug_kind_is_full(debug_kind)) {
        dr_fprintf(STDERR, "unexpected debug_kind: %x\n", debug_kind);
    }

    dll_export_addr = get_real_proc_addr(dll_data->handle, "dll_export");
    dll_export_offs = lookup_and_wrap(dll_path, dll_base, base_name,
                                      "dll_export", DRSYM_DEFAULT_FLAGS);
    ASSERT(dll_export_addr == dll_base + dll_export_offs);

    /* dll_public is a function in the dll we wouldn't be able to find without
     * drsyms and debug info.
     */
    (void)lookup_and_wrap(dll_path, dll_base, base_name,
                          "dll_public", DRSYM_DEFAULT_FLAGS);

    /* stack_trace is a static function in the DLL that we use to get PCs of all
     * the functions we've looked up so far.
     */
    dr_snprintf(base_name + strlen(base_name),
                BUFFER_SIZE_ELEMENTS(base_name) - strlen(base_name), "!stack_trace");
    r = drsym_lookup_symbol(dll_path, base_name, &stack_trace_offs, DRSYM_DEFAULT_FLAGS);
    ASSERT(r == DRSYM_SUCCESS);
    ok = drwrap_wrap(dll_base + stack_trace_offs, pre_stack_trace, post_func);
    ASSERT(ok);

    check_enumerate_dll_syms(dll_path);

    drsym_free_resources(dll_path);
}

static const char *dll_syms[] = {
    "dll_export",
    "dll_static",
    "dll_public",
    "stack_trace",
    NULL
};

/* FIXME: We don't support getting mangled or fully demangled symbols on
 * Windows PDB.
 */
static const char *dll_syms_mangled_pdb[] = {
    "dll_export",
    "dll_static",
    "dll_public",
    "stack_trace",
    NULL

};

static const char *dll_syms_mangled[] = {
    "dll_export",
    /* x64 MinGW gcc 4.7 matches linux.
     * XXX: will 32-bit MinGW/Cygwin gcc 4.7 also?  For now leaving as X64.
     */
#if defined(LINUX) || defined(X64)
    /* the L is a GNU extension to the Itanium mangling spec which isn't used
     * by MinGW 32-bit through 4.6.1
     */
    "_ZL10dll_statici",
#else
    "_Z10dll_statici",
#endif
    "_Z10dll_publici",
    "_Z11stack_tracev",
    NULL
};

static const char *dll_syms_short_pdb[] = {
    "dll_export",
    "dll_static",
    "dll_public",
    "stack_trace",
    NULL
};

static const char *dll_syms_short[] = {
    "dll_export",
    "dll_static()",
    "dll_public()",
    "stack_trace()",
    NULL
};

static const char *dll_syms_full_pdb[] = {
    "dll_export",
    "dll_static",
    "dll_public",
    "stack_trace",
    NULL
};

static const char *dll_syms_full[] = {
    "dll_export",
    "dll_static(int)",
    "dll_public(int)",
    "stack_trace(void)",
    NULL
};

typedef struct {
    bool syms_found[BUFFER_SIZE_ELEMENTS(dll_syms) - 1];
    const char **syms_expected;
} dll_syms_found_t;

/* If this was a symbol we expected that we haven't found yet, mark it found,
 * and check the mangling to see if it matches our expected mangling.
 */
static bool
enum_sym_cb(const char *name, size_t modoffs, void *data)
{
    dll_syms_found_t *syms_found = (dll_syms_found_t*)data;
    uint i;
    for (i = 0; i < BUFFER_SIZE_ELEMENTS(syms_found->syms_found); i++) {
        if (syms_found->syms_found[i])
            continue;
        if (strstr(name, dll_syms[i]) != NULL) {
            syms_found->syms_found[i] = true;
            /* Ignore () at function end. */
            if (strcmp(name, syms_found->syms_expected[i]) != 0) {
                dr_fprintf(STDERR, "symbol had wrong mangling:\n"
                           " expected: %s\n actual: %s\n",
                           syms_found->syms_expected[i], name);
            }
        }
    }
    return true;
}

static void
enum_syms_with_flags(const char *dll_path, const char **syms_expected,
                     uint flags)
{
    dll_syms_found_t syms_found;
    drsym_error_t r;
    uint i;
    drsym_debug_kind_t debug_kind;
    r = drsym_get_module_debug_kind(dll_path, &debug_kind);
    ASSERT(r == DRSYM_SUCCESS);

    memset(&syms_found, 0, sizeof(syms_found));
    syms_found.syms_expected = syms_expected;
    r = drsym_enumerate_symbols(dll_path, enum_sym_cb, &syms_found, flags);
    ASSERT(r == DRSYM_SUCCESS);
    for (i = 0; i < BUFFER_SIZE_ELEMENTS(syms_found.syms_found); i++)
        if (!syms_found.syms_found[i])
            dr_fprintf(STDERR, "failed to find symbol for %s!\n", dll_syms[i]);

#ifdef WINDOWS
    if (TEST(DRSYM_PDB, debug_kind)) {
        /* drsym_search_symbols should find the same symbols with the short
         * mangling, regardless of the flags used by the previous enumerations.
         */
        memset(&syms_found, 0, sizeof(syms_found));
        syms_found.syms_expected = dll_syms_short_pdb;
        r = drsym_search_symbols(dll_path, "*!*dll_*", false, enum_sym_cb,
                                 &syms_found);
        ASSERT(r == DRSYM_SUCCESS);
        r = drsym_search_symbols(dll_path, "*!*stack_trace*", false, enum_sym_cb,
                                 &syms_found);
        ASSERT(r == DRSYM_SUCCESS);
        for (i = 0; i < BUFFER_SIZE_ELEMENTS(syms_found.syms_found); i++)
            if (!syms_found.syms_found[i])
                dr_fprintf(STDERR, "failed to find symbol for %s!\n", dll_syms[i]);
    }
#endif
}


/* Enumerate all symbols in the dll and verify that we at least find the ones
 * we expected to be there, and that DRSYM_LEAVE_MANGLED was respected.
 */
static void
check_enumerate_dll_syms(const char *dll_path)
{
    drsym_debug_kind_t debug_kind;
    drsym_error_t r = drsym_get_module_debug_kind(dll_path, &debug_kind);
    ASSERT(r == DRSYM_SUCCESS);

    dr_fprintf(STDERR, "enumerating with DRSYM_LEAVE_MANGLED\n");
    enum_syms_with_flags(dll_path, TEST(DRSYM_PDB, debug_kind) ? dll_syms_mangled_pdb :
                         dll_syms_mangled, DRSYM_LEAVE_MANGLED);
    dr_fprintf(STDERR, "enumerating with DRSYM_DEMANGLE\n");
    enum_syms_with_flags(dll_path, TEST(DRSYM_PDB, debug_kind) ? dll_syms_short_pdb :
                         dll_syms_short,   DRSYM_DEMANGLE);
    dr_fprintf(STDERR, "enumerating with DRSYM_DEMANGLE_FULL\n");
    enum_syms_with_flags(dll_path, TEST(DRSYM_PDB, debug_kind) ? dll_syms_full_pdb :
                         dll_syms_full,    (DRSYM_DEMANGLE|DRSYM_DEMANGLE_FULL));
}

#ifdef LINUX
/* Test if we can look up glibc symbols.  This only works if the user is using
 * glibc (and not some other libc) and is has debug info installed for it, so we
 * avoid making assertions if we can't find the symbols.  The purpose of this
 * test is really to see if we can follow the .gnu_debuglink section into
 * /usr/lib/debug/$mod_dir/$debuglink.
 */
static void
lookup_glibc_syms(void *dc, const module_data_t *dll_data)
{
    const char *libc_path;
    app_pc libc_base;
    size_t malloc_offs;
    size_t gi_malloc_offs;
    drsym_error_t r;

    /* i#479: DR loads a private copy of libc.  The result should be the same
     * both times, so avoid running twice.
     */
    static bool already_called = false;
    if (already_called) {
        return;
    }
    already_called = true;

    libc_path = dll_data->full_path;
    libc_base = dll_data->start;

    /* FIXME: When drsyms can read .dynsym we should always find malloc. */
    malloc_offs = 0;
    r = drsym_lookup_symbol(libc_path, "libc!malloc", &malloc_offs,
                            DRSYM_DEFAULT_FLAGS);
    if (r == DRSYM_SUCCESS)
        ASSERT(malloc_offs != 0);

    /* __GI___libc_malloc is glibc's internal reference to malloc.  They use
     * these internal symbols so that glibc calls to exported functions are
     * never pre-empted by other libraries.
     */
    gi_malloc_offs = 0;
    r = drsym_lookup_symbol(libc_path, "libc!__GI___libc_malloc",
                            &gi_malloc_offs, DRSYM_DEFAULT_FLAGS);
    /* We can't compare the offsets because the exported offset and internal
     * offset are probably going to be different.
     */
    if (r == DRSYM_SUCCESS)
        ASSERT(gi_malloc_offs != 0);

    if (malloc_offs != 0 && gi_malloc_offs != 0) {
        dr_fprintf(STDERR, "found glibc malloc and __GI___libc_malloc.\n");
    } else {
        dr_fprintf(STDERR, "couldn't find glibc malloc or __GI___libc_malloc.\n");
    }
}
#endif /* LINUX */

typedef struct {
    const char *mangled;
    const char *dem_full;
    const char *demangled;
} cpp_name_t;

/* Table of mangled and unmangled symbols taken as a random sample from a
 * 32-bit Linux Chromium binary.
 */
static cpp_name_t symbols_unix[] = {
    {"_ZN4baseL9kDeadTaskE",
     "base::kDeadTask",
     "base::kDeadTask"},
    {"xmlRelaxNGParseImportRefs",
     "xmlRelaxNGParseImportRefs",
     "xmlRelaxNGParseImportRefs"},
    {"_ZL16piOverFourDouble",
     "piOverFourDouble",
     "piOverFourDouble"},
    {"_ZL8kint8min",
     "kint8min",
     "kint8min"},
    {"_ZZN7WebCore19SVGAnimatedProperty20LookupOrCreateHelperINS_32SVGAnimatedStaticPropertyTearOffIbEEbLb1EE21lookupOrCreateWrapperEPNS_10SVGElementEPKNS_15SVGPropertyInfoERbE19__PRETTY_FUNCTION__",
     "WebCore::SVGAnimatedProperty::LookupOrCreateHelper<WebCore::SVGAnimatedStaticPropertyTearOff<bool>, bool, true, E>::lookupOrCreateWrapper(WebCore::SVGElement*, WebCore::SVGPropertyInfo const*, bool&)::__PRETTY_FUNCTION__",
     "WebCore::SVGAnimatedProperty::LookupOrCreateHelper<>::lookupOrCreateWrapper()::__PRETTY_FUNCTION__"},
    {"_ZL26GrNextArrayAllocationCounti",
     "GrNextArrayAllocationCount(int)",
     "GrNextArrayAllocationCount()"},
    {"_ZN18safe_browsing_util25GeneratePhishingReportUrlERKSsS1_b",
     "safe_browsing_util::GeneratePhishingReportUrl(std::string const&, std::string, bool)",
     "safe_browsing_util::GeneratePhishingReportUrl()"},
    {"_ZN9__gnu_cxx8hash_mapIjPN10disk_cache9EntryImplENS_4hashIjEESt8equal_toIjESaIS3_EE4findERKj",
     "__gnu_cxx::hash_map<unsigned int, disk_cache::EntryImpl*, __gnu_cxx::hash<unsigned int>, std::equal_to<unsigned int>, std::allocator<disk_cache::EntryImpl*> >::find(unsigned int const&)",
     "__gnu_cxx::hash_map<>::find()"},
    {"_ZN18shortcuts_provider8ShortcutC2ERKSbItN4base20string16_char_traitsESaItEERK4GURLS6_RKSt6vectorIN17AutocompleteMatch21ACMatchClassificationESaISC_EES6_SG_",
     "shortcuts_provider::Shortcut::Shortcut(std::basic_string<unsigned short, base::string16_char_traits, std::allocator<unsigned short> > const&, GURL const&, std::basic_string<unsigned short, base::string16_char_traits, std::allocator<unsigned short> > const, std::vector<AutocompleteMatch::ACMatchClassification, std::allocator<AutocompleteMatch> > const&, std::basic_string<unsigned short, base::string16_char_traits, std::allocator<unsigned short> > const, std::vector<AutocompleteMatch::ACMatchClassification, std::allocator<AutocompleteMatch> > const)",
     "shortcuts_provider::Shortcut::Shortcut()"},
    /* FIXME: We should teach libelftc how to demangle this. */
    {"_ZN10linked_ptrIN12CrxInstaller14WhitelistEntryEE4copyIS1_EEvPKS_IT_E",
     "_ZN10linked_ptrIN12CrxInstaller14WhitelistEntryEE4copyIS1_EEvPKS_IT_E",
     "linked_ptr<>::copy<>()"},
    {"_ZN27ScopedRunnableMethodFactoryIN6webkit5ppapi18PPB_Scrollbar_ImplEED1Ev",
     "ScopedRunnableMethodFactory<webkit::ppapi::PPB_Scrollbar_Impl>::~ScopedRunnableMethodFactory(void)",
     "ScopedRunnableMethodFactory<>::~ScopedRunnableMethodFactory()"},
    {"_ZN2v88internal9HashTableINS0_21StringDictionaryShapeEPNS0_6StringEE9FindEntryEPNS0_7IsolateES4_",
     "v8::internal::HashTable<v8::internal::StringDictionaryShape, v8::internal::String*>::FindEntry(v8::internal::Isolate*, v8::internal::HashTable<v8::internal::StringDictionaryShape, v8::internal::String*>)",
     "v8::internal::HashTable<>::FindEntry()"},
    {"_ZNK7WebCore8Settings19localStorageEnabledEv",
     "WebCore::Settings::localStorageEnabled(void) const",
     "WebCore::Settings::localStorageEnabled()"},
    {"_ZNSt4listIPN5media12VideoCapture12EventHandlerESaIS3_EE14_M_create_nodeERKS3_",
     "std::list<media::VideoCapture::EventHandler*, std::allocator<media::VideoCapture::EventHandler*> >::_M_create_node(media::VideoCapture::EventHandler* const&)",
     "std::list<>::_M_create_node()"},
    {"_ZNK9__gnu_cxx13new_allocatorISt13_Rb_tree_nodeISt4pairIKiP20RenderWidgetHostViewEEE8max_sizeEv",
     "__gnu_cxx::new_allocator<std::_Rb_tree_node<std::pair<int const, RenderWidgetHostView*> >>::max_size(void) const",
     "__gnu_cxx::new_allocator<>::max_size()"},
};

#ifdef WINDOWS
static cpp_name_t symbols_pdb[] = {
    {"?synchronizeRequiredExtensions@SVGSVGElement@WebCore@@EAEXXZ",
     "WebCore::SVGSVGElement::synchronizeRequiredExtensions(void)",
     "WebCore::SVGSVGElement::synchronizeRequiredExtensions"},
    {"??$?0$04@WebString@WebKit@@QAE@AAY04$$CBD@Z",
     "WebKit::WebString::WebString<5>(char const (&)[5])",
     "WebKit::WebString::WebString<5>"},
    {"?createParser@PluginDocument@WebCore@@EAE?AV?$PassRefPtr@VDocumentParser@WebCore@@@WTF@@XZ",
     "WebCore::PluginDocument::createParser(void)",
     "WebCore::PluginDocument::createParser"},
    {"?_Compat@?$_Vector_const_iterator@V?$_Iterator@$00@?$list@U?$pair@$$CBHPAVWebIDBCursor@WebKit@@@std@@V?$allocator@U?$pair@$$CBHPAVWebIDBCursor@WebKit@@@std@@@2@@std@@V?$allocator@V?$_Iterator@$00@?$list@U?$pair@$$CBHPAVWebIDBCursor@WebKit@@@std@@V?$allocator@U?$pair@$$CBHPAVWebIDBCursor@WebKit@@@std@@@2@@std@@@3@@std@@QBEXABV12@@Z",
     "std::_Vector_const_iterator<class std::list<struct std::pair<int const ,class WebKit::WebIDBCursor *>,class std::allocator<struct std::pair<int const ,class WebKit::WebIDBCursor *> > >::_Iterator<1>,class std::allocator<class std::list<struct std::pair<int const ,class WebKit::WebIDBCursor *>,class std::allocator<struct std::pair<int const ,class WebKit::WebIDBCursor *> > >::_Iterator<1> > >::_Compat(class std::_Vector_const_iterator<class std::list<struct std::pair<int const ,class WebKit::WebIDBCursor *>,class std::allocator<struct std::pair<int const ,class WebKit::WebIDBCursor *> > >::_Iterator<1>,class std::allocator<class std::list<struct std::pair<int const ,class WebKit::WebIDBCursor *>,class std::allocator<struct std::pair<int const ,class WebKit::WebIDBCursor *> > >::_Iterator<1> > > const &)const ",
     "std::_Vector_const_iterator<std::list<std::pair<int const ,WebKit::WebIDBCursor *>,std::allocator<std::pair<int const ,WebKit::WebIDBCursor *> > >::_Iterator<1>,std::allocator<std::list<std::pair<int const ,WebKit::WebIDBCursor *>,std::allocator<std::pair<int const ,WebKit::WebIDBCursor *> > >::_Iterator<1> > >::_Compat"},
    {"??$MatchAndExplain@VNotificationDetails@@@?$PropertyMatcher@V?$Details@$$CBVAutofillCreditCardChange@@@@PBVAutofillCreditCardChange@@@internal@testing@@QBE_NABVNotificationDetails@@PAVMatchResultListener@2@@Z",
     "testing::internal::PropertyMatcher<class Details<class AutofillCreditCardChange const >,class AutofillCreditCardChange const *>::MatchAndExplain<class NotificationDetails>(class NotificationDetails const &,class testing::MatchResultListener *)const ",
     "testing::internal::PropertyMatcher<Details<AutofillCreditCardChange const >,AutofillCreditCardChange const *>::MatchAndExplain<NotificationDetails>"},
    {"?MD5Sum@base@@YAXPBXIPAUMD5Digest@1@@Z",
     "base::MD5Sum(void const *,unsigned int,struct base::MD5Digest *)",
     "base::MD5Sum"},
    {"?create@EntryCallbacks@WebCore@@SA?AV?$PassOwnPtr@VEntryCallbacks@WebCore@@@WTF@@V?$PassRefPtr@VEntryCallback@WebCore@@@4@V?$PassRefPtr@VErrorCallback@WebCore@@@4@V?$PassRefPtr@VDOMFileSystemBase@WebCore@@@4@ABVString@4@_N@Z",
     "WebCore::EntryCallbacks::create(class WTF::PassRefPtr<class WebCore::EntryCallback>,class WTF::PassRefPtr<class WebCore::ErrorCallback>,class WTF::PassRefPtr<class WebCore::DOMFileSystemBase>,class WTF::String const &,bool)",
     "WebCore::EntryCallbacks::create"},
    {"?ReadReplyParam@ClipboardHostMsg_ReadAsciiText@@SA_NPBVMessage@IPC@@PAU?$Tuple1@V?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@@@@Z",
     "ClipboardHostMsg_ReadAsciiText::ReadReplyParam(class IPC::Message const *,struct Tuple1<class std::basic_string<char,struct std::char_traits<char>,class std::allocator<char> > > *)",
     "ClipboardHostMsg_ReadAsciiText::ReadReplyParam"},
    {"?begin@?$HashMap@PAVValue@v8@@PAVGlobalHandleInfo@WebCore@@U?$PtrHash@PAVValue@v8@@@WTF@@U?$HashTraits@PAVValue@v8@@@6@U?$HashTraits@PAVGlobalHandleInfo@WebCore@@@6@@WTF@@QAE?AU?$HashTableIteratorAdapter@V?$HashTable@PAVValue@v8@@U?$pair@PAVValue@v8@@PAVGlobalHandleInfo@WebCore@@@std@@U?$PairFirstExtractor@U?$pair@PAVValue@v8@@PAVGlobalHandleInfo@WebCore@@@std@@@WTF@@U?$PtrHash@PAVValue@v8@@@6@U?$PairHashTraits@U?$HashTraits@PAVValue@v8@@@WTF@@U?$HashTraits@PAVGlobalHandleInfo@WebCore@@@2@@6@U?$HashTraits@PAVValue@v8@@@6@@WTF@@U?$pair@PAVValue@v8@@PAVGlobalHandleInfo@WebCore@@@std@@@2@XZ",
     "WTF::HashMap<class v8::Value *,class WebCore::GlobalHandleInfo *,struct WTF::PtrHash<class v8::Value *>,struct WTF::HashTraits<class v8::Value *>,struct WTF::HashTraits<class WebCore::GlobalHandleInfo *> >::begin(void)",
     "WTF::HashMap<v8::Value *,WebCore::GlobalHandleInfo *,WTF::PtrHash<v8::Value *>,WTF::HashTraits<v8::Value *>,WTF::HashTraits<WebCore::GlobalHandleInfo *> >::begin"},
    {"??D?$_Deque_iterator@V?$linked_ptr@V?$CallbackRunner@U?$Tuple1@H@@@@@@V?$allocator@V?$linked_ptr@V?$CallbackRunner@U?$Tuple1@H@@@@@@@std@@$00@std@@QBEAAV?$linked_ptr@V?$CallbackRunner@U?$Tuple1@H@@@@@@XZ",
     "std::_Deque_iterator<class linked_ptr<class CallbackRunner<struct Tuple1<int> > >,class std::allocator<class linked_ptr<class CallbackRunner<struct Tuple1<int> > > >,1>::operator*(void)const ",
     "std::_Deque_iterator<linked_ptr<CallbackRunner<Tuple1<int> > >,std::allocator<linked_ptr<CallbackRunner<Tuple1<int> > > >,1>::operator*"},
    {"??$PerformAction@$$A6AXABVFilePath@@0PBVDictionaryValue@base@@PBVExtension@@@Z@?$ActionResultHolder@X@internal@testing@@SAPAV012@ABV?$Action@$$A6AXABVFilePath@@0PBVDictionaryValue@base@@PBVExtension@@@Z@2@ABV?$tuple@ABVFilePath@@ABV1@PBVDictionaryValue@base@@PBVExtension@@XXXXXX@tr1@std@@@Z",
     "testing::internal::ActionResultHolder<void>::PerformAction<void (class FilePath const &,class FilePath const &,class base::DictionaryValue const *,class Extension const *)>(class testing::Action<void (class FilePath const &,class FilePath const &,class base::DictionaryValue const *,class Extension const *)> const &,class std::tr1::tuple<class FilePath const &,class FilePath const &,class base::DictionaryValue const *,class Extension const *,void,void,void,void,void,void> const &)",
     "testing::internal::ActionResultHolder<void>::PerformAction<void __cdecl(FilePath const &,FilePath const &,base::DictionaryValue const *,Extension const *)>"},
    {"?ClassifyInputEvent@ppapi@webkit@@YA?AW4PP_InputEvent_Class@@W4Type@WebInputEvent@WebKit@@@Z",
     "webkit::ppapi::ClassifyInputEvent(enum WebKit::WebInputEvent::Type)",
     "webkit::ppapi::ClassifyInputEvent"},
};
#endif

static void
test_demangle_symbols(cpp_name_t *symbols, size_t symbols_sz)
{
    char sym_buf[2048];
    unsigned i;
    size_t len;

    for (i = 0; i < symbols_sz; i++) {
        cpp_name_t *sym = &symbols[i];

        /* Full demangling. */
        len = drsym_demangle_symbol(sym_buf, sizeof(sym_buf), sym->mangled,
                                    DRSYM_DEMANGLE_FULL);
        if (len == 0 || len >= sizeof(sym_buf)) {
            dr_fprintf(STDERR, "Failed to unmangle %s\n", sym->mangled);
        } else if (strcmp(sym_buf, sym->dem_full) != 0) {
            dr_fprintf(STDERR, "Unexpected unmangling:\n"
                       " actual: %s\n expected: %s\n",
                       sym_buf, sym->dem_full);
        }

        /* Short demangling (no templates or overloads). */
        len = drsym_demangle_symbol(sym_buf, sizeof(sym_buf), sym->mangled,
                                    DRSYM_DEMANGLE);
        if (len == 0 || len >= sizeof(sym_buf)) {
            dr_fprintf(STDERR, "Failed to unmangle %s\n", sym->mangled);
        } else if (strcmp(sym_buf, sym->demangled) != 0) {
            dr_fprintf(STDERR, "Unexpected unmangling:\n"
                       " actual: %s\n expected: %s\n",
                       sym_buf, sym->demangled);
        }
    }

    /* Test overflow. */
    len = drsym_demangle_symbol(sym_buf, 6, symbols[0].mangled, DRSYM_DEMANGLE_FULL);
    if (len == 0) {
        dr_fprintf(STDERR, "got error instead of overflow\n");
    } else if (len <= 6) {
        dr_fprintf(STDERR, "unexpected demangling success\n");
    } else {
        size_t old_len = 6;
        dr_fprintf(STDERR, "got correct overflow\n");
        /* Resize the buffer in a loop until it demangles correctly. */
        while (len > old_len && len < sizeof(sym_buf)) {
            old_len = len;
            len = drsym_demangle_symbol(sym_buf, old_len, symbols[0].mangled,
                                        DRSYM_DEMANGLE_FULL);
        }
        if (strcmp(sym_buf, symbols[0].dem_full) != 0) {
            dr_fprintf(STDERR, "retrying with demangle return value failed.\n");
        }
    }
}

static void
test_demangle(void)
{
    test_demangle_symbols(symbols_unix, BUFFER_SIZE_ELEMENTS(symbols_unix));
#ifdef WINDOWS
    test_demangle_symbols(symbols_pdb, BUFFER_SIZE_ELEMENTS(symbols_pdb));
#endif
    dr_fprintf(STDERR, "finished unmangling.\n");
}

static void
event_exit(void)
{
    drwrap_exit();
    drsym_exit();
    /* Check that all symbols we looked up got called. */
    ASSERT(call_count == 4);
    dr_fprintf(STDERR, "all done\n");
}
