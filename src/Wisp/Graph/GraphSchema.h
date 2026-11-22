// The graph schema.
//
// The interface a domain projects itself through. The implementations
// live WITH their consumer (the Reference Viewer's AssetReferenceSchema
// is in the editor plugin; a future visual-script/serialized graph is an
// asset whose model lives in engine/assetio) - the domain never lives in
// Wisp. The schema owns its query state (the root, the depth, the
// duplicates flag); the canvas re-runs Build() when the consumer calls
// Rebuild() (the freshness hook).

#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/Graph/GraphLayout.h"
#include "Wisp/Graph/GraphView.h"

#include <string>
#include <utility>
#include <vector>

namespace wisp {
namespace graph {

	struct GraphSchema
	{
		virtual ~GraphSchema() = default;

		// Projects the domain into the view (the geometry + the
		// title/subtitle + the flags via the layout) and fills the
		// node -> asset map (the click/tooltip reverse lookup).
		virtual void Build(GraphView& out, NodeMap& nodeToAsset) = 0;

		// The domain content inside the node (the thumbnail / the type
		// badge); the canvas owns the shell, the title, the subtitle.
		virtual void PaintNodeContent(DrawList&, int, int,
			const glm::vec2&, const glm::vec2&, const ViewNode&, int) const {}

		virtual std::string TooltipText(NodeId) const { return {}; }

		virtual void OnClick(NodeId) {}         // the plain click (e.g. the recenter)
		virtual void OnActivate(NodeId) {}      // the double-click

		// The right-click: the schema shows its own menu (the shared
		// asset menu for the asset surfaces). The default does nothing.
		virtual void ShowContextMenu(NodeId, const glm::vec2&) {}

		virtual bool NodesMovable() const { return false; }
		virtual bool PersistPositions() const { return false; }
		// An editing schema's hooks (a viewer leaves them be): the nodes a
		// drag moved and where (graph space), a click on an edge (its index
		// in the view), a click on the empty canvas, a right-click there
		// (the graph-space point and the screen point for a menu), and a key
		// the canvas has focus for (true = handled).
		virtual void OnNodesMoved(const std::vector<std::pair<NodeId, glm::vec2>>&) {}
		virtual void OnEdgeClick(size_t) {}
		virtual void OnBackgroundClick() {}
		// A double-click on empty canvas, at its graph-space position.
		virtual void OnBackgroundActivate(const glm::vec2&) {}
		virtual void ShowBackgroundMenu(const glm::vec2&, const glm::vec2&) {}
		virtual bool OnKey(int) { return false; }

		// One node's drag (default: every node or none, NodesMovable).
		virtual bool NodeMovable(NodeId) const { return NodesMovable(); }
		// Snapped node drags and the arrow-key nudge step (0 = free).
		virtual float SnapGrid() const { return 0.0f; }
		// The selection the canvas changed (a marquee, Ctrl+click, Ctrl+A,
		// Escape). A schema owning the selection flags the nodes Selected in
		// its next Build; a rebuild keeps the flagged nodes selected.
		virtual void OnSelectionChanged(const std::vector<NodeId>&) {}

		// Connecting: a drag from a node's rim (or a Shift+drag) to another
		// node, drawn as a rubber band.
		virtual bool CanStartConnect(NodeId) const { return false; }
		virtual bool CanConnect(NodeId, NodeId) const { return true; }
		virtual void OnConnect(NodeId, NodeId) {}

		virtual std::string EdgeTooltipText(size_t) const { return {}; }
		// Live state read at every paint (no rebuild per frame): a fill
		// running along the edge (0..1, below 0 none) and a fading glow (0..1).
		virtual float EdgeProgress(size_t) const { return -1.0f; }
		virtual float EdgeFlash(size_t) const { return 0.0f; }

		// The path drawn in the canvas's top-left corner (the last segment
		// brightest).
		virtual std::vector<std::string> Breadcrumb() const { return {}; }

		// A drag-and-drop payload dropped on the canvas, at its graph-space
		// point.
		virtual bool AcceptsDrop(const DragPayload&) const { return false; }
		virtual void OnDrop(const DragPayload&, const glm::vec2&) {}
	};

} // namespace graph
} // namespace wisp
