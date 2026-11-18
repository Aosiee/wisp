#pragma once

// A line-level diff for the code editor's
// change markers - the hunks that turn a base text (the version control's)
// into the current one. Myers' O(ND) algorithm over the lines between the
// common prefix and suffix; past `maxEdits` differing lines the middle is
// one modified hunk (a rewrite reads as one). Language-free and headless.

#include <cstdint>
#include <string_view>
#include <vector>

namespace wisp::code {

	enum class LineChangeKind : uint8_t { Added, Modified, Deleted };

	struct LineChange
	{
		LineChangeKind Kind = LineChangeKind::Modified;
		int Line = 0;      // the first current line (Deleted: the line the removal sits above; the line count at the end)
		int Count = 0;     // current lines (0 when Deleted)
		int BaseLine = 0;  // the first base line
		int BaseCount = 0; // base lines (0 when Added)
	};

	std::vector<std::string_view> SplitLines(std::string_view text);
	std::vector<LineChange> DiffLines(const std::vector<std::string_view>& base, const std::vector<std::string_view>& current, int maxEdits = 2000);

}
