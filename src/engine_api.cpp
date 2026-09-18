#include "xmatch/matching_engine_api.hpp"
#include "engine/engine.hpp"

extern "C" {

std::uint32_t xmatch_api_version() {
    return xmatch::kApiVersion;
}

xmatch::IMatchingEngine*
xmatch_create(xmatch::IEventListener* listener) {
    if (!listener) return nullptr;
    try {
        return new xmatch::detail::Engine(listener);
    } catch (...) {
        return nullptr;
    }
}

void xmatch_destroy(xmatch::IMatchingEngine* engine) {
    try {
        delete engine;
    } catch (...) {
        // destructors here are noexcept in practice; guard anyway per the
        // "never throw across the ABI" contract.
    }
}

} // extern "C"
