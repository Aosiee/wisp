#pragma once

#include "Wisp/Core.h"
#include "Wisp/DrawList.h"
#include "Wisp/FontCache.h"
#include "Wisp/Style.h"

namespace wisp {

template<typename T> class WidgetRef;

} // namespace wisp

// ---- All widget components -------------------------------------------------

#include "Wisp/Components/Spacer.h"
#include "Wisp/Components/TextBlock.h"
#include "Wisp/Components/Image.h"
#include "Wisp/Components/Border.h"
#include "Wisp/Components/BoxPanel.h"
#include "Wisp/Components/Button.h"
#include "Wisp/Components/CheckBox.h"
#include "Wisp/Components/SizeBox.h"
#include "Wisp/Components/ScaleBox.h"
#include "Wisp/Components/Overlay.h"
#include "Wisp/Components/Canvas.h"
#include "Wisp/Components/ScrollBox.h"
#include "Wisp/Components/Slider.h"
#include "Wisp/Components/EditableText.h"
#include "Wisp/Components/Dropdown.h"
#include "Wisp/Components/ValueBox.h"
#include "Wisp/Components/ProgressBar.h"
#include "Wisp/Components/Menu.h"
#include "Wisp/Components/GraphData.h"
#include "Wisp/Components/LineGraph.h"
#include "Wisp/Components/BarChart.h"
#include "Wisp/Components/Sparkline.h"
#if defined(WISP_DEBUG_TOOLS)
#include "Wisp/Components/WidgetReflector.h"
#endif // WISP_DEBUG_TOOLS
#include "Wisp/Components/Separator.h"
#include "Wisp/Components/Throbber.h"
#include "Wisp/Components/RadioButtons.h"
#include "Wisp/Components/WrapBox.h"
#include "Wisp/Components/UniformGridPanel.h"
#include "Wisp/Components/Splitter.h"
#include "Wisp/Composites/FrameTimeGraph.h"
#include "Wisp/Docking.h"
