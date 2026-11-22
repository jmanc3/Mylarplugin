#pragma once

#include <string>
#include <vector>

// Runs argv without a shell. Unload interrupts communication and reaps the child.
int run_cancellable_command(const std::vector<std::string> &argv, std::string &output, bool merge_stderr = false);
