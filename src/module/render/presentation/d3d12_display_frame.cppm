module;

#include "_d3d12_module_prelude.h"
#include <cstdint>
#include <string>

export module mcdr.render.display;

export namespace d3d12_mc::display {
int prewarm(void* context, uint64_t generation);
int preparePrograms(void* context, uint64_t generation, void** prepared);
int validatePreparedPrograms(void* context, void* prepared);
void exchangePreparedPrograms(void* context, void* prepared) noexcept;
void destroyPreparedPrograms(void* prepared) noexcept;
int prepare(void* context, const void* input, uint32_t bytes);
int submit(void* context, const void* input, uint32_t bytes);
int retire(void* context);
std::string lastFailureText();
}
