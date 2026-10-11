#include <bridger/translate/conversion_report.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace bridger;
using namespace bridger::translate;

TEST_CASE("Report counts issues and caps examples", "[translate]") {
    ConversionReport report;
    CHECK(report.exact());

    for (i32 i = 0; i < 20; ++i) report.add(IssueKind::UnmappedBlock, "mod:thing", {i, 0, 0});
    report.add(IssueKind::LossyBlock, "minecraft:x", {0, 0, 0}, 5);

    CHECK_FALSE(report.exact());
    CHECK(report.total(IssueKind::UnmappedBlock) == 20);
    CHECK(report.total(IssueKind::LossyBlock) == 5);
    CHECK(report.total() == 25);

    const auto* unmapped = report.issues(IssueKind::UnmappedBlock);
    REQUIRE(unmapped != nullptr);
    CHECK(unmapped->at("mod:thing").examples.size() == ConversionReport::kMaxExamples);
    CHECK(report.issues(IssueKind::ClippedBlock) == nullptr);
}

TEST_CASE("Reports from worker threads merge without losing counts", "[translate]") {
    ConversionReport a;
    ConversionReport b;
    a.add(IssueKind::UnmappedBiome, "mod:biome", {1, 2, 3}, 2);
    b.add(IssueKind::UnmappedBiome, "mod:biome", {4, 5, 6}, 3);
    b.add(IssueKind::RemovedEntity, "minecraft:cow", {0, 0, 0});

    a.merge(b);
    CHECK(a.total(IssueKind::UnmappedBiome) == 5);
    CHECK(a.total(IssueKind::RemovedEntity) == 1);
    CHECK(a.issues(IssueKind::UnmappedBiome)->at("mod:biome").examples.size() == 2);
}
