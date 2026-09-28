#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace wisp {

class GraphSeries
{
public:
	explicit GraphSeries(uint32_t capacity = 512)
		: m_Capacity(capacity), m_Buffer(capacity, 0.0f), m_Head(0), m_Count(0) {}

	void Push(float value)
	{
		m_Buffer[m_Head] = value;
		m_Head = (m_Head + 1) % m_Capacity;
		if (m_Count < m_Capacity) m_Count++;
	}

	void Clear() { m_Count = 0; m_Head = 0; }

	float Latest() const
	{
		if (m_Count == 0) return 0.0f;
		uint32_t idx = (m_Head == 0) ? m_Capacity - 1 : m_Head - 1;
		return m_Buffer[idx];
	}

	float Sample(uint32_t i) const
	{
		// 0 = oldest, m_Count-1 = newest
		if (i >= m_Count) return 0.0f;
		uint32_t oldestIdx = (m_Count < m_Capacity) ? 0 : m_Head;
		return m_Buffer[(oldestIdx + i) % m_Capacity];
	}

	uint32_t Count() const { return m_Count; }

	struct Stats { float Min, Max, Avg; };

	Stats ComputeStats(uint32_t lastN) const
	{
		Stats s = { 0, 0, 0 };
		if (m_Count == 0) return s;
		uint32_t n = lastN > 0 ? (lastN < m_Count ? lastN : m_Count) : m_Count;
		float sum = 0;
		uint32_t start = m_Count - n;
		for (uint32_t i = 0; i < n; i++)
		{
			float v = Sample(start + i);
			if (i == 0) { s.Min = v; s.Max = v; }
			else { if (v < s.Min) s.Min = v; if (v > s.Max) s.Max = v; }
			sum += v;
		}
		s.Avg = sum / (float)n;
		return s;
	}

	template<typename Fn>
	void ForEachSample(uint32_t lastN, Fn fn) const
	{
		uint32_t n = lastN > 0 ? (lastN < m_Count ? lastN : m_Count) : m_Count;
		uint32_t start = m_Count - n;
		for (uint32_t i = 0; i < n; i++)
			fn(i, Sample(start + i));
	}

private:
	uint32_t m_Capacity;
	std::vector<float> m_Buffer;
	uint32_t m_Head;
	uint32_t m_Count;
};

class GraphData
{
public:
	GraphSeries& Series(const std::string& name, uint32_t capacity = 512)
	{
		auto it = m_Series.find(name);
		if (it != m_Series.end()) return *it->second;
		auto s = std::make_unique<GraphSeries>(capacity);
		GraphSeries* ptr = s.get();
		m_Series[name] = std::move(s);
		return *ptr;
	}

	GraphSeries* Find(const std::string& name)
	{
		auto it = m_Series.find(name);
		return it != m_Series.end() ? it->second.get() : nullptr;
	}

	void RemoveSeries(const std::string& name) { m_Series.erase(name); }

	template<typename Fn>
	void ForEachSeries(Fn fn) const
	{
		for (auto& [name, s] : m_Series)
			fn(name, *s);
	}

private:
	std::unordered_map<std::string, std::unique_ptr<GraphSeries>> m_Series;
};

} // namespace wisp
