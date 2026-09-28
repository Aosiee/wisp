#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include "Wisp/Components/GraphData.h"
#include "Wisp/Components/LineGraph.h"
#include <cmath>
#include <memory>
#include <string>

namespace wisp {

class FrameTimeGraph : public Widget
{
public:
	WISP_WIDGET_TYPE(FrameTimeGraph)

	FrameTimeGraph(FontCache* fc, FontHandle font, float fontSize)
		: m_FontCache(fc), m_Font(font), m_FontSize(fontSize)
	{
		m_Data = std::make_shared<GraphData>();
		m_Data->Series("frame_ms", 512);
		m_Data->Series("draw_calls", 512);

		m_Graph = std::make_unique<LineGraph>(fc, font, fontSize);
		SetupParentChild(this, m_Graph.get());
		m_Graph->SetData(m_Data);
		m_Graph->ShowSeries({"frame_ms"});
		m_Graph->SetVisibleWindow(300);
		m_Graph->AddBudgetLine(16.6f, {0.15f,0.7f,0.25f,1}, "60fps");
		m_Graph->AddBudgetLine(33.3f, {0.9f,0.65f,0.15f,1}, "30fps");
	}

	// Call each frame — or in the Sandbox, call from a timer
	void PushFrame(float dtMs, uint32_t drawCalls)
	{
		m_Data->Series("frame_ms").Push(dtMs);
		m_Data->Series("draw_calls").Push((float)drawCalls);
		m_FrameCount++;
	}

	void SetHighWaterline(float y) { m_Graph->SetHighWaterline(y, {0.9f,0.3f,0.3f,1}); }
	void SetLowWaterline(float y)  { m_Graph->SetLowWaterline(y, {0.3f,0.3f,0.9f,1}); }
	void SetHighWaterEnabled(bool e) { m_Graph->SetHighWaterEnabled(e); }
	void SetLowWaterEnabled(bool e)  { m_Graph->SetLowWaterEnabled(e); }
	void AddSpikeTag(uint32_t idx, const std::string& label) { m_Graph->AddSpikeTag(idx, label); }

	glm::vec2 ComputeDesiredSize() const override { return { 500, 200 }; }

	void ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const override
	{
		float gh = geom.LocalSize.y - 24;
		out.Add(m_Graph.get(), geom.MakeChild({0, 0}, {geom.LocalSize.x, gh}));
	}

	int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId, const WidgetStyleContext& style) const override
	{
		glm::vec2 pos = geom.LocalToAbsolute({0,0});
		float w = geom.LocalSize.x, h = geom.LocalSize.y;

		// Paint the graph
		ArrangedChildren arranged;
		ArrangeChildren(geom, arranged);
		int nextLayer = layerId;
		for (auto& c : arranged)
			nextLayer = c.WidgetPtr->OnPaint(args, c.Geom, drawList, nextLayer, style);

		// Readout text at bottom
		if (m_FontCache)
		{
			float latestMs = m_Data->Series("frame_ms").Latest();
			float fps = latestMs > 0 ? 1000.0f / latestMs : 0;
			char buf[128];
			snprintf(buf, sizeof(buf), "%.2f ms  |  %.0f fps  |  draw calls: %.0f  |  frames: %u",
				latestMs, fps, m_Data->Series("draw_calls").Latest(), m_FrameCount);
			auto ms = m_FontCache->MeasureText(m_Font, buf, m_FontSize * 0.65f);
			float tx = pos.x + 4, ty = pos.y + h - ms.y - 2;
			PaintText(drawList, nextLayer, {tx, ty}, buf, {0.7f,0.7f,0.75f,1}, args.InheritedClipIndex);
			nextLayer++;
		}
		return nextLayer;
	}

	void ForEachChild(std::function<void(Widget*)> fn) override { if (m_Graph) fn(m_Graph.get()); }

private:
	void PaintText(DrawList& dl, int layer, const glm::vec2& pos, const std::string& text,
		const glm::vec4& tint, int clip) const
	{
		if (!m_FontCache || text.empty()) return;
		float px = pos.x, py = pos.y;
		const uint8_t* p = (const uint8_t*)text.data();
		const uint8_t* e = p + text.size();
		while (p < e)
		{
			uint32_t cp;
			if((*p&0x80)==0){cp=*p;p++;}
			else if((*p&0xE0)==0xC0&&p+1<e){cp=((*p&0x1F)<<6)|(p[1]&0x3F);p+=2;}
			else if((*p&0xF0)==0xE0&&p+2<e){cp=((*p&0x0F)<<12)|((p[1]&0x3F)<<6)|(p[2]&0x3F);p+=3;}
			else{p++;continue;}
			const GlyphInfo* gi = m_FontCache->GetGlyph(m_Font, cp);
			if(gi)
			{
				glm::vec2 uv[4]={{gi->AtlasU0,gi->AtlasV0},{gi->AtlasU1,gi->AtlasV0},{gi->AtlasU1,gi->AtlasV1},{gi->AtlasU0,gi->AtlasV1}};
				dl.AddText(layer, {px, py + gi->BearingY}, std::string(1,(char)cp), uv, gi->AdvanceX, gi->Height, tint, clip);
				px += gi->AdvanceX;
			}
		}
	}

	FontCache* m_FontCache;
	FontHandle m_Font;
	float m_FontSize;
	std::shared_ptr<GraphData> m_Data;
	std::unique_ptr<LineGraph> m_Graph;
	mutable uint32_t m_FrameCount = 0;
};

using SFrameTimeGraph = FrameTimeGraph;

} // namespace wisp
