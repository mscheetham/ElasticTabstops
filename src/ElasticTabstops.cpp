// This file is part of ElasticTabstops.
// 
// Original work Copyright (C)2007-2014 Nick Gravgaard, David Kinder
// Derived work Copyright (C)2016 Justin Dailey <dail8859@yahoo.com>
// 
// ElasticTabstops is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either
// version 2 of the License, or (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

#include <malloc.h>
#include "ElasticTabstops.h"

static SciFnDirect Scintilla_DirectFunction;

static LONG_PTR inline call_edit(sptr_t edit, UINT msg, DWORD wp = 0, LONG_PTR lp = 0)
{
	return Scintilla_DirectFunction(edit, msg, wp, lp);
}

static int get_line_start(sptr_t edit, int pos)
{
	int line = call_edit(edit, SCI_LINEFROMPOSITION, pos);
	return call_edit(edit, SCI_POSITIONFROMLINE, line);
}

static int get_line_end(sptr_t edit, int pos)
{
	int line = call_edit(edit, SCI_LINEFROMPOSITION, pos);
	return call_edit(edit, SCI_GETLINEENDPOSITION, line);
}

static bool is_line_end(sptr_t edit, int pos)
{
	int line = call_edit(edit, SCI_LINEFROMPOSITION, pos);
	int end_pos = call_edit(edit, SCI_GETLINEENDPOSITION, line);
	return (pos == end_pos);
}

struct et_tabstop
{
	int text_width_pix;
	int *widest_width_pix;
	bool ends_in_tab;
};

struct et_line
{
	int num_tabs;
};

enum direction
{
	BACKWARDS,
	FORWARDS
};


static et_tabstop* grid_buffer = NULL;
static int grid_buffer_size = 0;
static int tab_width_minimum;
static int tab_width_padding;


static int get_text_width(sptr_t edit, int start, int end)
{
	TextRange range;
	range.chrg.cpMin = start;
	range.chrg.cpMax = end;
	range.lpstrText = (char*)_alloca(end - start + 1);
	call_edit(edit, SCI_GETTEXTRANGE, 0, (sptr_t)&range);

	LONG_PTR style = call_edit(edit, SCI_GETSTYLEAT, start);

	return call_edit(edit, SCI_TEXTWIDTH, style, (LONG_PTR)range.lpstrText);
}

static int calc_tab_width(int text_width_in_tab)
{
	if (text_width_in_tab < tab_width_minimum)
	{
		text_width_in_tab = tab_width_minimum;
	}
	return text_width_in_tab + tab_width_padding;
}

static bool change_line(sptr_t edit, int& location, direction which_dir)
{
	int line = call_edit(edit, SCI_LINEFROMPOSITION, location);
	if (which_dir == FORWARDS)
	{
		location = call_edit(edit, SCI_POSITIONFROMLINE, line + 1);
	}
	else
	{
		if (line <= 0)
		{
			return false;
		}
		location = call_edit(edit, SCI_POSITIONFROMLINE, line - 1);
	}
	return (location >= 0);
}

static int get_block_boundary(sptr_t edit, int& location, direction which_dir)
{
	int current_pos;
	int max_tabs = 0;
	bool orig_line = true;

	location = get_line_start(edit, location);
	do
	{
		int tabs_on_line = 0;

		current_pos = location;
		unsigned char current_char = (unsigned char)call_edit(edit, SCI_GETCHARAT, current_pos);
		bool current_char_ends_line = is_line_end(edit, current_pos);

		while (current_char != '\0' && !current_char_ends_line)
		{
			if (current_char == '\t')
			{
				tabs_on_line++;
				if (tabs_on_line > max_tabs)
				{
					max_tabs = tabs_on_line;
				}
			}
			current_pos = call_edit(edit, SCI_POSITIONAFTER, current_pos);
			current_char = (unsigned char)call_edit(edit, SCI_GETCHARAT, current_pos);
			current_char_ends_line = is_line_end(edit, current_pos);
		}
		if (tabs_on_line == 0 && !orig_line)
		{
			return max_tabs;
		}
		orig_line = false;
	} while (change_line(edit, location, which_dir));
	return max_tabs;
}

static int get_nof_tabs_between(sptr_t edit, int start, int end)
{
	int current_pos = get_line_start(edit, start);
	int max_tabs = 0;

	do
	{
		unsigned char current_char = (unsigned char)call_edit(edit, SCI_GETCHARAT, current_pos);
		bool current_char_ends_line = is_line_end(edit, current_pos);

		int tabs_on_line = 0;
		while (current_char != '\0' && !current_char_ends_line)
		{
			if (current_char == '\t')
			{
				tabs_on_line++;
				if (tabs_on_line > max_tabs)
				{
					max_tabs = tabs_on_line;
				}
			}
			current_pos = call_edit(edit, SCI_POSITIONAFTER, current_pos);
			current_char = (unsigned char)call_edit(edit, SCI_GETCHARAT, current_pos);
			current_char_ends_line = is_line_end(edit, current_pos);
		}
	} while (change_line(edit, current_pos, FORWARDS) && current_pos < end);
	return max_tabs;
}

static void stretch_tabstops(sptr_t edit, int block_start_linenum, int block_nof_lines, int max_tabs)
{
	int l, t;
	et_line* lines = (et_line*)_alloca(sizeof(et_line) * block_nof_lines);
	memset(lines, 0, sizeof(et_line) * block_nof_lines);

	int new_buffer_size = sizeof(et_tabstop) * __max(1, block_nof_lines * max_tabs);
	if (new_buffer_size > grid_buffer_size)
	{
		et_tabstop* new_buffer = (et_tabstop*)realloc(grid_buffer, new_buffer_size);
		if (new_buffer == NULL)
		{
			free(grid_buffer);
			return;
		}
		grid_buffer = new_buffer;
		grid_buffer_size = new_buffer_size;
	}
	memset(grid_buffer, 0, new_buffer_size);

	et_tabstop** grid = (et_tabstop**)_alloca(sizeof(et_tabstop*) * block_nof_lines);
	for (l = 0; l < block_nof_lines; l++)
	{
		grid[l] = grid_buffer + (l * max_tabs);
	}

	// get width of text in cells
	for (l = 0; l < block_nof_lines; l++) // for each line
	{
		int text_width_in_tab = 0;
		int current_line_num = block_start_linenum + l;
		int current_tab_num = 0;
		bool cell_empty = true;

		int current_pos = call_edit(edit, SCI_POSITIONFROMLINE, current_line_num);
		int cell_start = current_pos;
		unsigned char current_char = (unsigned char)call_edit(edit, SCI_GETCHARAT, current_pos);
		bool current_char_ends_line = is_line_end(edit, current_pos);
		// maybe change this to search forwards for tabs/newlines

		while (current_char != '\0')
		{
			if (current_char_ends_line)
			{
				grid[l][current_tab_num].ends_in_tab = false;
				text_width_in_tab = 0;
				break;
			}
			else if (current_char == '\t')
			{
				if (!cell_empty)
				{
					text_width_in_tab = get_text_width(edit, cell_start, current_pos);
				}
				grid[l][current_tab_num].ends_in_tab = true;
				grid[l][current_tab_num].text_width_pix = calc_tab_width(text_width_in_tab);
				current_tab_num++;
				lines[l].num_tabs++;
				text_width_in_tab = 0;
				cell_empty = true;
			}
			else
			{
				if (cell_empty)
				{
					cell_start = current_pos;
					cell_empty = false;
				}
			}
			current_pos = call_edit(edit, SCI_POSITIONAFTER, current_pos);
			current_char = (unsigned char)call_edit(edit, SCI_GETCHARAT, current_pos);
			current_char_ends_line = is_line_end(edit, current_pos);
		}
	}

	// find columns blocks and stretch to fit the widest cell
	for (t = 0; t < max_tabs; t++) // for each column
	{
		bool starting_new_block = true;
		int first_line_in_block = 0;
		int max_width = 0;
		for (l = 0; l < block_nof_lines; l++) // for each line
		{
			if (starting_new_block)
			{
				starting_new_block = false;
				first_line_in_block = l;
				max_width = 0;
			}
			if (grid[l][t].ends_in_tab)
			{
				grid[l][t].widest_width_pix = &(grid[first_line_in_block][t].text_width_pix); // point widestWidthPix at first 
				if (grid[l][t].text_width_pix > max_width)
				{
					max_width = grid[l][t].text_width_pix;
					grid[first_line_in_block][t].text_width_pix = max_width;
				}
			}
			else // end column block
			{
				starting_new_block = true;
			}
		}
	}

	// set tabstops
	for (l = 0; l < block_nof_lines; l++) // for each line
	{
		int current_line_num = block_start_linenum + l;
		int acc_tabstop = 0;

		call_edit(edit, SCI_CLEARTABSTOPS, current_line_num);

		for (t = 0; t < lines[l].num_tabs; t++)
		{
			if (grid[l][t].widest_width_pix != NULL)
			{
				acc_tabstop += *(grid[l][t].widest_width_pix);
				call_edit(edit, SCI_ADDTABSTOP, current_line_num, acc_tabstop);
			}
			else
			{
				break;
			}
		}
	}
}

void ElasticTabstops_OnModify(HWND sci, int start, int end)
{
	// Get the direction pointer and function. Not the cleanest but it works for now
	sptr_t edit = SendMessage(sci, SCI_GETDIRECTPOINTER, 0, 0);
	Scintilla_DirectFunction = (SciFnDirect)SendMessage(sci, SCI_GETDIRECTFUNCTION, 0, 0);

	// Only stretch tabs if it is using actual tab characters
	if (call_edit(edit, SCI_GETUSETABS) == 0) return;

	// Adjust widths based on character size
	const int char_width = call_edit(edit, SCI_TEXTWIDTH, STYLE_DEFAULT, (LONG_PTR)"A");
	tab_width_padding = char_width;
	tab_width_minimum = char_width * call_edit(edit, SCI_GETTABWIDTH) - tab_width_padding;

	int max_tabs_between = get_nof_tabs_between(edit, start, end);
	int max_tabs_backwards = get_block_boundary(edit, start, BACKWARDS);
	int max_tabs_forwards = get_block_boundary(edit, end, FORWARDS);
	int max_tabs = __max(__max(max_tabs_between, max_tabs_backwards), max_tabs_forwards);

	int block_start_linenum = call_edit(edit, SCI_LINEFROMPOSITION, start);
	int block_end_linenum = call_edit(edit, SCI_LINEFROMPOSITION, end);
	int block_nof_lines = (block_end_linenum - block_start_linenum) + 1;

	stretch_tabstops(edit, block_start_linenum, block_nof_lines, max_tabs);
}