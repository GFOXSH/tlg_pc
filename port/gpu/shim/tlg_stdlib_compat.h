// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// FFX 1.1.4 uses these declarations without including their defining headers.
// libstdc++ happens to expose some transitively; libc++ and MinGW do not.
#include <mutex>
#include <cmath>
#include <new>
#include <cstdlib>
#include <wchar.h>
