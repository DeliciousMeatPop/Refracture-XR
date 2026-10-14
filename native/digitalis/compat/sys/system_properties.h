// No Android system properties on Windows: Digitalis' config falls back to its env variables.
#pragma once
#include <stdint.h>
struct prop_info;
inline const prop_info* __system_property_find(const char*) { return nullptr; }
inline void __system_property_read_callback(const prop_info*,
                                            void (*)(void*, const char*, const char*, uint32_t),
                                            void*) {}
