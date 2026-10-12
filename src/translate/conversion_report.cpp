#include <bridger/translate/conversion_report.hpp>

namespace bridger::translate {

std::string_view to_string(IssueKind kind) noexcept {
    switch (kind) {
    case IssueKind::UnmappedBlock: return "unmapped block";
    case IssueKind::LossyBlock: return "lossy block";
    case IssueKind::UnmappedBlockEntity: return "unmapped block entity";
    case IssueKind::UnmappedEntity: return "unmapped entity";
    case IssueKind::UnmappedBiome: return "unmapped biome";
    case IssueKind::DroppedSecondaryLayer: return "dropped secondary layer";
    case IssueKind::RemovedBlockEntity: return "removed block entity";
    case IssueKind::RemovedEntity: return "removed entity";
    case IssueKind::ClippedBlock: return "clipped block";
    }
    return "unknown";
}

void ConversionReport::add(IssueKind kind, std::string_view subject, BlockPos where, u64 count) {
    IssueEntry& entry = issues_[kind][std::string(subject)];
    entry.count += count;
    if (entry.examples.size() < kMaxExamples) entry.examples.push_back({where.x + origin_.x, where.y + origin_.y, where.z + origin_.z});
}

void ConversionReport::merge(const ConversionReport& other) {
    for (const auto& [kind, subjects] : other.issues_) {
        SubjectMap& mine = issues_[kind];
        for (const auto& [subject, entry] : subjects) {
            IssueEntry& target = mine[subject];
            target.count += entry.count;
            for (const BlockPos& p : entry.examples) {
                if (target.examples.size() >= kMaxExamples) break;
                target.examples.push_back(p);
            }
        }
    }
}

u64 ConversionReport::total(IssueKind kind) const noexcept {
    const auto it = issues_.find(kind);
    if (it == issues_.end()) return 0;
    u64 sum = 0;
    for (const auto& [subject, entry] : it->second) sum += entry.count;
    return sum;
}

u64 ConversionReport::total() const noexcept {
    u64 sum = 0;
    for (const auto& [kind, subjects] : issues_)
        for (const auto& [subject, entry] : subjects) sum += entry.count;
    return sum;
}

const ConversionReport::SubjectMap* ConversionReport::issues(IssueKind kind) const noexcept {
    const auto it = issues_.find(kind);
    return it == issues_.end() ? nullptr : &it->second;
}

} // namespace bridger::translate
