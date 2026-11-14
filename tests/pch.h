#pragma once

// The unit tests' precompiled header: the standard headers most test files use, and the test framework.
// Every test file gets it without naming it (xmake.lua's set_pcxxheader), so a file still includes what
// it uses. Only headers that rarely change belong here: a change to one rebuilds every test.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Test.h"
