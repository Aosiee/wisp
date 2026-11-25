#include "Wisp/Clipboard.h"
#include "Wisp/Application.h"

#include "hearth/platform/Platform.h"
#include <cstring>

namespace wisp {

std::string Clipboard::Get()
{
	hearth::platform::Platform* platform = UIApplication::IsValid() ? UIApplication::Get().GetPlatform() : nullptr;
	return platform ? platform->GetClipboard() : std::string();
}

void Clipboard::Set(const std::string& text)
{
	if (hearth::platform::Platform* platform = UIApplication::IsValid() ? UIApplication::Get().GetPlatform() : nullptr)
		platform->SetClipboard(text);
}

} // namespace wisp
