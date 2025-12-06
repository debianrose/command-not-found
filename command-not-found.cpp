/**
 * command-not-found - utility for suggesting package installations
 * 
 * Based on original Termux command-not-found utility
 * Modifications: Copyright 2025 Vladislav Demianov
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * 
 * This file contains modifications to the original Termux command-not-found utility.
 */
#include <cstring>
#include <filesystem>
#include <iostream>
#include <list>
#include <map>
#include <string>
#include <string_view>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <sys/cdefs.h>

#ifndef __TERMUX_PREFIX__
#error "__TERMUX_PREFIX__ not defined"
#endif

const std::list<std::string_view> main_commands = {
#ifdef __aarch64__
#include "commands-aarch64-termux-main.h"
#elif defined __arm__
#include "commands-arm-termux-main.h"
#elif defined __i686__
#include "commands-i686-termux-main.h"
#elif defined __x86_64__
#include "commands-x86_64-termux-main.h"
#else
#error Failed to detect arch
#endif
};

const std::list<std::string_view> root_commands = {
#ifdef __aarch64__
#include "commands-aarch64-termux-root.h"
#elif defined __arm__
#include "commands-arm-termux-root.h"
#elif defined __i686__
#include "commands-i686-termux-root.h"
#elif defined __x86_64__
#include "commands-x86_64-termux-root.h"
#else
#error Failed to detect arch
#endif
};

const std::list<std::string_view> x11_commands = {
#ifdef __aarch64__
#include "commands-aarch64-termux-x11.h"
#elif defined __arm__
#include "commands-arm-termux-x11.h"
#elif defined __i686__
#include "commands-i686-termux-x11.h"
#elif defined __x86_64__
#include "commands-x86_64-termux-x11.h"
#else
#error Failed to detect arch
#endif
};

struct info {
  std::string binary, repository;
};

struct Config {
    bool show_root = true;
    bool show_x11 = true;
    std::string custom_message = "";
    std::string color_custom = "";
    std::string message_not_found = "{cmd}: command not found";
    std::string message_exact_match = "The program {cmd} is not installed. Install it by executing:";
    std::string message_suggestion = "No command {cmd} found, did you mean:";
    std::string message_command_in_package = " Command {cmd} in package {pkg}";
    std::map<std::string, bool> ignored_packages;
    int max_suggestions = 0;
    bool show_suggestions = true;
    bool show_exact_match = true;
    bool show_not_found = true;
    std::string package_manager = "pkg";
};

std::string replace_placeholders(const std::string& text, const std::string& cmd, const std::string& pkg = "") {
    std::string result = text;
    size_t pos;

    pos = result.find("{cmd}");
    while (pos != std::string::npos) {
        result.replace(pos, 5, cmd);
        pos = result.find("{cmd}", pos + cmd.length());
    }

    pos = result.find("{pkg}");
    while (pos != std::string::npos) {
        result.replace(pos, 5, pkg);
        pos = result.find("{pkg}", pos + pkg.length());
    }

    return result;
}

void print_colored(const std::string& color, const std::string& text) {
    if (color.empty()) {
        std::cerr << text;
        return;
    }

    std::map<std::string, std::string> colors = {
        {"red", "\033[31m"},
        {"green", "\033[32m"},
        {"yellow", "\033[33m"},
        {"blue", "\033[34m"},
        {"magenta", "\033[35m"},
        {"cyan", "\033[36m"},
        {"white", "\033[37m"},
        {"bright_red", "\033[91m"},
        {"bright_green", "\033[92m"},
        {"bright_yellow", "\033[93m"},
        {"bright_blue", "\033[94m"},
        {"bright_magenta", "\033[95m"},
        {"bright_cyan", "\033[96m"},
        {"bright_white", "\033[97m"},
        {"reset", "\033[0m"}
    };

    auto it = colors.find(color);
    if (it != colors.end()) {
        std::cerr << it->second << text << "\033[0m";
    } else {
        std::cerr << text;
    }
}

Config load_config() {
    Config config;
    std::string config_path = std::string(getenv("HOME")) + "/.config/.cnfrc";
    std::ifstream config_file(config_path);
    if (!config_file.is_open()) return config;

    std::string line;
    while (std::getline(config_file, line)) {
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos) continue;

        std::string key = line.substr(0, eq_pos);
        std::string value = line.substr(eq_pos + 1);

        std::stringstream ss(value);
        std::string token;
        std::getline(ss, token, ';');
        value = token;

        if (key == "show-root") config.show_root = (value == "true");
        else if (key == "show-x11") config.show_x11 = (value == "true");
        else if (key == "custom-message") config.custom_message = value;
        else if (key == "color-custom") config.color_custom = value;
        else if (key == "message-not-found") config.message_not_found = value;
        else if (key == "message-exact-match") config.message_exact_match = value;
        else if (key == "message-suggestion") config.message_suggestion = value;
        else if (key == "message-command-in-package") config.message_command_in_package = value;
        else if (key == "max-suggestions") config.max_suggestions = std::stoi(value);
        else if (key == "show-suggestions") config.show_suggestions = (value == "true");
        else if (key == "show-exact-match") config.show_exact_match = (value == "true");
        else if (key == "show-not-found") config.show_not_found = (value == "true");
        else if (key == "package-manager") config.package_manager = value;
        else if (key.find("ignore-") == 0) {
            std::string pkg = key.substr(7);
            config.ignored_packages[pkg] = (value == "true");
        }
    }
    return config;
}

inline int termux_min3(int a, int b, int c) {
  return (a < b ? (a < c ? a : c) : (b < c ? b : c));
}

int termux_levenshtein_distance(char const *s1, char const *s2) {
  int s1len = strlen(s1);
  int s2len = strlen(s2);
  int x, y;
  int **matrix;
  int distance;
  matrix = (int **)malloc(sizeof *matrix * (s2len + 1));

  if (!matrix) {
    std::cerr << "Memory allocation seem to have failed" << std::endl;
    return -2;
  }

  matrix[0] = (int *)malloc(sizeof *matrix[0] * (s1len + 1));

  if (!matrix[0]) {
    std::cerr << "Memory allocation seem to have failed" << std::endl;
    return -3;
  }

  matrix[0][0] = 0;
  for (x = 1; x <= s2len; x++) {
    matrix[x] = (int *)malloc(sizeof *matrix[x] * (s1len + 1));

    if (!matrix[x]) {
      std::cerr << "Memory allocation seem to have failed" << std::endl;
      return -4;
    }

    matrix[x][0] = matrix[x - 1][0] + 1;
  }
  for (y = 1; y <= s1len; y++) {
    matrix[0][y] = matrix[0][y - 1] + 1;
  }
  for (x = 1; x <= s2len; x++) {
    for (y = 1; y <= s1len; y++) {
      matrix[x][y] =
          termux_min3(matrix[x - 1][y] + 1, matrix[x][y - 1] + 1,
                      matrix[x - 1][y - 1] + (s1[y - 1] == s2[x - 1] ? 0 : 1));
    }
  }
  distance = matrix[s2len][s1len];

  for (x = 0; x <= s2len; x++) {
    free(matrix[x]);
  }
  free(matrix);

  return distance;
}

int termux_look_for_packages(const char *command_not_found,
                             const std::list<std::string_view> &cmds,
                             int *best_distance,
                             std::map<std::string, info> &pkg_map,
                             const char repository[],
                             const Config &config) {
  std::string current_package;
  std::string current_binary;
  int distance;
  for (auto it = cmds.begin(); it != cmds.end(); ++it) {
    std::string_view current_line = *it;
    if (current_line[0] != ' ') {
      current_package = current_line;
    } else {
      current_binary = current_line.substr(1);
      if (config.ignored_packages.find(current_package) != config.ignored_packages.end()) {
          continue;
      }
      distance = termux_levenshtein_distance(command_not_found,
                                             current_binary.c_str());
      if (distance < -1) {
        return -distance;
      } else if (*best_distance == distance) {
        pkg_map.insert(std::pair<std::string, info>(
            current_package, {current_binary, repository}));
      } else if (*best_distance == -1 || distance < *best_distance) {
        pkg_map.clear();
        *best_distance = distance;
        pkg_map.insert(std::pair<std::string, info>(
            current_package, {current_binary, repository}));
      }
    }
  }
  return 0;
}

int main(int argc, const char *argv[]) {
  if (argc != 2) {
    std::cerr << "command-not-found v" COMMAND_NOT_FOUND_VERSION << std::endl
              << "usage: command-not-found <command>" << std::endl;
    return 1;
  }

  Config config = load_config();
  const char *command = argv[1];
  int best_distance = -1;
  std::map<std::string, info> package_map;
  std::map<std::string, info>::iterator it;
  int res;
  std::string_view sources_prefix =
      __TERMUX_PREFIX__ "/etc/apt/sources.list.d/";

  res = termux_look_for_packages(command, main_commands, &best_distance,
                                 package_map, "", config);
  if (res != 0) {
    return res;
  }

  if (config.show_root) {
    res = termux_look_for_packages(command, root_commands, &best_distance,
                                   package_map, "root", config);
    if (res != 0) {
      return res;
    }
  }

  if (config.show_x11) {
    res = termux_look_for_packages(command, x11_commands, &best_distance,
                                   package_map, "x11", config);
    if (res != 0) {
      return res;
    }
  }

  if (!config.custom_message.empty()) {
    if (!config.color_custom.empty()) {
      print_colored(config.color_custom, config.custom_message);
      std::cerr << std::endl;
    } else {
      std::cerr << config.custom_message << std::endl;
    }
  }

  if (best_distance == -1 || best_distance > 3) {
    if (config.show_not_found) {
      std::string msg = replace_placeholders(config.message_not_found, command);
      std::cerr << msg << std::endl;
    }
  } else if (best_distance == 0) {
    if (config.show_exact_match) {
      std::string msg = replace_placeholders(config.message_exact_match, command);
      std::cerr << msg << std::endl;

      int count = 0;
      for (it = package_map.begin(); it != package_map.end(); ++it) {
        if (config.max_suggestions > 0 && count >= config.max_suggestions) break;

        std::cerr << " " << config.package_manager << " install " << it->first;
        if (it->second.repository != "" &&
            !std::filesystem::exists(std::string(sources_prefix) +
                                     it->second.repository + ".list")) {
          std::cerr << ", after running " << config.package_manager << " install " << it->second.repository
                    << "-repo" << std::endl;
        } else {
          std::cerr << std::endl;
        }
        if (next(it) != package_map.end() &&
            (config.max_suggestions == 0 || count + 1 < config.max_suggestions)) {
          std::cerr << "or" << std::endl;
        }
        count++;
      }
    }
  } else {
    if (config.show_suggestions) {
      std::string msg = replace_placeholders(config.message_suggestion, command);
      std::cerr << msg << std::endl;

      int count = 0;
      for (it = package_map.begin(); it != package_map.end(); ++it) {
        if (config.max_suggestions > 0 && count >= config.max_suggestions) break;

        std::string msg_pkg = replace_placeholders(config.message_command_in_package,
                                                   it->second.binary, it->first);
        std::cerr << msg_pkg;
        if (it->second.repository != "" &&
            !std::filesystem::exists(std::string(sources_prefix) +
                                     it->second.repository + ".list")) {
          std::cerr << " from the " << it->second.repository << "-repo repository"
                    << std::endl;
        } else {
          std::cerr << std::endl;
        }
        count++;
      }
    }
  }
  return 127;
}
