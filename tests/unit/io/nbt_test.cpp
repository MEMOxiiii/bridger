#include "../test_util.hpp"

#include <bridger/io/compression.hpp>
#include <bridger/io/nbt/mutf8.hpp>
#include <bridger/io/nbt/nbt_value.hpp>
#include <bridger/io/nbt/nbt_view.hpp>
#include <bridger/io/nbt/nbt_writer.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace bridger;
using namespace bridger::io::nbt;

TEST_CASE("NBT reference files round-trip byte-exactly", "[io][nbt]") {
    const auto [file, endian] = GENERATE(std::pair{"nbt/all_types_be.nbt", Endian::Big},
                                         std::pair{"nbt/all_types_le.nbt", Endian::Little});
    CAPTURE(file);
    const std::vector<u8> original = test::read_file(test::data_path(file));

    const auto root = read_nbt(original, endian);
    REQUIRE(root);
    const auto encoded = write_nbt(*root, endian);
    REQUIRE(encoded);
    CHECK(*encoded == original);

    // The streaming copy path (write_view) is byte-exact too.
    const auto doc = NbtDocument::parse(original, endian);
    REQUIRE(doc);
    CHECK(doc->consumed_bytes() == original.size());
    NbtWriter w(endian);
    w.write_view(doc->root_name(), doc->root());
    const auto copied = std::move(w).finish();
    REQUIRE(copied);
    CHECK(*copied == original);
}

TEST_CASE("Big- and little-endian files decode to the same tree", "[io][nbt]") {
    const auto be = read_nbt(test::read_file(test::data_path("nbt/all_types_be.nbt")), Endian::Big);
    const auto le = read_nbt(test::read_file(test::data_path("nbt/all_types_le.nbt")), Endian::Little);
    REQUIRE(be);
    REQUIRE(le);
    CHECK(*be == *le);

    // Converting byte order through the writer.
    const auto as_le = write_nbt(*be, Endian::Little);
    REQUIRE(as_le);
    CHECK(*as_le == test::read_file(test::data_path("nbt/all_types_le.nbt")));
}

TEST_CASE("Gzip-compressed NBT reference file", "[io][nbt]") {
    const auto gz = test::read_file(test::data_path("nbt/all_types_be.nbt.gz"));
    REQUIRE(io::detect_compression(gz) == io::Compression::Gzip);
    const auto raw = io::decompress(gz, io::Compression::Gzip);
    REQUIRE(raw);
    CHECK(*raw == test::read_file(test::data_path("nbt/all_types_be.nbt")));
}

TEST_CASE("Zero-copy views read every tag type", "[io][nbt]") {
    const auto bytes = test::read_file(test::data_path("nbt/all_types_be.nbt"));
    const auto doc = NbtDocument::parse(bytes, Endian::Big);
    REQUIRE(doc);
    CHECK(doc->root_name() == "bridger test");
    const auto c = doc->root().as_compound();
    REQUIRE(c);

    CHECK(c->find("byte").as_byte() == i8{-7});
    CHECK(c->find("short").as_short() == i16{-12345});
    CHECK(c->find("int").as_int() == 123456789);
    CHECK(c->find("long").as_long() == i64{-9'000'000'000'000'000'000});
    CHECK(c->find("float").as_float() == 0.5f);
    CHECK(c->find("double").as_double() == -1234.5678);
    CHECK(c->find("string").as_string() == "hello world");
    CHECK(c->find("empty_string").as_string() == "");
    CHECK_FALSE(c->find("int").as_short()); // no implicit conversions
    CHECK_FALSE(c->find("missing"));

    const auto bytes_view = c->find("byte_array").as_byte_array();
    REQUIRE(bytes_view);
    CHECK(std::vector<u8>(bytes_view->begin(), bytes_view->end()) == std::vector<u8>{0, 1, 127, 128, 255});

    const auto ints = c->find("int_array").as_int_array();
    REQUIRE(ints);
    CHECK(ints->to_vector() == std::vector<i32>{0, -1, 2147483647, -2147483647 - 1});
    const auto longs = c->find("long_array").as_long_array();
    REQUIRE(longs);
    CHECK((*longs)[2] == 9223372036854775807LL);

    const auto items = c->find("list_of_compounds", TagType::List).as_list();
    REQUIRE(items);
    CHECK(items->element_type() == TagType::Compound);
    REQUIRE(items->size() == 2);
    CHECK(items->at(1).as_compound()->find("id").as_string() == "minecraft:stick");
    usize n = 0;
    for (const NbtView item : *items) n += item.as_compound()->size();
    CHECK(n == 4);

    const auto nested = c->find("nested").as_compound()->find("a").as_compound()->find("b").as_compound()->find("c");
    CHECK(nested.as_compound()->find("d").as_int() == 4);

    // Entry order is preserved, not sorted.
    std::vector<std::string_view> names;
    for (const NbtEntry& e : *c) names.push_back(e.name);
    REQUIRE(names.size() >= 2);
    CHECK(names[names.size() - 2] == "z_last");
    CHECK(names.back() == "a_after_z");
}

TEST_CASE("Every truncation of a valid document is rejected without crashing", "[io][nbt]") {
    const auto bytes = test::read_file(test::data_path("nbt/all_types_be.nbt"));
    for (usize len = 0; len < bytes.size(); ++len) {
        const auto doc = NbtDocument::parse(std::span<const u8>(bytes.data(), len), Endian::Big);
        CHECK_FALSE(doc);
    }
}

TEST_CASE("Malformed NBT is rejected with a precise error", "[io][nbt]") {
    // Root: compound "" containing a list with a negative length.
    const std::vector<u8> negative_list{0x0A, 0, 0, 0x09, 0, 1, 'l', 0x03, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    auto r = NbtDocument::parse(negative_list, Endian::Big);
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::Malformed);

    // Non-empty list of TAG_End.
    const std::vector<u8> end_list{0x0A, 0, 0, 0x09, 0, 1, 'l', 0x00, 0, 0, 0, 1, 0x00};
    r = NbtDocument::parse(end_list, Endian::Big);
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::Malformed);

    // Invalid tag type 13.
    const std::vector<u8> bad_type{0x0A, 0, 0, 0x0D, 0, 1, 'x', 0x00};
    r = NbtDocument::parse(bad_type, Endian::Big);
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::Malformed);
    CHECK(r.error().context().offset == 3u);

    // A list claiming 2^31-1 compounds in a tiny buffer fails fast.
    const std::vector<u8> huge_list{0x0A, 0, 0, 0x09, 0, 1, 'l', 0x0A, 0x7F, 0xFF, 0xFF, 0xFF, 0x00};
    r = NbtDocument::parse(huge_list, Endian::Big);
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::UnexpectedEof);

    // Byte array longer than the data.
    const std::vector<u8> long_array{0x0A, 0, 0, 0x07, 0, 1, 'a', 0x00, 0x00, 0x10, 0x00, 1, 2, 3};
    r = NbtDocument::parse(long_array, Endian::Big);
    REQUIRE_FALSE(r);
    CHECK(r.error().code() == ErrorCode::UnexpectedEof);
}

TEST_CASE("Nesting depth is limited", "[io][nbt]") {
    // root compound + N nested compounds named "" + closing TAG_Ends.
    auto nested = [](usize depth) {
        std::vector<u8> v{0x0A, 0, 0};
        for (usize i = 0; i < depth; ++i) v.insert(v.end(), {0x0A, 0, 0});
        v.insert(v.end(), depth + 1, 0x00);
        return v;
    };
    const auto deepest = nested(511); // 512 levels including the root
    const auto doc = NbtDocument::parse(deepest, Endian::Big);
    REQUIRE(doc);
    // Everything that walks a validated tree must cope with the maximum depth too.
    CHECK(doc->root().payload_bytes().size() == deepest.size() - 3);
    const NbtRoot tree = materialize(*doc);
    CHECK(write_nbt(tree, Endian::Big) == deepest);

    const auto too_deep = NbtDocument::parse(nested(512), Endian::Big);
    REQUIRE_FALSE(too_deep);
    CHECK(too_deep.error().code() == ErrorCode::LimitExceeded);

    NbtLimits strict;
    strict.max_depth = 4;
    CHECK_FALSE(NbtDocument::parse(nested(4), Endian::Big, strict));
}

TEST_CASE("Streaming writer produces valid NBT and catches misuse", "[io][nbt]") {
    NbtWriter w(Endian::Little);
    w.begin_root_compound("");
    w.write_int("DataVersion", 4556);
    w.begin_list("Pos", TagType::Double, 3);
    w.write_double({}, 1.5);
    w.write_double({}, 2.5);
    w.write_double({}, -3.0);
    w.end_list();
    w.begin_list("empty", TagType::End, 0);
    w.end_list();
    w.begin_compound("sub");
    w.write_string("id", "minecraft:chest");
    w.end_compound();
    w.end_compound();
    const auto bytes = std::move(w).finish();
    REQUIRE(bytes);

    const auto doc = NbtDocument::parse(*bytes, Endian::Little);
    REQUIRE(doc);
    const auto c = *doc->root().as_compound();
    CHECK(c.find("DataVersion").as_int() == 4556);
    CHECK(c.find("Pos").as_list()->at(2).as_double() == -3.0);
    CHECK(c.find("empty").as_list()->empty());
    CHECK(c.find("sub").as_compound()->find("id").as_string() == "minecraft:chest");

    NbtWriter bad(Endian::Big);
    bad.begin_root_compound();
    bad.begin_list("l", TagType::Int, 2);
    bad.write_int({}, 1);
    bad.write_string({}, "wrong type");
    CHECK_FALSE(std::move(bad).finish());

    NbtWriter unterminated(Endian::Big);
    unterminated.begin_root_compound();
    CHECK_FALSE(std::move(unterminated).finish());
}

TEST_CASE("Float bit patterns survive a round-trip", "[io][nbt]") {
    NbtCompound c;
    c.set("nan", std::bit_cast<f32>(0x7FC00123u));
    c.set("neg_zero", -0.0);
    const NbtRoot root{"", NbtValue(std::move(c))};
    const auto bytes = write_nbt(root, Endian::Big);
    REQUIRE(bytes);
    const auto back = read_nbt(*bytes, Endian::Big);
    REQUIRE(back);
    CHECK(*back == root);
    const auto* comp = back->value.get_if<NbtCompound>();
    REQUIRE(comp);
    CHECK(std::bit_cast<u32>(*comp->find("nan")->get_if<f32>()) == 0x7FC00123u);
}

TEST_CASE("Modified UTF-8 conversion", "[io][nbt]") {
    // NUL is C0 80; U+1F309 (bridge at night) is a surrogate pair of 3-byte sequences.
    const std::string utf8 = std::string("a\0b", 3) + "\xF0\x9F\x8C\x89" + "\xD8\xAC";
    const auto m = utf8_to_mutf8(utf8);
    REQUIRE(m);
    CHECK(*m == std::string("a\xC0\x80" "b" "\xED\xA0\xBC\xED\xBC\x89" "\xD8\xAC"));
    const auto back = mutf8_to_utf8(*m);
    REQUIRE(back);
    CHECK(*back == utf8);

    // Plain ASCII is passed through unchanged.
    CHECK(is_plain_ascii("minecraft:stone"));
    CHECK(utf8_to_mutf8("minecraft:stone") == std::string("minecraft:stone"));

    // Unpaired surrogate (legal in Java strings) becomes U+FFFD and is flagged.
    bool lossy = false;
    const auto lone = mutf8_to_utf8("\xED\xA0\xBC", &lossy);
    REQUIRE(lone);
    CHECK(lossy);
    CHECK(*lone == "\xEF\xBF\xBD");

    CHECK_FALSE(utf8_to_mutf8("\xFF"));        // invalid UTF-8
    CHECK_FALSE(mutf8_to_utf8("\xF0\x9F\x8C")); // 4-byte forms are not modified UTF-8
}
