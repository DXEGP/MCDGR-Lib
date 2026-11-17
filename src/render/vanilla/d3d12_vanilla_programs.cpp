module;

#include "_d3d12_module_prelude.h"
#include "mcdr_abi.h"
#include <memory>
#include <string>

module mcdr.render.vanilla;

import mcdr.render.frameprep;
import mcdr.render.display;
import mcdr.render.oit;

namespace d3d12_mc::vanilla {
namespace {

thread_local std::string programsFailure;

struct ContextProgramsIdentity {};

struct PreparedFramePrograms {
    void* context = nullptr;
    std::shared_ptr<void> identity;
    uint64_t fenceValue = 0;
    uint64_t generation = 0;
    void* frameprep = nullptr;
    void* display = nullptr;
    void* oit = nullptr;

    ~PreparedFramePrograms() {
        frameprep::destroyPreparedPrograms(frameprep);
        display::destroyPreparedPrograms(display);
        oit::destroyPreparedPrograms(oit);
    }
};

int recordProgramsFailure(int result) {
    switch (result) {
    case MCDR_FRAME_PROGRAMS_SUCCESS:
        programsFailure.clear();
        break;
    case MCDR_FRAME_PROGRAMS_INVALID_REQUEST:
        programsFailure = "Invalid frame program generation, flags or candidate";
        break;
    case MCDR_FRAME_PROGRAMS_COMPILATION_FAILED:
        programsFailure = "Frame program compilation failed";
        break;
    case MCDR_FRAME_PROGRAMS_BUSY:
        programsFailure = "Frame programs require an idle command context";
        break;
    case MCDR_FRAME_PROGRAMS_STALE:
        programsFailure = "Frame program candidate was superseded";
        break;
    case MCDR_FRAME_PROGRAMS_INVALID_CONTEXT:
        programsFailure = "Frame program context is invalid";
        break;
    case MCDR_FRAME_PROGRAMS_SUBMISSION_UNKNOWN:
        programsFailure = "Frame program context retains unknown GPU work";
        break;
    }
    return result;
}

int validateProgramBoundary(void* opaqueContext) {
    const auto control = static_cast<CommandContextControlStatus>(validateCommandContextControl(opaqueContext));
    switch (control) {
    case CommandContextControlStatus::Invalid: return MCDR_FRAME_PROGRAMS_INVALID_CONTEXT;
    case CommandContextControlStatus::Busy: return MCDR_FRAME_PROGRAMS_BUSY;
    case CommandContextControlStatus::SubmissionUnknown: return MCDR_FRAME_PROGRAMS_SUBMISSION_UNKNOWN;
    case CommandContextControlStatus::Ready: break;
    }
    const auto* context = static_cast<const CommandContext*>(opaqueContext);
    return context->vanillaFrameState ? MCDR_FRAME_PROGRAMS_BUSY : MCDR_FRAME_PROGRAMS_SUCCESS;
}

}

std::string frameProgramsFailureText() { return programsFailure; }

int prepareFramePrograms(void* opaqueContext, uint64_t generation, uint32_t flags, void** candidate) {
    if (!candidate) return recordProgramsFailure(MCDR_FRAME_PROGRAMS_INVALID_REQUEST);
    *candidate = nullptr;
    if (generation == 0 || (flags & ~3u) != 0)
        return recordProgramsFailure(MCDR_FRAME_PROGRAMS_INVALID_REQUEST);
    const auto boundary = validateProgramBoundary(opaqueContext);
    if (boundary != MCDR_FRAME_PROGRAMS_SUCCESS) return recordProgramsFailure(boundary);
    auto* context = static_cast<CommandContext*>(opaqueContext);
    auto prepared = std::make_unique<PreparedFramePrograms>();
    prepared->context = opaqueContext;
    if (!context->controlIdentity)
        context->controlIdentity = std::make_shared<ContextProgramsIdentity>();
    prepared->identity = context->controlIdentity;
    prepared->fenceValue = context->fenceValue;
    prepared->generation = generation;
    const auto frameprep = frameprep::preparePrograms(opaqueContext, generation, flags, &prepared->frameprep);
    if (frameprep != MCDR_FRAME_PROGRAMS_SUCCESS) return recordProgramsFailure(frameprep);
    const auto display = display::preparePrograms(opaqueContext, generation, &prepared->display);
    if (display != MCDR_FRAME_PROGRAMS_SUCCESS) return recordProgramsFailure(display);
    const auto oit = oit::preparePrograms(opaqueContext, generation, &prepared->oit);
    if (oit != MCDR_FRAME_PROGRAMS_SUCCESS) return recordProgramsFailure(oit);
    *candidate = prepared.release();
    return recordProgramsFailure(MCDR_FRAME_PROGRAMS_SUCCESS);
}

int commitFramePrograms(void* opaqueContext, void* opaqueCandidate) {
    if (!opaqueCandidate) return recordProgramsFailure(MCDR_FRAME_PROGRAMS_INVALID_REQUEST);
    const auto boundary = validateProgramBoundary(opaqueContext);
    if (boundary != MCDR_FRAME_PROGRAMS_SUCCESS) return recordProgramsFailure(boundary);
    auto* context = static_cast<CommandContext*>(opaqueContext);
    const auto* candidate = static_cast<const PreparedFramePrograms*>(opaqueCandidate);
    if (candidate->context != opaqueContext || candidate->identity != context->controlIdentity)
        return recordProgramsFailure(MCDR_FRAME_PROGRAMS_INVALID_CONTEXT);
    if (candidate->fenceValue != context->fenceValue)
        return recordProgramsFailure(MCDR_FRAME_PROGRAMS_STALE);
    const auto frameprep = frameprep::validatePreparedPrograms(opaqueContext, candidate->frameprep);
    if (frameprep != MCDR_FRAME_PROGRAMS_SUCCESS) return recordProgramsFailure(frameprep);
    const auto display = display::validatePreparedPrograms(opaqueContext, candidate->display);
    if (display != MCDR_FRAME_PROGRAMS_SUCCESS) return recordProgramsFailure(display);
    const auto oit = oit::validatePreparedPrograms(opaqueContext, candidate->oit);
    if (oit != MCDR_FRAME_PROGRAMS_SUCCESS) return recordProgramsFailure(oit);
    programsFailure.clear();
    frameprep::exchangePreparedPrograms(opaqueContext, candidate->frameprep);
    display::exchangePreparedPrograms(opaqueContext, candidate->display);
    oit::exchangePreparedPrograms(opaqueContext, candidate->oit);
    context->frameProgramGeneration = candidate->generation;
    abortFramePrograms(opaqueCandidate);
    return MCDR_FRAME_PROGRAMS_SUCCESS;
}

void abortFramePrograms(void* opaqueCandidate) noexcept {
    const std::unique_ptr<PreparedFramePrograms> candidate(static_cast<PreparedFramePrograms*>(opaqueCandidate));
}

}
