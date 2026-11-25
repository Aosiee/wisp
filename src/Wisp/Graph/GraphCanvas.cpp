// The graph canvas implementation.

#include "hearth/Zone.h"
#include "Wisp/Graph/GraphCanvas.h"

#include "hearth/platform/Keys.h"
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
#include "Wisp/Icons.h"
#include "Wisp/StyleTokens.h"

namespace wisp {

namespace {

namespace key = hearth::platform::key;

// A key held in the focused window (the canvas's modifiers).
bool KeyHeld(int code)
{
	return UIApplication::IsValid() && UIApplication::Get().IsKeyDown(nullptr, code);
}

} // namespace

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
		const glm::vec4 kEdgeState = { 0.6f, 0.62f, 0.68f, 0.9f }; // a Straight (state machine) edge at rest

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
		const glm::vec2& viewportSize, int minLevel, int maxLevel)
	{
		minLevel = std::clamp(minLevel, kZoomLevelMin, kZoomLevelMax);
		maxLevel = std::clamp(maxLevel, minLevel, kZoomLevelMax);
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
		int level = maxLevel;
		while (level > minLevel && ZoomScale(level) > fit)
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
			if (!m_Schema || m_Interaction != Interaction::None)
				return nullptr;
			std::string text;
			if (m_HasHover)
				text = m_Schema->TooltipText(m_HoveredNode);
			else if (m_HoveredEdge >= 0)
				text = m_Schema->EdgeTooltipText((size_t)m_HoveredEdge);
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
		m_LastPolledMouse = glm::vec2(-1e9f, -1e9f);
		// A rebuild mid-drag would snap the dragged nodes back: it waits for
		// the drop.
		if (m_Interaction == Interaction::NodeDrag)
		{
			m_NeedsRebuild = true;
			return;
		}
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
		// The stale view ids must not survive the rebuild (the tree-mode
		// occurrence ids are per-layout) - a re-matched id would silently
		// re-select a different node. A schema that owns the selection flags
		// its nodes, and those stay selected.
		m_Selected.clear();
		for (const ViewNode& node : m_View.Nodes)
			if (node.Flags & NodeFlags::Selected)
				m_Selected.insert(node.Id);
		m_HoveredEdge = -1;
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
		m_AutoFrame = true;
		m_FramedSize = glm::vec2(0.0f); // the next paint frames (it knows the size)
		if (!m_HasCachedGeometry)
			return;
		const std::vector<NodeId> empty;
		m_Transform = FrameTransform(m_View, empty, m_Size, m_FrameZoomMin, m_FrameZoomMax);
		m_LastPolledMouse = glm::vec2(-1e9f, -1e9f);
		SaveViewState();
	}

	void GraphCanvas::FrameSelection()
	{
		if (!m_HasCachedGeometry)
			return;
		std::vector<NodeId> selected(m_Selected.begin(), m_Selected.end());
		m_Transform = FrameTransform(m_View, selected, m_Size, m_FrameZoomMin, m_FrameZoomMax);
		m_LastPolledMouse = glm::vec2(-1e9f, -1e9f);
		SaveViewState();
	}

	int GraphCanvas::OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& dl,
		int layerId, const WidgetStyleContext&) const
	{
		HEARTH_ZONE("GraphCanvas - Paint");

		m_Origin = geom.LocalToAbsolute({ 0, 0 });
		m_Size = geom.LocalSize;
		m_HasCachedGeometry = true;
		m_DpiScale = args.DpiScale;

		if (m_NeedsRebuild && m_Schema && m_Interaction != Interaction::NodeDrag)
		{
			DoBuild();
			m_NeedsRebuild = false;
			if (m_FirstBuild)
			{
				m_FirstBuild = false;
				const std::vector<NodeId> empty;
				m_Transform = FrameTransform(m_View, empty, m_Size, m_FrameZoomMin, m_FrameZoomMax);
				m_FramedSize = m_Size;
			}
		}
		if (m_AutoFrame && m_Size != m_FramedSize && !m_View.Nodes.empty())
		{
			const std::vector<NodeId> empty;
			m_Transform = FrameTransform(m_View, empty, m_Size, m_FrameZoomMin, m_FrameZoomMax);
			m_FramedSize = m_Size;
			m_EdgeCacheValid = false;
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
		PaintEdgeLabels(dl, layerId + 3, bodyClip);
		PaintConnect(dl, layerId + 5, bodyClip);
		PaintMarquee(dl, layerId + 5, bodyClip);
		PaintOverlays(dl, layerId + 6, bodyClip, worldClip);

		UpdateHover();
		return layerId + 9;
	}

	Reply GraphCanvas::OnMouseWheel(const glm::vec2& absPos, float delta)
	{
		if (!m_HasCachedGeometry)
			return Reply::Unhandled();
		m_AutoFrame = false;
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

		if (m_MinimapRect.z > 0.0f && absPos.x >= m_MinimapRect.x && absPos.y >= m_MinimapRect.y
			&& absPos.x <= m_MinimapRect.x + m_MinimapRect.z && absPos.y <= m_MinimapRect.y + m_MinimapRect.w)
		{
			BeginInteraction(Interaction::Minimap, absPos);
			m_AutoFrame = false;
			PanMinimapTo(absPos);
			return Reply::Handled_().CaptureMouse().SetFocus();
		}

		const glm::vec2 local = absPos - m_Origin;
		const HitResult hit = HitTest(m_View, m_Transform, local);
		if (hit.Kind == HitResult::Kind::Node)
		{
			const bool shift = KeyHeld(key::LeftShift)
				|| KeyHeld(key::RightShift);
			if (m_Schema && m_Schema->CanStartConnect(hit.Node))
			{
				const ViewNode* node = FindViewNode(hit.Node);
				if (node && (shift || InNodeRim(*node, ScreenToWorld(absPos), RimWidth(*node))))
				{
					BeginInteraction(Interaction::Connect, absPos);
					m_ConnectFrom = hit.Node;
					m_ConnectTarget = 0;
					return Reply::Handled_().CaptureMouse().SetFocus();
				}
			}

			const bool ctrl = KeyHeld(key::LeftControl)
				|| KeyHeld(key::RightControl);
			m_CtrlAtDown = ctrl;
			m_PressedSelected = false;
			if (ctrl && m_Selected.count(hit.Node))
				m_Selected.erase(hit.Node);
			else if (!ctrl && m_Selected.count(hit.Node))
				m_PressedSelected = true; // the group drags together; a click narrows it
			else
			{
				if (!ctrl)
					m_Selected.clear();
				m_Selected.insert(hit.Node);
			}
			m_InteractionNode = hit.Node;
			if (m_Schema && m_Schema->NodeMovable(hit.Node))
			{
				BeginInteraction(Interaction::NodeDrag, absPos);
				m_DragStart.clear();
				// The pressed node first: the snap aligns it, the rest follow.
				for (const ViewNode& node : m_View.Nodes)
					if (node.Id == hit.Node)
						m_DragStart.push_back({ node.Id, node.Position });
				for (const ViewNode& node : m_View.Nodes)
					if (node.Id != hit.Node && m_Selected.count(node.Id) && m_Schema->NodeMovable(node.Id))
						m_DragStart.push_back({ node.Id, node.Position });
			}
			else
			{
				BeginInteraction(Interaction::None, absPos);
			}
			return Reply::Handled_().CaptureMouse().SetFocus();
		}

		// An edge selects in an editing schema; the empty canvas (or an edge
		// a viewer ignores) starts the marquee.
		if (hit.Kind == HitResult::Kind::Edge && m_Schema)
		{
			m_Selected.clear();
			m_Schema->OnEdgeClick(hit.Edge);
			return Reply::Handled_().SetFocus();
		}
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
		if (m_Interaction == Interaction::Minimap)
		{
			PanMinimapTo(absPos);
			return Reply::Handled_();
		}
		if (m_Interaction == Interaction::Connect)
		{
			m_LastMouse = absPos;
			m_ConnectTarget = 0;
			const HitResult hit = HitTest(m_View, m_Transform, absPos - m_Origin);
			if (hit.Kind == HitResult::Kind::Node && hit.Node != m_ConnectFrom && m_Schema
				&& m_Schema->CanConnect(m_ConnectFrom, hit.Node))
				m_ConnectTarget = hit.Node;
			return Reply::Handled_();
		}
		if (m_Interaction == Interaction::NodeDrag)
		{
			if (glm::length(absPos - m_AnchorScreen) >= kPanDragThresholdPx)
				m_MovedPastThreshold = true;
			if (!m_MovedPastThreshold)
				return Reply::Handled_();
			glm::vec2 delta = (absPos - m_Origin - m_Anchor) / ZoomScale(m_Transform.ZoomLevel);
			// The pressed node lands on the grid (Alt drags freely).
			const float grid = m_Schema ? m_Schema->SnapGrid() : 0.0f;
			const bool alt = KeyHeld(key::LeftAlt) || KeyHeld(key::RightAlt);
			if (grid > 0.0f && !alt && !m_DragStart.empty())
				delta = SnapToGrid(m_DragStart.front().second + delta, grid) - m_DragStart.front().second;
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
		// At rest: the rim of a node a connect drag can start from.
		SetCursor(m_HoverRim ? CursorType::Crosshair : CursorType::Default);
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
				else if (m_Schema)
					m_Schema->ShowBackgroundMenu(ScreenToWorld(absPos), absPos);
			}
			if (m_Interaction == Interaction::PanRightDrag)
				SaveViewState();
			EndInteraction();
			return Reply::Handled_().ReleaseCapture();
		}
		if (button == 0)
		{
			if (m_Interaction == Interaction::Minimap)
			{
				SaveViewState();
				EndInteraction();
				return Reply::Handled_().ReleaseCapture();
			}
			if (m_Interaction == Interaction::Connect)
			{
				const NodeId from = m_ConnectFrom, to = m_ConnectTarget;
				EndInteraction();
				SetCursor(CursorType::Default);
				if (to != 0 && m_Schema)
					m_Schema->OnConnect(from, to);
				return Reply::Handled_().ReleaseCapture();
			}
			if (m_Interaction == Interaction::Marquee)
			{
				if (m_MovedPastThreshold)
				{
					const bool ctrl = KeyHeld(key::LeftControl)
						|| KeyHeld(key::RightControl);
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
					NotifySelection();
				}
				else
				{
					m_Selected.clear();
					// A second click on the background soon after the first
					// activates it (a state machine adds a node there).
					const double now = std::chrono::duration<double>(
						std::chrono::steady_clock::now().time_since_epoch()).count();
					const bool doubleClick = m_LastClickNode == 0 && now - m_LastClickTime < kDoubleClickSeconds;
					m_LastClickTime = doubleClick ? -1e9 : now;
					m_LastClickNode = 0;
					if (m_Schema)
					{
						if (doubleClick)
							m_Schema->OnBackgroundActivate(ScreenToWorld(absPos));
						else
							m_Schema->OnBackgroundClick();
					}
				}
			}
			else if (m_Interaction == Interaction::NodeDrag || m_Interaction == Interaction::None)
			{
				if (!m_MovedPastThreshold && m_InteractionNode != 0 && m_CtrlAtDown)
				{
					// Ctrl+click adds or removes the node, nothing else.
					NotifySelection();
				}
				else if (!m_MovedPastThreshold && m_InteractionNode != 0)
				{
					if (m_PressedSelected)
					{
						m_Selected.clear();
						m_Selected.insert(m_InteractionNode);
					}
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
				{
					if (m_MovedPastThreshold && m_Schema)
					{
						std::vector<std::pair<NodeId, glm::vec2>> moved;
						for (const auto& [id, start] : m_DragStart)
							for (const ViewNode& node : m_View.Nodes)
								if (node.Id == id)
									moved.push_back({ id, node.Position });
						m_Schema->OnNodesMoved(moved);
					}
					SaveViewState();
				}
			}
			const bool wasDrag = m_Interaction == Interaction::NodeDrag;
			EndInteraction();
			if (wasDrag && m_NeedsRebuild && m_Schema && m_HasCachedGeometry)
			{
				DoBuild(); // a rebuild the drag held back
				m_NeedsRebuild = false;
			}
			return Reply::Handled_().ReleaseCapture();
		}
		return Reply::Unhandled();
	}

	Reply GraphCanvas::OnKeyDown(int key, int mods)
	{
		const bool ctrl = (mods & 2) != 0;
		const bool shift = (mods & 1) != 0;
		if (key == key::Escape && m_Interaction == Interaction::Connect)
		{
			EndInteraction();
			return Reply::Handled_().ReleaseCapture();
		}
		if (m_Schema && m_Schema->OnKey(key))
			return Reply::Handled_();
		if (ctrl && key == key::A && m_Schema)
		{
			m_Selected.clear();
			for (const ViewNode& node : m_View.Nodes)
				if (m_Schema->NodeMovable(node.Id))
					m_Selected.insert(node.Id);
			NotifySelection();
			return Reply::Handled_();
		}
		if (key == key::Escape && !m_Selected.empty())
		{
			m_Selected.clear();
			NotifySelection();
			return Reply::Handled_();
		}
		if ((key == key::Left || key == key::Right || key == key::Up || key == key::Down) && !ctrl
			&& m_Schema && !m_Selected.empty() && m_Interaction == Interaction::None)
		{
			const float grid = m_Schema->SnapGrid();
			const float step = (grid > 0.0f ? grid : 8.0f) * (shift ? 4.0f : 1.0f);
			const glm::vec2 delta = key == key::Left ? glm::vec2(-step, 0.0f) : key == key::Right ? glm::vec2(step, 0.0f)
				: key == key::Up ? glm::vec2(0.0f, -step) : glm::vec2(0.0f, step);
			std::vector<std::pair<NodeId, glm::vec2>> moved;
			for (ViewNode& node : m_View.Nodes)
				if (m_Selected.count(node.Id) && m_Schema->NodeMovable(node.Id))
				{
					node.Position += delta;
					moved.push_back({ node.Id, node.Position });
				}
			if (moved.empty())
				return Reply::Unhandled();
			m_EdgeCacheValid = false;
			m_Schema->OnNodesMoved(moved);
			return Reply::Handled_();
		}
		if (key == key::Home)
		{
			FrameAll();
			return Reply::Handled_();
		}
		if (key == key::F)
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
		// Every fourth dot is a major one, so distances read at a glance.
		const glm::vec4 dot = tokens::WithAlpha(tokens::Border, 0.9f);
		const glm::vec4 major = tokens::BorderStrong;
		for (float wx = x0; wx <= worldClip.x + worldClip.z; wx += kGridSpacing)
			for (float wy = y0; wy <= worldClip.y + worldClip.w; wy += kGridSpacing)
			{
				const bool isMajor = std::fmod(std::abs(wx), kGridSpacing * 4.0f) < 0.5f && std::fmod(std::abs(wy), kGridSpacing * 4.0f) < 0.5f;
				const float d = isMajor ? 3.0f : 2.0f;
				dl.AddBox(layer, WorldToScreen({ wx, wy }) - glm::vec2(d * 0.5f), { d, d }, isMajor ? major : dot, clip);
			}
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

			const bool hovered = m_Interaction == Interaction::None && (m_HasHover ? (edge.From == m_HoveredNode || edge.To == m_HoveredNode) : (int)i == m_HoveredEdge);
			const bool highlighted = (edge.Flags & EdgeFlags::Highlighted) != 0;
			const glm::vec4 colour = EdgeColour(i);
			const LineStyle stroke = (edge.Kind == EdgeKind::Soft) ? LineStyle::Dashed : LineStyle::Solid;
			const float width = highlighted ? thickness + 1.5f : hovered ? thickness + 0.8f : thickness;

			// A transition that just fired glows, fading out.
			const float flash = m_Schema ? std::clamp(m_Schema->EdgeFlash(i), 0.0f, 1.0f) : 0.0f;
			if (flash > 0.0f)
				dl.AddPolyline(layer, points, width + 7.0f * flash, tokens::WithAlpha(tokens::Success, 0.3f * flash), clip);
			dl.AddPolyline(layer, points, width, colour, clip, stroke);

			// A live cross-fade fills the edge from its tail, a dot at its head.
			const float progress = m_Schema ? m_Schema->EdgeProgress(i) : -1.0f;
			if (progress >= 0.0f)
			{
				const std::vector<glm::vec2> filled = PolylinePrefix(points, progress);
				if (filled.size() >= 2)
					dl.AddPolyline(layer + 1, filled, width + 2.0f, tokens::Success, clip);
				const float d = std::clamp(9.0f * scale, 6.0f, 10.0f);
				const float r[4] = { d * 0.5f, d * 0.5f, d * 0.5f, d * 0.5f };
				dl.AddRoundedBox(layer + 1, filled.back() - glm::vec2(d * 0.5f), { d, d }, tokens::Success, r, 1.5f, tokens::Panel, clip);
			}
		}
	}

	glm::vec4 GraphCanvas::EdgeColour(size_t edgeIndex) const
	{
		const ViewEdge& edge = m_View.Edges[edgeIndex];
		const bool hovered = m_Interaction == Interaction::None && (m_HasHover ? (edge.From == m_HoveredNode || edge.To == m_HoveredNode) : (int)edgeIndex == m_HoveredEdge);
		glm::vec4 colour;
		if (edge.Flags & EdgeFlags::Highlighted)
			colour = tokens::Accent;
		else if (hovered)
			colour = kEdgeHover;
		else if (edge.Flags & EdgeFlags::EdgeWarning)
			colour = tokens::Warning;
		else if (edge.Flags & EdgeFlags::Emphasis)
			colour = tokens::Accent;
		else if (m_View.Shape == EdgeShape::Straight)
			colour = kEdgeState;
		else if (edge.Flags & EdgeFlags::BackEdge)
			colour = kEdgeBack;
		else
			colour = (edge.Flags & EdgeFlags::Outgoing) ? kEdgeOutgoing : kEdgeIncoming;
		const float flash = m_Schema ? std::clamp(m_Schema->EdgeFlash(edgeIndex), 0.0f, 1.0f) : 0.0f;
		if (m_Schema && m_Schema->EdgeProgress(edgeIndex) >= 0.0f)
			return tokens::Success;
		return flash > 0.0f ? glm::mix(colour, tokens::Success, flash) : colour;
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

			const glm::vec4 colour = EdgeColour(i);

			const glm::vec2 dir = glm::normalize(s3 - points[points.size() - 2] + glm::vec2(1e-4f));
			const glm::vec2 perp = { -dir.y, dir.x };
			const float ah = std::clamp(12.0f * scale, 6.0f, 16.0f);
			// The tip sits in the gap before the card (the curve runs to
			// the card's edge beneath it) so the arrow reads as pointing
			// AT the target, not an orphan at its border. The head is
			// pulled toward white so it reads clearly against the dark
			// canvas and any edge colour.
			const glm::vec2 tip = s3 - dir * ah * 0.5f;
			// A coloured edge (selected, emphasised, warned, live) keeps its
			// colour on the head.
			const bool coloured = (edge.Flags & (EdgeFlags::Highlighted | EdgeFlags::Emphasis | EdgeFlags::EdgeWarning)) != 0
				|| (m_Schema && (m_Schema->EdgeProgress(i) >= 0.0f || m_Schema->EdgeFlash(i) > 0.0f));
			const glm::vec4 headColour = coloured ? colour : glm::vec4(
				glm::mix(glm::vec3(colour), glm::vec3(1.0f), 0.65f), colour.a);
			dl.AddTriangle(layer, tip, headColour,
				tip - dir * ah + perp * ah * 0.45f, headColour,
				tip - dir * ah - perp * ah * 0.45f, headColour, clip);
		}
	}

	void GraphCanvas::PaintEdgeLabels(DrawList& dl, int layer, int clip) const
	{
		const float scale = ZoomScale(m_Transform.ZoomLevel);
		if (m_View.Shape != EdgeShape::Straight || scale < 0.66f || !TextLegible(m_BaseSubtitleSize))
			return;
		const float textSize = (float)QuantizedTextSize(m_BaseSubtitleSize);
		const glm::vec2 padding(5.0f, 2.0f);

		// The cards and the chips placed so far: a chip goes at the first
		// spot along its edge that covers neither, else it is left to the
		// selection and the tooltip. The selected edge places first, and
		// always.
		std::vector<glm::vec4> taken;
		for (const ViewNode& node : m_View.Nodes)
			taken.push_back({ WorldToScreen(node.Position) - glm::vec2(2.0f), node.Size * scale + glm::vec2(4.0f) });
		auto overlaps = [&](const glm::vec2& at, const glm::vec2& size) {
			for (const glm::vec4& r : taken)
				if (at.x < r.x + r.z && at.x + size.x > r.x && at.y < r.y + r.w && at.y + size.y > r.y)
					return true;
			return false;
		};
		std::vector<size_t> order;
		for (size_t i = 0; i < m_View.Edges.size(); i++)
			if (m_View.Edges[i].Flags & EdgeFlags::Highlighted)
				order.insert(order.begin(), i);
			else
				order.push_back(i);

		for (size_t i : order)
		{
			const ViewEdge& edge = m_View.Edges[i];
			if (edge.Label.empty() || i >= m_EdgePoints.size() || m_EdgePoints[i].size() < 2)
				continue;
			const bool highlighted = (edge.Flags & EdgeFlags::Highlighted) != 0;
			const glm::vec2 s0 = m_EdgePoints[i].front();
			const glm::vec2 s3 = m_EdgePoints[i].back();
			const glm::vec2 chipSize = m_FontCache->MeasureText(m_Font, edge.Label, textSize) + padding * 2.0f;
			const glm::vec2 along = glm::normalize(s3 - s0 + glm::vec2(1e-4f));
			const glm::vec2 side(-along.y, along.x);
			// A laned edge (it shares its nodes) keeps its chip on its own
			// side of the line.
			const float reach = std::abs(side.x) * chipSize.x * 0.5f + std::abs(side.y) * chipSize.y * 0.5f;
			const glm::vec2 push = EdgeLane(m_View, i) > 0 ? side * (reach + 3.0f) : glm::vec2(0.0f);
			bool placed = false;
			glm::vec2 chipPos(0.0f);
			for (float t : { 0.5f, 0.38f, 0.62f, 0.26f, 0.74f })
			{
				chipPos = glm::round(glm::mix(s0, s3, t) + push - chipSize * 0.5f);
				if (!overlaps(chipPos, chipSize))
				{
					placed = true;
					break;
				}
			}
			if (!placed && !highlighted)
				continue;
			if (!placed)
				chipPos = glm::round(glm::mix(s0, s3, 0.5f) + push - chipSize * 0.5f);
			taken.push_back({ chipPos, chipSize });
			const float radius = std::min(chipSize.y * 0.5f, 6.0f);
			const float radii[4] = { radius, radius, radius, radius };
			dl.AddRoundedBox(layer, chipPos, chipSize, tokens::Raised, radii, 1.0f, highlighted ? tokens::Accent : tokens::Border, clip);
			PaintTextScaled(dl, layer + 1, clip, chipPos + padding, m_BaseSubtitleSize, edge.Label,
				highlighted ? tokens::TextPrimary : tokens::TextSecondary);
		}
	}

	void GraphCanvas::PaintNodes(DrawList& dl, int layer, int clip, const glm::vec4& worldClip) const
	{
		const float scale = ZoomScale(m_Transform.ZoomLevel);
		const LodTier tier = LodTierFor(m_Transform.ZoomLevel);
		const bool floored = TitleFloored();
		const int titlePx = TitlePixels();

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
			const bool active = (node.Flags & NodeFlags::Active) != 0;
			const bool connectEnd = m_Interaction == Interaction::Connect && (node.Id == m_ConnectFrom || node.Id == m_ConnectTarget);

			const float radius = (node.Flags & NodeFlags::Pill) ? size.y * 0.5f : std::clamp(6.0f * scale, 0.5f, 8.0f);
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
			else if (connectEnd)
			{
				outline = tokens::Accent;
				outlineWidth = 2.0f;
			}
			else if (active)
			{
				outline = tokens::Success;
				outlineWidth = 2.0f;
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

			if (active)
			{
				// The live glow: soft rings around the card.
				for (int ring = 2; ring >= 1; ring--)
				{
					const float grow = 3.0f * (float)ring;
					const float r = radius + grow;
					const float rr[4] = { r, r, r, r };
					dl.AddRoundedBox(layer - 1, pos - glm::vec2(grow), size + glm::vec2(grow * 2.0f),
						tokens::WithAlpha(tokens::Success, ring == 1 ? 0.22f : 0.1f), rr, 0.0f, {}, clip);
				}
			}
			else
				dl.AddDropShadow(layer - 1, pos, size, { 0.0f, 0.0f, 0.0f, 0.35f }, 6.0f, { 2.0f, 2.0f }, clip);
			dl.AddRoundedBox(layer, pos, size, fill, radii, outlineWidth, outline, clip);

			// The rim a connect drag starts from, lit under the cursor.
			if (hovered && m_HoverRim && m_Interaction == Interaction::None)
			{
				const float r = radius + 2.0f;
				const float rr[4] = { r, r, r, r };
				dl.AddRoundedBox(layer + 1, pos - glm::vec2(2.0f), size + glm::vec2(4.0f), { 0.0f, 0.0f, 0.0f, 0.0f }, rr, 2.0f,
					tokens::Accent, clip);
			}

			if (tier == LodTier::Boxes && !floored)
				continue;

			// The warning badge, top right.
			if ((node.Flags & NodeFlags::NodeWarning) && TextLegible(m_BaseSubtitleSize))
			{
				const int px = std::max(9, QuantizedTextSize(m_BaseSubtitleSize));
				const float w = m_FontCache->MeasureText(m_Font, icons::Warning, (float)px).x;
				PaintTextPixels(dl, layer + 2, clip, { pos.x + size.x - w - 5.0f * scale - 2.0f, pos.y + 3.0f * scale }, px,
					icons::Warning, tokens::Warning);
			}

			// Zoomed out (or a floored title): the title alone, centred.
			if (overflow || tier != LodTier::Full || floored)
			{
				if (!floored && !TextLegible(m_BaseTitleSize))
					continue;
				if (size.y < (float)titlePx * 1.1f)
					continue; // the floored title would spill out of the card
				const std::string& title = CachedTitle(node, size.x - 8.0f);
				const glm::vec2 w = m_FontCache->MeasureText(m_Font, title, (float)titlePx);
				PaintTextPixels(dl, layer + 1, clip,
					{ pos.x + (size.x - w.x) * 0.5f, std::round(pos.y + (size.y - (float)titlePx * 1.2f) * 0.5f) },
					titlePx, title, wisp::tokens::TextPrimary);
				if (m_Schema && tier == LodTier::Full)
					m_Schema->PaintNodeContent(dl, layer + 1, clip, pos + glm::vec2(6.0f * scale), { size.x - 12.0f * scale, 0.0f },
						node, m_Transform.ZoomLevel);
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
			const glm::vec2 tw = m_FontCache->MeasureText(m_Font, title, (float)titlePx);
			PaintTextPixels(dl, layer + 1, clip,
				{ pos.x + (size.x - tw.x) * 0.5f, pos.y + pad + contentH + 2.0f * scale },
				titlePx, title, { 0.92f, 0.92f, 0.97f, 1.0f });

			if (!node.Subtitle.empty())
			{
				const std::string& sub = CachedSubtitle(node, size.x - 8.0f);
				const glm::vec2 sw = m_FontCache->MeasureText(m_Font, sub, (float)QuantizedTextSize(m_BaseSubtitleSize));
				PaintTextScaled(dl, layer + 1, clip,
					{ pos.x + (size.x - sw.x) * 0.5f, pos.y + pad + contentH + titleH + 2.0f * scale },
					m_BaseSubtitleSize, sub, tokens::TextSecondary);
			}
		}
	}

	const std::string& GraphCanvas::CachedTitle(const ViewNode& node, float maxWidth) const
	{
		const int size = TitlePixels();
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

	void GraphCanvas::PaintConnect(DrawList& dl, int layer, int clip) const
	{
		if (m_Interaction != Interaction::Connect)
			return;
		const ViewNode* from = FindViewNode(m_ConnectFrom);
		if (!from)
			return;
		const ViewNode* to = FindViewNode(m_ConnectTarget);
		const glm::vec2 mouseWorld = ScreenToWorld(m_LastMouse);
		const glm::vec2 target = to ? to->Position + to->Size * 0.5f : mouseWorld;
		const glm::vec2 a = WorldToScreen(NodeBorderPoint(*from, target));
		const glm::vec2 b = to ? WorldToScreen(NodeBorderPoint(*to, from->Position + from->Size * 0.5f)) : m_LastMouse;
		const glm::vec4 colour = to ? tokens::Accent : tokens::TextSecondary;
		const float scale = ZoomScale(m_Transform.ZoomLevel);
		dl.AddLine(layer, a, b, std::clamp(2.0f * scale, 1.5f, 3.0f), colour, clip, to ? LineStyle::Solid : LineStyle::Dashed);
		const glm::vec2 dir = glm::normalize(b - a + glm::vec2(1e-4f));
		const glm::vec2 perp = { -dir.y, dir.x };
		const float ah = std::clamp(12.0f * scale, 7.0f, 16.0f);
		dl.AddTriangle(layer, b, colour, b - dir * ah + perp * ah * 0.45f, colour, b - dir * ah - perp * ah * 0.45f, colour, clip);
	}

	void GraphCanvas::PaintOverlays(DrawList& dl, int layer, int clip, const glm::vec4& worldClip) const
	{
		const float dpi = std::max(0.5f, m_DpiScale);
		const int px = (int)std::lround(tokens::FontSmall * dpi);
		const float inset = std::round(10.0f * dpi);

		// The breadcrumb: where this graph is.
		if (m_Schema)
		{
			const std::vector<std::string> path = m_Schema->Breadcrumb();
			if (!path.empty())
			{
				const std::string separator = "  \u203a  ";
				float width = 0.0f;
				for (size_t i = 0; i < path.size(); i++)
					width += m_FontCache->MeasureText(m_Font, path[i] + (i + 1 < path.size() ? separator : ""), (float)px).x;
				const glm::vec2 pad(8.0f * dpi, 4.0f * dpi);
				const glm::vec2 at = m_Origin + glm::vec2(inset);
				const float r = 5.0f * dpi;
				const float rr[4] = { r, r, r, r };
				dl.AddRoundedBox(layer, at, { width + pad.x * 2.0f, (float)px * 1.25f + pad.y * 2.0f }, tokens::WithAlpha(tokens::Panel, 0.85f), rr,
					1.0f, tokens::Border, clip);
				float x = at.x + pad.x;
				for (size_t i = 0; i < path.size(); i++)
				{
					const bool last = i + 1 == path.size();
					PaintTextPixels(dl, layer + 1, clip, { x, at.y + pad.y }, px, path[i], last ? tokens::TextPrimary : tokens::TextSecondary);
					x += m_FontCache->MeasureText(m_Font, path[i], (float)px).x;
					if (!last)
					{
						PaintTextPixels(dl, layer + 1, clip, { x, at.y + pad.y }, px, separator, tokens::TextSecondary);
						x += m_FontCache->MeasureText(m_Font, separator, (float)px).x;
					}
				}
			}
		}

		// The zoom, bottom left.
		{
			char zoom[16];
			std::snprintf(zoom, sizeof(zoom), "%d%%", (int)std::lround(ZoomScale(m_Transform.ZoomLevel) * 100.0f));
			PaintTextPixels(dl, layer + 1, clip, { m_Origin.x + inset, m_Origin.y + m_Size.y - inset - (float)px * 1.3f }, px, zoom,
				tokens::TextSecondary);
		}

		// The drop target.
		if (AcceptsDrag(UIApplication::IsValid() ? UIApplication::Get().GetDragOperation() : nullptr))
		{
			const float r = 4.0f * dpi;
			const float rr[4] = { r, r, r, r };
			dl.AddRoundedBox(layer, m_Origin + glm::vec2(2.0f), m_Size - glm::vec2(4.0f), tokens::WithAlpha(tokens::Accent, 0.06f), rr,
				2.0f, tokens::Accent, clip);
		}

		// The minimap: shown while some of the graph is out of view.
		m_MinimapRect = glm::vec4(0.0f);
		if (!m_ShowMinimap || m_View.Nodes.empty())
			return;
		bool outside = false;
		for (const ViewNode& node : m_View.Nodes)
			if (node.Position.x < worldClip.x || node.Position.y < worldClip.y || node.Position.x + node.Size.x > worldClip.x + worldClip.z
				|| node.Position.y + node.Size.y > worldClip.y + worldClip.w)
				outside = true;
		const glm::vec2 maxSize = glm::vec2(160.0f, 96.0f) * dpi;
		if (!outside || m_Size.x < maxSize.x * 2.4f || m_Size.y < maxSize.y * 2.4f)
			return;
		const glm::vec2 pad(6.0f * dpi);
		m_Minimap = ComputeMinimap(m_View, worldClip, maxSize - pad * 2.0f);
		if (!m_Minimap.Valid)
			return;
		const glm::vec2 frameSize = m_Minimap.Size + pad * 2.0f;
		const glm::vec2 frameAt = m_Origin + m_Size - frameSize - glm::vec2(inset);
		m_MinimapRect = { frameAt.x, frameAt.y, frameSize.x, frameSize.y };
		const float r = 6.0f * dpi;
		const float rr[4] = { r, r, r, r };
		dl.AddRoundedBox(layer, frameAt, frameSize, tokens::WithAlpha(tokens::PanelAlt, 0.94f), rr, 1.0f, tokens::BorderStrong, clip);
		const int mapClip = dl.PushClip({ frameAt.x, frameAt.y, frameSize.x, frameSize.y }, clip);
		auto toMap = [&](const glm::vec2& world) { return frameAt + pad + (world - m_Minimap.WorldMin) * m_Minimap.Scale; };
		for (const ViewNode& node : m_View.Nodes)
		{
			glm::vec4 colour = glm::mix(node.Fill, tokens::TextSecondary, 0.35f);
			if (node.Flags & NodeFlags::Active)
				colour = tokens::Success;
			else if (m_Selected.count(node.Id))
				colour = tokens::TextPrimary;
			const glm::vec2 size = glm::max(node.Size * m_Minimap.Scale, glm::vec2(2.0f));
			const float nr = std::min(2.0f * dpi, size.y * 0.5f);
			const float nrr[4] = { nr, nr, nr, nr };
			dl.AddRoundedBox(layer + 1, toMap(node.Position), size, colour, nrr, 0.0f, {}, mapClip);
		}
		const glm::vec2 viewAt = toMap({ worldClip.x, worldClip.y });
		const glm::vec2 viewSize = glm::vec2(worldClip.z, worldClip.w) * m_Minimap.Scale;
		const float zr[4] = { 2.0f, 2.0f, 2.0f, 2.0f };
		dl.AddRoundedBox(layer + 2, viewAt, viewSize, tokens::WithAlpha(tokens::Accent, 0.08f), zr, 1.5f, tokens::Accent, mapClip);
	}

	void GraphCanvas::PanMinimapTo(const glm::vec2& absPos)
	{
		if (!m_Minimap.Valid || m_MinimapRect.z <= 0.0f)
			return;
		const glm::vec2 pad(6.0f * std::max(0.5f, m_DpiScale));
		const glm::vec2 world = m_Minimap.WorldMin + (absPos - glm::vec2(m_MinimapRect.x, m_MinimapRect.y) - pad) / m_Minimap.Scale;
		// Centre the view on the clicked point.
		m_Transform.Pan = m_Size * 0.5f - world * ZoomScale(m_Transform.ZoomLevel);
		m_LastPolledMouse = glm::vec2(-1e9f, -1e9f);
	}

	const ViewNode* GraphCanvas::FindViewNode(NodeId id) const
	{
		auto it = m_NodeIndex.find(id);
		if (it == m_NodeIndex.end() || it->second >= m_View.Nodes.size())
			return nullptr;
		return &m_View.Nodes[it->second];
	}

	float GraphCanvas::RimWidth(const ViewNode& node) const
	{
		return std::min(8.0f / ZoomScale(m_Transform.ZoomLevel), node.Size.y * 0.22f);
	}

	void GraphCanvas::NotifySelection()
	{
		if (m_Schema)
			m_Schema->OnSelectionChanged(GetSelectedNodes());
	}

	std::vector<NodeId> GraphCanvas::GetSelectedNodes() const
	{
		// In view order, so the schema sees a stable order.
		std::vector<NodeId> out;
		for (const ViewNode& node : m_View.Nodes)
			if (m_Selected.count(node.Id))
				out.push_back(node.Id);
		return out;
	}

	glm::vec2 GraphCanvas::MouseGraphPosition() const
	{
		if (!UIApplication::IsValid())
			return ScreenToWorld(m_Origin + m_Size * 0.5f);
		const glm::vec2 mouse = UIApplication::Get().GetMousePos();
		if (mouse.x < m_Origin.x || mouse.y < m_Origin.y || mouse.x > m_Origin.x + m_Size.x || mouse.y > m_Origin.y + m_Size.y)
			return ScreenToWorld(m_Origin + m_Size * 0.5f); // off the canvas: its centre
		return ScreenToWorld(mouse);
	}

	bool GraphCanvas::AcceptsDrag(const DragOperation* op) const
	{
		if (!op || !m_Schema || !m_HasCachedGeometry)
			return false;
		const glm::vec2 at = op->CurrentPos;
		if (at.x < m_Origin.x || at.y < m_Origin.y || at.x > m_Origin.x + m_Size.x || at.y > m_Origin.y + m_Size.y)
			return false;
		return m_Schema->AcceptsDrop(op->Payload);
	}

	Reply GraphCanvas::OnDragEnter(DragOperation& op)
	{
		return m_Schema && m_Schema->AcceptsDrop(op.Payload) ? Reply::Handled_() : Reply::Unhandled();
	}

	Reply GraphCanvas::OnDragOver(DragOperation& op)
	{
		return m_Schema && m_Schema->AcceptsDrop(op.Payload) ? Reply::Handled_() : Reply::Unhandled();
	}

	Reply GraphCanvas::OnDragLeave(DragOperation&)
	{
		return Reply::Unhandled();
	}

	Reply GraphCanvas::OnDrop(DragOperation& op)
	{
		if (!m_Schema || !m_Schema->AcceptsDrop(op.Payload))
			return Reply::Unhandled();
		m_Schema->OnDrop(op.Payload, ScreenToWorld(op.CurrentPos));
		return Reply::Handled_();
	}

	void GraphCanvas::PaintTextScaled(DrawList& dl, int layer, int clip, const glm::vec2& at,
		float baseSize, const std::string& text, const glm::vec4& colour) const
	{
		PaintTextPixels(dl, layer, clip, at, QuantizedTextSize(baseSize), text, colour);
	}

	void GraphCanvas::PaintTextPixels(DrawList& dl, int layer, int clip, const glm::vec2& at,
		int pixels, const std::string& text, const glm::vec4& colour) const
	{
		const float size = (float)pixels;
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
		if (i == Interaction::PanMiddle || i == Interaction::PanRightPending || i == Interaction::NodeDrag)
			m_AutoFrame = false;
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
		// A menu over the canvas keeps the hover it opened on.
		if (UIApplication::Get().HasOpenPopups())
			return;
		m_LastPolledMouse = mouse;

		NodeId newHover = 0;
		int newEdge = -1;
		bool rim = false;
		const bool inMinimap = m_MinimapRect.z > 0.0f && mouse.x >= m_MinimapRect.x && mouse.y >= m_MinimapRect.y
			&& mouse.x <= m_MinimapRect.x + m_MinimapRect.z && mouse.y <= m_MinimapRect.y + m_MinimapRect.w;
		if (!inMinimap && mouse.x >= m_Origin.x && mouse.x <= m_Origin.x + m_Size.x
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
					rim = m_Schema && m_Schema->CanStartConnect(node.Id) && InNodeRim(node, world, RimWidth(node));
					break;
				}
			}
			// Off every node: the nearest edge in a ~6 px band.
			if (newHover == 0 && m_Schema)
				newEdge = NearestEdge(m_View, world, 6.0f / ZoomScale(m_Transform.ZoomLevel));
		}
		if (newHover != m_HoveredNode || newEdge != m_HoveredEdge)
		{
			m_HoveredNode = newHover;
			m_HoveredEdge = newEdge;
			// A new thing under the cursor restarts the tooltip's dwell.
			UIApplication::Get().ResetToolTip(this);
		}
		m_HasHover = newHover != 0;
		m_HoverRim = rim;
	}

} // namespace graph
} // namespace wisp
