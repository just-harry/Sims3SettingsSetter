#pragma once
#include <toml++/toml.hpp>

// Initialize the allocator hooks
// Could probably make this standalone for like, any other x32 game? idk, a lot of s3ss fits that bill I guess
void InitializeAllocatorHooks(const toml::table* config);

// Global flag to check if hooks are currently active
extern bool g_mimallocActive;
