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

/* drsyms UNIX arch-specific (ELF or PECOFF) header, separated so we don't
 * have to include dwarf.h everywhere
 */

#ifndef DRSYMS_ARCH_H
#define DRSYMS_ARCH_H

#include "drsyms.h"
#include "dwarf.h"
#include "libdwarf.h"

/***************************************************************************
 * Platform-specific: Linux (ELF) or Cygwin (PECOFF)
 */

void
drsym_obj_init(void);

void *
drsym_obj_mod_init_pre(byte *map_base, size_t file_size);

bool
drsym_obj_mod_init_post(void *mod_in);

bool
drsym_obj_dwarf_init(void *mod_in, Dwarf_Debug *dbg);

void
drsym_obj_mod_exit(void *mod_in);

drsym_debug_kind_t
drsym_obj_info_avail(void *mod_in);

byte *
drsym_obj_load_base(void *mod_in);

const char *
drsym_obj_debuglink_section(void *mod_in);

uint
drsym_obj_num_symbols(void *mod_in);

const char *
drsym_obj_symbol_name(void *mod_in, uint idx);

drsym_error_t
drsym_obj_symbol_offs(void *mod_in, uint idx, size_t *offs_start OUT,
                       size_t *offs_end OUT);

drsym_error_t
drsym_obj_addrsearch_symtab(void *mod_in, size_t modoffs, uint *idx OUT);

bool
drsym_obj_same_file(const char *path1, const char *path2);

const char *
drsym_obj_debug_path(void);

/***************************************************************************
 * DWARF
 */

void *
drsym_dwarf_init(Dwarf_Debug dbg);

void
drsym_dwarf_exit(void *mod_in);

bool
drsym_dwarf_search_addr2line(void *mod_in, Dwarf_Addr pc, drsym_info_t *sym_info INOUT);

#endif /* DRSYMS_ARCH_H */
