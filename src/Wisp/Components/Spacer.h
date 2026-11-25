#pragma once
#include "Wisp/Core.h"
#include "Wisp/DrawList.h"

namespace wisp {

// ---- Spacer -----------------------------------------------------------------

class Spacer : public Widget
{
public:
	WISP_WIDGET_TYPE(Spacer)

	Spacer(const glm::vec2& size) : m_Size(size) {}

	void SetSize(const glm::vec2& size) { if (size != m_Size) { m_Size = size; InvalidateLayout(); } }
	const glm::vec2& GetSize() const { return m_Size; }

	glm::vec2 ComputeDesiredSize() const override { return m_Size; }
	void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}
	int OnPaint(const PaintArgs&, const Geometry&, DrawList&, int layerId, const WidgetStyleContext&) const override { return layerId; }

private:
	glm::vec2 m_Size;
};


} // namespace wisp
