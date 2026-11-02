/**
* @copyright 2026 - Max Bebök
* @license MIT
*/
#include "doctest/doctest.h"

#include "project/inputMap.h"

using namespace Project::InputMap;

TEST_SUITE("InputMap")
{
  TEST_CASE("bind bits match the runtime")
  {
    // P64::Input::Bind in n64/engine/include/input/input.h
    CHECK(std::string(BINDS[0].id) == "A");
    CHECK(std::string(BINDS[5].id) == "R");
    CHECK(std::string(BINDS[6].id) == "D_UP");
    CHECK(std::string(BINDS[10].id) == "C_UP");
    CHECK(std::string(BINDS[14].id) == "STICK_UP");
    CHECK(std::string(BINDS[17].id) == "STICK_RIGHT");
    CHECK(BIND_STICK == 0x3C000);
  }

  TEST_CASE("json round trip")
  {
    Conf conf{};
    conf.actions = defaultActions();
    conf.actions[0].deadzone = 12;
    conf.actions[0].range = 60;
    conf.actions[0].curve = 2.0f;

    auto back = fromJson(toJson(conf));
    REQUIRE(back.actions.size() == conf.actions.size());
    for(size_t i = 0; i < conf.actions.size(); ++i) {
      CHECK(back.actions[i].name == conf.actions[i].name);
      CHECK(back.actions[i].type == conf.actions[i].type);
      CHECK(back.actions[i].binds == conf.actions[i].binds);
    }
    CHECK(back.actions[0].deadzone == 12);
    CHECK(back.actions[0].range == 60);
    CHECK(back.actions[0].curve == doctest::Approx(2.0f));
  }

  TEST_CASE("missing or broken json gives an empty map")
  {
    CHECK(fromJson(nlohmann::json::object()).actions.empty());
    CHECK(fromJson(nlohmann::json::array()).actions.empty());
    CHECK(fromJson(nlohmann::json::parse(R"({"actions":[{"name":"x","binds":["A","NOPE"]}]})")).actions[0].binds == 1u);
  }

  TEST_CASE("names")
  {
    CHECK(validateName("jump").empty());
    CHECK(validateName("jump_2").empty());
    CHECK_FALSE(validateName("").empty());
    CHECK_FALSE(validateName("2jump").empty());
    CHECK_FALSE(validateName("jump high").empty());
    CHECK_FALSE(validateName("delete").empty());

    Conf conf{};
    conf.actions = defaultActions();
    CHECK(validate(conf).empty());
    CHECK(makeName("Jump High", conf) == "Jump_High");
    CHECK(makeName("confirm", conf) == "confirm2");
    CHECK(makeName("3d", conf) == "action_3d");
    CHECK(makeName("new", conf) == "new_");

    conf.actions.push_back(conf.actions[0]);
    CHECK_FALSE(validate(conf).empty()); // duplicate
  }

  TEST_CASE("too many actions")
  {
    Conf conf{};
    for(uint32_t i = 0; i <= MAX_ACTIONS; ++i) {
      Action a{};
      a.name = "a" + std::to_string(i);
      conf.actions.push_back(a);
    }
    CHECK_FALSE(validate(conf).empty());
    conf.actions.pop_back();
    CHECK(validate(conf).empty());
  }

  TEST_CASE("generated code")
  {
    Conf conf{};
    conf.actions = defaultActions();
    auto header = generateHeader(conf);
    CHECK(header.find("constexpr uint8_t move = 0;") != std::string::npos);
    CHECK(header.find("constexpr uint8_t confirm = 2;") != std::string::npos);

    auto src = generateSource(conf);
    CHECK(src.find("ActionType::STICK") != std::string::npos);
    CHECK(src.find("ACTION_COUNT = " + std::to_string(conf.actions.size())) != std::string::npos);

    auto empty = generateSource(Conf{});
    CHECK(empty.find("ACTION_COUNT = 0") != std::string::npos);
    CHECK(empty.find("ACTIONS[] = {\n    {") != std::string::npos); // never a zero-length array
  }
}
