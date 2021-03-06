/* Disassembly display.

   Copyright (C) 1998-2019 Free Software Foundation, Inc.

   Contributed by Hewlett-Packard Company.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"
#include "arch-utils.h"
#include "symtab.h"
#include "breakpoint.h"
#include "frame.h"
#include "value.h"
#include "source.h"
#include "disasm.h"
#include "tui/tui.h"
#include "tui/tui-data.h"
#include "tui/tui-win.h"
#include "tui/tui-layout.h"
#include "tui/tui-winsource.h"
#include "tui/tui-stack.h"
#include "tui/tui-file.h"
#include "tui/tui-disasm.h"
#include "tui/tui-source.h"
#include "progspace.h"
#include "objfiles.h"

#include "gdb_curses.h"

struct tui_asm_line 
{
  CORE_ADDR addr;
  char *addr_string;
  char *insn;
};

/* Function to set the disassembly window's content.
   Disassemble count lines starting at pc.
   Return address of the count'th instruction after pc.  */
static CORE_ADDR
tui_disassemble (struct gdbarch *gdbarch, struct tui_asm_line *asm_lines,
		 CORE_ADDR pc, int count)
{
  string_file gdb_dis_out;

  /* Now construct each line.  */
  for (; count > 0; count--, asm_lines++)
    {
      xfree (asm_lines->addr_string);
      xfree (asm_lines->insn);
      
      print_address (gdbarch, pc, &gdb_dis_out);
      asm_lines->addr = pc;
      asm_lines->addr_string = xstrdup (gdb_dis_out.c_str ());

      gdb_dis_out.clear ();

      pc = pc + gdb_print_insn (gdbarch, pc, &gdb_dis_out, NULL);

      asm_lines->insn = xstrdup (gdb_dis_out.c_str ());

      /* Reset the buffer to empty.  */
      gdb_dis_out.clear ();
    }
  return pc;
}

/* Find the disassembly address that corresponds to FROM lines above
   or below the PC.  Variable sized instructions are taken into
   account by the algorithm.  */
static CORE_ADDR
tui_find_disassembly_address (struct gdbarch *gdbarch, CORE_ADDR pc, int from)
{
  CORE_ADDR new_low;
  int max_lines;
  int i;
  struct tui_asm_line *asm_lines;

  max_lines = (from > 0) ? from : - from;
  if (max_lines <= 1)
     return pc;

  asm_lines = XALLOCAVEC (struct tui_asm_line, max_lines);
  memset (asm_lines, 0, sizeof (struct tui_asm_line) * max_lines);

  new_low = pc;
  if (from > 0)
    {
      tui_disassemble (gdbarch, asm_lines, pc, max_lines);
      new_low = asm_lines[max_lines - 1].addr;
    }
  else
    {
      CORE_ADDR last_addr;
      int pos;
      struct bound_minimal_symbol msymbol;
              
      /* Find backward an address which is a symbol and for which
         disassembling from that address will fill completely the
         window.  */
      pos = max_lines - 1;
      do {
         new_low -= 1 * max_lines;
         msymbol = lookup_minimal_symbol_by_pc_section (new_low, 0);

         if (msymbol.minsym)
            new_low = BMSYMBOL_VALUE_ADDRESS (msymbol);
         else
            new_low += 1 * max_lines;

         tui_disassemble (gdbarch, asm_lines, new_low, max_lines);
         last_addr = asm_lines[pos].addr;
      } while (last_addr > pc && msymbol.minsym);

      /* Scan forward disassembling one instruction at a time until
         the last visible instruction of the window matches the pc.
         We keep the disassembled instructions in the 'lines' window
         and shift it downward (increasing its addresses).  */
      if (last_addr < pc)
        do
          {
            CORE_ADDR next_addr;
                 
            pos++;
            if (pos >= max_lines)
              pos = 0;

            next_addr = tui_disassemble (gdbarch, &asm_lines[pos],
					 last_addr, 1);

            /* If there are some problems while disassembling exit.  */
            if (next_addr <= last_addr)
              break;
            last_addr = next_addr;
          } while (last_addr <= pc);
      pos++;
      if (pos >= max_lines)
         pos = 0;
      new_low = asm_lines[pos].addr;
    }
  for (i = 0; i < max_lines; i++)
    {
      xfree (asm_lines[i].addr_string);
      xfree (asm_lines[i].insn);
    }
  return new_low;
}

/* Function to set the disassembly window's content.  */
enum tui_status
tui_set_disassem_content (tui_source_window_base *win_info,
			  struct gdbarch *gdbarch, CORE_ADDR pc)
{
  int i;
  int offset = win_info->horizontal_offset;
  int max_lines, line_width;
  CORE_ADDR cur_pc;
  struct tui_locator_window *locator = tui_locator_win_info_ptr ();
  int tab_len = tui_tab_width;
  struct tui_asm_line *asm_lines;
  int insn_pos;
  int addr_size, insn_size;
  char *line;
  
  if (pc == 0)
    return TUI_FAILURE;

  tui_alloc_source_buffer (win_info);

  win_info->gdbarch = gdbarch;
  win_info->start_line_or_addr.loa = LOA_ADDRESS;
  win_info->start_line_or_addr.u.addr = pc;
  cur_pc = locator->addr;

  /* Window size, excluding highlight box.  */
  max_lines = win_info->height - 2;
  line_width = win_info->width - 2;

  /* Get temporary table that will hold all strings (addr & insn).  */
  asm_lines = XALLOCAVEC (struct tui_asm_line, max_lines);
  memset (asm_lines, 0, sizeof (struct tui_asm_line) * max_lines);

  tui_disassemble (gdbarch, asm_lines, pc, max_lines);

  /* Determine maximum address- and instruction lengths.  */
  addr_size = 0;
  insn_size = 0;
  for (i = 0; i < max_lines; i++)
    {
      size_t len = strlen (asm_lines[i].addr_string);

      if (len > addr_size)
        addr_size = len;

      len = strlen (asm_lines[i].insn);
      if (len > insn_size)
	insn_size = len;
    }

  /* Align instructions to the same column.  */
  insn_pos = (1 + (addr_size / tab_len)) * tab_len;

  /* Allocate memory to create each line.  */
  line = (char*) alloca (insn_pos + insn_size + 1);

  /* Now construct each line.  */
  win_info->content.resize (max_lines);
  for (i = 0; i < max_lines; i++)
    {
      int cur_len;

      tui_source_element *src = &win_info->content[i];
      strcpy (line, asm_lines[i].addr_string);
      cur_len = strlen (line);
      memset (line + cur_len, ' ', insn_pos - cur_len);
      strcpy (line + insn_pos, asm_lines[i].insn);

      /* Now copy the line taking the offset into account.  */
      xfree (src->line);
      if (strlen (line) > offset)
	src->line = xstrndup (&line[offset], line_width);
      else
	src->line = xstrdup ("");

      src->line_or_addr.loa = LOA_ADDRESS;
      src->line_or_addr.u.addr = asm_lines[i].addr;
      src->is_exec_point = asm_lines[i].addr == cur_pc;

      xfree (asm_lines[i].addr_string);
      xfree (asm_lines[i].insn);
    }
  return TUI_SUCCESS;
}


/* Function to display the disassembly window with disassembled code.  */
void
tui_show_disassem (struct gdbarch *gdbarch, CORE_ADDR start_addr)
{
  struct symtab *s = find_pc_line_symtab (start_addr);
  struct tui_win_info *win_with_focus = tui_win_with_focus ();
  struct tui_line_or_address val;

  val.loa = LOA_ADDRESS;
  val.u.addr = start_addr;
  tui_add_win_to_layout (DISASSEM_WIN);
  tui_update_source_window (TUI_DISASM_WIN, gdbarch, s, val, FALSE);

  /* If the focus was in the src win, put it in the asm win, if the
     source view isn't split.  */
  if (tui_current_layout () != SRC_DISASSEM_COMMAND 
      && win_with_focus == TUI_SRC_WIN)
    tui_set_win_focus_to (TUI_DISASM_WIN);
}


/* Function to display the disassembly window.  */
void
tui_show_disassem_and_update_source (struct gdbarch *gdbarch,
				     CORE_ADDR start_addr)
{
  struct symtab_and_line sal;

  tui_show_disassem (gdbarch, start_addr);
  if (tui_current_layout () == SRC_DISASSEM_COMMAND)
    {
      struct tui_line_or_address val;

      /* Update what is in the source window if it is displayed too,
         note that it follows what is in the disassembly window and
         visa-versa.  */
      sal = find_pc_line (start_addr, 0);
      val.loa = LOA_LINE;
      val.u.line_no = sal.line;
      tui_update_source_window (TUI_SRC_WIN, gdbarch, sal.symtab, val, TRUE);
      if (sal.symtab)
	{
	  set_current_source_symtab_and_line (sal);
	  tui_update_locator_fullname (symtab_to_fullname (sal.symtab));
	}
      else
	tui_update_locator_fullname ("?");
    }
}

void
tui_get_begin_asm_address (struct gdbarch **gdbarch_p, CORE_ADDR *addr_p)
{
  struct tui_locator_window *locator;
  struct gdbarch *gdbarch = get_current_arch ();
  CORE_ADDR addr;

  locator = tui_locator_win_info_ptr ();

  if (locator->addr == 0)
    {
      struct bound_minimal_symbol main_symbol;

      /* Find address of the start of program.
         Note: this should be language specific.  */
      main_symbol = lookup_minimal_symbol ("main", NULL, NULL);
      if (main_symbol.minsym == 0)
        main_symbol = lookup_minimal_symbol ("MAIN", NULL, NULL);
      if (main_symbol.minsym == 0)
        main_symbol = lookup_minimal_symbol ("_start", NULL, NULL);
      if (main_symbol.minsym)
        addr = BMSYMBOL_VALUE_ADDRESS (main_symbol);
      else
        addr = 0;
    }
  else				/* The target is executing.  */
    {
      gdbarch = locator->gdbarch;
      addr = locator->addr;
    }

  *gdbarch_p = gdbarch;
  *addr_p = addr;
}

/* Determine what the low address will be to display in the TUI's
   disassembly window.  This may or may not be the same as the low
   address input.  */
CORE_ADDR
tui_get_low_disassembly_address (struct gdbarch *gdbarch,
				 CORE_ADDR low, CORE_ADDR pc)
{
  int pos;

  /* Determine where to start the disassembly so that the pc is about
     in the middle of the viewport.  */
  pos = tui_default_win_viewport_height (DISASSEM_WIN, DISASSEM_COMMAND) / 2;
  pc = tui_find_disassembly_address (gdbarch, pc, -pos);

  if (pc < low)
    pc = low;
  return pc;
}

/* Scroll the disassembly forward or backward vertically.  */
void
tui_disasm_window::do_scroll_vertical (int num_to_scroll)
{
  if (!content.empty ())
    {
      CORE_ADDR pc;
      struct tui_line_or_address val;

      pc = content[0].line_or_addr.u.addr;
      if (num_to_scroll >= 0)
	num_to_scroll++;
      else
	--num_to_scroll;

      val.loa = LOA_ADDRESS;
      val.u.addr = tui_find_disassembly_address (gdbarch, pc, num_to_scroll);
      tui_update_source_window_as_is (this, gdbarch,
				      NULL, val, FALSE);
    }
}

bool
tui_disasm_window::location_matches_p (struct bp_location *loc, int line_no)
{
  return (content[line_no].line_or_addr.loa == LOA_ADDRESS
	  && content[line_no].line_or_addr.u.addr == loc->address);
}
