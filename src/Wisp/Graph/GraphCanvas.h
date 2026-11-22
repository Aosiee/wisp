// The graph canvas.
//
// The reusable Wisp widget that draws a GraphView and owns the view
// transform, the selection, and the pan/zoom/hit input. It knows nothing
// about the domain - the schema (a consumer-owned GraphSchema) projects
// the domain into the view, paints the node content, and answers the
// clicks. The Reference Viewer is the first consumer; the RenderGraph
// viewer (F-1) is the second.

#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include "Wisp/Graph/GraphSchema.h"
#include "Wisp/Graph/GraphView.h"

#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace wisp {
namespace graph {

	// The level-of-detail tier: >= 0.5 full node, 0.25..0.5 title only,
	// below that the coloured boxes and the edges only.
	enum class LodTier : uint8_t { Full, TitleOnly, Boxes };
	LodTier LodTierFor(int zoomLevel);

	// The zoom anchor: the pan that keeps the world point under the
	// cursor fixed when the zoom level steps.
	glm::vec2 ZoomAnchorPan(const ViewTransform& transform, const glm::vec2& cursorLocal, int newLevel);

	// The adaptive bezier tessellation: the sample count for a
	// screen-space curve length (<= 64).
	int EdgeSampleCount(float screenLength);

	// The transform that frames the nodes (the empty subset = all) in a
	// viewport, choosing the largest zoom level that fits (Home/F).
	ViewTransform FrameTransform(const GraphView& view, const std::vector<NodeId>& subset,
		const glm::vec2& viewportSize);

	class GraphCanvas : public Widget
	{
	public:
		WISP_WIDGET_TYPE(GraphCanvas)

		GraphCanvas(FontCache* fontCache, FontHandle font, GraphSchema* schema, const std::string& viewStateKey);

		// Re-runs the schema build + the layout; the first build frames
		// the graph, the later ones preserve the view transform.
		void Rebuild();

		void FrameAll();
		void FrameSelection();

		void SetDrawDotGrid(bool b) { m_DrawDotGrid = b; }
		void SetViewStateKey(const std::string& key) { m_ViewStateKey = key; }

		const ViewTransform& GetTransform() const { return m_Transform; }
		void SetTransform(const ViewTransform& t) { m_Transform = t; }

		glm::vec2 ComputeDesiredSize() const override { return { 640.0f, 480.0f }; }
		void ArrangeChildren(const Geometry&, ArrangedChildren&) const override {}

		int OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList,
			int layerId, const WidgetStyleContext& style) const override;

		Reply OnMouseWheel(const glm::vec2& absPos, float delta) override;
		Reply OnMouseButtonDown(const glm::vec2& absPos, int button) override;
		Reply OnMouseButtonUp(const glm::vec2& absPos, int button) override;
		Reply OnMouseMove(const glm::vec2& absPos) override;
		Reply OnKeyDown(int key, int mods) override;

	private:
		enum class Interaction : uint8_t { None, PanMiddle, PanRightPending, PanRightDrag, Marquee, NodeDrag };

		// The screen-space mapping: screen = origin + world * scale + pan.
		glm::vec2 WorldToScreen(const glm::vec2& world) const { return m_Origin + world * ZoomScale(m_Transform.ZoomLevel) + m_Transform.Pan; }
		glm::vec2 ScreenToWorld(const glm::vec2& screen) const { return (screen - m_Origin - m_Transform.Pan) / ZoomScale(m_Transform.ZoomLevel); }

		// The quantized pixel size for a base size at the current
		// zoom level and the ui scale.
		int QuantizedTextSize(float baseSize) const
		{
			return std::max(1, (int)std::lround(baseSize * ZoomScale(m_Transform.ZoomLevel)
				* std::max(0.5f, m_DpiScale)));
		}

		// The readability floor: below ~6 px the titles are sub-pixel, so
		// the title tier skips them.
		bool TextLegible(float baseSize) const { return QuantizedTextSize(baseSize) >= 6; }

		bool NodeVisible(const ViewNode& node, const glm::vec4& worldClip) const;
		void PaintDotGrid(DrawList& dl, int layer, int clip, const glm::vec4& worldClip) const;
		void PaintEdges(DrawList& dl, int layer, int clip, const glm::vec4& worldClip, const glm::vec2& viewport) const;
		void PaintArrowheads(DrawList& dl, int layer, int clip, const glm::vec2& viewport) const;
		void PaintNodes(DrawList& dl, int layer, int clip, const glm::vec4& worldClip) const;
		void PaintMarquee(DrawList& dl, int layer, int clip) const;
		void PaintTextScaled(DrawList& dl, int layer, int clip, const glm::vec2& at, float baseSize,
			const std::string& text, const glm::vec4& colour) const;
		void UpdateHover() const;

		// The screen-space edge control points via the node index map
		// (the O(1) lookups instead of the linear scans).
		bool EdgeScreenPoints(size_t edgeIndex, glm::vec2& s0, glm::vec2& s1, glm::vec2& s2, glm::vec2& s3) const;

		// The truncated title/subtitle, cached per node per quantized
		// size (the measure + the chop runs once, not per paint).
		const std::string& CachedTitle(const ViewNode& node, float maxWidth) const;
		const std::string& CachedSubtitle(const ViewNode& node, float maxWidth) const;

		void LoadViewState();
		void SaveViewState() const;

		// The schema build + the per-build cache bookkeeping (shared by
		// the eager Rebuild and the paint's deferred first build).
		void DoBuild() const;

		void BeginInteraction(Interaction i, const glm::vec2& anchor);
		void EndInteraction();
		void OpenContextMenu(NodeId node, const glm::vec2& at);
		void SetSelection(const std::vector<NodeId>& nodes, bool add);

		FontCache* m_FontCache = nullptr;
		FontHandle m_Font = 0;
		GraphSchema* m_Schema = nullptr;
		std::string m_ViewStateKey;

		// The view data the paint rebuilds (mutable - OnPaint is const).
		mutable GraphView m_View;
		mutable NodeMap m_NodeToAsset;
		mutable ViewTransform m_Transform;
		mutable bool m_NeedsRebuild = true;
		mutable bool m_FirstBuild = true;
		// Until the user moves the view, a new viewport size (the panel
		// docked, resized, first laid out) frames the graph again.
		mutable bool m_AutoFrame = true;
		mutable glm::vec2 m_FramedSize{ 0.0f };
		mutable float m_DpiScale = 1.0f;
		mutable glm::vec2 m_Origin{};
		mutable glm::vec2 m_Size{};
		mutable bool m_HasCachedGeometry = false;
		mutable NodeId m_HoveredNode = 0;
		mutable bool m_HasHover = false;
		mutable glm::vec2 m_LastPolledMouse{ -1e9f, -1e9f };

		// The per-build and per-transform caches (the 500-node budget:
		// the edge tessellation only re-runs when the transform moves,
		// the node lookups are O(1), the truncation runs once per node
		// per text size).
		mutable uint64_t m_ViewGeneration = 0;
		mutable std::unordered_map<NodeId, size_t> m_NodeIndex;
		mutable std::vector<std::vector<glm::vec2>> m_EdgePoints;
		mutable ViewTransform m_EdgeCacheTransform;
		mutable bool m_EdgeCacheValid = false;
		mutable std::unordered_map<NodeId, std::pair<int, std::string>> m_TitleCache;
		mutable std::unordered_map<NodeId, std::pair<int, std::string>> m_SubtitleCache;

		std::unordered_set<NodeId> m_Selected;
		bool m_DrawDotGrid = false;
		float m_BaseTitleSize = 13.0f;
		float m_BaseSubtitleSize = 11.0f;

		// The interaction state (the events arrive window-absolute; the
		// origin/size are cached by the last paint - the profiler pattern).
		Interaction m_Interaction = Interaction::None;
		NodeId m_InteractionNode = 0;
		std::vector<std::pair<NodeId, glm::vec2>> m_DragStart;
		glm::vec2 m_Anchor{};
		glm::vec2 m_AnchorScreen{};
		glm::vec2 m_LastMouse{};
		bool m_MovedPastThreshold = false;
		double m_LastClickTime = -1e9;
		NodeId m_LastClickNode = 0;
	};

} // namespace graph
} // namespace wisp
