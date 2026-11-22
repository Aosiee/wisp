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

#include "Wisp/DrawList.h"
#include "Wisp/Graph/GraphLayout.h"
#include "Wisp/Graph/GraphView.h"

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
		virtual void ShowBackgroundMenu(const glm::vec2&, const glm::vec2&) {}
		virtual bool OnKey(int) { return false; }
	};

} // namespace graph
} // namespace wisp
