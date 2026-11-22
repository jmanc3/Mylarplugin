#pragma once

#include <stop_token>

// Requested on the compositor thread before joining any in-process client.
extern std::stop_source plugin_lifetime;
