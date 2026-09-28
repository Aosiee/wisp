#include "Wisp/Clipboard.h"
#include "TinyEngine/Core/Application.h"
#include <GLFW/glfw3.h>
#include <cstring>

namespace wisp {

std::string Clipboard::Get()
{
	auto* window = static_cast<GLFWwindow*>(TinyEngine::Application::Get().GetWindow().GetNativeWindow());
	const char* text = glfwGetClipboardString(window);
	return text ? std::string(text) : std::string();
}

void Clipboard::Set(const std::string& text)
{
	auto* window = static_cast<GLFWwindow*>(TinyEngine::Application::Get().GetWindow().GetNativeWindow());
	glfwSetClipboardString(window, text.c_str());
}

} // namespace wisp
