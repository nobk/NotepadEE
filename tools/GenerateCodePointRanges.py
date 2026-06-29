# Script to generate WHITESPACE_CASES and PUNCTUATION_CASES macros for Cntchr.cpp
# Run from the repo root:
#   python tools/GenerateCodePointRanges.py

import os
import sys
import unicodedata
from datetime import datetime

# Add scintilla/scripts to path for FileGenerator
scriptDir = os.path.dirname(os.path.abspath(__file__))
scintillaScripts = os.path.join(scriptDir, '..', 'scintilla', 'scripts')
sys.path.insert(0, os.path.abspath(scintillaScripts))

from FileGenerator import Regenerate


def collect_codepoints() -> tuple:
	"""Collect whitespace and punctuation codepoints matching the original script logic."""
	whitespace_categories = {'Zs', 'Zl', 'Zp', 'Cc'}
	punctuation_categories = {'Pc', 'Pd', 'Ps', 'Pe', 'Pi', 'Pf', 'Po', 'Sm'}

	whitespace_codepoints = []
	punctuation_codepoints = []

	for i in range(0, 0x110000):
		try:
			char = chr(i)
			category = unicodedata.category(char)
			if category in whitespace_categories:
				whitespace_codepoints.append(i)
			elif category in punctuation_categories:
				punctuation_codepoints.append(i)
		except ValueError:
			continue

	return whitespace_codepoints, punctuation_codepoints


def merge_consecutive(codepoints):
	"""Merge only 3+ consecutive codepoints into ranges (Rg)."""
	if not codepoints:
		return []

	codepoints.sort()

	ranges = []
	start = codepoints[0]
	end = codepoints[0]

	for i in range(1, len(codepoints)):
		if codepoints[i] == end + 1:
			end = codepoints[i]
		else:
			# Only merge 3 or more consecutive into a range
			if end - start >= 2:
				ranges.append((start, end))   # range
			else:
				# Output each separately
				for j in range(start, end + 1):
					ranges.append((j, None))
			start = codepoints[i]
			end = codepoints[i]

	# Handle the last segment
	if end - start >= 2:
		ranges.append((start, end))
	else:
		for j in range(start, end + 1):
			ranges.append((j, None))

	return ranges


def generate_switch_code(codepoints, func_name, line_width=102):
	"""Generate the C++ macro definition matching the original script format."""
	nlen = len(codepoints)

	# Merge consecutive ranges
	ranges = merge_consecutive(codepoints)

	# Build output lines
	case_lines = []
	current_line = "    "

	for start, end in ranges:
		if end is None:
			value = f"0x{start:02X}"
		else:
			value = f"Rg(0x{start:02X}, 0x{end:02X})"

		# Check if adding this value would exceed line width
		if len(current_line) + len(value) + 2 > line_width:
			case_lines.append(current_line + "\\\n")
			current_line = "    "

		current_line += value + ", "

	# Add the last line
	if current_line != "    ":
		case_lines.append(current_line.rstrip(', ') + ", \\\n")

	# Replace trailing ',\' on the last line with the comment
	if case_lines:
		case_lines[-1] = case_lines[-1].rstrip(', \\\n') + f" // {nlen} total {func_name} code points"

	uset_code = f"#define {func_name.upper()}_CASES \\\n{''.join(case_lines)}"
	return uset_code


def generate() -> None:
	cntchr_path = os.path.abspath(
		os.path.join(scriptDir, '..', 'src', 'Cntchr.cpp')
	)

	# Import platform here (not at top, matching original script layout)
	import platform

	print(f'Collecting Unicode data (Python {platform.python_version()}, Unicode {unicodedata.unidata_version})...')

	whitespace_cps, punctuation_cps = collect_codepoints()

	print(f'  Whitespace:  {len(whitespace_cps)} codepoints')
	print(f'  Punctuation: {len(punctuation_cps)} codepoints')

	# Generate macro code
	ws_code = generate_switch_code(whitespace_cps, "whitespace")
	pu_code = generate_switch_code(punctuation_cps, "punctuation")

	# Build output: version info + whitespace macro + punctuation macro (flattened to lines)
	output = [
		f'// Generated: Python {platform.python_version()}, 'f'Unicode {unicodedata.unidata_version}',
	]
	output.extend(ws_code.split('\r\n'))
	output.extend(pu_code.split('\r\n'))

	Regenerate(cntchr_path, '//', output)
	print('Done.')


if __name__ == '__main__':
	generate()
