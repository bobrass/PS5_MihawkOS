/*
 * PS5 Vulkan Template - what every sample may include, at file scope.
 *
 * Each sample is compiled inside a namespace of its own (wrap.cpp.in), so two
 * samples' classes of the same name (VulkanExample, Vertex...) stay apart in
 * one title. A header first included inside that namespace would land in it:
 * every header a sample includes is included here first, where its include
 * guard then keeps it.
 *
 * Copyright (C) 2026 Mihawk
 *
 * This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <assert.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <math.h>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "vulkanexamplebase.h"
#include "proton_launch.h"
#include "proton_install.h"
#include "proton_update.h"
#include "proton_steam.h"
#include "proton_webui.h"
#include "../../src/pw_qrcodegen.h"
#include "ps5_samples.h"
#include "VulkanglTFModel.h"
#include "VulkanRaytracingSample.h"
#include "VulkanFrameBuffer.hpp"
#include "frustum.hpp"
#include "tiny_gltf.h"
#define IMGUI_DEFINE_MATH_OPERATORS // ImVec2 arithmetic, for the navigation rectangle
#include <imgui.h>
#include "imgui_internal.h" // SetNavIDWithRectRel: see the launcher's menu
#include <ktx.h>
#include <ktxvulkan.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#ifdef PS5_UI
#include "kit.hpp"
#endif
