#include "Wisp/Applet/Applet.h"

#include "Wisp/Application.h"
#include "Wisp/Log.h"
#include "Wisp/Rhi/RhiRenderer.h"
#include "Wisp/StyleTokens.h"

#include "hearth/platform/Event.h"
#include "hearth/platform/Window.h"
#include "hearth/rhi/vulkan/VulkanRhiDevice.h"
#include "hearth/rhi/vulkan/WindowPresenter.h"

#include <rapidjson/document.h>

#include <algorithm>
#include <fstream>
#include <iterator>

namespace wisp {

	namespace platform = hearth::platform;
	namespace rhi = hearth::rhi;

	// A torn-out dock window: its own platform window and presenter, the
	// applet's UI client drawing it.
	class Applet::Window final : public HostWindow
	{
	public:
		Window(Applet& applet, const std::string& title, uint32_t width, uint32_t height)
			: m_Applet(applet)
		{
			hearth::App& app = applet.m_App;
			platform::WindowDesc desc;
			desc.Title = title;
			desc.Width = width;
			desc.Height = height;
			desc.Visible = app.Desc().Display == hearth::DisplayMode::Windowed;
			m_Window = std::make_unique<platform::Window>(app.GetPlatform(), desc);

			rhi::WindowPresenterDesc presenterDesc;
			presenterDesc.Window = m_Window.get();
			presenterDesc.Owner = this;
			m_Presenter = std::make_unique<rhi::WindowPresenter>(app.GetDevice(), presenterDesc);
			m_Presenter->SetVsync(app.Desc().Vsync);
			rhi::FrameClient* clients[] = { &applet.m_Client };
			m_Presenter->SetClients(clients);
		}

		~Window() override
		{
			m_Applet.m_CloseRequests.erase(m_Window.get());
			m_Presenter->Stop();
			m_Presenter.reset();
			m_Window.reset();
		}

		platform::Window* PlatformWindow() const override { return m_Window.get(); }
		void SetTitle(const std::string& title) override { m_Window->SetTitle(title); }
		void Present() override { m_Presenter->Present(); }

	private:
		Applet& m_Applet;
		std::unique_ptr<platform::Window> m_Window;
		std::unique_ptr<rhi::WindowPresenter> m_Presenter;
	};

	Applet::Applet(hearth::App& app)
		: m_App(app)
	{
		auto renderer = std::make_unique<RhiRenderer>();
		renderer->Init(app.GetDevice(), &m_Fonts);

		UIApplicationDesc desc;
		desc.Fonts = &m_Fonts;
		desc.Platform = &app.GetPlatform();
		desc.Renderer = std::move(renderer);
		// Every window's record reads the context list on the record workers.
		rhi::VulkanDevice* device = app.GetDevice().GetVulkan();
		desc.WaitForRenderIdle = [device] { rhi::WindowPresenter::WaitForAllRecordIdle(*device); };
		UIApplication::Init(std::move(desc));
		// The default context is the main window's from the start (input
		// before the first frame finds it).
		if (WindowContext* context = UIApplication::Get().GetWindowContext(nullptr))
			context->Window = MainWindow();

		app.AddFrameClient(m_Client);
		m_Handler = app.AddEventHandler([this](const platform::Event& event) { return Route(event); });

		// The input script, by frame, ahead of the platform's own events.
		const std::vector<std::string>& args = app.Desc().Args;
		for (size_t i = 0; i + 1 < args.size(); i++)
		{
			if (args[i] == "--input-script" && !LoadScript(args[i + 1]))
				HEARTH_LOG_ERROR(LogWisp, "Applet: the input script '{}' could not be read", args[i + 1]);
		}
		if (!m_Script.empty())
		{
			m_LoopClients.push_back(app.GetLoop().Add(hearth::Phase::BeginFrame, 100, "wisp.script",
				[this](const hearth::Frame& frame) { PostScript(frame.Index); }));
			// One frame for the last event's effect to lay out, one to draw it.
			app.SetCaptureReady([this] { return m_App.GetLoop().FrameIndex() >= m_ScriptEnd + 2; });
		}
	}

	Applet::~Applet()
	{
		for (const hearth::ClientId id : m_LoopClients)
			m_App.GetLoop().Remove(id);
		m_App.SetCaptureReady(nullptr);
		m_App.RemoveEventHandler(m_Handler);
		// No record of the main window may still draw the UI.
		m_App.RemoveFrameClient(m_Client);
		rhi::WindowPresenter::WaitForAllRecordIdle(*m_App.GetDevice().GetVulkan());
		// The roots, the window state and the renderer, before the App takes
		// the device down.
		UIApplication::Shutdown();
	}

	const fonts::HouseFaces& Applet::AddHouseFaces()
	{
		m_Faces = fonts::AddHouseFaces(m_Fonts, tokens::FontBody);
		fonts::ApplyHouseFaces(UIApplication::Get(), m_Faces);
		return m_Faces;
	}

	void Applet::SetRoot(std::unique_ptr<Widget> root)
	{
		UIApplication& ui = UIApplication::Get();
		if (!ui.GetWindowContext(MainWindow()))
			ui.CreateWindowContext(MainWindow(), std::move(root));
		else
			ui.SetRootWidget(std::move(root));
	}

	void* Applet::MainWindow() const
	{
		return m_App.GetWindow();
	}

	bool Applet::CanOpenWindows() const
	{
		return !m_App.IsOffscreen();
	}

	std::unique_ptr<HostWindow> Applet::OpenWindow(const std::string& title, uint32_t width, uint32_t height,
		std::function<void()> onCloseRequested)
	{
		if (!CanOpenWindows())
			return nullptr;
		auto window = std::make_unique<Window>(*this, title, width, height);
		m_CloseRequests[window->PlatformWindow()] = std::move(onCloseRequested);
		return window;
	}

	bool Applet::Route(const platform::Event& event)
	{
		UIApplication& ui = UIApplication::Get();
		void* window = event.Source;
		switch (event.Type)
		{
		case platform::EventType::WindowClose:
		{
			// A dock window's close is the dock's to apply at its next update.
			const auto it = m_CloseRequests.find(event.Source);
			if (it == m_CloseRequests.end())
				return false;
			it->second();
			return true;
		}
		case platform::EventType::CursorMove:
			ui.SetMousePosition(window, { (float)event.CursorX, (float)event.CursorY });
			return false;
		case platform::EventType::MouseButton:
			return event.KeyAction == platform::Action::Release ? ui.OnMouseButtonUp(window, event.Button)
																 : ui.OnMouseButtonDown(window, event.Button);
		case platform::EventType::Scroll:
			ui.OnMouseWheel(window, (float)event.ScrollY);
			return false;
		case platform::EventType::Key:
			// The platform's modifier bits are Wisp's (Shift 1, Ctrl 2, Alt 4).
			return event.KeyAction == platform::Action::Release ? ui.OnKeyUp(window, event.KeyCode, event.Mods & 7)
																 : ui.OnKeyDown(window, event.KeyCode, event.Mods & 7);
		case platform::EventType::Char:
			return ui.OnTextInput(window, event.Codepoint);
		case platform::EventType::WindowFocus:
			ui.OnWindowFocusChanged(window, event.Focused);
			return false;
		case platform::EventType::CursorEnter:
			ui.OnCursorEntered(window, event.Entered);
			return false;
		default:
			return false;
		}
	}

	bool Applet::LoadScript(const std::string& path)
	{
		std::ifstream in(path);
		if (!in)
			return false;
		const std::string json((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		rapidjson::Document doc;
		doc.Parse(json.c_str());
		if (!doc.IsObject() || !doc.HasMember("events") || !doc["events"].IsArray())
			return false;
		for (const auto& e : doc["events"].GetArray())
		{
			if (!e.IsObject())
				continue;
			ScriptEvent ev;
			ev.Frame = e.HasMember("frame") && e["frame"].IsUint64() ? e["frame"].GetUint64() : 0;
			ev.Type = e.HasMember("type") && e["type"].IsString() ? e["type"].GetString() : "";
			ev.X = e.HasMember("x") && e["x"].IsNumber() ? e["x"].GetFloat() : 0.0f;
			ev.Y = e.HasMember("y") && e["y"].IsNumber() ? e["y"].GetFloat() : 0.0f;
			ev.Button = e.HasMember("button") && e["button"].IsInt() ? e["button"].GetInt() : 0;
			ev.Key = e.HasMember("key") && e["key"].IsInt() ? e["key"].GetInt() : 0;
			ev.Mods = e.HasMember("mods") && e["mods"].IsInt() ? e["mods"].GetInt() : 0;
			ev.Delta = e.HasMember("delta") && e["delta"].IsNumber() ? e["delta"].GetFloat() : 0.0f;
			ev.Text = e.HasMember("text") && e["text"].IsString() ? e["text"].GetString() : "";
			m_ScriptEnd = std::max(m_ScriptEnd, ev.Frame);
			m_Script.push_back(std::move(ev));
		}
		return true;
	}

	void Applet::PostScript(uint64_t frame)
	{
		platform::Window* window = m_App.GetWindow();
		const auto post = [&](platform::EventType type, auto&& fill) {
			platform::Event event;
			event.Type = type;
			event.Source = window;
			fill(event);
			m_App.PostEvent(std::move(event));
		};
		const auto move = [&](const ScriptEvent& ev) {
			post(platform::EventType::CursorMove, [&](platform::Event& e) { e.CursorX = ev.X; e.CursorY = ev.Y; });
		};
		// Wisp routes a press to what it hovered at the last layout, so the
		// pointer reaches a click's, a press's or a release's position a
		// frame ahead, as a hand's pointer would.
		for (const ScriptEvent& ev : m_Script)
		{
			if (ev.Frame == frame + 1 && (ev.Type == "click" || ev.Type == "down" || ev.Type == "up"))
				move(ev);
		}
		for (const ScriptEvent& ev : m_Script)
		{
			if (ev.Frame != frame)
				continue;
			if (ev.Type == "move")
				move(ev);
			else if (ev.Type == "click")
			{
				move(ev);
				for (const platform::Action action : { platform::Action::Press, platform::Action::Release })
					post(platform::EventType::MouseButton, [&](platform::Event& e) { e.Button = ev.Button; e.KeyAction = action; });
			}
			else if (ev.Type == "down" || ev.Type == "up")
			{
				move(ev);
				const platform::Action action = ev.Type == "down" ? platform::Action::Press : platform::Action::Release;
				post(platform::EventType::MouseButton, [&](platform::Event& e) { e.Button = ev.Button; e.KeyAction = action; });
			}
			else if (ev.Type == "wheel")
				post(platform::EventType::Scroll, [&](platform::Event& e) { e.ScrollY = ev.Delta; });
			else if (ev.Type == "key")
			{
				for (const platform::Action action : { platform::Action::Press, platform::Action::Release })
					post(platform::EventType::Key, [&](platform::Event& e) { e.KeyCode = ev.Key; e.Mods = ev.Mods; e.KeyAction = action; });
			}
			else if (ev.Type == "text")
			{
				for (const unsigned char c : ev.Text)
					post(platform::EventType::Char, [&](platform::Event& e) { e.Codepoint = c; });
			}
		}
	}

} // namespace wisp
