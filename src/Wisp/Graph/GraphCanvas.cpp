// The graph canvas implementation.

#include "tepch.h"
#include "Wisp/Graph/GraphCanvas.h"

#include "TinyEngine/Input/Input.h"
#include "TinyEngine/Input/KeyCodes.h"
#include "Wisp/Application.h"
#include "Wisp/Components/Border.h"
#include "Wisp/Components/SizeBox.h"
#include "Wisp/Components/TextBlock.h"

#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "Wisp/StyleTokens.h"

namespace wisp {
namespace graph {

	namespace {

		constexpr int kZoomLevelMin = 0;
		constexpr int kZoomLevelMax = 14;
		// The click-vs-drag threshold. The Wisp events are the PHYSICAL
		// pixels (the HiDPI framebuffer coords), so this is larger than
		// the logical-pixel idiom: a natural click micro-motion must not
		// turn a right-click into a pan (the no-context-menu bug) or a
		// double-click into a drag.
		constexpr float kPanDragThresholdPx = 8.0f;
		constexpr float kDoubleClickSeconds = 0.35;
		constexpr int kMaxEdgeSamples = 64;

		const glm::vec4 kEdgeOutgoing = { 0.85f, 0.62f, 0.25f, 0.9f };
		const glm::vec4 kEdgeIncoming = { 0.45f, 0.6f, 0.8f, 0.85f };
		const glm::vec4 kEdgeBack = { 0.38f, 0.38f, 0.44f, 0.8f };
		const glm::vec4 kEdgeHover = { 1.0f, 0.9f, 0.5f, 1.0f };

		glm::vec2 BezierPoint(glm::vec2 p0, glm::vec2 p1, glm::vec2 p2, glm::vec2 p3, float t)
		{
			const float u = 1.0f - t;
			return u * u * u * p0 + 3.0f * u * u * t * p1 + 3.0f * u * t * t * p2 + t * t * t * p3;
		}

		// The title/subtitle truncated with "..." to fit the width (no
		// per-node clip needed - the 500-node paint budget).
		std::string TruncateToFit(FontCache* fc, FontHandle font, float size, const std::string& text, float maxWidth)
		{
			if (text.empty() || fc->MeasureText(font, text, size).x <= maxWidth)
				return text;
			const float ellipsis = fc->MeasureText(font, "...", size).x;
			std::string out = text;
			while (!out.empty() && fc->MeasureText(font, out, size).x + ellipsis > maxWidth)
				out.pop_back();
			out += "...";
			return out;
		}

	} // namespace

	LodTier LodTierFor(int zoomLevel)
	{
		const float scale = ZoomScale(zoomLevel);
		if (scale >= 0.5f) return LodTier::Full;
		if (scale >= 0.25f) return LodTier::TitleOnly;
		return LodTier::Boxes;
	}

	glm::vec2 ZoomAnchorPan(const ViewTransform& transform, const glm::vec2& cursorLocal, int newLevel)
	{
		const glm::vec2 world = (cursorLocal - transform.Pan) / ZoomScale(transform.ZoomLevel);
		return cursorLocal - world * ZoomScale(newLevel);
	}

	int EdgeSampleCount(float screenLength)
	{
		return std::clamp((int)std::ceil(screenLength / 6.0f), 8, kMaxEdgeSamples);
	}

	ViewTransform FrameTransform(const GraphView& view, const std::vector<NodeId>& subset,
		const glm::vec2& viewportSize)
	{
		glm::vec2 min = { 1e18f, 1e18f }, max = { -1e18f, -1e18f };
		const bool useSubset = !subset.empty();
		std::unordered_set<NodeId> subsetSet(subset.begin(), subset.end());
		for (const ViewNode& node : view.Nodes)
		{
			if (useSubset && !subsetSet.count(node.Id))
				continue;
			min = glm::min(min, node.Position);
			max = glm::max(max, node.Position + node.Size);
		}
		if (min.x > max.x || min.y > max.y)
			return { glm::vec2(0.0f), kDefaultZoomLevel };

		const glm::vec2 size = max - min;
		const glm::vec2 centre = (min + max) * 0.5f;
		const float fit = std::min(viewportSize.x / std::max(1.0f, size.x),
			viewportSize.y / std::max(1.0f, size.y)) * 0.9f;

		// The largest zoom level whose scale still fits.
		int level = kZoomLevelMax;
		while (level > kZoomLevelMin && ZoomScale(level) > fit)
			level--;
		const float scale = ZoomScale(level);
		return { viewportSize * 0.5f - centre * scale, level };
	}

	GraphCanvas::GraphCanvas(FontCache* fontCache, FontHandle font, GraphSchema* schema,
		const std::string& viewStateKey)
		: m_FontCache(fontCache), m_Font(font), m_Schema(schema), m_ViewStateKey(viewStateKey)
	{
		SetSupportsFocus(true);
		SetClipping(Clipping::ClipToBounds);
		LoadViewState();
		// The dwell tooltip: the factory reads the hovered node (tracked
		// per paint - the profiler-panel pattern).
		SetToolTip([this]() -> std::unique_ptr<Widget> {
			if (!m_HasHover || !m_Schema)
				return nullptr;
			const std::string text = m_Schema->TooltipText(m_HoveredNode);
			if (text.empty())
				return nullptr;
			auto block = std::make_unique<wisp::TextBlock>(m_FontCache, m_Font, m_BaseTitleSize,
				text, wisp::tokens::TextPrimary);
			block->SetWrapMode(wisp::WrapMode::Word);
			block->SetAutoWrap(true);
			auto border = std::make_unique<wisp::Border>();
			border->SetBrush({ wisp::tokens::Popup });
			border->SetCornerRadii(wisp::tokens::RadiusMedium + 1.0f);
			border->SetOutline(1.0f, wisp::tokens::BorderStrong);
			border->SetPadding(8.0f, 6.0f);
			border->SetChild(std::move(block));
			auto sized = std::make_unique<wisp::SizeBox>();
			sized->SetWidthOverride(420.0f);
			sized->SetChild(std::move(border));
			return sized;
		});
	}

	void GraphCanvas::Rebuild()
	{
		// The stale view ids must not survive the rebuild (the tree-mode
		// occurrence ids are per-layout) - a re-matched id would
		// silently re-select or re-highlight a different node.
		m_Selected.clear();
		m_LastPolledMouse = glm::vec2(-1e9f, -1e9f);
		if (m_Schema && m_HasCachedGeometry)
		{
			// The eager path: once the canvas has painted (the geometry is
			// known), the build runs NOW so a caller's follow-up FrameAll
			// frames the FRESH view (the recenter flow).
			DoBuild();
		}
		else
			m_NeedsRebuild = true;
	}

	void GraphCanvas::DoBuild() const
	{
		m_Schema->Build(m_View, m_NodeToAsset);
		m_ViewGeneration++;
		m_EdgeCacheValid = false;
		m_NodeIndex.clear();
		for (size_t i = 0; i < m_View.Nodes.size(); i++)
			m_NodeIndex[m_View.Nodes[i].Id] = i;
		m_TitleCache.clear();
		m_SubtitleCache.clear();
	}

	void GraphCanvas::FrameAll()
	{
		if (!m_HasCachedGeometry)
			return;
		const std::vector<NodeId> empty;
		m_Transform = FrameTransform(m_View, empty, m_Size);
		m_LastPolledMouse = glm::vec2(-1e9f, -1e9f);
		SaveViewState();
	}

	void GraphCanvas::FrameSelection()
	{
		if (!m_HasCachedGeometry)
			return;
		std::vector<NodeId> selected(m_Selected.begin(), m_Selected.end());
		m_Transform = FrameTransform(m_View, selected, m_Size);
		m_LastPolledMouse = glm::vec2(-1e9f, -1e9f);
		SaveViewState();
	}

	int GraphCanvas::OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& dl,
		int layerId, const WidgetStyleContext&) const
	{
		TE_PROFILE_SCOPE_C("GraphCanvas - Paint", (uint32_t)TinyEngine::ProfilerChannel::Cpu);

		m_Origin = geom.LocalToAbsolute({ 0, 0 });
		m_Size = geom.LocalSize;
		m_HasCachedGeometry = true;
		m_DpiScale = args.DpiScale;

		if (m_NeedsRebuild && m_Schema)
		{
			DoBuild();
			m_NeedsRebuild = false;
			if (m_FirstBuild)
			{
				m_FirstBuild = false;
				const std::vector<NodeId> empty;
				m_Transform = FrameTransform(m_View, empty, m_Size);
			}
		}

		dl.AddBox(layerId, m_Origin, m_Size, wisp::tokens::Panel, args.InheritedClipIndex);
		const int bodyClip = dl.PushClip({ m_Origin.x, m_Origin.y, m_Size.x, m_Size.y }, args.InheritedClipIndex);

		// The visible world rect (the culling window).
		const glm::vec2 worldMin = ScreenToWorld(m_Origin);
		const glm::vec2 worldMax = ScreenToWorld(m_Origin + m_Size);
		const glm::vec4 worldClip = { std::min(worldMin.x, worldMax.x), std::min(worldMin.y, worldMax.y),
			std::abs(worldMax.x - worldMin.x), std::abs(worldMax.y - worldMin.y) };

		if (m_DrawDotGrid)
			PaintDotGrid(dl, layerId + 1, bodyClip, worldClip);
		PaintEdges(dl, layerId + 1, bodyClip, worldClip, m_Size);
		PaintNodes(dl, layerId + 2, bodyClip, worldClip);
		// The arrowheads sit ABOVE the nodes (the tip lands on the card's
		// edge - under the nodes it would be occluded).
		PaintArrowheads(dl, layerId + 3, bodyClip, m_Size);
		PaintMarquee(dl, layerId + 4, bodyClip);

		UpdateHover();
		return layerId + 5;
	}

	Reply GraphCanvas::OnMouseWheel(const glm::vec2& absPos, float delta)
	{
		if (!m_HasCachedGeometry)
			return Reply::Unhandled();
		const int newLevel = std::clamp(m_Transform.ZoomLevel + (delta > 0.0f ? 1 : -1),
			kZoomLevelMin, kZoomLevelMax);
		if (newLevel == m_Transform.ZoomLevel)
			return Reply::Handled_();
		m_Transform.Pan = ZoomAnchorPan(m_Transform, absPos - m_Origin, newLevel);
		m_Transform.ZoomLevel = newLevel;
		// The hover must re-resolve against the new view (the mouse is
		// parked during the wheel zoom).
		m_LastPolledMouse = glm::vec2(-1e9f, -1e9f);
		SaveViewState();
		return Reply::Handled_();
	}

	Reply GraphCanvas::OnMouseButtonDown(const glm::vec2& absPos, int button)
	{
		if (!m_HasCachedGeometry)
			return Reply::Unhandled();

		if (button == 2)
		{
			BeginInteraction(Interaction::PanMiddle, absPos);
			return Reply::Handled_().CaptureMouse().SetFocus();
		}
		if (button == 1)
		{
			BeginInteraction(Interaction::PanRightPending, absPos);
			return Reply::Handled_().CaptureMouse().SetFocus();
		}
		if (button != 0)
			return Reply::Unhandled();

		const glm::vec2 local = absPos - m_Origin;
		const HitResult hit = HitTest(m_View, m_Transform, local);
		if (hit.Kind == HitResult::Kind::Node)
		{
			const bool ctrl = TinyEngine::Input::IsKeyPressed(TE_KEY_LEFT_CONTROL)
				|| TinyEngine::Input::IsKeyPressed(TE_KEY_RIGHT_CONTROL);
			if (ctrl && m_Selected.count(hit.Node))
				m_Selected.erase(hit.Node);
			else
			{
				if (!ctrl)
					m_Selected.clear();
				m_Selected.insert(hit.Node);
			}
			m_InteractionNode = hit.Node;
			if (m_Schema && m_Schema->NodesMovable())
			{
				m_DragStart.clear();
				for (NodeId id : m_Selected)
				{
					for (const ViewNode& node : m_View.Nodes)
						if (node.Id == id)
						{
							m_DragStart.push_back({ id, node.Position });
							break;
						}
				}
				BeginInteraction(Interaction::NodeDrag, absPos);
			}
			else
			{
				BeginInteraction(Interaction::None, absPos);
			}
			return Reply::Handled_().CaptureMouse().SetFocus();
		}

		// The empty canvas (or an edge): the marquee.
		BeginInteraction(Interaction::Marquee, absPos);
		return Reply::Handled_().CaptureMouse().SetFocus();
	}

	Reply GraphCanvas::OnMouseMove(const glm::vec2& absPos)
	{
		if (m_Interaction == Interaction::PanMiddle)
		{
			m_Transform.Pan += absPos - m_AnchorScreen;
			m_AnchorScreen = absPos;
			return Reply::Handled_();
		}
		if (m_Interaction == Interaction::PanRightPending)
		{
			if (glm::length(absPos - m_AnchorScreen) >= kPanDragThresholdPx)
			{
				m_Interaction = Interaction::PanRightDrag;
				m_Transform.Pan += absPos - m_AnchorScreen;
				m_AnchorScreen = absPos;
			}
			return Reply::Handled_();
		}
		if (m_Interaction == Interaction::PanRightDrag)
		{
			m_Transform.Pan += absPos - m_AnchorScreen;
			m_AnchorScreen = absPos;
			return Reply::Handled_();
		}
		if (m_Interaction == Interaction::NodeDrag)
		{
			if (glm::length(absPos - m_AnchorScreen) >= kPanDragThresholdPx)
				m_MovedPastThreshold = true;
			const glm::vec2 delta = (absPos - m_Origin - m_Anchor) / ZoomScale(m_Transform.ZoomLevel);
			for (const auto& [id, start] : m_DragStart)
			{
				for (ViewNode& node : m_View.Nodes)
					if (node.Id == id)
						node.Position = start + delta;
			}
			// The moved nodes change the edge geometry - the cached
			// screen polylines must re-tessellate.
			m_EdgeCacheValid = false;
			m_LastMouse = absPos;
			return Reply::Handled_();
		}
		if (m_Interaction == Interaction::Marquee)
		{
			if (glm::length(absPos - m_AnchorScreen) >= kPanDragThresholdPx)
				m_MovedPastThreshold = true;
			m_LastMouse = absPos;
			return Reply::Handled_();
		}
		return Reply::Unhandled();
	}

	Reply GraphCanvas::OnMouseButtonUp(const glm::vec2& absPos, int button)
	{
		if (button == 2 && m_Interaction == Interaction::PanMiddle)
		{
			EndInteraction();
			SaveViewState();
			return Reply::Handled_().ReleaseCapture();
		}
		if (button == 1)
		{
			if (m_Interaction == Interaction::PanRightPending)
			{
				const HitResult hit = HitTest(m_View, m_Transform, absPos - m_Origin);
				if (hit.Kind == HitResult::Kind::Node)
					OpenContextMenu(hit.Node, absPos);
			}
			if (m_Interaction == Interaction::PanRightDrag)
				SaveViewState();
			EndInteraction();
			return Reply::Handled_().ReleaseCapture();
		}
		if (button == 0)
		{
			if (m_Interaction == Interaction::Marquee)
			{
				if (m_MovedPastThreshold)
				{
					const bool ctrl = TinyEngine::Input::IsKeyPressed(TE_KEY_LEFT_CONTROL)
						|| TinyEngine::Input::IsKeyPressed(TE_KEY_RIGHT_CONTROL);
					std::vector<NodeId> hitNodes;
					const glm::vec2 a = ScreenToWorld(m_AnchorScreen);
					const glm::vec2 b = ScreenToWorld(m_LastMouse);
					const glm::vec2 mn = glm::min(a, b), mx = glm::max(a, b);
					for (const ViewNode& node : m_View.Nodes)
					{
						if (node.Position.x + node.Size.x < mn.x || node.Position.x > mx.x
							|| node.Position.y + node.Size.y < mn.y || node.Position.y > mx.y)
							continue;
						hitNodes.push_back(node.Id);
					}
					SetSelection(hitNodes, ctrl);
				}
				else
				{
					m_Selected.clear();
				}
			}
			else if (m_Interaction == Interaction::NodeDrag || m_Interaction == Interaction::None)
			{
				if (!m_MovedPastThreshold && m_InteractionNode != 0)
				{
					// The plain click vs the double-click.
					const double now = std::chrono::duration<double>(
						std::chrono::steady_clock::now().time_since_epoch()).count();
					if (m_LastClickNode == m_InteractionNode && now - m_LastClickTime < kDoubleClickSeconds)
					{
						if (m_Schema)
							m_Schema->OnActivate(m_InteractionNode);
						m_LastClickTime = -1e9;
						m_LastClickNode = 0;
					}
					else
					{
						if (m_Schema)
							m_Schema->OnClick(m_InteractionNode);
						m_LastClickTime = now;
						m_LastClickNode = m_InteractionNode;
					}
				}
				if (m_Interaction == Interaction::NodeDrag)
					SaveViewState();
			}
			EndInteraction();
			return Reply::Handled_().ReleaseCapture();
		}
		return Reply::Unhandled();
	}

	Reply GraphCanvas::OnKeyDown(int key, int mods)
	{
		(void)mods;
		if (key == TE_KEY_HOME)
		{
			FrameAll();
			return Reply::Handled_();
		}
		if (key == TE_KEY_F)
		{
			FrameSelection();
			return Reply::Handled_();
		}
		return Reply::Unhandled();
	}

	bool GraphCanvas::NodeVisible(const ViewNode& node, const glm::vec4& worldClip) const
	{
		return node.Position.x + node.Size.x >= worldClip.x && node.Position.x <= worldClip.x + worldClip.z
			&& node.Position.y + node.Size.y >= worldClip.y && node.Position.y <= worldClip.y + worldClip.w;
	}

	void GraphCanvas::PaintDotGrid(DrawList& dl, int layer, int clip, const glm::vec4& worldClip) const
	{
		const float scale = ZoomScale(m_Transform.ZoomLevel);
		constexpr float kGridSpacing = 32.0f;
		const float px = kGridSpacing * scale;
		if (px < 12.0f)
			return;
		const float x0 = std::floor(worldClip.x / kGridSpacing) * kGridSpacing;
		const float y0 = std::floor(worldClip.y / kGridSpacing) * kGridSpacing;
		const glm::vec4 dot = { 0.14f, 0.14f, 0.2f, 0.6f };
		for (float wx = x0; wx <= worldClip.x + worldClip.z; wx += kGridSpacing)
			for (float wy = y0; wy <= worldClip.y + worldClip.w; wy += kGridSpacing)
				dl.AddBox(layer, WorldToScreen({ wx, wy }) - glm::vec2(1.0f), { 2.0f, 2.0f }, dot, clip);
	}

	bool GraphCanvas::EdgeScreenPoints(size_t edgeIndex,
		glm::vec2& s0, glm::vec2& s1, glm::vec2& s2, glm::vec2& s3) const
	{
		if (edgeIndex >= m_View.Edges.size())
			return false;
		const ViewEdge& edge = m_View.Edges[edgeIndex];
		if (!m_NodeIndex.count(edge.From) || !m_NodeIndex.count(edge.To))
			return false;

		glm::vec2 p0, p1, p2, p3;
		EdgeControlPoints(m_View, edgeIndex, p0, p1, p2, p3);
		s0 = WorldToScreen(p0);
		s1 = WorldToScreen(p1);
		s2 = WorldToScreen(p2);
		s3 = WorldToScreen(p3);
		return true;
	}

	void GraphCanvas::PaintEdges(DrawList& dl, int layer, int clip, const glm::vec4& worldClip,
		const glm::vec2& viewport) const
	{
		(void)worldClip;
		const float scale = ZoomScale(m_Transform.ZoomLevel);
		const glm::vec4 clipRect = { m_Origin.x, m_Origin.y, viewport.x, viewport.y };
		const float thickness = std::clamp(1.5f * scale, 1.0f, 3.0f);

		// The re-tessellation: only when the transform moved (or the view
		// rebuilt) - the static frames reuse the cached screen polylines.
		if (!m_EdgeCacheValid || m_EdgeCacheTransform.ZoomLevel != m_Transform.ZoomLevel
			|| m_EdgeCacheTransform.Pan != m_Transform.Pan)
		{
			m_EdgeCacheTransform = m_Transform;
			m_EdgeCacheValid = true;
			m_EdgePoints.resize(m_View.Edges.size());
			for (size_t i = 0; i < m_View.Edges.size(); i++)
			{
				const ViewEdge& edge = m_View.Edges[i];
				glm::vec2 s0, s1, s2, s3;
				if (!EdgeScreenPoints(i, s0, s1, s2, s3))
				{
					m_EdgePoints[i].clear();
					continue;
				}
				if (edge.Flags & EdgeFlags::BackEdge)
				{
					const glm::vec2 dir = glm::normalize(s3 - s0 + glm::vec2(1e-4f));
					const glm::vec2 perp = { -dir.y, dir.x };
					const float bow = 14.0f * scale + 6.0f;
					s1 += perp * bow;
					s2 += perp * bow;
				}
				const int samples = EdgeSampleCount(glm::length(s3 - s0));
				std::vector<glm::vec2>& points = m_EdgePoints[i];
				points.resize((size_t)samples + 1);
				for (int s = 0; s <= samples; s++)
					points[(size_t)s] = BezierPoint(s0, s1, s2, s3, (float)s / (float)samples);
			}
		}

		for (size_t i = 0; i < m_View.Edges.size(); i++)
		{
			if (i >= m_EdgePoints.size() || m_EdgePoints[i].empty())
				continue;
			const ViewEdge& edge = m_View.Edges[i];
			const std::vector<glm::vec2>& points = m_EdgePoints[i];
			const glm::vec2 s0 = points.front();
			const glm::vec2 s3 = points.back();

			// The cull: the endpoint bounding box (plus the back-edge bow
			// margin) against the clip rect.
			const float margin = 24.0f;
			if (std::max(s0.x, s3.x) + margin < clipRect.x || std::min(s0.x, s3.x) - margin > clipRect.x + clipRect.z
				|| std::max(s0.y, s3.y) + margin < clipRect.y || std::min(s0.y, s3.y) - margin > clipRect.y + clipRect.w)
				continue;

			const bool hoverIncident = m_HasHover && (edge.From == m_HoveredNode || edge.To == m_HoveredNode);
			glm::vec4 colour;
			if (hoverIncident)
				colour = kEdgeHover;
			else if (edge.Flags & EdgeFlags::BackEdge)
				colour = kEdgeBack;
			else
				colour = (edge.Flags & EdgeFlags::Outgoing) ? kEdgeOutgoing : kEdgeIncoming;

			const LineStyle stroke = (edge.Kind == EdgeKind::Soft) ? LineStyle::Dashed : LineStyle::Solid;
			dl.AddPolyline(layer, points, hoverIncident ? thickness + 0.8f : thickness, colour, clip, stroke);
		}
	}

	void GraphCanvas::PaintArrowheads(DrawList& dl, int layer, int clip, const glm::vec2& viewport) const
	{
		const float scale = ZoomScale(m_Transform.ZoomLevel);
		const glm::vec4 clipRect = { m_Origin.x, m_Origin.y, viewport.x, viewport.y };
		for (size_t i = 0; i < m_View.Edges.size(); i++)
		{
			if (i >= m_EdgePoints.size() || m_EdgePoints[i].size() < 2)
				continue;
			const ViewEdge& edge = m_View.Edges[i];
			if (edge.Kind != EdgeKind::Hard)
				continue;
			const std::vector<glm::vec2>& points = m_EdgePoints[i];
			const glm::vec2 s3 = points.back();

			// The cull (the same endpoint window as the edges).
			const glm::vec2 s0 = points.front();
			const float margin = 24.0f;
			if (std::max(s0.x, s3.x) + margin < clipRect.x || std::min(s0.x, s3.x) - margin > clipRect.x + clipRect.z
				|| std::max(s0.y, s3.y) + margin < clipRect.y || std::min(s0.y, s3.y) - margin > clipRect.y + clipRect.w)
				continue;

			const bool hoverIncident = m_HasHover && (edge.From == m_HoveredNode || edge.To == m_HoveredNode);
			glm::vec4 colour;
			if (hoverIncident)
				colour = kEdgeHover;
			else if (edge.Flags & EdgeFlags::BackEdge)
				colour = kEdgeBack;
			else
				colour = (edge.Flags & EdgeFlags::Outgoing) ? kEdgeOutgoing : kEdgeIncoming;

			const glm::vec2 dir = glm::normalize(s3 - points[points.size() - 2] + glm::vec2(1e-4f));
			const glm::vec2 perp = { -dir.y, dir.x };
			const float ah = std::clamp(12.0f * scale, 6.0f, 16.0f);
			// The tip sits in the gap before the card (the curve runs to
			// the card's edge beneath it) so the arrow reads as pointing
			// AT the target, not an orphan at its border. The head is
			// pulled toward white so it reads clearly against the dark
			// canvas and any edge colour.
			const glm::vec2 tip = s3 - dir * ah * 0.5f;
			const glm::vec4 headColour = glm::vec4(
				glm::mix(glm::vec3(colour), glm::vec3(1.0f), 0.65f), colour.a);
			dl.AddTriangle(layer, tip, headColour,
				tip - dir * ah + perp * ah * 0.45f, headColour,
				tip - dir * ah - perp * ah * 0.45f, headColour, clip);
		}
	}

	void GraphCanvas::PaintNodes(DrawList& dl, int layer, int clip, const glm::vec4& worldClip) const
	{
		const float scale = ZoomScale(m_Transform.ZoomLevel);
		const LodTier tier = LodTierFor(m_Transform.ZoomLevel);

		for (const ViewNode& node : m_View.Nodes)
		{
			if (!NodeVisible(node, worldClip))
				continue;

			const glm::vec2 pos = WorldToScreen(node.Position);
			const glm::vec2 size = node.Size * scale;
			const bool selected = m_Selected.count(node.Id) != 0;
			const bool hovered = m_HasHover && node.Id == m_HoveredNode;
			const bool broken = (node.Flags & NodeFlags::Broken) != 0;
			const bool overflow = (node.Flags & NodeFlags::Overflow) != 0;

			const float radius = std::clamp(6.0f * scale, 0.5f, 8.0f);
			const float radii[4] = { radius, radius, radius, radius };

			glm::vec4 fill = node.Fill;
			if (overflow)
				fill = { 0.2f, 0.2f, 0.26f, 1.0f };

			glm::vec4 outline = { 0.0f, 0.0f, 0.0f, 0.0f };
			float outlineWidth = 0.0f;
			if (selected)
			{
				outline = { 1.0f, 1.0f, 1.0f, 0.95f };
				outlineWidth = 1.5f;
			}
			else if (hovered)
			{
				outline = wisp::tokens::TextLink;
				outlineWidth = 1.25f;
			}
			else if (broken)
			{
				outline = wisp::tokens::Error;
				outlineWidth = 1.5f;
			}

			dl.AddDropShadow(layer - 1, pos, size, { 0.0f, 0.0f, 0.0f, 0.35f }, 6.0f, { 2.0f, 2.0f }, clip);
			dl.AddRoundedBox(layer, pos, size, fill, radii, outlineWidth, outline, clip);

			if (tier == LodTier::Boxes)
				continue;

			if (overflow || tier == LodTier::TitleOnly)
			{
				if (!TextLegible(m_BaseTitleSize))
					continue;
				const std::string& title = CachedTitle(node, size.x - 8.0f);
				const float textSize = (float)QuantizedTextSize(m_BaseTitleSize);
				const glm::vec2 w = m_FontCache->MeasureText(m_Font, title, textSize);
				PaintTextScaled(dl, layer + 1, clip,
					{ pos.x + (size.x - w.x) * 0.5f, pos.y + (size.y - textSize * 1.2f) * 0.5f },
					m_BaseTitleSize, title, wisp::tokens::TextPrimary);
				continue;
			}

			// The full node: the content zone (the schema's domain bits),
			// the title, the subtitle.
			const float pad = 6.0f * scale;
			const float titleH = m_BaseTitleSize * 1.35f * scale;
			const float subH = m_BaseSubtitleSize * 1.35f * scale;
			const float textBlockH = node.Subtitle.empty() ? titleH : titleH + subH + 2.0f * scale;
			const float contentH = std::max(0.0f, size.y - pad * 2.0f - textBlockH);
			const glm::vec2 contentPos = { pos.x + pad, pos.y + pad };
			const glm::vec2 contentSize = { size.x - pad * 2.0f, contentH };

			if (m_Schema)
				m_Schema->PaintNodeContent(dl, layer + 1, clip, contentPos, contentSize, node, m_Transform.ZoomLevel);

			const std::string& title = CachedTitle(node, size.x - 8.0f);
			const glm::vec2 tw = m_FontCache->MeasureText(m_Font, title, (float)QuantizedTextSize(m_BaseTitleSize));
			PaintTextScaled(dl, layer + 1, clip,
				{ pos.x + (size.x - tw.x) * 0.5f, pos.y + pad + contentH + 2.0f * scale },
				m_BaseTitleSize, title, { 0.92f, 0.92f, 0.97f, 1.0f });

			if (!node.Subtitle.empty())
			{
				const std::string& sub = CachedSubtitle(node, size.x - 8.0f);
				const glm::vec2 sw = m_FontCache->MeasureText(m_Font, sub, (float)QuantizedTextSize(m_BaseSubtitleSize));
				PaintTextScaled(dl, layer + 1, clip,
					{ pos.x + (size.x - sw.x) * 0.5f, pos.y + pad + contentH + titleH + 2.0f * scale },
					m_BaseSubtitleSize, sub, { 0.55f, 0.55f, 0.62f, 1.0f });
			}
		}
	}

	const std::string& GraphCanvas::CachedTitle(const ViewNode& node, float maxWidth) const
	{
		const int size = QuantizedTextSize(m_BaseTitleSize);
		auto it = m_TitleCache.find(node.Id);
		if (it != m_TitleCache.end() && it->second.first == size)
			return it->second.second;
		std::string truncated = TruncateToFit(m_FontCache, m_Font, (float)size, node.Title, maxWidth);
		return m_TitleCache.insert_or_assign(node.Id, std::make_pair(size, std::move(truncated))).first->second.second;
	}

	const std::string& GraphCanvas::CachedSubtitle(const ViewNode& node, float maxWidth) const
	{
		const int size = QuantizedTextSize(m_BaseSubtitleSize);
		auto it = m_SubtitleCache.find(node.Id);
		if (it != m_SubtitleCache.end() && it->second.first == size)
			return it->second.second;
		std::string truncated = TruncateToFit(m_FontCache, m_Font, (float)size, node.Subtitle, maxWidth);
		return m_SubtitleCache.insert_or_assign(node.Id, std::make_pair(size, std::move(truncated))).first->second.second;
	}

	void GraphCanvas::PaintMarquee(DrawList& dl, int layer, int clip) const
	{
		if (m_Interaction != Interaction::Marquee || !m_MovedPastThreshold)
			return;
		const glm::vec2 mn = glm::min(m_AnchorScreen, m_LastMouse);
		const glm::vec2 mx = glm::max(m_AnchorScreen, m_LastMouse);
		dl.AddBox(layer, mn, mx - mn, { 0.3f, 0.5f, 0.8f, 0.15f }, clip);
		const float radii[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		dl.AddRoundedBox(layer, mn, mx - mn, { 0.0f, 0.0f, 0.0f, 0.0f }, radii, 1.0f, { 0.5f, 0.7f, 1.0f, 0.9f }, clip);
	}

	void GraphCanvas::PaintTextScaled(DrawList& dl, int layer, int clip, const glm::vec2& at,
		float baseSize, const std::string& text, const glm::vec4& colour) const
	{
		const float size = (float)QuantizedTextSize(baseSize);
		const FontMetrics metrics = m_FontCache->GetMetrics(m_Font, size);
		const uint8_t* p = (const uint8_t*)text.data();
		const uint8_t* end = p + text.size();
		float penX = at.x;
		uint32_t prevCp = 0;
		while (p < end)
		{
			uint32_t cp;
			if ((*p & 0x80) == 0) { cp = *p; p += 1; }
			else if ((*p & 0xE0) == 0xC0 && p + 1 < end) { cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
			else if ((*p & 0xF0) == 0xE0 && p + 2 < end) { cp = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
			else { p++; continue; }

			const GlyphInfo* gi = m_FontCache->GetGlyph(m_Font, cp, size);
			if (!gi)
				continue;
			float advance = gi->AdvanceX;
			if (prevCp)
				advance += m_FontCache->GetKerning(m_Font, prevCp, cp, size);
			AddGlyph(dl, layer, { penX, at.y + metrics.Ascent }, *gi, colour, clip);
			penX += std::round(advance);
			prevCp = cp;
		}
	}

	void GraphCanvas::LoadViewState()
	{
		if (m_ViewStateKey.empty())
			return;
		rapidjson::Document doc;
		{
			std::ifstream in("saved/config/user/" + m_ViewStateKey);
			if (!in.is_open())
				return;
			std::stringstream buffer;
			buffer << in.rdbuf();
			doc.Parse(buffer.str().c_str());
			if (doc.HasParseError())
				return;
		}
		if (doc.HasMember("panX") && doc["panX"].IsNumber() && doc.HasMember("panY") && doc["panY"].IsNumber())
			m_Transform.Pan = { doc["panX"].GetFloat(), doc["panY"].GetFloat() };
		if (doc.HasMember("zoomLevel") && doc["zoomLevel"].IsInt())
			m_Transform.ZoomLevel = std::clamp(doc["zoomLevel"].GetInt(), kZoomLevelMin, kZoomLevelMax);
	}

	void GraphCanvas::SaveViewState() const
	{
		if (m_ViewStateKey.empty())
			return;
		std::filesystem::create_directories("saved/config/user");
		rapidjson::StringBuffer buffer;
		rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
		writer.StartObject();
		writer.Key("panX");
		writer.Double(m_Transform.Pan.x);
		writer.Key("panY");
		writer.Double(m_Transform.Pan.y);
		writer.Key("zoomLevel");
		writer.Int(m_Transform.ZoomLevel);
		writer.EndObject();
		std::ofstream out("saved/config/user/" + m_ViewStateKey, std::ios::trunc);
		if (out.is_open())
			out << buffer.GetString();
	}

	void GraphCanvas::BeginInteraction(Interaction i, const glm::vec2& anchor)
	{
		m_Interaction = i;
		m_Anchor = anchor - m_Origin;
		m_AnchorScreen = anchor;
		m_LastMouse = anchor;
		m_MovedPastThreshold = false;
	}

	void GraphCanvas::EndInteraction()
	{
		m_Interaction = Interaction::None;
		m_InteractionNode = 0;
		m_DragStart.clear();
		m_MovedPastThreshold = false;
	}

	void GraphCanvas::OpenContextMenu(NodeId node, const glm::vec2& at)
	{
		if (!m_Schema)
			return;
		m_Schema->ShowContextMenu(node, at);
	}

	void GraphCanvas::SetSelection(const std::vector<NodeId>& nodes, bool add)
	{
		if (!add)
			m_Selected.clear();
		for (NodeId id : nodes)
			m_Selected.insert(id);
	}

	void GraphCanvas::UpdateHover() const
	{
		if (!UIApplication::IsValid())
			return;
		const glm::vec2 mouse = UIApplication::Get().GetMousePos();
		if (mouse == m_LastPolledMouse)
			return; // the mouse is parked - keep the current hover
		m_LastPolledMouse = mouse;

		NodeId newHover = 0;
		if (mouse.x >= m_Origin.x && mouse.x <= m_Origin.x + m_Size.x
			&& mouse.y >= m_Origin.y && mouse.y <= m_Origin.y + m_Size.y)
		{
			// The node-only hit (the tooltip and the incident-edge
			// highlight are node-driven - no edge distances per paint).
			const glm::vec2 world = ScreenToWorld(mouse);
			for (size_t i = m_View.Nodes.size(); i-- > 0;)
			{
				const ViewNode& node = m_View.Nodes[i];
				if (world.x >= node.Position.x && world.x <= node.Position.x + node.Size.x
					&& world.y >= node.Position.y && world.y <= node.Position.y + node.Size.y)
				{
					newHover = node.Id;
					break;
				}
			}
		}
		if (newHover != m_HoveredNode)
		{
			m_HoveredNode = newHover;
			if (UIApplication::IsValid() && UIApplication::Get().HasOpenPopups())
				UIApplication::Get().RequestDismissPopups();
		}
		m_HasHover = newHover != 0;
	}

} // namespace graph
} // namespace wisp
