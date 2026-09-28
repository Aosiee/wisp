#pragma once

#include <cmath>
#include <functional>

namespace wisp {

enum class Easing
{
	Linear,
	QuadEaseIn,
	QuadEaseOut,
	QuadEaseInOut,
	CubicEaseIn,
	CubicEaseOut,
	CubicEaseInOut,
};

inline float ApplyEasing(float t, Easing e)
{
	if (t <= 0.0f) return 0.0f;
	if (t >= 1.0f) return 1.0f;

	switch (e)
	{
	case Easing::Linear:
		return t;
	case Easing::QuadEaseIn:
		return t * t;
	case Easing::QuadEaseOut:
		return t * (2.0f - t);
	case Easing::QuadEaseInOut:
		return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
	case Easing::CubicEaseIn:
		return t * t * t;
	case Easing::CubicEaseOut:
	{
		float t1 = t - 1.0f;
		return t1 * t1 * t1 + 1.0f;
	}
	case Easing::CubicEaseInOut:
		return t < 0.5f ? 4.0f * t * t * t : (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f;
	}
	return t;
}

class CurveSequence
{
public:
	void Play(float duration, Easing easing = Easing::QuadEaseOut)
	{
		m_Duration = duration;
		m_Easing = easing;
		m_Elapsed = 0.0f;
		m_Playing = true;
		m_Forward = true;
	}

	void Reverse()
	{
		m_Playing = true;
		m_Forward = !m_Forward;
	}

	bool IsPlaying() const { return m_Playing; }

	float GetLerp() const
	{
		if (m_Duration <= 0.0f) return m_Forward ? 1.0f : 0.0f;
		float raw = m_Elapsed / m_Duration;
		if (raw >= 1.0f) raw = 1.0f;
		float eased = ApplyEasing(raw, m_Easing);
		return m_Forward ? eased : 1.0f - eased;
	}

	void Tick(float dt)
	{
		if (!m_Playing) return;
		m_Elapsed += dt;
		if (m_Elapsed >= m_Duration)
		{
			m_Elapsed = m_Duration;
			m_Playing = false;
		}
	}

private:
	float  m_Elapsed = 0.0f;
	float  m_Duration = 0.0f;
	bool   m_Playing = false;
	bool   m_Forward = true;
	Easing m_Easing = Easing::QuadEaseOut;
};

} // namespace wisp
