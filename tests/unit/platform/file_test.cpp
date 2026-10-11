// Memory-mapped files and the atomic output writer (docs/safety.md).
// Failure tests inject faults through FileOps and check that no data is lost at
// any step and that the error message says where every file is.

#include "../test_util.hpp"

#include <bridger/platform/atomic_replace.hpp>
#include <bridger/platform/mapped_file.hpp>
#include <bridger/platform/paths.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

using namespace bridger;
using namespace bridger::platform;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::vector<u8> kOld{'o', 'l', 'd'};
const std::vector<u8> kNew{'n', 'e', 'w', '!'};

Status accept(const std::filesystem::path&) { return {}; }

// Wraps the real OS operations; fails the chosen step, simulating the file-system
// state each ReplaceFileW error code leaves behind.
class FaultyOps final : public FileOps {
public:
    enum class Fault { None, Create, Write, Flush, Close, MoveNew, Replace1175, Replace1176, Replace1177, ReplaceOther, RestoreMove };
    explicit FaultyOps(Fault f) : fault_(f) {}

    Result<Handle> create_new(const std::filesystem::path& p) override {
        if (fault_ == Fault::Create) return make_error(ErrorCode::AccessDenied, "injected create failure");
        return real().create_new(p);
    }
    Status write(Handle h, std::span<const u8> b) override {
        if (fault_ == Fault::Write) return make_error(ErrorCode::IoError, "injected write failure (disk full)");
        return real().write(h, b);
    }
    Status flush(Handle h) override {
        if (fault_ == Fault::Flush) return make_error(ErrorCode::IoError, "injected flush failure");
        return real().flush(h);
    }
    Status close(Handle h) override {
        auto s = real().close(h);
        if (fault_ == Fault::Close) return make_error(ErrorCode::IoError, "injected close failure");
        return s;
    }
    Status move_new(const std::filesystem::path& f, const std::filesystem::path& t) override {
        if (fault_ == Fault::MoveNew) return make_error(ErrorCode::AccessDenied, "injected rename failure");
        if (fault_ == Fault::RestoreMove && f.string().ends_with(".bridger-bak"))
            return make_error(ErrorCode::AccessDenied, "injected restore failure");
        return real().move_new(f, t);
    }
    Status move_replace(const std::filesystem::path& f, const std::filesystem::path& t) override {
        return real().move_replace(f, t);
    }
    ReplaceOutcome replace(const std::filesystem::path& d, const std::filesystem::path& r,
                           const std::filesystem::path& b) override {
        switch (fault_) {
        case Fault::Replace1175: return ReplaceOutcome::RemoveReplacedFailed;   // nothing moved
        case Fault::Replace1176: return ReplaceOutcome::MoveReplacementFailed;  // nothing moved
        case Fault::Replace1177:
        case Fault::RestoreMove:
            // Original was moved to the backup name; the replacement was not moved in.
            (void)real().move_new(d, b);
            return ReplaceOutcome::MoveReplacement2Failed;
        case Fault::ReplaceOther: return ReplaceOutcome::OtherError;
        default: return real().replace(d, r, b);
        }
    }
    Status remove(const std::filesystem::path& p) override { return real().remove(p); }
    bool exists(const std::filesystem::path& p) override { return real().exists(p); }

private:
    static FileOps& real() { return os_file_ops(); }
    Fault fault_;
};

usize count_files(const std::filesystem::path& dir) {
    usize n = 0;
    for ([[maybe_unused]] const auto& e : std::filesystem::directory_iterator(dir)) ++n;
    return n;
}

} // namespace

TEST_CASE("MappedFile maps file contents, including empty files", "[platform]") {
    test::TempDir dir;
    const auto path = dir.path / "data.bin";
    test::write_file(path, kNew);
    auto mf = MappedFile::open(path);
    REQUIRE(mf);
    CHECK(std::vector<u8>(mf->data().begin(), mf->data().end()) == kNew);

    test::write_file(dir.path / "empty.bin", {});
    auto empty = MappedFile::open(dir.path / "empty.bin");
    REQUIRE(empty);
    CHECK(empty->size() == 0);

    auto missing = MappedFile::open(dir.path / "missing.bin");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code() == ErrorCode::FileNotFound);
}

TEST_CASE("Atomic write creates a new file and leaves nothing behind", "[platform][safety]") {
    test::TempDir dir;
    const auto dest = dir.path / "out.mcworld";
    REQUIRE(write_file_atomically(dest, kNew, accept));
    CHECK(test::read_file(dest) == kNew);
    CHECK(count_files(dir.path) == 1);
}

TEST_CASE("Atomic write replaces an existing file and removes the backup", "[platform][safety]") {
    test::TempDir dir;
    const auto dest = dir.path / "out.mcworld";
    test::write_file(dest, kOld);
    REQUIRE(write_file_atomically(dest, kNew, accept));
    CHECK(test::read_file(dest) == kNew);
    CHECK(count_files(dir.path) == 1);
}

TEST_CASE("Unicode destination paths work", "[platform][safety]") {
    test::TempDir dir;
    const auto name = path_from_utf8("\xD8\xAC\xD8\xB3\xD8\xB1 \xF0\x9F\x8C\x89.mcworld");
    REQUIRE(name);
    REQUIRE(write_file_atomically(dir.path / *name, kNew, accept));
    CHECK(test::read_file(dir.path / *name) == kNew);
}

TEST_CASE("Validation failure discards the output and never touches the destination", "[platform][safety]") {
    test::TempDir dir;
    const auto dest = dir.path / "out.mcworld";
    test::write_file(dest, kOld);
    auto reject = [](const std::filesystem::path&) -> Status { return make_error(ErrorCode::Malformed, "bad output"); };
    const auto r = write_file_atomically(dest, kNew, reject);
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::ValidationFailed);
    CHECK(test::read_file(dest) == kOld);
    CHECK(count_files(dir.path) == 1);
}

TEST_CASE("Abandoned writer deletes its temp file", "[platform][safety]") {
    test::TempDir dir;
    {
        auto w = AtomicFileWriter::create(dir.path / "out.bin");
        REQUIRE(w);
        REQUIRE(w->write(kNew));
        CHECK(count_files(dir.path) == 1); // the temp file
    }
    CHECK(count_files(dir.path) == 0);
}

TEST_CASE("Failures before the move leave the destination untouched", "[platform][safety]") {
    using F = FaultyOps::Fault;
    const F fault = GENERATE(F::Create, F::Write, F::Flush, F::Close);
    CAPTURE(static_cast<int>(fault));
    test::TempDir dir;
    const auto dest = dir.path / "out.mcworld";
    test::write_file(dest, kOld);
    FaultyOps ops(fault);
    const auto r = write_file_atomically(dest, kNew, accept, ops);
    REQUIRE_FALSE(r);
    CHECK(test::read_file(dest) == kOld);
}

TEST_CASE("Rename failure for a new file keeps the output and says where", "[platform][safety]") {
    test::TempDir dir;
    const auto dest = dir.path / "out.mcworld";
    FaultyOps ops(FaultyOps::Fault::MoveNew);
    auto w = AtomicFileWriter::create(dest, ops);
    REQUIRE(w);
    REQUIRE(w->write(kNew));
    const auto r = w->commit(accept);
    REQUIRE_FALSE(r);
    CHECK_THAT(r.error().message(), ContainsSubstring("temp") && ContainsSubstring("present"));
    CHECK(test::read_file(w->temp_path()) == kNew); // nothing lost
    CHECK_FALSE(std::filesystem::exists(dest));
}

TEST_CASE("ReplaceFileW errors 1175/1176 leave the destination unchanged", "[platform][safety]") {
    using F = FaultyOps::Fault;
    const F fault = GENERATE(F::Replace1175, F::Replace1176);
    test::TempDir dir;
    const auto dest = dir.path / "out.mcworld";
    test::write_file(dest, kOld);
    FaultyOps ops(fault);
    auto w = AtomicFileWriter::create(dest, ops);
    REQUIRE(w);
    REQUIRE(w->write(kNew));
    const auto r = w->commit(accept);
    REQUIRE_FALSE(r);
    CHECK(test::read_file(dest) == kOld);
    CHECK(test::read_file(w->temp_path()) == kNew);
    CHECK_THAT(r.error().message(), ContainsSubstring("destination is unchanged"));
}

TEST_CASE("ReplaceFileW error 1177 restores the original from the backup", "[platform][safety]") {
    test::TempDir dir;
    const auto dest = dir.path / "out.mcworld";
    test::write_file(dest, kOld);
    FaultyOps ops(FaultyOps::Fault::Replace1177);
    auto w = AtomicFileWriter::create(dest, ops);
    REQUIRE(w);
    REQUIRE(w->write(kNew));
    const auto r = w->commit(accept);
    REQUIRE_FALSE(r);
    CHECK(test::read_file(dest) == kOld);                // original back in place
    CHECK(test::read_file(w->temp_path()) == kNew);      // new output kept
    CHECK_THAT(r.error().message(), ContainsSubstring("original was restored"));
}

TEST_CASE("If restoring the backup fails, both files survive and are reported", "[platform][safety]") {
    test::TempDir dir;
    const auto dest = dir.path / "out.mcworld";
    test::write_file(dest, kOld);
    FaultyOps ops(FaultyOps::Fault::RestoreMove);
    auto w = AtomicFileWriter::create(dest, ops);
    REQUIRE(w);
    REQUIRE(w->write(kNew));
    const auto r = w->commit(accept);
    REQUIRE_FALSE(r);
    CHECK_THAT(r.error().message(), ContainsSubstring("backup") && ContainsSubstring("could not be restored"));
    CHECK(test::read_file(w->temp_path()) == kNew);
    // The original is intact under the backup name.
    bool found_backup = false;
    for (const auto& e : std::filesystem::directory_iterator(dir.path)) {
        if (e.path().string().ends_with(".bridger-bak")) {
            CHECK(test::read_file(e.path()) == kOld);
            found_backup = true;
        }
    }
    CHECK(found_backup);
}

TEST_CASE("Destination failing validation after replace restores the previous file", "[platform][safety]") {
    test::TempDir dir;
    const auto dest = dir.path / "out.mcworld";
    test::write_file(dest, kOld);
    int calls = 0;
    auto second_fails = [&calls](const std::filesystem::path&) -> Status {
        if (++calls == 2) return make_error(ErrorCode::Malformed, "destination corrupt");
        return {};
    };
    const auto r = write_file_atomically(dest, kNew, second_fails);
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::ValidationFailed);
    CHECK(test::read_file(dest) == kOld);
}

TEST_CASE("Temp names are siblings of the destination", "[platform]") {
    const auto t = sibling_temp_path(std::filesystem::path("C:/worlds/out.mcworld"), "bridger-tmp");
    REQUIRE(t);
    CHECK(t->parent_path() == std::filesystem::path("C:/worlds"));
    CHECK(t->filename().string().starts_with("out.mcworld."));
    CHECK(t->filename().string().ends_with(".bridger-tmp"));
    CHECK(*t != *sibling_temp_path(std::filesystem::path("C:/worlds/out.mcworld"), "bridger-tmp"));
}
