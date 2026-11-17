#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace d3d12_mc::vanilla::scheduler {

enum class Access : uint32_t {
    Read = 1,
    Write = 2,
    ReadWrite = 3,
    Uav = 4,
};

enum class ExternalState : uint32_t {
    Unchanged,
    Known,
    Unknown,
};

inline constexpr uint32_t kGraphicsState = 1u << 0;
inline constexpr uint32_t kDescriptors = 1u << 1;
inline constexpr uint32_t kAllGraphics = kGraphicsState | kDescriptors;
inline constexpr uint32_t kUavState = 0x8;
inline constexpr uint32_t kWriteStates = 0x4 | 0x8 | 0x10 | 0x100 | 0x400 | 0x1000 | 0x800000;
inline constexpr uint32_t kReadStates = 0x1 | 0x2 | 0x20 | 0x40 | 0x80 | 0x200 | 0x800 | 0x2000 | 0x1000000;

inline bool reads(Access access) {
    return access == Access::Read || access == Access::ReadWrite || access == Access::Uav;
}

inline bool writes(Access access) {
    return access == Access::Write || access == Access::ReadWrite || access == Access::Uav;
}

struct ResourceUse {
    void* resource = nullptr;
    uint64_t version = 0;
    uint32_t subresource = UINT32_MAX;
    uint32_t subresourceCount = 1;
    uint32_t totalSubresources = 1;
    Access access = Access::Read;
    uint32_t requiredState = 0;
    uint32_t initialState = 0;
    uint32_t postExternalState = 0;
    ExternalState externalState = ExternalState::Unchanged;
};

struct Dependency {
    uint32_t priorPass = 0;
    void* resource = nullptr;
    bool readAfterWrite = false;
    bool writeAfterRead = false;
    bool writeAfterWrite = false;
};

struct Barrier {
    void* resource = nullptr;
    uint32_t subresource = 0;
    uint32_t before = 0;
    uint32_t after = 0;
    bool uav = false;
};

struct ScheduleResult {
    bool ok = false;
    const char* error = nullptr;
    std::vector<Dependency> dependencies;
    std::vector<Barrier> barriers;
    std::vector<ResourceUse> uses;
    uint32_t effects = 0;
    uint32_t pass = 0;
    uint64_t serial = 0;
};

class PassResourceScheduler {
public:
    ScheduleResult prepare(uint32_t pass, const ResourceUse* uses, uint32_t count,
                           uint32_t effects = 0) const {
        ScheduleResult result;
        prepareInto(result, pass, uses, count, effects);
        return result;
    }

    void prepareInto(ScheduleResult& result, uint32_t pass, const ResourceUse* uses,
                     uint32_t count, uint32_t effects = 0) const {
        result.ok = false;
        result.error = nullptr;
        result.dependencies.clear();
        result.barriers.clear();
        result.uses.clear();
        result.effects = effects;
        result.pass = pass;
        result.serial = serial;
        const auto reject = [&result](const char* error) {
            result.error = error;
            result.dependencies.clear();
            result.barriers.clear();
            result.uses.clear();
        };
        if (poisoned) { reject("recording failed; reset is required"); return; }
        if (pass == 0 || pass <= lastPass || (count != 0 && uses == nullptr) ||
            (effects & ~kAllGraphics) != 0) {
            reject("invalid pass declaration"); return;
        }
        result.uses.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const ResourceUse& use = uses[i];
            if (use.resource == nullptr || use.version == 0 || use.subresourceCount == 0 ||
                use.totalSubresources == 0 ||
                (use.subresource != UINT32_MAX && (use.subresource >= use.totalSubresources ||
                 use.subresourceCount > use.totalSubresources - use.subresource)) ||
                (use.subresource == UINT32_MAX && use.subresourceCount != use.totalSubresources)) {
                reject("invalid resource identity, version, or subresource range"); return;
            }
            if (!validBits(use.initialState) || !validState(use.access, use.requiredState) ||
                use.externalState == ExternalState::Unknown ||
                (use.externalState != ExternalState::Known &&
                 use.externalState != ExternalState::Unchanged) ||
                (use.externalState == ExternalState::Known && !validBits(use.postExternalState))) {
                reject("invalid access/state or unknown external resource state"); return;
            }
            for (const ResourceUse& prior : result.uses) {
                if (prior.resource == use.resource &&
                    (prior.version != use.version || prior.totalSubresources != use.totalSubresources)) {
                    reject("conflicting same-pass resource identity"); return;
                }
                if (overlaps(prior, use) && (writes(prior.access) || writes(use.access) ||
                    prior.requiredState != use.requiredState || prior.initialState != use.initialState ||
                    prior.externalState != use.externalState ||
                    prior.postExternalState != use.postExternalState)) {
                    reject("conflicting same-pass resource uses"); return;
                }
            }
            result.uses.push_back(use);
        }

        for (const ResourceUse& use : result.uses) {
            auto known = versions.find(use.resource);
            if (known != versions.end() && use.version != known->second) {
                reject("resource incarnation changed within the frame"); return;
            }
            const auto tracked = frontiers.find(use.resource);
            if (tracked != frontiers.end() && tracked->second.size() != use.totalSubresources) {
                reject("resource subresource layout changed within the frame"); return;
            }
            bool needsUav = false;
            const uint32_t first = use.subresource == UINT32_MAX ? 0 : use.subresource;
            if (tracked != frontiers.end()) {
                for (uint32_t subresource = first; subresource < first + use.subresourceCount; ++subresource) {
                    const auto& prior = tracked->second[subresource];
                    addDependency(result, prior.writer, use.resource, reads(use.access), false, writes(use.access));
                    if (writes(use.access)) addDependency(result, prior.reader, use.resource, false, true, false);
                    needsUav = needsUav || (use.initialState == kUavState &&
                        use.requiredState == kUavState && prior.exitState == kUavState &&
                        (prior.lastWrites || writes(use.access)));
                }
            }
            if (needsUav && std::none_of(result.barriers.begin(), result.barriers.end(),
                [&use](const Barrier& barrier) { return barrier.uav && barrier.resource == use.resource; })) {
                result.barriers.push_back({use.resource, UINT32_MAX, kUavState, kUavState, true});
            }
            if (use.initialState != use.requiredState) {
                for (uint32_t subresource = first; subresource < first + use.subresourceCount; ++subresource) {
                    const auto duplicate = std::find_if(result.barriers.begin(), result.barriers.end(),
                        [&use, subresource](const Barrier& barrier) {
                            return !barrier.uav && barrier.resource == use.resource &&
                                   barrier.subresource == subresource;
                        });
                    if (duplicate == result.barriers.end()) {
                        result.barriers.push_back({use.resource, subresource, use.initialState,
                                                   use.requiredState, false});
                    }
                }
            }
        }
        result.ok = true;
    }

    bool commit(const ScheduleResult& result) {
        if (poisoned || !result.ok || result.serial != serial || result.pass <= lastPass) return false;
        for (const ResourceUse& use : result.uses) {
            if (!validBits(use.postExternalState)) return false;
        }
        for (const ResourceUse& use : result.uses) {
            versions[use.resource] = use.version;
            auto& tracked = frontiers[use.resource];
            if (tracked.empty()) tracked.resize(use.totalSubresources);
            const uint32_t first = use.subresource == UINT32_MAX ? 0 : use.subresource;
            for (uint32_t index = first; index < first + use.subresourceCount; ++index) {
                auto& prior = tracked[index];
                if (writes(use.access)) {
                    prior.writer = result.pass;
                    prior.reader = 0;
                } else {
                    prior.reader = result.pass;
                }
                prior.exitState = use.externalState == ExternalState::Known
                    ? use.postExternalState : use.requiredState;
                prior.lastWrites = writes(use.access);
            }
        }
        lastPass = result.pass;
        ++serial;
        return true;
    }

    void fail() { poisoned = true; }
    bool failed() const { return poisoned; }

    void reset() {
        versions.clear();
        frontiers.clear();
        poisoned = false;
        lastPass = 0;
        ++serial;
    }

private:
    struct Frontier {
        uint32_t writer = 0;
        uint32_t reader = 0;
        uint32_t exitState = 0;
        bool lastWrites = false;
    };

    static void addDependency(ScheduleResult& result, uint32_t pass, void* resource,
                              bool raw, bool war, bool waw) {
        if (pass == 0 || (!raw && !war && !waw)) return;
        auto found = std::find_if(result.dependencies.begin(), result.dependencies.end(),
            [pass, resource](const Dependency& dependency) {
                return dependency.priorPass == pass && dependency.resource == resource;
            });
        if (found == result.dependencies.end()) result.dependencies.push_back({pass, resource, raw, war, waw});
        else {
            found->readAfterWrite |= raw;
            found->writeAfterRead |= war;
            found->writeAfterWrite |= waw;
        }
    }

    static bool overlaps(const ResourceUse& left, const ResourceUse& right) {
        if (left.resource != right.resource) return false;
        if (left.subresource == UINT32_MAX || right.subresource == UINT32_MAX) return true;
        const uint64_t leftEnd = static_cast<uint64_t>(left.subresource) + left.subresourceCount;
        const uint64_t rightEnd = static_cast<uint64_t>(right.subresource) + right.subresourceCount;
        return left.subresource < rightEnd && right.subresource < leftEnd;
    }

    static bool validBits(uint32_t state) {
        const uint32_t write = state & kWriteStates;
        return (state & ~(kWriteStates | kReadStates)) == 0 &&
               (write == 0 || ((write & (write - 1)) == 0 && state == write));
    }

    static bool validState(Access access, uint32_t state) {
        if (!validBits(state) || state == 0) return false;
        if (access == Access::Read) return (state & kWriteStates) == 0 || state == kUavState;
        if (access == Access::Write || access == Access::ReadWrite || access == Access::Uav) {
            return (state & kWriteStates) != 0 && (access != Access::Uav || state == kUavState);
        }
        return false;
    }

    std::unordered_map<void*, uint64_t> versions;
    std::unordered_map<void*, std::vector<Frontier>> frontiers;
    uint32_t lastPass = 0;
    uint64_t serial = 1;
    bool poisoned = false;
};

}
