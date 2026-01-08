
#include "quick_shortcut_menu.h"

#include "heart.h"

#include "defer.h"
#include "popup.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <linux/input-event-codes.h>
#include <random>
#include <xkbcommon/xkbcommon.h>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <ctype.h>

struct KeyPress {
    uint32_t key = 0;
    int state = 0;
};

struct KeySequence {
    std::string representation;
    std::string name;
    std::string command;
    std::vector<KeyPress> keys;

    bool possible = false;
    bool full_match = false;
};

static std::vector<KeySequence> sequences;

// and every press is eventually released (per-key balance is zero).
bool balanced(const std::vector<KeyPress> &keys) {
    std::unordered_map<int,int> balance; // balance[key] = number of outstanding presses

    for (const auto &kp : keys) {
        if (kp.state == 1) {
            // press: open one more outstanding press
            ++balance[kp.key];
        } else if (kp.state == 0) {
            // release: must have a prior outstanding press
            auto it = balance.find(kp.key);
            if (it == balance.end() || it->second == 0) {
                continue;
            }
            if (--it->second == 0)
                balance.erase(it); // optional: keep map small
        }
        // ignore other states if any
    }

    // all presses must be released
    return balance.empty();
}

std::string default_sequences = R"end(
name=edit this menu
visual=e
keys=press 18 release 18
command=xdg-open ~/.config/mylar/quick_shortcuts.txt
)end";

#include <unordered_map>

struct KeyStroke {
    uint32_t keycode;
    bool shift;
};

static const std::unordered_map<char, KeyStroke> ascii_to_key = {
    // letters
    {'a', {KEY_A, false}}, {'A', {KEY_A, true}},
    {'b', {KEY_B, false}}, {'B', {KEY_B, true}},
    {'c', {KEY_C, false}}, {'C', {KEY_C, true}},
    {'d', {KEY_D, false}}, {'D', {KEY_D, true}},
    {'e', {KEY_E, false}}, {'E', {KEY_E, true}},
    {'f', {KEY_F, false}}, {'F', {KEY_F, true}},
    {'g', {KEY_G, false}}, {'G', {KEY_G, true}},
    {'h', {KEY_H, false}}, {'H', {KEY_H, true}},
    {'i', {KEY_I, false}}, {'I', {KEY_I, true}},
    {'j', {KEY_J, false}}, {'J', {KEY_J, true}},
    {'k', {KEY_K, false}}, {'K', {KEY_K, true}},
    {'l', {KEY_L, false}}, {'L', {KEY_L, true}},
    {'m', {KEY_M, false}}, {'M', {KEY_M, true}},
    {'n', {KEY_N, false}}, {'N', {KEY_N, true}},
    {'o', {KEY_O, false}}, {'O', {KEY_O, true}},
    {'p', {KEY_P, false}}, {'P', {KEY_P, true}},
    {'q', {KEY_Q, false}}, {'Q', {KEY_Q, true}},
    {'r', {KEY_R, false}}, {'R', {KEY_R, true}},
    {'s', {KEY_S, false}}, {'S', {KEY_S, true}},
    {'t', {KEY_T, false}}, {'T', {KEY_T, true}},
    {'u', {KEY_U, false}}, {'U', {KEY_U, true}},
    {'v', {KEY_V, false}}, {'V', {KEY_V, true}},
    {'w', {KEY_W, false}}, {'W', {KEY_W, true}},
    {'x', {KEY_X, false}}, {'X', {KEY_X, true}},
    {'y', {KEY_Y, false}}, {'Y', {KEY_Y, true}},
    {'z', {KEY_Z, false}}, {'Z', {KEY_Z, true}},

    // numbers
    {'1', {KEY_1, false}}, {'!', {KEY_1, true}},
    {'2', {KEY_2, false}}, {'@', {KEY_2, true}},
    {'3', {KEY_3, false}}, {'#', {KEY_3, true}},
    {'4', {KEY_4, false}}, {'$', {KEY_4, true}},
    {'5', {KEY_5, false}}, {'%', {KEY_5, true}},
    {'6', {KEY_6, false}}, {'^', {KEY_6, true}},
    {'7', {KEY_7, false}}, {'&', {KEY_7, true}},
    {'8', {KEY_8, false}}, {'*', {KEY_8, true}},
    {'9', {KEY_9, false}}, {'(', {KEY_9, true}},
    {'0', {KEY_0, false}}, {')', {KEY_0, true}},

    // whitespace & punctuation
    {' ', {KEY_SPACE, false}},
    {'\n', {KEY_ENTER, false}},
    {'\t', {KEY_TAB, false}},
    {'-', {KEY_MINUS, false}}, {'_', {KEY_MINUS, true}},
    {'=', {KEY_EQUAL, false}}, {'+', {KEY_EQUAL, true}},
    {'[', {KEY_LEFTBRACE, false}}, {'{', {KEY_LEFTBRACE, true}},
    {']', {KEY_RIGHTBRACE, false}}, {'}', {KEY_RIGHTBRACE, true}},
    {'\\', {KEY_BACKSLASH, false}}, {'|', {KEY_BACKSLASH, true}},
    {';', {KEY_SEMICOLON, false}}, {':', {KEY_SEMICOLON, true}},
    {'\'', {KEY_APOSTROPHE, false}}, {'"', {KEY_APOSTROPHE, true}},
    {',', {KEY_COMMA, false}}, {'<', {KEY_COMMA, true}},
    {'.', {KEY_DOT, false}}, {'>', {KEY_DOT, true}},
    {'/', {KEY_SLASH, false}}, {'?', {KEY_SLASH, true}},
    {'`', {KEY_GRAVE, false}}, {'~', {KEY_GRAVE, true}},
};

std::vector<KeyStroke> string_to_keys(const std::string& s) {
    std::vector<KeyStroke> result;

    for (char c : s) {
        auto it = ascii_to_key.find(c);
        if (it != ascii_to_key.end()) {
            result.push_back(it->second);
        }
        // else: unsupported character
    }

    return result;
}

void parse_sequences() {
    sequences.clear();
    const char* home = std::getenv("HOME");
    if (!home) throw std::runtime_error("HOME environment variable not set");

    std::filesystem::path filepath = std::filesystem::path(home) / ".config/mylar/quick_shortcuts.txt";
    if (!std::filesystem::exists(filepath)) {
        std::filesystem::create_directories(filepath.parent_path());
        
        std::ofstream out(filepath, std::ios::trunc);
        out << default_sequences << std::endl;
    }

    if (std::filesystem::exists(filepath)) {
        std::ifstream in(filepath);
        std::string line;
        KeySequence s;
        while (std::getline(in, line)) {
            if (line.empty()) // skip empty line
                continue;
            if (line[0] == '#') // skip comments
                continue;
            if (line.starts_with("name="))
                s.name = line.substr(strlen("name="));
            if (line.starts_with("visual="))
                s.representation = line.substr(strlen("visual="));
            if (line.starts_with("keys=")) {
                auto val = line.substr(strlen("keys="));
                std::string token;
                std::stringstream ss(val);
                bool down = false;
                while (std::getline(ss, token, ' ')) {
                    if (token == "press") {
                        down = true;
                    } else if (token == "release") {
                        down = false;
                    } else {
                        if (token.size() == 1) {
                            if (isalpha(token[0])) {
                                auto keys = string_to_keys(token);
                                auto stroke = keys[0];
                                if (stroke.shift && down)
                                    s.keys.push_back({KEY_LEFTSHIFT, down});
                                s.keys.push_back({stroke.keycode, down});
                                continue;
                            }
                        }
                        try {
                            uint32_t num = std::atoi(token.c_str());
                            s.keys.push_back({num, down});
                        } catch (...) {
                            
                        }
                    }
                }
            }
            if (line.starts_with("command="))
                s.command = line.substr(strlen("command="));
            if (!s.name.empty() && !s.command.empty() && !s.keys.empty() && !s.representation.empty()) {
                sequences.push_back(s);
                s = KeySequence();
            }
        }
        if (!s.name.empty() && !s.command.empty() && !s.keys.empty() && !s.representation.empty())
            sequences.push_back(s);
    }
}

static bool debugging_key_presses = false;

void open_quick_shortcut_menu() {
    static const float option_height = 18;
    static const float seperator_size = 5;
    float s = scale(hypriso->monitor_from_cursor());
    parse_sequences();
    
    float height = option_height * s * sequences.size() + (s * option_height * .95);
    auto p = actual_root->child(::vbox, 277, height);
    p->name = "quick_shortcut_menu";
    consume_everything(p);
    p->receive_events_even_if_obstructed = true;
    p->custom_type = (int) TYPE::OUR_POPUP;

    Bounds mb = bounds_monitor(hypriso->monitor_from_cursor());
    p->real_bounds = Bounds(mb.x + mb.w * .5 - p->wanted_bounds.w * .5, mb.h * .05, p->wanted_bounds.w, p->wanted_bounds.h);
    p->when_mouse_enters_container = paint {
        setCursorImageUntilUnset("default");
        hypriso->send_false_position(-1, -1);
        consume_event(root, c);
    };
    p->when_mouse_leaves_container = paint {
        unsetCursorImage(true);
        consume_event(root, c);
    };
    p->when_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root) return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage == (int) STAGE::RENDER_POST_WINDOWS) {
            renderfix
            auto b = c->real_bounds;
            render_drop_shadow(rid, 1.0, {0, 0, 0, .07}, 7 * s, 2.0f, b);
            rect(b, {1, 1, 1, .95}, 0, 7 * s);
        }
    };
    p->pre_layout = [](Container *actual_root, Container *c, const Bounds &b) {
        ::layout(actual_root, c, c->real_bounds);
    };

    for (int i = 0; i < sequences.size(); i++) {
        auto option = p->child(FILL_SPACE, s * option_height);
        option->skip_delete = true;
        option->custom_type = i;

        option->when_paint = [](Container *actual_root, Container *c) {
            auto root = get_rendering_root();
            if (!root) return;
            auto [rid, s, stage, active_id] = roots_info(actual_root, root);
            if (stage == (int)STAGE::RENDER_POST_WINDOWS) {
                renderfix 
                if (c->state.mouse_button_pressed == BTN_LEFT) {
                    if (c->state.mouse_pressing) {
                        auto b = c->real_bounds;
                        rect(b, {0, 0, 0, .2}, 0, 0.0, 2.0, false);
                    }
                }
                if (c->state.mouse_hovering) {
                    auto b = c->real_bounds;
                    rect(b, {0, 0, 0, .1}, 0, 0.0, 2.0f, false);
                }

                auto index = c->custom_type;
                std::string text;
                for (int i = 0; i < sequences.size(); i++) {
                    if (i == index) {
                        text = sequences[i].representation + ": " + sequences[i].name;
                    }
                }
                
                auto info = gen_text_texture(mylar_font, text, 13 * s, {0, 0, 0, 1});
                draw_texture(info, c->real_bounds.x + 14 * s, center_y(c, info.h));
                free_text_texture(info.id);
            }
        };
        option->when_clicked = paint {
            if (c->state.mouse_button_pressed != BTN_LEFT)
                return;

            auto index = c->custom_type;
            for (int i = 0; i < sequences.size(); i++) {
                if (i == index) {
                    launch_command(sequences[i].command);
                }
            }

            if (auto above = first_above_of(c, TYPE::OUR_POPUP))
                popup::close(above->uuid);
        };
    }
    auto latest_key_press = p->child(FILL_SPACE, FILL_SPACE);
    latest_key_press->when_paint = [](Container *actual_root, Container *c) {
        auto root = get_rendering_root();
        if (!root)
            return;
        auto [rid, s, stage, active_id] = roots_info(actual_root, root);
        if (stage == (int)STAGE::RENDER_POST_WINDOWS) {
            renderfix 
            if (c->state.mouse_hovering) {
                auto b = c->real_bounds;
                rect(b, {0, 0, 0, .2}, 3, 7 * s, 2.0, false);
            }
            if (c->state.mouse_button_pressed == BTN_LEFT) {
                if (c->state.mouse_pressing) {
                    auto b = c->real_bounds;
                    rect(b, {0, 0, 0, .1}, 3, 7 * s, 2.0f, false);
                }
            }

            std::string text = "Turn on key printing...";
            RGBA color = {.4, .4, .4, .9};
            if (debugging_key_presses) {
                text = "Turn off key printing";
            }
            auto info = gen_text_texture(mylar_font, text, 12 * s, color);
            draw_texture(info, c->real_bounds.x - 11 * s + c->real_bounds.w - info.w, center_y(c, info.h));
            free_text_texture(info.id);
        }
    };
    latest_key_press->when_clicked = paint {
        if (c->state.mouse_button_pressed != BTN_LEFT)
            return;

        debugging_key_presses = !debugging_key_presses;
        if (auto above = first_above_of(c, TYPE::OUR_POPUP))
            popup::close(above->uuid);
    };
    
    for (auto m : actual_monitors)
        hypriso->damage_entire(*datum<int>(m, "cid"));
}

bool quick_shortcut_menu::on_key_press(int id, int key, int state, bool update_mods) {
    if (debugging_key_presses)
        notify(fz("{}", key));
    //key = key + 8; // Because to xkbcommon it's +8 from libinput
    //key = hypriso->keycode_to_keysym(key);
    
    static std::vector<KeyPress> keys;
    static int keys_pressed_since_menu_opened = 0; // lets us know how many keys to check for sequence possibility
    
    keys.push_back({(uint32_t) key, state});
    if (keys.size() > 30)
        keys.erase(keys.begin());
    bool nothing_held_down = balanced(keys);

    bool consume = false;
    auto c = container_by_name("quick_shortcut_menu", actual_root);
    bool menu_open = c;
    if (menu_open) 
        consume = true;
    if (key == KEY_ESC)
        consume = false;

    if (menu_open) {
        keys_pressed_since_menu_opened++;
        // if no sequence left possible, simply close menu
        bool a_sequence_is_possible = false;

        for (auto &s : sequences) {
            s.possible = true;
            for (int i = 0; i < keys_pressed_since_menu_opened; i++) {
                auto matching_key = keys[keys.size() - keys_pressed_since_menu_opened + i];
                if (i < s.keys.size()) {
                    if (s.keys[i].key != matching_key.key || s.keys[i].state != matching_key.state) {
                        s.possible = false;
                        goto out;
                    }
                } else {
                    s.possible = false;
                    goto out;
                }
            }
            if (s.possible && s.keys.size() == keys_pressed_since_menu_opened)
                s.full_match = true;
            out:
        }

        int possible_count = 0;
        for (auto &s : sequences)
            if (s.possible)
                possible_count++;

        if (possible_count > 0)
            a_sequence_is_possible = true;

        for (auto &s : sequences) {
            if (s.possible && s.full_match) {
                launch_command(s.command);
                a_sequence_is_possible = false; // close menu
            }
        }
            
        if (!a_sequence_is_possible) {
            keys.clear();
            popup::close(c->uuid);
        }
    } else {
        if (keys.size() >= 2 && nothing_held_down) {
            auto before = keys[keys.size() - 2];
            auto now = keys[keys.size() - 1];
            if (before.key == KEY_RIGHTALT && now.key == KEY_RIGHTALT) {
                if (before.state == 1 && now.state == 0) {
                    keys_pressed_since_menu_opened = 0;
                    open_quick_shortcut_menu();
                }
            }
        }
    }

    return consume; // conumse key event
}

