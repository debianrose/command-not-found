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
 */

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace fs = std::filesystem;

#ifndef __TERMUX_PREFIX__
#error "__TERMUX_PREFIX__ not defined"
#endif

constexpr std::string_view MAIN_REPO = "";
constexpr std::string_view ROOT_REPO = "root";
constexpr std::string_view X11_REPO = "x11";
constexpr std::string_view SOURCES_PREFIX = __TERMUX_PREFIX__ "/etc/apt/sources.list.d/";
constexpr size_t MAX_SUGGESTIONS_DEFAULT = 0;
constexpr int MAX_LEVENSHTEIN_DISTANCE = 3;
constexpr size_t PATH_BUFFER_SIZE = 256;
constexpr size_t COLOR_MAP_SIZE = 14;

struct ColorCode {
    std::string_view name;
    std::string_view code;
};

constexpr std::array<ColorCode, COLOR_MAP_SIZE> COLOR_MAP = {{
    {"red", "\033[31m"},
    {"green", "\033[32m"},
    {"yellow", "\033[33m"},
    {"blue", "\033[34m"},
    {"magenta", "\033[35m"},
    {"cyan", "\033[36m"},
    {"white", "\033[37m"},
    {"bright_red", "\033[91m"},
    {"bright_green", "\033[92m"},
    {"bright_magenta", "\033[95m"},
    {"bright_cyan", "\033[96m"},
    {"bright_white", "\033[97m"}
}};

static const std::string_view main_commands[] = {
#ifdef __aarch64__
#include "commands-aarch64-termux-main.h"
#elif defined __arm__
#include "commands-arm-termux-main.h"
#elif defined __i686__
#include "commands-i686-termux-main.h"
#elif defined __x86_64__
#include "commands-x86_64-termux-main.h"
#else
#error "Unsupported architecture"
#endif
    ""
};

static const std::string_view root_commands[] = {
#ifdef __aarch64__
#include "commands-aarch64-termux-root.h"
#elif defined __arm__
#include "commands-arm-termux-root.h"
#elif defined __i686__
#include "commands-i686-termux-root.h"
#elif defined __x86_64__
#include "commands-x86_64-termux-root.h"
#endif
    ""
};

static const std::string_view x11_commands[] = {
#ifdef __aarch64__
#include "commands-aarch64-termux-x11.h"
#elif defined __arm__
#include "commands-arm-termux-x11.h"
#elif defined __i686__
#include "commands-i686-termux-x11.h"
#elif defined __x86_64__
#include "commands-x86_64-termux-x11.h"
#endif
    ""
};

struct PackageInfo {
    std::string_view binary;
    std::string_view repository;
};

struct CommandEntry {
    std::string_view package;
    std::string_view binary;
    std::string_view repository;
};

struct Config {
    bool show_root{true};
    bool show_x11{true};
    bool show_suggestions{true};
    bool show_exact_match{true};
    bool show_not_found{true};
    size_t max_suggestions{MAX_SUGGESTIONS_DEFAULT};
    std::string_view custom_message;
    std::string_view color_custom;
    std::string_view message_not_found{"{cmd}: command not found"};
    std::string_view message_exact_match{"The program {cmd} is not installed. Install it by executing:"};
    std::string_view message_suggestion{"No command {cmd} found, did you mean:"};
    std::string_view message_command_in_package{" Command {cmd} in package {pkg}"};
    std::string_view package_manager{"pkg"};
    
    uint64_t ignored_packages_mask{0};
};

static size_t parse_commands(const std::string_view* commands, size_t count, 
                             CommandEntry* entries, size_t max_entries,
                             std::string_view repository) noexcept {
    size_t entry_count = 0;
    std::string_view current_package;
    
    for (size_t i = 0; i < count && entry_count < max_entries; ++i) {
        std::string_view line = commands[i];
        if (line.empty()) continue;
        
        if (line[0] != ' ') {
            current_package = line;
        } else if (!current_package.empty() && line.length() > 1) {
            entries[entry_count++] = {current_package, line.substr(1), repository};
        }
    }
    return entry_count;
}

static int fast_levenshtein(std::string_view s1, std::string_view s2) noexcept {
    const size_t len1 = s1.length();
    const size_t len2 = s2.length();
    
    if (len1 == 0) return static_cast<int>(len2);
    if (len2 == 0) return static_cast<int>(len1);
    
    std::array<int, 256> small_buffer;
    int* buffer;
    
    if (len2 + 1 <= small_buffer.size()) {
        buffer = small_buffer.data();
    } else {
        buffer = new int[len2 + 1];
    }
    
    for (size_t j = 0; j <= len2; ++j) {
        buffer[j] = static_cast<int>(j);
    }
    
    for (size_t i = 1; i <= len1; ++i) {
        int prev = static_cast<int>(i);
        const char c1 = s1[i - 1];
        
        for (size_t j = 1; j <= len2; ++j) {
            const int temp = buffer[j];
            const int cost = (c1 == s2[j - 1]) ? 0 : 1;
            
            buffer[j] = std::min({prev + 1,
                                  buffer[j] + 1,
                                  buffer[j - 1] + cost});
            
            prev = temp;
        }
    }
    
    const int result = buffer[len2];
    
    if (len2 + 1 > small_buffer.size()) {
        delete[] buffer;
    }
    
    return result;
}

static Config load_config() noexcept {
    Config config;
    
    const char* home = getenv("HOME");
    if (!home) return config;
    
    char config_path[PATH_BUFFER_SIZE];
    int written = snprintf(config_path, sizeof(config_path), 
                           "%s/.config/.cnfrc", home);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(config_path)) {
        return config;
    }
    
    std::ifstream file(config_path);
    if (!file.is_open()) return config;
    
    std::string line;
    line.reserve(128);
    
    while (std::getline(file, line)) {
        if (line.empty()) continue;
        
        size_t eq_pos = line.find('=');
        if (eq_pos == std::string::npos || eq_pos == 0) continue;
        
        std::string_view key(line.data(), eq_pos);
        std::string_view value(line.data() + eq_pos + 1, line.length() - eq_pos - 1);
        
        size_t comment_pos = value.find(';');
        if (comment_pos != std::string_view::npos) {
            value = value.substr(0, comment_pos);
        }
        
        if (key == "show-root") {
            config.show_root = (value == "true");
        } else if (key == "show-x11") {
            config.show_x11 = (value == "true");
        } else if (key == "show-suggestions") {
            config.show_suggestions = (value == "true");
        } else if (key == "show-exact-match") {
            config.show_exact_match = (value == "true");
        } else if (key == "show-not-found") {
            config.show_not_found = (value == "true");
        } else if (key == "max-suggestions") {
            int val = 0;
            auto [ptr, ec] = std::from_chars(value.data(), 
                                             value.data() + value.size(), val);
            if (ec == std::errc() && val >= 0) {
                config.max_suggestions = static_cast<size_t>(val);
            }
        } else if (key == "custom-message") {
            config.custom_message = value;
        } else if (key == "color-custom") {
            config.color_custom = value;
        } else if (key == "message-not-found") {
            config.message_not_found = value;
        } else if (key == "message-exact-match") {
            config.message_exact_match = value;
        } else if (key == "message-suggestion") {
            config.message_suggestion = value;
        } else if (key == "message-command-in-package") {
            config.message_command_in_package = value;
        } else if (key == "package-manager") {
            config.package_manager = value;
        }
    }
    
    return config;
}

static void print_colored(std::string_view color, std::string_view text) noexcept {
    if (color.empty()) {
        std::fwrite(text.data(), 1, text.size(), stderr);
        return;
    }
    
    for (const auto& [name, code] : COLOR_MAP) {
        if (name == color) {
            std::fwrite(code.data(), 1, code.size(), stderr);
            std::fwrite(text.data(), 1, text.size(), stderr);
            std::fwrite("\033[0m", 1, 4, stderr);
            return;
        }
    }
    
    std::fwrite(text.data(), 1, text.size(), stderr);
}

static void replace_and_write(std::string_view format, std::string_view cmd, 
                              std::string_view pkg = {}) noexcept {
    size_t last_pos = 0;
    size_t pos;
    
    while ((pos = format.find('{', last_pos)) != std::string_view::npos) {
        if (pos > last_pos) {
            std::fwrite(format.data() + last_pos, 1, pos - last_pos, stderr);
        }
        
        size_t end_pos = format.find('}', pos);
        if (end_pos == std::string_view::npos) break;
        
        std::string_view placeholder(format.data() + pos, end_pos - pos + 1);
        
        if (placeholder == "{cmd}") {
            std::fwrite(cmd.data(), 1, cmd.size(), stderr);
        } else if (placeholder == "{pkg}" && !pkg.empty()) {
            std::fwrite(pkg.data(), 1, pkg.size(), stderr);
        } else {
            std::fwrite(placeholder.data(), 1, placeholder.size(), stderr);
        }
        
        last_pos = end_pos + 1;
    }
    
    if (last_pos < format.size()) {
        std::fwrite(format.data() + last_pos, 1, format.size() - last_pos, stderr);
    }
}

static int find_best_matches(std::string_view command,
                             const std::string_view* cmd_array,
                             size_t cmd_count,
                             CommandEntry* best_matches,
                             size_t max_matches,
                             std::string_view repository) noexcept {
    
    CommandEntry entries[1024];
    size_t entry_count = parse_commands(cmd_array, cmd_count, entries, 1024, repository);
    
    int best_distance = -1;
    size_t match_count = 0;
    
    for (size_t i = 0; i < entry_count; ++i) {
        const auto& entry = entries[i];
        
        int distance = fast_levenshtein(command, entry.binary);
        
        if (distance > MAX_LEVENSHTEIN_DISTANCE) continue;
        
        if (best_distance == -1 || distance < best_distance) {
            best_distance = distance;
            best_matches[0] = entry;
            match_count = 1;
        } else if (distance == best_distance && match_count < max_matches) {
            best_matches[match_count++] = entry;
        }
    }
    
    return (match_count > 0) ? best_distance : -1;
}

static bool repo_exists(std::string_view repository) noexcept {
    if (repository.empty()) return true;
    
    char repo_path[PATH_BUFFER_SIZE];
    int written = snprintf(repo_path, sizeof(repo_path),
                          "%.*s%.*s.list",
                          static_cast<int>(SOURCES_PREFIX.size()),
                          SOURCES_PREFIX.data(),
                          static_cast<int>(repository.size()),
                          repository.data());
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(repo_path)) {
        return true;
    }
    
    std::error_code ec;
    return fs::exists(repo_path, ec);
}

int main(int argc, const char* argv[]) {
    if (argc != 2) {
        std::fprintf(stderr, "command-not-found v%s\nusage: command-not-found <command>\n", 
                    COMMAND_NOT_FOUND_VERSION);
        return 1;
    }
    
    Config config = load_config();
    std::string_view command(argv[1]);
    
    CommandEntry best_matches[32] = {};
    int best_distance = -1;
    
    size_t main_count = sizeof(main_commands)/sizeof(main_commands[0]) - 1; // -1 для завершающего элемента
    int dist = find_best_matches(command, main_commands, main_count,
                                 best_matches, 32, MAIN_REPO);
    if (dist >= 0) {
        best_distance = dist;
    }
    
    if (config.show_root) {
        size_t root_count = sizeof(root_commands)/sizeof(root_commands[0]) - 1;
        if (root_count > 0) {
            dist = find_best_matches(command, root_commands, root_count,
                                     best_matches, 32, ROOT_REPO);
            if (dist >= 0 && (best_distance == -1 || dist < best_distance)) {
                best_distance = dist;
            }
        }
    }
    
    if (config.show_x11) {
        size_t x11_count = sizeof(x11_commands)/sizeof(x11_commands[0]) - 1;
        if (x11_count > 0) {
            dist = find_best_matches(command, x11_commands, x11_count,
                                     best_matches, 32, X11_REPO);
            if (dist >= 0 && (best_distance == -1 || dist < best_distance)) {
                best_distance = dist;
            }
        }
    }
    
    if (!config.custom_message.empty()) {
        print_colored(config.color_custom, config.custom_message);
        std::fputc('\n', stderr);
    }
    
    if (best_distance == -1 || best_distance > MAX_LEVENSHTEIN_DISTANCE) {
        if (config.show_not_found) {
            replace_and_write(config.message_not_found, command);
            std::fputc('\n', stderr);
        }
    } else if (best_distance == 0) {
        if (config.show_exact_match) {
            replace_and_write(config.message_exact_match, command);
            std::fputc('\n', stderr);
            
            size_t shown = 0;
            for (size_t i = 0; i < 32 && !best_matches[i].package.empty(); ++i) {
                if (config.max_suggestions > 0 && shown >= config.max_suggestions) break;
                
                bool repo_available = repo_exists(best_matches[i].repository);
                
                std::fprintf(stderr, " %.*s install %.*s",
                            static_cast<int>(config.package_manager.size()),
                            config.package_manager.data(),
                            static_cast<int>(best_matches[i].package.size()),
                            best_matches[i].package.data());
                
                if (!best_matches[i].repository.empty() && !repo_available) {
                    std::fprintf(stderr, ", after running %.*s install %.*s-repo\n",
                                static_cast<int>(config.package_manager.size()),
                                config.package_manager.data(),
                                static_cast<int>(best_matches[i].repository.size()),
                                best_matches[i].repository.data());
                } else {
                    std::fputc('\n', stderr);
                }
                
                if (i + 1 < 32 && !best_matches[i + 1].package.empty() &&
                    (config.max_suggestions == 0 || shown + 1 < config.max_suggestions)) {
                    std::fputs("or\n", stderr);
                }
                shown++;
            }
        }
    } else if (config.show_suggestions) {
        replace_and_write(config.message_suggestion, command);
        std::fputc('\n', stderr);
        
        size_t shown = 0;
        for (size_t i = 0; i < 32 && !best_matches[i].package.empty(); ++i) {
            if (config.max_suggestions > 0 && shown >= config.max_suggestions) break;
            
            bool repo_available = repo_exists(best_matches[i].repository);
            
            replace_and_write(config.message_command_in_package, 
                             best_matches[i].binary, best_matches[i].package);
            
            if (!best_matches[i].repository.empty() && !repo_available) {
                std::fprintf(stderr, " from the %.*s-repo repository\n",
                            static_cast<int>(best_matches[i].repository.size()),
                            best_matches[i].repository.data());
            } else {
                std::fputc('\n', stderr);
            }
            shown++;
        }
    }
    
    return 127;
}
