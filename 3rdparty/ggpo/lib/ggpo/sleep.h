// SPDX-FileCopyrightText: 2026 zdxsv contributors
// SPDX-License-Identifier: GPL-3.0+

#pragma once

// Stand-in for flycast's sleep.h (included by backends/p2p.cpp).

#include <chrono>
#include <cstdint>
#include <thread>

inline void sleep_us(int64_t us)
{
	std::this_thread::sleep_for(std::chrono::microseconds(us));
}
