#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rn {

// Copies a SPIR-V module into out with every uninitialized Function-storage variable zero-initialized (as Adreno
// drivers treat them). Returns false, leaving out untouched, when there is nothing to change or the module does
// not parse. patched receives the number of variables changed.
bool ZeroInitFunctionVariables(const uint32_t* code, size_t words, std::vector<uint32_t>* out, uint32_t* patched);

}  // namespace rn
