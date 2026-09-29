#include "../src/util/json.h"
#include "test_util.h"

#include <chrono>
#include <string>

using fernsdr::Json;

TEST_CASE(json_parses_a_control_message) {
    Json value;
    CHECK(Json::parse(R"({"type":"tune","freq":7100000,"mode":"usb","bw":[300,2800],"nr":false})", value));
    CHECK(value["type"].string() == "tune");
    CHECK_NEAR(value["freq"].number(), 7100000.0, 1e-9);
    CHECK(value["mode"].string() == "usb");
    CHECK_EQ(static_cast<long long>(value["bw"].size()), 2);
    CHECK_NEAR(value["bw"][0].number(), 300.0, 1e-9);
    CHECK(!value["nr"].boolean(true));
}

TEST_CASE(json_missing_fields_yield_defaults) {
    Json value;
    CHECK(Json::parse(R"({"a":1})", value));
    CHECK(value["absent"].is_null());
    CHECK_NEAR(value["absent"].number(42.0), 42.0, 1e-9);
    CHECK(value["absent"].string("fallback") == "fallback");
    CHECK(value["a"].string("fallback") == "fallback");  // wrong type, same treatment
    CHECK(value[99u].is_null());
}

TEST_CASE(json_round_trips_through_serialisation) {
    Json object = Json::make_object();
    object.set("name", "40m");
    object.set("center", 7100000.0);
    object.set("enabled", true);
    object.set("nothing", Json());
    Json array = Json::make_array();
    array.push_back(1.0);
    array.push_back("two");
    object.set("list", array);

    Json reparsed;
    CHECK(Json::parse(object.serialize(), reparsed));
    CHECK(reparsed["name"].string() == "40m");
    CHECK_NEAR(reparsed["center"].number(), 7100000.0, 1e-6);
    CHECK(reparsed["enabled"].boolean());
    CHECK(reparsed["nothing"].is_null());
    CHECK(reparsed["list"][1].string() == "two");
}

TEST_CASE(json_serialises_integers_without_exponent_notation) {
    // Frequencies must survive as exact integers: 7.1e+06 in the wire format
    // would be both ugly and lossy.
    Json object = Json::make_object();
    object.set("freq", 1234567890.0);
    CHECK(object.serialize() == R"({"freq":1234567890})");
}

TEST_CASE(json_escapes_strings_correctly) {
    Json object = Json::make_object();
    object.set("text", std::string("quote\" backslash\\ newline\n control\x01"));
    Json reparsed;
    CHECK(Json::parse(object.serialize(), reparsed));
    CHECK(reparsed["text"].string() == "quote\" backslash\\ newline\n control\x01");
}

TEST_CASE(json_decodes_escapes_including_unicode) {
    Json value;
    CHECK(Json::parse(R"({"s":"aéb\tc"})", value));
    CHECK(value["s"].string() == "a\xc3\xa9" "b\tc");
}

TEST_CASE(json_rejects_malformed_input) {
    Json value;
    CHECK(!Json::parse("", value));
    CHECK(!Json::parse("{", value));
    CHECK(!Json::parse(R"({"a":})", value));
    CHECK(!Json::parse(R"({"a" 1})", value));
    CHECK(!Json::parse(R"({"a":1,})", value));
    CHECK(!Json::parse("[1,2", value));
    CHECK(!Json::parse(R"("unterminated)", value));
    CHECK(!Json::parse("nul", value));
    CHECK(!Json::parse(R"({"a":1} trailing)", value));
    for (const char* number : {"+1", "01", "0x10", "1.", ".5", "1e", "1e+", "-01"}) {
        CHECK(!Json::parse(number, value));
    }
}

TEST_CASE(json_serialises_large_numbers_without_integer_overflow) {
    Json value(1e300);
    Json parsed;
    CHECK(Json::parse(value.serialize(), parsed));
    CHECK(parsed.number() == 1e300);
    CHECK(Json(INFINITY).serialize() == "null");
    CHECK(Json(NAN).serialize() == "null");
}

TEST_CASE(json_rejects_deeply_nested_input) {
    // A hostile client must not be able to blow the stack.
    std::string deep;
    for (int i = 0; i < 500; i++) deep += "[";
    for (int i = 0; i < 500; i++) deep += "]";
    Json value;
    CHECK(!Json::parse(deep, value));
}

TEST_CASE(json_handles_deep_but_legal_nesting) {
    std::string ok;
    for (int i = 0; i < 20; i++) ok += "[";
    ok += "1";
    for (int i = 0; i < 20; i++) ok += "]";
    Json value;
    CHECK(Json::parse(ok, value));
}

TEST_CASE(json_refuses_a_repeated_key) {
    // Which of two values a reader believes is exactly the kind of ambiguity
    // a proxy and this server could disagree on.
    Json value;
    CHECK(!Json::parse(R"({"type":"tune","type":"chat"})", value));
    CHECK(!Json::parse(R"({"a":{"b":1,"b":2}})", value));
    CHECK(Json::parse(R"({"a":{"b":1},"b":{"a":2}})", value));
}

TEST_CASE(json_bounds_the_members_of_one_object) {
    // Each key used to be checked against every earlier one, so 7,000 keys in
    // one 64 kB listener message held the network thread for 32 ms. Keys are
    // now checked in a hash set, and an object may hold at most 1024 of them,
    // which also bounds what keys picked to collide in that set could cost.
    const auto object = [](int keys) {
        std::string text = "{";
        for (int i = 0; i < keys; i++) {
            if (i) text += ',';
            text += "\"k" + std::to_string(i) + "\":0";
        }
        return text + "}";
    };
    Json value;
    std::string reason;
    CHECK(Json::parse(object(1024), value, reason));
    CHECK_EQ(static_cast<long long>(value.size()), 1024);
    CHECK(!Json::parse(object(1025), value, reason));
    CHECK(reason.find("more than 1024 members") != std::string::npos);

    // A hundred objects of a thousand keys each still parse at once.
    std::string many = "[";
    for (int i = 0; i < 100; i++) many += (i ? "," : "") + object(1000);
    many += "]";
    const auto start = std::chrono::steady_clock::now();
    CHECK(Json::parse(many, value));
    CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < 2.0);
}

TEST_CASE(json_says_why_it_refused_a_file) {
    Json value;
    std::string reason;
    CHECK(!Json::parse(R"({"accent":"#fff","accent":"#000"})", value, reason));
    CHECK(reason.find("\"accent\" appears twice") != std::string::npos);
    CHECK(!Json::parse("{\"a\":", value, reason));
    CHECK(reason == "it is not valid JSON");
}

TEST_CASE(json_numbers_read_back_as_the_same_double) {
    // Found by fuzzing: numbers with a fraction were written with ten
    // significant digits, so 1/3 came back as 0.3333333333 and a second
    // round wrote something else again.
    for (const double value : {0.1, 1.0 / 3.0, -888888888888888.8825, 1e-7, 123456.789012345, 145800000.25,
                               -2.5e-3, 1e300, 1e15, 6.02214076e23, 4.9e-324}) {
        fernsdr::Json parsed;
        CHECK(fernsdr::Json::parse(fernsdr::Json(value).serialize(), parsed));
        CHECK(parsed.number() == value);
    }
    CHECK(fernsdr::Json(0.1).serialize() == "0.1");
    CHECK(fernsdr::Json(-2.5e-3).serialize() == "-0.0025");
}
