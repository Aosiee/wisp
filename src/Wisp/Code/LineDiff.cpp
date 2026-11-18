#include "Wisp/Code/LineDiff.h"

#include <algorithm>

namespace wisp::code {

	std::vector<std::string_view> SplitLines(std::string_view text)
	{
		std::vector<std::string_view> lines;
		size_t start = 0;
		for (size_t i = 0; i <= text.size(); i++)
			if (i == text.size() || text[i] == '\n')
			{
				std::string_view line = text.substr(start, i - start);
				if (!line.empty() && line.back() == '\r')
					line.remove_suffix(1);
				lines.push_back(line);
				start = i + 1;
			}
		// A trailing newline ends the last line; it does not start another.
		if (!text.empty() && text.back() == '\n')
			lines.pop_back();
		return lines;
	}

	std::vector<LineChange> DiffLines(const std::vector<std::string_view>& base, const std::vector<std::string_view>& current, int maxEdits)
	{
		std::vector<LineChange> out;
		int prefix = 0;
		while (prefix < (int)base.size() && prefix < (int)current.size() && base[(size_t)prefix] == current[(size_t)prefix])
			prefix++;
		int suffix = 0;
		while (suffix < (int)base.size() - prefix && suffix < (int)current.size() - prefix
			&& base[base.size() - 1 - (size_t)suffix] == current[current.size() - 1 - (size_t)suffix])
			suffix++;
		const int n = (int)base.size() - prefix - suffix;
		const int m = (int)current.size() - prefix - suffix;
		if (n == 0 && m == 0)
			return out;
		auto a = [&](int i) { return base[(size_t)(prefix + i)]; };
		auto b = [&](int j) { return current[(size_t)(prefix + j)]; };

		// The edit script: 0 = equal, 1 = delete (base), 2 = insert (current), in order.
		std::vector<uint8_t> script;
		bool solved = false;
		if (n > 0 && m > 0)
		{
			const int max = std::min(n + m, maxEdits);
			std::vector<int> v((size_t)(2 * (n + m) + 3), 0);
			const int offset = n + m + 1;
			std::vector<std::vector<int>> trace; // v's window [-d-1, d+1] before step d
			int found = -1;
			for (int d = 0; d <= max && found < 0; d++)
			{
				trace.emplace_back(v.begin() + (offset - d - 1), v.begin() + (offset + d + 2));
				for (int k = -d; k <= d; k += 2)
				{
					int x = (k == -d || (k != d && v[(size_t)(offset + k - 1)] < v[(size_t)(offset + k + 1)])) ? v[(size_t)(offset + k + 1)]
						: v[(size_t)(offset + k - 1)] + 1;
					int y = x - k;
					while (x < n && y < m && a(x) == b(y))
						x++, y++;
					v[(size_t)(offset + k)] = x;
					if (x >= n && y >= m)
					{
						found = d;
						break;
					}
				}
			}
			if (found >= 0)
			{
				solved = true;
				int x = n, y = m;
				for (int d = found; d >= 0; d--)
				{
					const std::vector<int>& w = trace[(size_t)d];
					auto at = [&](int k) { return w[(size_t)(k + d + 1)]; };
					const int k = x - y;
					const int prevK = (k == -d || (k != d && at(k - 1) < at(k + 1))) ? k + 1 : k - 1;
					const int prevX = d == 0 ? 0 : at(prevK);
					const int prevY = prevX - prevK;
					while (x > prevX && y > prevY)
					{
						script.push_back(0);
						x--, y--;
					}
					if (d > 0)
						script.push_back(x == prevX ? 2 : 1);
					x = prevX;
					y = prevY;
				}
				std::reverse(script.begin(), script.end());
			}
		}
		if (!solved)
		{
			script.assign((size_t)n, 1);
			script.insert(script.end(), (size_t)m, 2);
		}

		int i = 0, j = 0;
		for (size_t s = 0; s < script.size();)
		{
			if (script[s] == 0)
			{
				i++, j++, s++;
				continue;
			}
			LineChange change;
			change.BaseLine = prefix + i;
			change.Line = prefix + j;
			while (s < script.size() && script[s] != 0)
			{
				if (script[s] == 1)
					change.BaseCount++, i++;
				else
					change.Count++, j++;
				s++;
			}
			change.Kind = change.BaseCount == 0 ? LineChangeKind::Added : change.Count == 0 ? LineChangeKind::Deleted : LineChangeKind::Modified;
			out.push_back(change);
		}
		return out;
	}

}
