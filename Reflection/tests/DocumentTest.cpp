/// Coverage for the format-agnostic `Value` document tree (`to_document`/`from_document`) and
/// the default JSON text encoder/decoder built on top of it (`write_json`/`parse_json`).

#include <cstdio>
#include <optional>
#include <unordered_map>
#include <vector>

#include <Reflection/Reflection.hpp>

namespace {

    int failures = 0;

    void check(bool condition, const char *description) {
        if (!condition) {
            (void)std::fprintf(stderr, "DocumentTest: %s\n", description);
            ++failures;
        }
    }

    struct Address {
        UString city;
        i32 zip = 0;
    };

    struct Person {
        UString name;
        i32 age = 0;
        bool active = false;
        f64 score = 0.0;
        std::vector<i32> lucky_numbers;
        std::vector<UString> tags;
        std::unordered_map<UString, i32> scores_by_name;
        std::unordered_map<i32, UString> names_by_id;
        std::optional<i32> nickname_length;
        std::optional<Address> home;
        Address work;
    };

} // namespace

SFT_REFLECT_TYPE(Address, "test.reflection.document.address");
SFT_REFLECT_FIELD(city);
SFT_REFLECT_FIELD(zip);
SFT_REFLECT_END();

SFT_REFLECT_TYPE(Person, "test.reflection.document.person");
SFT_REFLECT_FIELD(name);
SFT_REFLECT_FIELD(age);
SFT_REFLECT_FIELD(active);
SFT_REFLECT_FIELD(score);
SFT_REFLECT_FIELD(lucky_numbers);
SFT_REFLECT_FIELD(tags);
SFT_REFLECT_FIELD(scores_by_name);
SFT_REFLECT_FIELD(names_by_id);
SFT_REFLECT_FIELD(nickname_length);
SFT_REFLECT_FIELD(home);
SFT_REFLECT_FIELD(work);
SFT_REFLECT_END();

namespace {

    [[nodiscard]] Person make_populated_person() {
        using namespace SFT::Reflection;
        Person person;
        person.name = UString{"Ada"};
        person.age = 37;
        person.active = true;
        person.score = 98.5;
        person.lucky_numbers = {7, 13, 42};
        person.tags = {UString{"engineer"}, UString{"pioneer"}};
        person.scores_by_name = {{UString{"math"}, 100}, {UString{"art"}, 60}};
        person.names_by_id = {{1, UString{"Ada"}}, {2, UString{"Grace"}}};
        person.nickname_length = 3;
        person.home = Address{.city = UString{"London"}, .zip = 12345};
        person.work = Address{.city = UString{"Cambridge"}, .zip = 67890};
        return person;
    }

} // namespace

int main() {
    using namespace SFT::Reflection;

    TypeRegistry &registry = TypeRegistry::instance();
    const TypeInfo &address_type = registry.type<Address>();
    const TypeInfo &person_type = registry.type<Person>();

    // ── to_document: shape checks on a fully-populated instance ──────────────────────────────
    const Person original = make_populated_person();
    auto document_result = to_document(person_type, &original);
    check(document_result.has_value(), "to_document must succeed for a fully-populated Person");

    const Value &document = *document_result;
    check(document.is_object(), "to_document must produce an Object for a struct");

    const Value *name_value = document.find("name");
    check(name_value != nullptr && name_value->is_string() && name_value->as_string().cpp_string_view() == "Ada",
          "the 'name' field must become a String value");

    const Value *age_value = document.find("age");
    check(age_value != nullptr && age_value->is_int() && age_value->as_int() == 37, "the 'age' field must become an Int value");

    const Value *active_value = document.find("active");
    check(active_value != nullptr && active_value->is_bool() && active_value->as_bool(), "the 'active' field must become a Bool value");

    const Value *score_value = document.find("score");
    check(score_value != nullptr && score_value->is_float() && score_value->as_float() == 98.5, "the 'score' field must become a Float value");

    const Value *lucky_value = document.find("lucky_numbers");
    check(lucky_value != nullptr && lucky_value->is_array() && lucky_value->size() == 3 && lucky_value->at(1).as_int() == 13,
          "a vector<i32> field must become an Array of Int values");

    const Value *tags_value = document.find("tags");
    check(tags_value != nullptr && tags_value->is_array() && tags_value->size() == 2 && tags_value->at(0).as_string().cpp_string_view() == "engineer",
          "a vector<UString> field must become an Array of String values");

    const Value *scores_value = document.find("scores_by_name");
    check(scores_value != nullptr && scores_value->is_object(), "a UString-keyed map must become an Object");
    const Value *math_score = scores_value != nullptr ? scores_value->find("math") : nullptr;
    check(math_score != nullptr && math_score->is_int() && math_score->as_int() == 100, "a UString-keyed map's members must be reachable by real string keys");

    const Value *names_value = document.find("names_by_id");
    check(names_value != nullptr && names_value->is_array() && names_value->size() == 2,
          "a non-string-keyed map must become an Array of [key, value] pairs");
    check(names_value != nullptr && names_value->at(0).is_array() && names_value->at(0).size() == 2,
          "each non-string-keyed map entry must be a two-element pair");

    const Value *nickname_value = document.find("nickname_length");
    check(nickname_value != nullptr && nickname_value->is_int() && nickname_value->as_int() == 3,
          "a present optional<int> field must become the contained Int value");

    const Value *home_value = document.find("home");
    check(home_value != nullptr && home_value->is_object(), "a present optional<Address> field must become the nested Object");
    check(home_value != nullptr && home_value->find("city") != nullptr && home_value->find("city")->as_string().cpp_string_view() == "London",
          "a nested optional struct's fields must round-trip through to_document");

    const Value *work_value = document.find("work");
    check(work_value != nullptr && work_value->is_object() && work_value->find("zip") != nullptr && work_value->find("zip")->as_int() == 67890,
          "a plain (non-optional) nested struct field must become a nested Object");

    // ── to_document: an absent optional becomes Null ─────────────────────────────────────────
    Person sparse;
    sparse.name = UString{"Empty"};
    auto sparse_document_result = to_document(person_type, &sparse);
    check(sparse_document_result.has_value(), "to_document must succeed for a sparsely-populated Person");
    const Value *sparse_nickname = sparse_document_result->find("nickname_length");
    check(sparse_nickname != nullptr && sparse_nickname->is_null(), "an absent optional<int> field must become a Null value");
    const Value *sparse_home = sparse_document_result->find("home");
    check(sparse_home != nullptr && sparse_home->is_null(), "an absent optional<Address> field must become a Null value");

    // ── from_document: full round trip through to_document ───────────────────────────────────
    Person restored;
    auto restore_result = from_document(person_type, &restored, document);
    check(restore_result.has_value(), "from_document must succeed round-tripping to_document's own output");
    check(restored.name.cpp_string_view() == "Ada" && restored.age == 37 && restored.active && restored.score == 98.5,
          "from_document must restore scalar/string fields");
    check(restored.lucky_numbers == original.lucky_numbers, "from_document must restore a vector<i32> field");
    check(restored.tags.size() == 2 && restored.tags[0].cpp_string_view() == "engineer", "from_document must restore a vector<UString> field");
    check(restored.scores_by_name.size() == 2 && restored.scores_by_name.at(UString{"math"}) == 100,
          "from_document must restore a UString-keyed map field");
    check(restored.names_by_id.size() == 2 && restored.names_by_id.at(1).cpp_string_view() == "Ada",
          "from_document must restore a non-string-keyed map field");
    check(restored.nickname_length.has_value() && *restored.nickname_length == 3, "from_document must restore a present optional<int>");
    check(restored.home.has_value() && restored.home->city.cpp_string_view() == "London", "from_document must restore a present optional<Address>");
    check(restored.work.zip == 67890, "from_document must restore a plain nested struct field");

    // ── from_document: absent Null clears a stale optional; missing member leaves field alone ──
    Person stale;
    stale.nickname_length = 999;
    stale.home = Address{.city = UString{"Stale"}, .zip = 1};
    stale.age = 1;
    auto clear_result = from_document(person_type, &stale, *sparse_document_result);
    check(clear_result.has_value(), "from_document must succeed against a document with Null optionals");
    check(!stale.nickname_length.has_value(), "from_document must clear a stale optional<int> when the document holds Null");
    check(!stale.home.has_value(), "from_document must clear a stale optional<Address> when the document holds Null");

    Value partial_document = Value::make_object();
    partial_document.set("age", Value(i64{55}));
    Person untouched;
    untouched.name = UString{"KeepMe"};
    auto partial_result = from_document(person_type, &untouched, partial_document);
    check(partial_result.has_value(), "from_document must succeed against a document missing most fields");
    check(untouched.name.cpp_string_view() == "KeepMe", "from_document must leave a field untouched when the document has no matching member");
    check(untouched.age == 55, "from_document must still apply the fields the document does have");

    // ── from_document: type mismatch is reported, not silently accepted ──────────────────────
    Value bad_document = Value::make_object();
    bad_document.set("age", Value(std::string_view{"not a number"}));
    Person untouched_by_error;
    auto mismatch_result = from_document(person_type, &untouched_by_error, bad_document);
    check(!mismatch_result.has_value() && mismatch_result.error().code == DocumentErrorCode::TypeMismatch,
          "from_document must reject a String where an Int field expects a number");

    // ── write_json / parse_json round trip ────────────────────────────────────────────────────
    const UString compact_json = write_json(document, false);
    check(!compact_json.empty(), "write_json must produce non-empty compact output");
    check(compact_json.cpp_string_view().find('\n') == std::string_view::npos, "compact write_json output must not contain newlines");

    const UString pretty_json = write_json(document, true);
    check(pretty_json.cpp_string_view().find('\n') != std::string_view::npos, "pretty write_json output must contain newlines");

    auto parsed_result = parse_json(compact_json.cpp_string_view());
    check(parsed_result.has_value(), "parse_json must successfully parse write_json's own compact output");

    Person restored_from_json;
    auto restore_from_json_result = from_document(person_type, &restored_from_json, *parsed_result);
    check(restore_from_json_result.has_value(), "from_document must accept a document that round-tripped through JSON text");
    check(restored_from_json.name.cpp_string_view() == "Ada" && restored_from_json.age == 37,
          "a Person round-tripped through JSON text must preserve scalar fields");
    check(restored_from_json.lucky_numbers == original.lucky_numbers, "a Person round-tripped through JSON text must preserve a vector<i32> field");
    check(restored_from_json.scores_by_name.size() == 2 && restored_from_json.scores_by_name.at(UString{"art"}) == 60,
          "a Person round-tripped through JSON text must preserve a UString-keyed map field");
    check(restored_from_json.names_by_id.size() == 2, "a Person round-tripped through JSON text must preserve a non-string-keyed map field");
    check(restored_from_json.home.has_value() && restored_from_json.home->zip == 12345,
          "a Person round-tripped through JSON text must preserve a present optional<Address>");

    auto pretty_parsed_result = parse_json(pretty_json.cpp_string_view());
    check(pretty_parsed_result.has_value(), "parse_json must successfully parse write_json's own pretty output too");

    // ── parse_json: hand-written text, escapes, and error cases ──────────────────────────────
    auto hand_written = parse_json(R"({"greeting": "hi\nthere", "count": 3, "ratio": 1.5e2, "flag": true, "nothing": null, "list": [1, 2, 3]})");
    check(hand_written.has_value(), "parse_json must parse a hand-written JSON object");
    check(hand_written.has_value() && hand_written->find("greeting") != nullptr &&
              hand_written->find("greeting")->as_string().cpp_string_view() == "hi\nthere",
          "parse_json must decode a \\n escape inside a string");
    check(hand_written.has_value() && hand_written->find("ratio") != nullptr && hand_written->find("ratio")->as_float() == 150.0,
          "parse_json must decode exponent notation");
    check(hand_written.has_value() && hand_written->find("nothing") != nullptr && hand_written->find("nothing")->is_null(),
          "parse_json must decode a null literal");
    check(hand_written.has_value() && hand_written->find("list") != nullptr && hand_written->find("list")->size() == 3,
          "parse_json must decode a nested array");

    check(!parse_json("{ not valid json").has_value(), "parse_json must reject malformed input");
    check(!parse_json("{}garbage").has_value(), "parse_json must reject trailing content after the top-level value");
    check(!parse_json("").has_value(), "parse_json must reject empty input");

    (void)address_type;

    if (failures != 0) {
        (void)std::fprintf(stderr, "DocumentTest: %d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
