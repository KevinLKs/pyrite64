/**
* @copyright 2026 - Max Bebök
* @license MIT
*/
#include "inputMap.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <unordered_set>

namespace
{
  using namespace Project::InputMap;

  // Words that would not compile as a constant name in the generated header
  const std::unordered_set<std::string> RESERVED{
    "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break",
    "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept",
    "const", "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await",
    "co_return", "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast",
    "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto",
    "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
    "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register",
    "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
    "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local",
    "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
    "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
  };

  int clampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

  std::string floatLiteral(float v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.4ff", (double)v);
    return buf;
  }

  Action makeAction(const char* name, Type type, uint32_t binds) {
    Action a{};
    a.name = name;
    a.type = type;
    a.binds = binds;
    return a;
  }
}

nlohmann::json Project::InputMap::toJson(const Conf &conf)
{
  auto actions = nlohmann::json::array();
  for(const auto &a : conf.actions)
  {
    auto binds = nlohmann::json::array();
    for(size_t i = 0; i < BINDS.size(); ++i) {
      if(a.binds & bit((int)i))binds.push_back(BINDS[i].id);
    }
    actions.push_back({
      {"name", a.name},
      {"type", a.type == Type::STICK ? "stick" : "button"},
      {"binds", binds},
      {"threshold", a.threshold},
      {"deadzone", a.deadzone},
      {"range", a.range},
      {"curve", a.curve},
    });
  }
  return {{"actions", actions}};
}

Project::InputMap::Conf Project::InputMap::fromJson(const nlohmann::json &j)
{
  Conf conf{};
  if(!j.is_object())return conf;

  for(const auto &aj : j.value("actions", nlohmann::json::array()))
  {
    if(!aj.is_object())continue;
    Action a{};
    a.name = aj.value("name", "");
    a.type = aj.value("type", "button") == "stick" ? Type::STICK : Type::BUTTON;
    for(const auto &b : aj.value("binds", nlohmann::json::array())) {
      if(!b.is_string())continue;
      auto id = b.get<std::string>();
      for(size_t i = 0; i < BINDS.size(); ++i) {
        if(id == BINDS[i].id)a.binds |= bit((int)i);
      }
    }
    a.threshold = clampInt(aj.value("threshold", 40), 0, 127);
    a.deadzone = clampInt(aj.value("deadzone", 8), 0, 127);
    a.range = clampInt(aj.value("range", 80), 1, 127);
    a.curve = std::clamp(aj.value("curve", 1.0f), 0.1f, 8.0f);
    conf.actions.push_back(a);
  }
  return conf;
}

std::vector<Project::InputMap::Action> Project::InputMap::defaultActions()
{
  constexpr uint32_t A = bit(0), B = bit(1), Z = bit(2), START = bit(3), L = bit(4), R = bit(5);
  return {
    makeAction("move",       Type::STICK,  BIND_STICK | BIND_DPAD),
    makeAction("camera",     Type::STICK,  BIND_CBTN),
    makeAction("confirm",    Type::BUTTON, A),
    makeAction("cancel",     Type::BUTTON, B),
    makeAction("pause",      Type::BUTTON, START),
    makeAction("trigger_z",  Type::BUTTON, Z),
    makeAction("shoulder_l", Type::BUTTON, L),
    makeAction("shoulder_r", Type::BUTTON, R),
    makeAction("menu_up",    Type::BUTTON, bit(6)  | bit(14)),
    makeAction("menu_down",  Type::BUTTON, bit(7)  | bit(15)),
    makeAction("menu_left",  Type::BUTTON, bit(8)  | bit(16)),
    makeAction("menu_right", Type::BUTTON, bit(9)  | bit(17)),
  };
}

std::string Project::InputMap::validateName(const std::string &name)
{
  if(name.empty())return "Name is empty";
  if(std::isdigit((unsigned char)name[0]))return "Name can't start with a number";
  for(char c : name) {
    if(!(std::isalnum((unsigned char)c) || c == '_'))return "Use only letters, numbers and _";
  }
  if(RESERVED.contains(name))return "'" + name + "' is a reserved C++ word";
  return "";
}

std::string Project::InputMap::validate(const Conf &conf)
{
  if(conf.actions.size() > MAX_ACTIONS) {
    return "Too many actions (" + std::to_string(conf.actions.size()) + "), the limit is " + std::to_string(MAX_ACTIONS);
  }
  std::unordered_set<std::string> seen{};
  for(const auto &a : conf.actions)
  {
    auto err = validateName(a.name);
    if(!err.empty())return "Action '" + a.name + "': " + err;
    if(!seen.insert(a.name).second)return "Action '" + a.name + "' exists twice";
  }
  return "";
}

std::string Project::InputMap::makeName(const std::string &base, const Conf &conf)
{
  std::string name{};
  for(char c : base) {
    if(std::isalnum((unsigned char)c) || c == '_')name += c;
    else if(c == ' ' || c == '-')name += '_';
  }
  if(name.empty() || std::isdigit((unsigned char)name[0]))name = "action_" + name;
  if(RESERVED.contains(name))name += "_";

  auto taken = [&](const std::string &n) {
    return std::any_of(conf.actions.begin(), conf.actions.end(), [&](const Action &a){ return a.name == n; });
  };
  if(!taken(name))return name;
  for(int i = 2;; ++i) {
    auto candidate = name + std::to_string(i);
    if(!taken(candidate))return candidate;
  }
}

std::string Project::InputMap::generateHeader(const Conf &conf)
{
  std::string res =
    "// NOTE: This file is autogenerated. Do not edit manually.\n"
    "// Actions are set in the editor under Project > Settings > Input.\n"
    "#pragma once\n"
    "#include <input/input.h>\n"
    "\n"
    "namespace P64::Action\n"
    "{\n";
  for(size_t i = 0; i < conf.actions.size(); ++i) {
    res += "  constexpr uint8_t " + conf.actions[i].name + " = " + std::to_string(i) + ";\n";
  }
  res += "}\n";
  return res;
}

std::string Project::InputMap::generateSource(const Conf &conf)
{
  std::string table{};
  std::string names{};
  for(const auto &a : conf.actions)
  {
    char binds[16];
    snprintf(binds, sizeof(binds), "0x%05X", a.binds);
    table += std::string("    {") + binds + ", ActionType::" + (a.type == Type::STICK ? "STICK" : "BUTTON")
      + ", " + std::to_string(clampInt(a.threshold, 0, 127))
      + ", " + std::to_string(clampInt(a.deadzone, 0, 127))
      + ", " + std::to_string(clampInt(a.range, 1, 127))
      + ", " + floatLiteral(a.curve) + "}, // " + a.name + "\n";
    names += "    \"" + a.name + "\",\n";
  }
  // C++ has no zero-length arrays, keep one unused entry for an empty map
  if(conf.actions.empty()) {
    table = "    {0, ActionType::BUTTON, 0, 0, 1, 1.0f},\n";
    names = "    \"\",\n";
  }

  return
    "// NOTE: This file is autogenerated. Do not edit manually.\n"
    "#include \"inputMap.h\"\n"
    "\n"
    "namespace P64::Input\n"
    "{\n"
    "  extern const ActionDef ACTIONS[];\n"
    "  extern const char* const ACTION_NAMES[];\n"
    "  extern const uint8_t ACTION_COUNT;\n"
    "\n"
    "  const ActionDef ACTIONS[] = {\n" + table + "  };\n"
    "\n"
    "  const char* const ACTION_NAMES[] = {\n" + names + "  };\n"
    "\n"
    "  const uint8_t ACTION_COUNT = " + std::to_string(conf.actions.size()) + ";\n"
    "}\n";
}
