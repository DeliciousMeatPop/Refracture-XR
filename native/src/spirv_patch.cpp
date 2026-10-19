// SPIR-V fixes for shaders that only ever ran on Quest (Adreno) drivers.
//
// Adreno zero-initializes function-scope variables; desktop drivers leave them undefined. Shaders that read a
// variable before writing it then behave differently. RE4's village has a material whose loop counter starts
// uninitialized (int i; while (true) { if (i >= 8) break; ... i++; }): on NVIDIA it can start at a large
// negative value, the loop runs for billions of iterations, and Windows resets the GPU (device lost).
// ZeroInitFunctionVariables gives every such OpVariable an OpConstantNull initializer.
#include "spirv_patch.h"

#include <map>
#include <set>

namespace rn {

namespace {

constexpr uint32_t kMagic = 0x07230203;
enum Op : uint32_t {
    OpTypeImage = 25, OpTypeSampler = 26, OpTypeSampledImage = 27, OpTypeRuntimeArray = 29, OpTypeOpaque = 31,
    OpTypePointer = 32, OpTypeFunction = 33, OpTypeEvent = 34, OpTypeDeviceEvent = 35, OpTypeReserveId = 36,
    OpTypeQueue = 37, OpTypePipe = 38, OpTypeForwardPointer = 39, OpConstantNull = 46, OpFunction = 54,
    OpVariable = 59,
};
constexpr uint32_t kStorageFunction = 7;

}  // namespace

bool ZeroInitFunctionVariables(const uint32_t* code, size_t words, std::vector<uint32_t>* out, uint32_t* patched) {
    if (words < 5 || code[0] != kMagic)
        return false;
    std::map<uint32_t, uint32_t> pointee;  // Function-storage pointer type -> pointee type
    std::set<uint32_t> not_nullable;       // types OpConstantNull cannot make
    std::vector<size_t> variables;         // word offsets of uninitialized Function OpVariables
    size_t first_function = 0;
    for (size_t at = 5; at < words;) {
        const uint32_t count = code[at] >> 16, op = code[at] & 0xffff;
        if (count == 0 || at + count > words)
            return false;
        switch (op) {
        case OpTypeImage: case OpTypeSampler: case OpTypeSampledImage: case OpTypeRuntimeArray: case OpTypeOpaque:
        case OpTypeFunction: case OpTypeEvent: case OpTypeDeviceEvent: case OpTypeReserveId: case OpTypeQueue:
        case OpTypePipe: case OpTypeForwardPointer:
            if (count >= 2)
                not_nullable.insert(code[at + 1]);
            break;
        case OpTypePointer:
            if (count == 4 && code[at + 2] == kStorageFunction)
                pointee[code[at + 1]] = code[at + 3];
            not_nullable.insert(code[at + 1]);  // pointers need VariablePointers for a null constant
            break;
        case OpFunction:
            if (!first_function)
                first_function = at;
            break;
        case OpVariable:
            if (count == 4 && code[at + 3] == kStorageFunction) {
                auto it = pointee.find(code[at + 1]);
                if (it != pointee.end() && !not_nullable.count(it->second))
                    variables.push_back(at);
            }
            break;
        }
        at += count;
    }
    if (variables.empty() || !first_function)
        return false;

    uint32_t bound = code[3];
    std::map<uint32_t, uint32_t> nulls;  // pointee type -> its OpConstantNull id
    for (size_t at : variables) {
        const uint32_t type = pointee[code[at + 1]];
        if (!nulls.count(type))
            nulls[type] = bound++;
    }
    out->clear();
    out->reserve(words + nulls.size() * 3 + variables.size());
    out->insert(out->end(), code, code + first_function);
    (*out)[3] = bound;
    for (const auto& [type, id] : nulls) {
        out->push_back(3u << 16 | OpConstantNull);
        out->push_back(type);
        out->push_back(id);
    }
    size_t next = 0;
    for (size_t at = first_function; at < words;) {
        const uint32_t count = code[at] >> 16;
        if (next < variables.size() && variables[next] == at) {
            out->push_back(5u << 16 | OpVariable);
            out->push_back(code[at + 1]);
            out->push_back(code[at + 2]);
            out->push_back(code[at + 3]);
            out->push_back(nulls[pointee[code[at + 1]]]);
            ++next;
        } else {
            out->insert(out->end(), code + at, code + at + count);
        }
        at += count;
    }
    if (patched)
        *patched = static_cast<uint32_t>(variables.size());
    return true;
}

}  // namespace rn
