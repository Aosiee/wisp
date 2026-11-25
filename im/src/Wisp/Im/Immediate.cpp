#include "Wisp/Im/Immediate.h"

#include "Wisp/Application.h"
#include "Wisp/Components/BoxPanel.h"
#include "Wisp/Components/Border.h"
#include "Wisp/Components/Button.h"
#include "Wisp/Components/CheckBox.h"
#include "Wisp/Components/ColourPicker.h"
#include "Wisp/Components/Dropdown.h"
#include "Wisp/Components/EditableText.h"
#include "Wisp/Components/GraphData.h"
#include "Wisp/Components/Image.h"
#include "Wisp/Components/ProgressBar.h"
#include "Wisp/Components/RadioButtons.h"
#include "Wisp/Components/ScrollBox.h"
#include "Wisp/Components/Separator.h"
#include "Wisp/Components/SizeBox.h"
#include "Wisp/Components/Slider.h"
#include "Wisp/Components/Spacer.h"
#include "Wisp/Components/Sparkline.h"
#include "Wisp/Components/TextBlock.h"
#include "Wisp/Components/ValueBox.h"
#include "Wisp/Kit/PanelKit.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wisp::im {

	struct Context::Node
	{
		Id NodeId = 0;
		Kind K = Kind::Root;
		Node* Parent = nullptr;
		uint64_t Frame = 0; // the last frame a call reached it

		wisp::Widget* Outer = nullptr;   // what sits in the parent's box
		wisp::Widget* Control = nullptr; // what the call updates
		BoxPanel* Box = nullptr;         // a container's box
		size_t Reserved = 0;             // the box's leading children the container keeps (a field's key)
		std::unique_ptr<wisp::Widget> Pending; // made or moved this frame, not yet in the parent's box
		Slot SlotData;
		Slot Preferred; // the slot the item asks for when Next() set none
		bool HasPreferred = false;
		std::vector<Node*> Children; // in box order, after the reserved ones
		const void* Tag = nullptr;   // Widget<T>'s type

		// What the widget reported since the last call, and what the call
		// last gave it.
		bool Clicked = false;
		bool Changed = false;
		bool B = false;
		int I = 0;
		float F = 0.0f;
		glm::vec4 C = { 0.0f, 0.0f, 0.0f, 0.0f };
		glm::vec2 Size = { 0.0f, 0.0f };
		std::string S;
		std::string Hint;
		std::vector<std::string> Options;
		std::vector<float> Values;
		std::unique_ptr<GraphSeries> Series;
		TextBlock* Label = nullptr;
		int Band = -1;
	};

	struct Context::Scope
	{
		Node* Container = nullptr;
		Kind K = Kind::Root;
		std::vector<Node*> Next; // this frame's children, in call order
		uint32_t Ordinals[(size_t)Kind::Count] = {};
		size_t IdDepth = 0;
		float Indent = 0.0f;
		int Band = 0;
		int Columns = 0;
		bool CellSlots = false; // a field's or a table row's items fill and centre
	};

	namespace {

		constexpr Id kRootSeed = 0x77697370696d0001ull; // "wispim" 1
		constexpr uint64_t kFnvPrime = 0x100000001b3ull;

		Id Mix(Id h)
		{
			h ^= h >> 33;
			h *= 0xff51afd7ed558ccdull;
			h ^= h >> 33;
			h *= 0xc4ceb9fe1a85ec53ull;
			h ^= h >> 33;
			return h;
		}

		Id Hash(Id seed, uint32_t salt, std::string_view bytes)
		{
			Id h = Mix(seed ^ (0x9e3779b97f4a7c15ull * (salt + 1)));
			for (char c : bytes)
			{
				h ^= (uint8_t)c;
				h *= kFnvPrime;
			}
			return Mix(h);
		}

		// "Label##key" shows "Label".
		std::string_view Visible(std::string_view label)
		{
			const size_t hashes = label.find("##");
			return hashes == std::string_view::npos ? label : label.substr(0, hashes);
		}

		bool SameSlot(const Slot& a, const Slot& b)
		{
			return a.PaddingLeft == b.PaddingLeft && a.PaddingTop == b.PaddingTop && a.PaddingRight == b.PaddingRight
				&& a.PaddingBottom == b.PaddingBottom && a.Align.H == b.Align.H && a.Align.V == b.Align.V
				&& a.FillWeight == b.FillWeight && a.MinWidth == b.MinWidth && a.MinHeight == b.MinHeight
				&& a.MaxWidth == b.MaxWidth && a.MaxHeight == b.MaxHeight;
		}

		float Normalise(float value, float min, float max)
		{
			return max > min ? (value - min) / (max - min) : 0.0f;
		}

	} // namespace

	Context::Context(FontCache* fonts, FontHandle font)
		: m_Fonts(fonts), m_Font(font), m_Root(std::make_unique<Node>())
	{
		m_Root->NodeId = kRootSeed;
		m_Root->K = Kind::Root;
		m_Scopes.resize(1);
	}

	Context::~Context() = default;

	// ---- The frame ----

	void Context::BeginFrame(BoxPanel* root)
	{
		m_Frame++;
		m_Root->Box = root;
		m_Root->Frame = m_Frame;
		m_Scopes.resize(std::max<size_t>(m_Scopes.size(), 1));
		m_Depth = 0;
		Scope& scope = m_Scopes[0];
		scope.Container = m_Root.get();
		scope.K = Kind::Root;
		scope.Next.clear();
		std::memset(scope.Ordinals, 0, sizeof(scope.Ordinals));
		scope.Indent = 0.0f;
		scope.Band = 0;
		scope.Columns = 0;
		scope.CellSlots = false;
		m_IdStack.clear();
		m_IdStack.push_back(kRootSeed);
		scope.IdDepth = 0;
		m_Next = {};
	}

	void Context::EndFrame()
	{
		m_UnbalancedScopes = 0;
		while (m_Depth > 0)
		{
			m_UnbalancedScopes++;
			if (m_Scopes[m_Depth].K == Kind::TableRow)
				CloseRow();
			else
				CloseScope(m_Scopes[m_Depth].K);
		}
		Reconcile(m_Scopes[0]);
		m_Next = {};
	}

	// ---- Ids and nodes ----

	Id Context::MakeId(Kind kind, std::string_view label)
	{
		Id id = Hash(m_IdStack.back(), (uint32_t)kind, label);
		// A repeated id in the same frame takes the next one in call order.
		for (auto it = m_Nodes.find(id); it != m_Nodes.end() && it->second->Frame == m_Frame; it = m_Nodes.find(id))
			id = Mix(id + 0x9e3779b97f4a7c15ull);
		return id;
	}

	Id Context::MakePositionalId(Kind kind)
	{
		const uint32_t ordinal = m_Scopes[m_Depth].Ordinals[(size_t)kind]++;
		char bytes[sizeof(ordinal) + 1];
		bytes[0] = '#';
		std::memcpy(bytes + 1, &ordinal, sizeof(ordinal));
		return MakeId(kind, std::string_view(bytes, sizeof(bytes)));
	}

	Context::Node* Context::Find(Id id, Kind kind)
	{
		auto it = m_Nodes.find(id);
		if (it == m_Nodes.end())
			return nullptr;
		Node* node = it->second.get();
		if (node->K != kind || node->Parent != m_Scopes[m_Depth].Container)
		{
			Detach(node);
			return nullptr;
		}
		return node;
	}

	Context::Node* Context::Create(Id id, Kind kind, std::unique_ptr<wisp::Widget> outer)
	{
		auto node = std::make_unique<Node>();
		Node* raw = node.get();
		raw->NodeId = id;
		raw->K = kind;
		raw->Parent = m_Scopes[m_Depth].Container;
		raw->Pending = std::move(outer);
		raw->Outer = raw->Pending.get();
		raw->Control = raw->Outer;
		m_Nodes.emplace(id, std::move(node));
		return raw;
	}

	void Context::Reach(Node* node, const Slot* preferred)
	{
		Scope& scope = m_Scopes[m_Depth];
		node->Frame = m_Frame;
		node->Parent = scope.Container;
		Slot slot;
		if (m_Next.IsSet)
			slot = m_Next.Data;
		else if (preferred)
			slot = *preferred;
		else if (node->HasPreferred)
			slot = node->Preferred;
		else if (scope.CellSlots)
		{
			slot.FillWeight = 1.0f;
			slot.Align.V = wisp::VAlign::Center;
		}
		slot.PaddingLeft += scope.Indent;
		node->SlotData = slot;
		m_Next = {};
		scope.Next.push_back(node);
	}

	void Context::OpenScope(Node* container, Kind kind)
	{
		m_Depth++;
		if (m_Scopes.size() <= m_Depth)
			m_Scopes.resize(m_Depth + 1);
		Scope& scope = m_Scopes[m_Depth];
		scope.Container = container;
		scope.K = kind;
		scope.Next.clear();
		std::memset(scope.Ordinals, 0, sizeof(scope.Ordinals));
		scope.IdDepth = m_IdStack.size();
		scope.Indent = 0.0f;
		scope.Band = 0;
		scope.Columns = 0;
		scope.CellSlots = kind == Kind::Field || kind == Kind::TableRow;
		m_IdStack.push_back(container->NodeId);
	}

	void Context::CloseScope(Kind kind)
	{
		if (m_Depth == 0)
		{
			m_UnbalancedScopes++;
			return;
		}
		Scope& scope = m_Scopes[m_Depth];
		if (scope.K != kind)
			m_UnbalancedScopes++;
		Reconcile(scope);
		m_IdStack.resize(scope.IdDepth);
		m_Depth--;
	}

	void Context::CloseRow()
	{
		Scope& row = m_Scopes[m_Depth];
		const int columns = m_Depth > 0 ? m_Scopes[m_Depth - 1].Columns : 0;
		while ((int)row.Next.size() < columns)
		{
			Next().Fill(1.0f);
			Spacing(0.0f, 0.0f);
		}
		CloseScope(Kind::TableRow);
	}

	void Context::Reconcile(Scope& scope)
	{
		Node* container = scope.Container;
		BoxPanel* box = container->Box;
		if (!box)
			return;
		bool same = scope.Next.size() == container->Children.size();
		for (size_t i = 0; same && i < scope.Next.size(); i++)
			same = scope.Next[i] == container->Children[i] && !scope.Next[i]->Pending;
		if (same)
		{
			for (size_t i = 0; i < scope.Next.size(); i++)
			{
				Slot& current = box->GetSlot(container->Reserved + i);
				if (!SameSlot(current, scope.Next[i]->SlotData))
				{
					current = scope.Next[i]->SlotData;
					box->InvalidateLayout();
				}
			}
			return;
		}
		// The order changed: every child comes out (from the back, so nothing
		// shifts), the unreached ones are destroyed, and this frame's go back
		// in call order.
		for (size_t i = container->Children.size(); i-- > 0;)
		{
			Node* child = container->Children[i];
			std::unique_ptr<wisp::Widget> widget = box->ReleaseChild(container->Reserved + i);
			if (child->Frame == m_Frame && child->Parent == container)
				child->Pending = std::move(widget);
			else
			{
				widget.reset();
				Forget(child);
			}
		}
		container->Children.clear();
		for (Node* child : scope.Next)
		{
			box->InsertChild(box->GetChildCount(), std::move(child->Pending), child->SlotData);
			container->Children.push_back(child);
		}
	}

	void Context::ClearChildren(Node* container)
	{
		for (size_t i = container->Children.size(); i-- > 0;)
		{
			Node* child = container->Children[i];
			container->Box->ReleaseChild(container->Reserved + i).reset();
			Forget(child);
		}
		container->Children.clear();
	}

	void Context::Detach(Node* node)
	{
		Node* parent = node->Parent;
		auto it = parent ? std::find(parent->Children.begin(), parent->Children.end(), node) : std::vector<Node*>::iterator {};
		if (parent && it != parent->Children.end())
		{
			const size_t index = (size_t)(it - parent->Children.begin());
			parent->Box->ReleaseChild(parent->Reserved + index).reset();
			parent->Children.erase(it);
		}
		else
			node->Pending.reset();
		Forget(node);
	}

	void Context::Forget(Node* node)
	{
		for (Node* child : node->Children)
			Forget(child);
		const Id id = node->NodeId;
		m_Nodes.erase(id);
	}

	bool Context::Options(Node* node, std::span<const std::string> options)
	{
		if (node->Options.size() == options.size() && std::equal(options.begin(), options.end(), node->Options.begin()))
			return false;
		node->Options.assign(options.begin(), options.end());
		return true;
	}

	// ---- Ids ----

	void Context::PushId(std::string_view id)
	{
		m_IdStack.push_back(Hash(m_IdStack.back(), 0xffffu, id));
	}

	void Context::PushId(int64_t id)
	{
		char bytes[sizeof(id)];
		std::memcpy(bytes, &id, sizeof(id));
		m_IdStack.push_back(Hash(m_IdStack.back(), 0xfffeu, std::string_view(bytes, sizeof(bytes))));
	}

	void Context::PopId()
	{
		if (m_IdStack.size() > m_Scopes[m_Depth].IdDepth + 1)
			m_IdStack.pop_back();
	}

	// ---- Layout ----

	SlotOptions& Context::Next()
	{
		return m_Next;
	}

	void Context::Indent(float width)
	{
		m_Scopes[m_Depth].Indent += width < 0.0f ? m_Style.IndentWidth : width;
	}

	void Context::Unindent(float width)
	{
		float& indent = m_Scopes[m_Depth].Indent;
		indent = std::max(0.0f, indent - (width < 0.0f ? m_Style.IndentWidth : width));
	}

	Context::Node* Context::PlaceContainer(Kind kind, std::string_view id, const Slot* preferred,
		const std::function<std::unique_ptr<wisp::Widget>(Node&)>& make)
	{
		const Id nodeId = id.empty() ? MakePositionalId(kind) : MakeId(kind, id);
		Node* node = Find(nodeId, kind);
		if (!node)
		{
			node = Create(nodeId, kind, nullptr);
			node->Pending = make(*node);
			node->Outer = node->Pending.get();
			if (!node->Control)
				node->Control = node->Outer;
		}
		Reach(node, preferred);
		OpenScope(node, kind);
		return node;
	}

	void Context::BeginRow(std::string_view id)
	{
		PlaceContainer(Kind::Row, id, nullptr, [](Node& node) {
			auto box = std::make_unique<BoxPanel>(BoxOrientation::Horizontal);
			node.Box = box.get();
			return box;
		});
	}

	void Context::EndRow() { CloseScope(Kind::Row); }

	void Context::BeginColumn(std::string_view id)
	{
		PlaceContainer(Kind::Column, id, nullptr, [](Node& node) {
			auto box = std::make_unique<BoxPanel>(BoxOrientation::Vertical);
			node.Box = box.get();
			return box;
		});
	}

	void Context::EndColumn() { CloseScope(Kind::Column); }

	void Context::BeginScroll(std::string_view id, float height)
	{
		Node* node = PlaceContainer(Kind::Scroll, id, nullptr, [height](Node& node) {
			auto scroll = std::make_unique<ScrollBox>();
			scroll->SetFitWidth(true);
			if (height > 0.0f)
				scroll->SetViewportHeight(height);
			auto column = std::make_unique<BoxPanel>(BoxOrientation::Vertical);
			node.Box = column.get();
			node.F = height;
			scroll->SetChild(std::move(column));
			return scroll;
		});
		if (node->F != height && height > 0.0f)
		{
			static_cast<ScrollBox*>(node->Control)->SetViewportHeight(height);
			node->F = height;
		}
	}

	void Context::EndScroll() { CloseScope(Kind::Scroll); }

	void Context::BeginField(std::string_view label)
	{
		Slot preferred;
		preferred.SetPadding(m_Style.FieldPaddingX, m_Style.FieldPaddingY);
		const std::string_view visible = Visible(label);
		const float keyWidth = m_Style.FieldKeyWidth;
		Node* node = PlaceContainer(Kind::Field, label, &preferred, [&](Node& node) {
			auto row = std::make_unique<BoxPanel>(BoxOrientation::Horizontal);
			auto key = std::make_unique<SizeBox>();
			key->SetWidthOverride(keyWidth);
			auto text = std::make_unique<TextBlock>(m_Fonts, m_Font, m_Style.FontSize, std::string(visible), tokens::TextSecondary);
			node.Label = text.get();
			key->SetChild(std::move(text));
			node.Control = key.get();
			node.F = keyWidth;
			*row + row->AddSlot().VAlign(wisp::VAlign::Center)[std::move(key)];
			node.Box = row.get();
			node.Reserved = 1;
			return row;
		});
		if (node->Label->GetText() != visible)
			node->Label->SetText(std::string(visible));
		if (node->F != keyWidth)
		{
			static_cast<SizeBox*>(node->Control)->SetWidthOverride(keyWidth);
			node->F = keyWidth;
		}
	}

	void Context::EndField() { CloseScope(Kind::Field); }

	bool Context::PlaceDisclosure(Kind kind, std::string_view label, bool defaultOpen)
	{
		const Id id = MakeId(kind, label);
		Node* node = Find(id, kind);
		if (!node)
		{
			auto body = std::make_unique<BoxPanel>(BoxOrientation::Vertical);
			BoxPanel* bodyRaw = body.get();
			auto disclosure = std::make_unique<kit::Disclosure>(m_Fonts, m_Font, std::string(Visible(label)), std::move(body), defaultOpen);
			if (kind == Kind::Tree)
				disclosure->GetSlot(1).PaddingLeft = m_Style.IndentWidth;
			node = Create(id, kind, std::move(disclosure));
			node->Box = bodyRaw;
		}
		Reach(node, nullptr);
		const bool open = static_cast<kit::Disclosure*>(node->Control)->IsOpen();
		if (open)
			OpenScope(node, kind);
		else
			ClearChildren(node);
		return open;
	}

	bool Context::Begin(std::string_view title) { return PlaceDisclosure(Kind::Section, title, true); }
	void Context::End() { CloseScope(Kind::Section); }
	bool Context::TreeNode(std::string_view label, bool defaultOpen) { return PlaceDisclosure(Kind::Tree, label, defaultOpen); }
	void Context::TreePop() { CloseScope(Kind::Tree); }

	void Context::BeginTable(std::string_view id, int columns)
	{
		PlaceContainer(Kind::Table, id, nullptr, [](Node& node) {
			auto column = std::make_unique<BoxPanel>(BoxOrientation::Vertical);
			node.Box = column.get();
			return column;
		});
		m_Scopes[m_Depth].Columns = std::max(columns, 0);
	}

	void Context::TableNextRow()
	{
		if (m_Scopes[m_Depth].K == Kind::TableRow)
			CloseRow();
		if (m_Scopes[m_Depth].K != Kind::Table)
		{
			m_UnbalancedScopes++;
			return;
		}
		PlaceContainer(Kind::TableRow, {}, nullptr, [](Node& node) {
			auto row = std::make_unique<BoxPanel>(BoxOrientation::Horizontal);
			node.Box = row.get();
			return row;
		});
	}

	void Context::EndTable()
	{
		if (m_Scopes[m_Depth].K == Kind::TableRow)
			CloseRow();
		CloseScope(Kind::Table);
	}

	void Context::Separator()
	{
		const Id id = MakePositionalId(Kind::Separator);
		Node* node = Find(id, Kind::Separator);
		if (!node)
			node = Create(id, Kind::Separator, std::make_unique<wisp::Separator>());
		Reach(node, nullptr);
	}

	void Context::Spacing(float width, float height)
	{
		const Id id = MakePositionalId(Kind::Spacing);
		Node* node = Find(id, Kind::Spacing);
		if (!node)
			node = Create(id, Kind::Spacing, std::make_unique<Spacer>(glm::vec2(width, height)));
		else
			static_cast<Spacer*>(node->Control)->SetSize({ width, height });
		Reach(node, nullptr);
	}

	// ---- Read-outs ----

	void Context::Text(std::string_view text)
	{
		TextColoured(tokens::TextPrimary, text);
	}

	void Context::TextColoured(const glm::vec4& colour, std::string_view text)
	{
		const Id id = MakePositionalId(Kind::Text);
		Node* node = Find(id, Kind::Text);
		if (!node)
			node = Create(id, Kind::Text, std::make_unique<TextBlock>(m_Fonts, m_Font, m_Style.FontSize, std::string(text), colour));
		else
		{
			auto* block = static_cast<TextBlock*>(node->Control);
			if (block->GetText() != text)
				block->SetText(std::string(text));
			block->SetColour(colour);
		}
		Reach(node, nullptr);
	}

	void Context::Heading(std::string_view text)
	{
		const Id id = MakeId(Kind::Heading, text);
		Node* node = Find(id, Kind::Heading);
		if (!node)
		{
			kit::SlottedWidget heading = kit::MakeSectionHeading(m_Fonts, m_Font, std::string(Visible(text)));
			node = Create(id, Kind::Heading, std::move(heading.Content));
			node->Preferred = heading.SlotData;
			node->HasPreferred = true;
		}
		Reach(node, nullptr);
	}

	void Context::KeyValue(std::string_view key, std::string_view value)
	{
		const int band = m_Scopes[m_Depth].Band++;
		const Id id = MakeId(Kind::KeyValue, key);
		Node* node = Find(id, Kind::KeyValue);
		if (!node)
		{
			TextBlock* text = nullptr;
			node = Create(id, Kind::KeyValue, kit::MakeKeyValue(m_Fonts, m_Font, std::string(Visible(key)), band, &text));
			node->Control = text;
			node->Band = band;
		}
		else if ((node->Band % 2) != (band % 2))
		{
			static_cast<Border*>(node->Outer)->SetBrush({ (band % 2) ? tokens::PanelAlt : tokens::Panel });
			node->Band = band;
		}
		auto* text = static_cast<TextBlock*>(node->Control);
		if (text->GetText() != value)
			text->SetText(std::string(value));
		Reach(node, nullptr);
	}

	void Context::ProgressBar(float fraction)
	{
		const Id id = MakePositionalId(Kind::Progress);
		Node* node = Find(id, Kind::Progress);
		if (!node)
			node = Create(id, Kind::Progress, std::make_unique<wisp::ProgressBar>());
		static_cast<wisp::ProgressBar*>(node->Control)->SetValue(fraction);
		Reach(node, nullptr);
	}

	void Context::Plot(std::string_view id, std::span<const float> values, float height)
	{
		const Id nodeId = MakeId(Kind::Plot, id);
		Node* node = Find(nodeId, Kind::Plot);
		if (!node)
		{
			auto box = std::make_unique<SizeBox>();
			box->SetHeightOverride(height);
			auto line = std::make_unique<Sparkline>();
			line->SetColor(m_Style.PlotColour);
			Sparkline* lineRaw = line.get();
			box->SetChild(std::move(line));
			node = Create(nodeId, Kind::Plot, std::move(box));
			node->Control = lineRaw;
			node->F = height;
			node->Values.assign(1, std::nanf("")); // never equal: the first frame fills the series
		}
		auto* line = static_cast<Sparkline*>(node->Control);
		if (node->Values.size() != values.size() || !std::equal(values.begin(), values.end(), node->Values.begin()))
		{
			const uint32_t needed = std::max<uint32_t>(2, (uint32_t)values.size());
			if (!node->Series || (uint32_t)node->I < needed)
			{
				node->Series = std::make_unique<GraphSeries>(needed);
				node->I = (int)needed;
				line->SetSeries(node->Series.get());
			}
			node->Series->Clear();
			for (float value : values)
				node->Series->Push(value);
			line->SetWindow((uint32_t)values.size());
			node->Values.assign(values.begin(), values.end());
		}
		if (node->F != height)
		{
			static_cast<SizeBox*>(node->Outer)->SetHeightOverride(height);
			node->F = height;
		}
		Reach(node, nullptr);
	}

	void Context::Image(const Brush& brush, const glm::vec2& size)
	{
		const Id id = MakePositionalId(Kind::Image);
		Node* node = Find(id, Kind::Image);
		if (!node)
		{
			auto image = std::make_unique<wisp::Image>(brush);
			image->SetOverrideSize(size);
			node = Create(id, Kind::Image, std::move(image));
			node->Size = size;
		}
		auto* image = static_cast<wisp::Image*>(node->Control);
		if (!(image->GetBrush() == brush))
			image->SetBrush(brush);
		if (node->Size != size)
		{
			image->SetOverrideSize(size);
			node->Size = size;
		}
		Reach(node, nullptr);
	}

	// ---- Controls ----

	bool Context::Button(std::string_view label)
	{
		const Id id = MakeId(Kind::Button, label);
		const std::string_view visible = Visible(label);
		Node* node = Find(id, Kind::Button);
		if (!node)
		{
			auto button = std::make_unique<wisp::Button>();
			auto text = std::make_unique<TextBlock>(m_Fonts, m_Font, m_Style.FontSize, std::string(visible), tokens::TextPrimary);
			TextBlock* textRaw = text.get();
			button->SetChild(std::move(text));
			wisp::Button* buttonRaw = button.get();
			node = Create(id, Kind::Button, std::move(button));
			node->Label = textRaw;
			buttonRaw->SetOnClicked([node] { node->Clicked = true; });
		}
		else if (node->Label->GetText() != visible)
			node->Label->SetText(std::string(visible));
		Reach(node, nullptr);
		return std::exchange(node->Clicked, false);
	}

	bool Context::Checkbox(std::string_view label, bool* value)
	{
		const Id id = MakeId(Kind::Checkbox, label);
		const std::string_view visible = Visible(label);
		Node* node = Find(id, Kind::Checkbox);
		if (!node)
		{
			auto box = std::make_unique<CheckBox>();
			box->SetLabel(std::string(visible));
			box->SetChecked(*value);
			CheckBox* boxRaw = box.get();
			node = Create(id, Kind::Checkbox, std::move(box));
			node->S = visible;
			boxRaw->SetOnStateChanged([node](bool checked) {
				node->Changed = true;
				node->B = checked;
			});
		}
		auto* box = static_cast<CheckBox*>(node->Control);
		if (node->S != visible)
		{
			box->SetLabel(std::string(visible));
			node->S = visible;
		}
		Reach(node, nullptr);
		if (std::exchange(node->Changed, false))
		{
			*value = node->B;
			return true;
		}
		if (box->IsChecked() != *value)
			box->SetChecked(*value);
		return false;
	}

	bool Context::Radio(std::string_view id, int* index, std::span<const std::string> options)
	{
		const Id nodeId = MakeId(Kind::Radio, id);
		Node* node = Find(nodeId, Kind::Radio);
		if (!node)
		{
			auto radios = std::make_unique<RadioButtons>(m_Fonts, m_Font, m_Style.FontSize);
			RadioButtons* raw = radios.get();
			node = Create(nodeId, Kind::Radio, std::move(radios));
			raw->SetOnSelectionChanged([node](int selected) {
				node->Changed = true;
				node->I = selected;
			});
		}
		auto* radios = static_cast<RadioButtons*>(node->Control);
		if (Options(node, options))
		{
			radios->SetOptions(node->Options);
			radios->SetSelectedIndex(*index);
		}
		Reach(node, nullptr);
		if (std::exchange(node->Changed, false))
		{
			*index = node->I;
			return true;
		}
		if (radios->GetSelectedIndex() != *index)
			radios->SetSelectedIndex(*index);
		return false;
	}

	bool Context::Radio(std::string_view id, int* index, std::initializer_list<std::string_view> options)
	{
		m_OptionScratch.resize(options.size());
		size_t i = 0;
		for (std::string_view option : options)
			m_OptionScratch[i++].assign(option);
		return Radio(id, index, std::span<const std::string>(m_OptionScratch));
	}

	bool Context::Slider(std::string_view id, float* value, float min, float max)
	{
		const Id nodeId = MakeId(Kind::SliderFloat, id);
		Node* node = Find(nodeId, Kind::SliderFloat);
		if (!node)
		{
			auto slider = std::make_unique<wisp::Slider>();
			slider->SetValue(Normalise(*value, min, max));
			wisp::Slider* raw = slider.get();
			node = Create(nodeId, Kind::SliderFloat, std::move(slider));
			raw->SetOnValueChanged([node](float t) {
				node->Changed = true;
				node->F = t;
			});
		}
		auto* slider = static_cast<wisp::Slider*>(node->Control);
		Reach(node, nullptr);
		if (std::exchange(node->Changed, false))
		{
			*value = min + node->F * (max - min);
			return true;
		}
		const float t = std::clamp(Normalise(*value, min, max), 0.0f, 1.0f);
		if (slider->GetValue() != t)
			slider->SetValue(t);
		return false;
	}

	bool Context::Slider(std::string_view id, int* value, int min, int max)
	{
		const Id nodeId = MakeId(Kind::SliderInt, id);
		Node* node = Find(nodeId, Kind::SliderInt);
		const float range = (float)std::max(max - min, 1);
		if (!node)
		{
			auto slider = std::make_unique<wisp::Slider>();
			slider->SetValue(Normalise((float)*value, (float)min, (float)max));
			wisp::Slider* raw = slider.get();
			node = Create(nodeId, Kind::SliderInt, std::move(slider));
			raw->SetOnValueChanged([node](float t) {
				node->Changed = true;
				node->F = t;
			});
		}
		auto* slider = static_cast<wisp::Slider*>(node->Control);
		slider->SetStepSize(1.0f / range);
		Reach(node, nullptr);
		if (std::exchange(node->Changed, false))
		{
			const int picked = min + (int)std::lround(node->F * range);
			if (picked != *value)
			{
				*value = picked;
				return true;
			}
		}
		const float t = std::clamp(Normalise((float)*value, (float)min, (float)max), 0.0f, 1.0f);
		if (slider->GetValue() != t)
			slider->SetValue(t);
		return false;
	}

	bool Context::DragFloat(std::string_view id, float* value, float step)
	{
		const Id nodeId = MakeId(Kind::DragFloat, id);
		Node* node = Find(nodeId, Kind::DragFloat);
		if (!node)
		{
			auto box = std::make_unique<SFloatBox>(m_Fonts, m_Font, m_Style.FontSize, *value);
			SFloatBox* raw = box.get();
			node = Create(nodeId, Kind::DragFloat, std::move(box));
			raw->SetOnValueChanged([node](float v) {
				node->Changed = true;
				node->F = v;
			});
		}
		auto* box = static_cast<SFloatBox*>(node->Control);
		box->SetStep(step);
		Reach(node, nullptr);
		if (std::exchange(node->Changed, false))
		{
			*value = node->F;
			return true;
		}
		if (box->GetValue() != *value)
			box->SetValue(*value);
		return false;
	}

	bool Context::DragInt(std::string_view id, int* value, int min, int max)
	{
		const Id nodeId = MakeId(Kind::DragInt, id);
		Node* node = Find(nodeId, Kind::DragInt);
		if (!node)
		{
			auto box = std::make_unique<SIntBox>(m_Fonts, m_Font, m_Style.FontSize, *value);
			if (min < max)
				box->SetClampRange(min, max);
			SIntBox* raw = box.get();
			node = Create(nodeId, Kind::DragInt, std::move(box));
			raw->SetOnValueChanged([node](int v) {
				node->Changed = true;
				node->I = v;
			});
		}
		auto* box = static_cast<SIntBox*>(node->Control);
		Reach(node, nullptr);
		if (std::exchange(node->Changed, false))
		{
			*value = node->I;
			return true;
		}
		if (box->GetValue() != *value)
			box->SetValue(*value);
		return false;
	}

	bool Context::InputText(std::string_view id, std::string* text, std::string_view placeholder)
	{
		const Id nodeId = MakeId(Kind::InputText, id);
		Node* node = Find(nodeId, Kind::InputText);
		if (!node)
		{
			auto field = std::make_unique<EditableText>(m_Fonts, m_Font, m_Style.FontSize, *text);
			if (!placeholder.empty())
				field->SetPlaceholder(std::string(placeholder));
			EditableText* raw = field.get();
			node = Create(nodeId, Kind::InputText, std::move(field));
			node->Hint = placeholder;
			raw->SetOnTextChanged([node](const std::string& value) {
				node->Changed = true;
				node->S = value;
			});
		}
		auto* field = static_cast<EditableText*>(node->Control);
		if (node->Hint != placeholder)
		{
			field->SetPlaceholder(std::string(placeholder));
			node->Hint = placeholder;
		}
		Reach(node, nullptr);
		if (std::exchange(node->Changed, false))
		{
			*text = node->S;
			return true;
		}
		if (field->GetText() != *text)
			field->SetText(*text);
		return false;
	}

	bool Context::Dropdown(std::string_view id, int* index, std::span<const std::string> options)
	{
		const Id nodeId = MakeId(Kind::Dropdown, id);
		Node* node = Find(nodeId, Kind::Dropdown);
		if (!node)
		{
			auto dropdown = std::make_unique<wisp::Dropdown>(m_Fonts, m_Font, m_Style.FontSize);
			wisp::Dropdown* raw = dropdown.get();
			node = Create(nodeId, Kind::Dropdown, std::move(dropdown));
			raw->SetOnSelectionChanged([node](int selected, const std::string&) {
				node->Changed = true;
				node->I = selected;
			});
		}
		auto* dropdown = static_cast<wisp::Dropdown*>(node->Control);
		if (Options(node, options))
		{
			dropdown->SetOptions(node->Options);
			dropdown->SetSelectedIndex(*index);
		}
		Reach(node, nullptr);
		if (std::exchange(node->Changed, false))
		{
			*index = node->I;
			return true;
		}
		if (dropdown->GetSelectedIndex() != *index)
			dropdown->SetSelectedIndex(*index);
		return false;
	}

	bool Context::Dropdown(std::string_view id, int* index, std::initializer_list<std::string_view> options)
	{
		m_OptionScratch.resize(options.size());
		size_t i = 0;
		for (std::string_view option : options)
			m_OptionScratch[i++].assign(option);
		return Dropdown(id, index, std::span<const std::string>(m_OptionScratch));
	}

	bool Context::ColourEdit(std::string_view id, glm::vec4* colour)
	{
		const Id nodeId = MakeId(Kind::Colour, id);
		Node* node = Find(nodeId, Kind::Colour);
		if (!node)
		{
			auto picker = std::make_unique<ColourPicker>(m_Fonts, m_Font, m_Style.FontSize);
			picker->SetColour(*colour);
			ColourPicker* raw = picker.get();
			node = Create(nodeId, Kind::Colour, std::move(picker));
			raw->SetOnColourChanged([node](const glm::vec4& value) {
				node->Changed = true;
				node->C = value;
			});
		}
		auto* picker = static_cast<ColourPicker*>(node->Control);
		Reach(node, nullptr);
		if (std::exchange(node->Changed, false))
		{
			*colour = node->C;
			return true;
		}
		if (picker->GetColour() != *colour)
			picker->SetColour(*colour);
		return false;
	}

	wisp::Widget* Context::PlaceCustom(std::string_view id, const void* tag,
		const std::function<std::unique_ptr<wisp::Widget>()>& factory)
	{
		const Id nodeId = MakeId(Kind::Custom, id);
		Node* node = Find(nodeId, Kind::Custom);
		if (node && node->Tag != tag)
		{
			Detach(node);
			node = nullptr;
		}
		if (!node)
		{
			node = Create(nodeId, Kind::Custom, factory());
			node->Tag = tag;
		}
		Reach(node, nullptr);
		return node->Control;
	}

} // namespace wisp::im

namespace wisp {

	ImmediateHost::ImmediateHost(FontCache* fonts, FontHandle font, FrameFn frame)
		: m_Root(std::make_unique<BoxPanel>(BoxOrientation::Vertical))
		, m_Context(std::make_unique<im::Context>(fonts, font))
		, m_Frame(std::move(frame))
	{
		SetupParentChild(this, m_Root.get());
		if (UIApplication::IsValid())
			m_Callback = UIApplication::Get().AddFrameCallback(this, [this] { RunFrame(); });
	}

	ImmediateHost::~ImmediateHost()
	{
		if (m_Callback && UIApplication::IsValid())
			UIApplication::Get().RemoveFrameCallback(m_Callback);
		// The widgets before the nodes their callbacks name.
		m_Root.reset();
		m_Context.reset();
	}

	void ImmediateHost::RunFrame()
	{
		m_Context->BeginFrame(m_Root.get());
		if (m_Frame)
			m_Frame(*m_Context);
		m_Context->EndFrame();
	}

	glm::vec2 ImmediateHost::ComputeDesiredSize() const
	{
		const glm::vec2 desired = m_Root->ComputeDesiredSize();
		m_Root->SetCachedDesiredSize(desired);
		return desired;
	}

	void ImmediateHost::ArrangeChildren(const Geometry& geom, ArrangedChildren& out) const
	{
		out.Add(m_Root.get(), geom.MakeChild({ 0.0f, 0.0f }, geom.LocalSize));
	}

	int ImmediateHost::OnPaint(const PaintArgs& args, const Geometry& geom, DrawList& drawList, int layerId,
		const WidgetStyleContext& style) const
	{
		PaintArgs childArgs = args;
		childArgs.InheritedClipIndex = ApplyWidgetClipping(this, drawList, args.InheritedClipIndex,
			geom.LocalToAbsolute({ 0.0f, 0.0f }), geom.LocalSize);
		WidgetStyleContext childStyle = style;
		childStyle.ColorAndOpacity = style.ColorAndOpacity * m_Root->GetRenderTint();
		childStyle.ColorAndOpacity.a *= m_Root->GetRenderOpacity();
		return m_Root->OnPaint(childArgs, geom.MakeChild({ 0.0f, 0.0f }, geom.LocalSize), drawList, layerId, childStyle);
	}

	void ImmediateHost::ForEachChild(std::function<void(Widget*)> fn)
	{
		fn(m_Root.get());
	}

} // namespace wisp
