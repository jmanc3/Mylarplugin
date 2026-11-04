#pragma once

// Shared includes for the dock translation units. Keeping them here means the
// individual dock modules only need to include what they actually use on top.
#include "dock/dock.h"
#include "dock/dock_types.h"
#include "dock/dock_ui.h"

#include "audio.h"
#include "client/raw_windowing.h"
#include "client/windowing.h"
#include "components.h"
#include "container.h"
#include "events.h"
#include "heart.h"
#include "hypriso.h"
#include "icons.h"
#include "pango_font_cache.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
