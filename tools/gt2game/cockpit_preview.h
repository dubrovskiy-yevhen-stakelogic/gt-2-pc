#pragma once
#include <cstdint>
#include <vector>
#include "gt2formats/gt_menu_images.h"

namespace gt2 { class GtfsVolume; }
namespace gt2game {
class GameWindow;
// This development view owns no career, arcade selection, course or simulation state.
bool CockpitPreviewRequested(const GameWindow& window, bool carShown);
void AppendCockpitPreviewButton(std::vector<gt2::MenuPrim>& prims, bool quest, int x, int y);
void RunCockpitPreview(GameWindow& window, const gt2::GtfsVolume& vol, uint32_t carId, int paint);
}
