/**
* @copyright 2026 - Max Bebök
* @license MIT
*/
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "json.hpp"

/**
 * Project input map: named actions bound to controller buttons and sticks.
 * Stored in the project file under "input", turned into C++ (src/p64/inputMap.h/.cpp)
 * at build time and read on the N64 by P64::Input (n64/engine/include/input/input.h).
 */
namespace Project::InputMap
{
  // Must match P64::Input::Bind on the runtime side, index = bit
  struct BindInfo
  {
    const char* id;    // saved in the project file
    const char* label; // shown in the editor
  };

  constexpr std::array<BindInfo, 18> BINDS{{
    {"A", "A"}, {"B", "B"}, {"Z", "Z"}, {"START", "Start"}, {"L", "L"}, {"R", "R"},
    {"D_UP", "D-Up"}, {"D_DOWN", "D-Down"}, {"D_LEFT", "D-Left"}, {"D_RIGHT", "D-Right"},
    {"C_UP", "C-Up"}, {"C_DOWN", "C-Down"}, {"C_LEFT", "C-Left"}, {"C_RIGHT", "C-Right"},
    {"STICK_UP", "Stick Up"}, {"STICK_DOWN", "Stick Down"},
    {"STICK_LEFT", "Stick Left"}, {"STICK_RIGHT", "Stick Right"},
  }};

  constexpr uint32_t bit(int idx) { return 1u << idx; }
  constexpr uint32_t BIND_DPAD  = bit(6) | bit(7) | bit(8) | bit(9);
  constexpr uint32_t BIND_CBTN  = bit(10) | bit(11) | bit(12) | bit(13);
  constexpr uint32_t BIND_STICK = bit(14) | bit(15) | bit(16) | bit(17);

  constexpr uint32_t MAX_ACTIONS = 32; // runtime keeps held state in one 32-bit mask

  enum class Type : int { BUTTON = 0, STICK = 1 };

  struct Action
  {
    std::string name{};
    Type type{Type::BUTTON};
    uint32_t binds{0};
    int threshold{40}; // BUTTON: raw stick units a stick direction needs (0-127)
    int deadzone{8};   // STICK: raw distance from centre that still reads as 0
    int range{80};     // STICK: raw distance that reads as full
    float curve{1.0f}; // STICK: exponent on the magnitude, 1 = linear
  };

  struct Conf
  {
    std::vector<Action> actions{};
  };

  nlohmann::json toJson(const Conf &conf);
  Conf fromJson(const nlohmann::json &j);

  /** A starting set of common actions (move, camera, confirm, cancel, ...). */
  std::vector<Action> defaultActions();

  /** Empty if the name is usable as a C++ identifier, otherwise why not. */
  std::string validateName(const std::string &name);

  /** First problem in the whole map (bad or duplicate name, too many actions), or empty. */
  std::string validate(const Conf &conf);

  /** Turns arbitrary text into a valid, unused action name. */
  std::string makeName(const std::string &base, const Conf &conf);

  /** Generated src/p64/inputMap.h: one constant per action in P64::Action. */
  std::string generateHeader(const Conf &conf);

  /** Generated src/p64/inputMap.cpp: the table P64::Input reads. */
  std::string generateSource(const Conf &conf);
}
