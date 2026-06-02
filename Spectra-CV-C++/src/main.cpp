#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <future>
#include <mutex>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <queue>
#include <functional>
#include <condition_variable>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <opencv2/opencv.hpp>
#include "ScreenCapture.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

using json = nlohmann::json;

struct ColorBound {
    cv::Scalar lower;
    cv::Scalar upper;
};

struct ZoneConfig {
    std::string name;
    cv::Rect coords;
    std::vector<ColorBound> bounds;
    double min_ratio = 0.0;
    std::string type;
    std::map<std::string, cv::Mat> binarized_templates;
    double max_value = -1.0; // -1.0 = no max limit
    double min_value = 0.0;
    double max_drop_per_s = -1.0; // -1.0 = no physical limit
    double max_refill_per_s = -1.0;
    std::vector<std::string> allowed_formats;
};

// --- MODE DEBUG ---
bool DEBUG_MODE = true;
std::string DEBUG_AGENT_NAME = "Chamber"; // Modify to test another agent

// --- SCREEN SELECTION ---
int MONITOR_INDEX = 0; // 0 = Primary Screen, 1 = Secondary Screen, etc.

// --- FRAME RATE LIMITER (FPS) ---
int TARGET_FPS = 10; // Max 30 FPS (drastically reduces CPU usage)

std::vector<ZoneConfig> active_zones;
std::string current_agent_name = "";
std::string current_phase = "combat"; // "buy_phase" or "combat"
std::mutex config_mutex;
int screen_width = 2560;
int screen_height = 1440;
json last_state;
bool force_debug_screenshot = false;

std::map<std::string, cv::Mat> raw_templates;

std::string get_exe_dir() {
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::string exe_path(buffer);
    return exe_path.substr(0, exe_path.find_last_of("\\/"));
}

void load_templates() {
    raw_templates.clear();
    std::vector<std::string> names = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "dot",
                                      "a_bullet_0", "a_bullet_1", "a_bullet_2", "a_bullet_3", 
                                      "a_bullet_4", "a_bullet_5", "a_bullet_6", "a_bullet_7", "a_bullet_8"};
    
    // Auto-scaling: if the player plays in 1080p or 4k, but templates were captured in 1440p
    double scale_factor = (double)screen_height / 1440.0;
    if (scale_factor <= 0.1) scale_factor = 1.0; // Safety against 0 height frames

    for (const auto& name : names) {
        std::string path = get_exe_dir() + "/templates/" + name + ".png";
        cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);
        if (!img.empty()) {
            if (std::abs(scale_factor - 1.0) > 0.01) { // If the resolution is not 1080p
                cv::Mat resized;
                cv::resize(img, resized, cv::Size(), scale_factor, scale_factor, cv::INTER_LINEAR);
                raw_templates[name] = resized;
            } else {
                raw_templates[name] = img;
            }
            if (DEBUG_MODE) std::cout << "[DEBUG] Template loaded: " << path << " (Scale: " << scale_factor << "x)" << std::endl;
        }
    }
}

std::vector<int> hex_to_rgb(const std::string& hex) {
    std::string h = hex;
    if (h[0] == '#') h = h.substr(1);
    if (h.length() == 3) {
        h = std::string(1, h[0]) + h[0] + h[1] + h[1] + h[2] + h[2];
    }
    int r = std::stoi(h.substr(0, 2), nullptr, 16);
    int g = std::stoi(h.substr(2, 2), nullptr, 16);
    int b = std::stoi(h.substr(4, 2), nullptr, 16);
    return {r, g, b};
}

void load_config(const std::string& agent_name) {
    std::string config_path = get_exe_dir() + "/agents_config.json";
    std::ifstream file(config_path);
    if (!file.is_open()) {
        if (DEBUG_MODE) std::cout << "[DEBUG] Error: Unable to read agents_config.json" << std::endl;
        else std::cerr << "{\"error\": \"Cannot open config file\"}" << std::endl;
        return;
    }
    json config;
    file >> config;

    if (!config.contains(agent_name)) {
        if (DEBUG_MODE) std::cout << "[DEBUG] Agent " << agent_name << " unknown or without configuration." << std::endl;
        else std::cerr << "{\"warning\": \"Agent not found\"}" << std::endl;
        return;
    }

    std::lock_guard<std::mutex> lock(config_mutex);
    active_zones.clear();
    current_agent_name = agent_name;
    last_state = json::object();

    auto agent_config = config[agent_name];
    for (auto& [key, zone] : agent_config.items()) {
        ZoneConfig zc;
        zc.name = key;
        zc.type = zone.value("type", "color");
        
        double pct_x = (double)zone["x_pct"] / 100.0;
        double pct_y = (double)zone["y_pct"] / 100.0;
        double pct_w = (double)zone["w_pct"] / 100.0;
        double pct_h = (double)zone["h_pct"] / 100.0;

        // Unreal Engine UI Canvas Scaling Logic
        double aspect = (double)screen_width / screen_height;
        double canvas_width, canvas_height;

        if (aspect < (16.0 / 9.0) - 0.01) {
            // Stretched (e.g. 1024x1080) -> UI fits to Width
            canvas_width = screen_width * (16.0 / 9.0);
            canvas_height = screen_width; // Because canvas_width / (16/9) = screen_width
        } else {
            // Native or Ultrawide -> UI fits to Height
            canvas_width = screen_height * (16.0 / 9.0);
            canvas_height = screen_height;
        }

        // X is anchored from the center of the screen
        double x_offset = (pct_x - 0.5) * canvas_width;
        int x = (int)std::round((screen_width / 2.0) + x_offset);
        
        // Y is anchored from the BOTTOM of the screen
        double dist_from_bottom = (1.0 - pct_y) * canvas_height;
        int y = (int)std::round(screen_height - dist_from_bottom);
        
        // Width and Height scale proportionally
        int w = std::max(1, (int)std::round(pct_w * canvas_width));
        int h = std::max(1, (int)std::round(pct_h * canvas_height));

        // Clamp to physical screen bounds
        x = std::max(0, std::min(x, screen_width - 1));
        y = std::max(0, std::min(y, screen_height - 1));
        w = std::max(1, std::min(w, screen_width - x));
        h = std::max(1, std::min(h, screen_height - y));

        zc.coords = cv::Rect(x, y, w, h);

        double default_ratio = zone.value("strict", false) ? 1.0 : 0.15;
        zc.min_ratio = zone.value("min_ratio", default_ratio);

        if (zone.contains("max_value")) zc.max_value = zone["max_value"].get<double>();
        if (zone.contains("min_value")) zc.min_value = zone["min_value"].get<double>();
        if (zone.contains("max_drop_per_s")) zc.max_drop_per_s = zone["max_drop_per_s"].get<double>();
        if (zone.contains("max_refill_per_s")) zc.max_refill_per_s = zone["max_refill_per_s"].get<double>();
        
        if (zone.contains("allowed_formats")) {
            for (auto& fmt : zone["allowed_formats"]) {
                zc.allowed_formats.push_back(fmt.get<std::string>());
            }
        }

        if ((zc.type == "color" || zc.type == "text" || zc.type == "text_bullet" || zc.type == "text_timer") && zone.contains("expected_color_rgb")) {
            int h_tol = zone.value("h_tol", (zc.type == "text" || zc.type == "text_bullet" || zc.type == "text_timer") ? 15 : 5);
            int s_tol = zone.value("s_tol", (zc.type == "text" || zc.type == "text_bullet" || zc.type == "text_timer") ? 40 : 10);
            int v_tol = zone.value("v_tol", (zc.type == "text" || zc.type == "text_bullet" || zc.type == "text_timer") ? 200 : 150);

            for (auto& color : zone["expected_color_rgb"]) {
                if (color.is_string()) {
                    auto rgb = hex_to_rgb(color.get<std::string>());
                    cv::Mat3b bgr(cv::Vec3b(rgb[2], rgb[1], rgb[0]));
                    cv::Mat3b hsv;
                    cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
                    cv::Vec3b hsv_val = hsv(0,0);
                    
                    cv::Scalar lower(std::max(0, hsv_val[0] - h_tol), std::max(0, hsv_val[1] - s_tol), std::max(0, hsv_val[2] - v_tol));
                    cv::Scalar upper(std::min(179, hsv_val[0] + h_tol), std::min(255, hsv_val[1] + s_tol), std::min(255, hsv_val[2] + v_tol));
                    
                    zc.bounds.push_back({lower, upper});
                }
            }
        }

        // Pre-process templates for text zones
        if (zc.type == "text" || zc.type == "text_bullet" || zc.type == "text_timer") {
            for (const auto& [name, temp_img] : raw_templates) {
                // Filter templates by type
                if ((zc.type == "text" || zc.type == "text_timer") && name.find("a_bullet") != std::string::npos) continue;
                if (zc.type == "text_bullet" && name.find("a_bullet") == std::string::npos) continue;

                cv::Mat gray, temp_mask;
                cv::cvtColor(temp_img, gray, cv::COLOR_BGR2GRAY);
                // Masked Grayscale: Isolate text from the purple background (around 116-137 in gray).
                // A threshold at 145 isolates the text while preserving its anti-aliasing (gray > 145).
                cv::threshold(gray, temp_mask, 145, 255, cv::THRESH_BINARY);
                
                cv::Mat clean_gray = cv::Mat::zeros(gray.size(), CV_8UC1);
                gray.copyTo(clean_gray, temp_mask);

                // Automatic crop to digit contour (removes useless margin)
                cv::Rect bbox = cv::boundingRect(temp_mask);
                if (bbox.width > 0 && bbox.height > 0) {
                    bbox.x = std::max(0, bbox.x - 1);
                    bbox.y = std::max(0, bbox.y - 1);
                    bbox.width = std::min(clean_gray.cols - bbox.x, bbox.width + 2);
                    bbox.height = std::min(clean_gray.rows - bbox.y, bbox.height + 2);
                    cv::Mat cropped = clean_gray(bbox).clone();
                    
                    // Calculate the exact mathematical scale compared to 4320p native (8K templates)
                    double base_scale = canvas_height / 4320.0;
                    
                    // We only need 3 scales around the mathematical perfect scale instead of 7 for standard fonts
                    std::vector<double> scales = {base_scale * 0.9, base_scale, base_scale * 1.1};
                    
                    // Fuel gauges (type "text") are physically larger on the HUD than standard timers
                    if (zc.type == "text") {
                        scales.push_back(base_scale * 1.25);
                        scales.push_back(base_scale * 1.4);
                        scales.push_back(base_scale * 1.55);
                    }
                    
                    for (double scale : scales) {
                        cv::Mat scaled;
                        cv::resize(cropped, scaled, cv::Size(), scale, scale, cv::INTER_LINEAR);
                        zc.binarized_templates[name + "#" + std::to_string(scale)] = scaled;
                    }
                    
                    // Save the original for visual debug
                    if (DEBUG_MODE) {
                        std::filesystem::create_directories("debug_images");
                        cv::imwrite("debug_images/template_debug_" + name + ".png", cropped);
                    }
                } else {
                    zc.binarized_templates[name] = temp_mask.clone();
                }
            }
        }

        active_zones.push_back(zc);
    }
    
    force_debug_screenshot = false;
    
    if (DEBUG_MODE) {
        std::cout << "[DEBUG] Agent " << agent_name << " loaded with " << active_zones.size() << " active zones." << std::endl;
    }
}

void send_message(const std::string& msg);

void process_stdin() {
    std::string line;
    while (std::getline(std::cin, line)) {
        try {
            json command = json::parse(line);
            
            auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            std::time_t now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm* now_tm = std::localtime(&now_c);
            char time_buf[20];
            std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", now_tm);
            int ms_remainder = now_ms % 1000;
            std::ostringstream ss;

            if (command["action"] == "set_agent") {
                ss << "[DEBUG] [" << time_buf << "." << std::setfill('0') << std::setw(3) << ms_remainder << "] Spectra CV received input (set_agent): " << command["agent"].get<std::string>();
                send_message(ss.str());
                load_config(command["agent"]);
            } else if (command["action"] == "set_phase") {
                current_phase = command["phase"].get<std::string>();
                ss << "[DEBUG] [" << time_buf << "." << std::setfill('0') << std::setw(3) << ms_remainder << "] Spectra CV received input (set_phase): " << current_phase;
                send_message(ss.str());
            } else if (command["action"] == "stop") {
                exit(0);
            }
        } catch (...) {
            // Ignore parse errors on stdin
        }
    }
}

json process_zone(const cv::Mat& roi, const ZoneConfig& zc) {
    if (roi.empty()) return 0;

    if (zc.type == "color") {
        if (zc.bounds.empty()) {
            cv::Scalar avg = cv::mean(roi);
            return (avg[0] + avg[1] + avg[2]) / 3 > 80 ? 1 : 0;
        }

        cv::Mat bgr, hsv;
        cv::cvtColor(roi, bgr, cv::COLOR_BGRA2BGR);
        cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);
        int total_pixels = hsv.rows * hsv.cols;

        for (const auto& b : zc.bounds) {
            cv::Mat mask;
            cv::inRange(hsv, b.lower, b.upper, mask);
            int matching = cv::countNonZero(mask);
            if ((double)matching / total_pixels >= zc.min_ratio) {
                return 1;
            }
        }
        return 0;
    } 
    else if (zc.type == "text" || zc.type == "text_bullet" || zc.type == "text_timer") {
        if (zc.binarized_templates.empty()) return 0;

        cv::Mat bgr, hsv, mask, clean_gray;
        cv::cvtColor(roi, bgr, cv::COLOR_BGRA2BGR);
        cv::cvtColor(bgr, hsv, cv::COLOR_BGR2HSV);

        // 1. Target the text white (eliminates light green, sky blue, etc.)
        // Limited to 215 (85% pure white) because the top and bottom of '0' are thinner 
        // and appear slightly "gray" due to the game's anti-aliasing.
        // If set to 240, it cuts the top of '0', making it "11", hence the "111" bug.
        cv::inRange(bgr, cv::Scalar(215, 215, 215), cv::Scalar(255, 255, 255), mask);

        // 2. STRICT FILTER PROBLEM: It destroys anti-aliasing on the edges (which are grayed).
        // Without its edges, a '0' becomes too thin, breaks, and the algo reads "111".
        // SOLUTION: Dilate this "white core" mask so it overflows and covers the edges.
        // (Limited to 1 pixel to avoid plugging the small internal holes of '8' and '3')
        cv::dilate(mask, mask, cv::Mat(), cv::Point(-1,-1), 1);

        // 3. Create a classic brightness mask to cut the background outside the dilated text.
        cv::Mat gray, lum_mask;
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
        cv::threshold(gray, lum_mask, 145, 255, cv::THRESH_BINARY);
        
        // 4. Combine: Keep bright pixels (>145) THAT ARE AROUND an ultra-white core!
        cv::bitwise_and(mask, lum_mask, mask);

        bool show_debug = DEBUG_MODE && (zc.name.rfind("C", 0) == 0);

        // If the zone contains no valid text
        if (cv::countNonZero(mask) < 3) {
            return -1.0; // Explicitly signal OCR failure to activate the physics fallback for ALL text types
        }

        clean_gray = cv::Mat::zeros(gray.size(), CV_8UC1);
        gray.copyTo(clean_gray, mask);



        // if (show_debug) std::cout << "[TEXT-DBG] " << zc.name
        //     << " zone=" << clean_gray.cols << "x" << clean_gray.rows
        //     << " white_pixels=" << cv::countNonZero(mask)
        //     << " nb_templates=" << zc.binarized_templates.size() << std::endl;

        if (DEBUG_MODE) {
            static std::map<std::string, std::chrono::steady_clock::time_point> last_roi_saves;
            static std::mutex debug_mutex;
            auto now = std::chrono::steady_clock::now();
            bool should_save = false;
            {
                std::lock_guard<std::mutex> lock(debug_mutex);
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_roi_saves[zc.name]).count() > 1000) {
                    last_roi_saves[zc.name] = now;
                    should_save = true;
                }
            }
            if (should_save) {
                std::filesystem::create_directories("debug_images");
                cv::imwrite("debug_images/roi_debug_" + zc.name + ".png", clean_gray);
            }
        }

        struct Match { std::string digit; int x; double score; int w; };
        std::vector<Match> matches;
        std::vector<std::pair<std::string, double>> best_scores_for_debug;

        // Increased threshold to 0.70 to completely eliminate false positives.
        // Fades and light backgrounds might now trigger an OCR failure (-1.0),
        // but our physics fallback will perfectly simulate the timer ticking down.
        double match_threshold = 0.70; 

        for (const auto& [name_scale, temp_mask] : zc.binarized_templates) {
            std::string real_name = name_scale;
            size_t delim_pos = name_scale.find('#');
            if (delim_pos != std::string::npos) {
                real_name = name_scale.substr(0, delim_pos);
            }

            if (zc.type == "text_bullet" && real_name.find("a_bullet_") != std::string::npos) {
                real_name = real_name.substr(9, 1);
            }

            if (clean_gray.rows < temp_mask.rows || clean_gray.cols < temp_mask.cols) {
                continue; // No log here to avoid spamming with the 7 scales
            }

            cv::Mat res;
            // Template matching on Masked Grayscale
            cv::matchTemplate(clean_gray, temp_mask, res, cv::TM_CCOEFF_NORMED);

            double minVal, maxVal;
            cv::minMaxLoc(res, &minVal, &maxVal);
            
            bool found = false;
            for (auto& pair : best_scores_for_debug) {
                if (pair.first == real_name) {
                    if (maxVal > pair.second) pair.second = maxVal;
                    found = true;
                    break;
                }
            }
            if (!found) best_scores_for_debug.push_back({real_name, maxVal});
            
            // Find all local maxima above the threshold
            for (int y = 0; y < res.rows; y++) {
                for (int x = 0; x < res.cols; x++) {
                    double score = res.at<float>(y, x);
                    if (score >= match_threshold) {
                        // Check if it is a local maximum (avoids close duplicates)
                        bool is_max = true;
                        for (int dy = -2; dy <= 2 && is_max; dy++) {
                            for (int dx = -2; dx <= 2 && is_max; dx++) {
                                if (y+dy >= 0 && y+dy < res.rows && x+dx >= 0 && x+dx < res.cols) {
                                    if (res.at<float>(y+dy, x+dx) > score) {
                                        is_max = false;
                                    }
                                }
                            }
                        }
                        if (is_max) {
                            std::string digit = (real_name == "dot") ? "." : real_name;
                            matches.push_back({digit, x, score, temp_mask.cols});
                        }
                    }
                }
            }
        }

        // if (show_debug) {
        //     // Show the best scale for each digit
        //     for (const auto& pair : best_scores_for_debug) {
        //         if (pair.second > 0.4) {
        //             std::cout << "[TEXT-DBG]   template '" << pair.first << "' max_score=" << pair.second 
        //                       << (pair.second >= match_threshold ? " *** MATCH ***" : "") << std::endl;
        //         }
        //     }
        if (matches.empty()) {
            if (zc.type == "text_timer") return -1.0;
            return 0;
        }

        // Non-Maximum Suppression (NMS): Sort by decreasing score to keep the best match
        std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) {
            return a.score > b.score;
        });

        std::vector<Match> kept_matches;
        for (const auto& m : matches) {
            bool overlap = false;
            for (const auto& kept : kept_matches) {
                // 1D overlap verification (Bounding Boxes on X axis)
                // If the boxes intersect (with 1 pixel tolerance to glue letters)
                if (m.x < kept.x + kept.w - 1 && kept.x < m.x + m.w - 1) { 
                    overlap = true;
                    break;
                }
            }
            if (!overlap) {
                kept_matches.push_back(m);
            }
        }

        // Sort from left to right to reconstruct the final number
        std::sort(kept_matches.begin(), kept_matches.end(), [](const Match& a, const Match& b) {
            return a.x < b.x;
        });

        std::string result = "";
        for (const auto& m : kept_matches) {
            result += m.digit;
        }

        // DYNAMIC CORRECTION AND VALIDATION (BASED ON allowed_formats)
        bool has_formats = !zc.allowed_formats.empty();
        
        if (has_formats) {
            bool allows_dot = false;
            for (const auto& fmt : zc.allowed_formats) {
                if (fmt.find('.') != std::string::npos) allows_dot = true;
            }

            if (!allows_dot) {
                result.erase(std::remove(result.begin(), result.end(), '.'), result.end());
            } else {
                size_t dot_pos = result.find('.');
                if (dot_pos != std::string::npos) {
                    if (dot_pos == result.length() - 1) {
                        result.erase(dot_pos, 1);
                    } else {
                        char last_char = result.back();
                        if (std::isdigit(last_char)) {
                            result = "0." + std::string(1, last_char);
                        }
                    }
                }
            }

            bool matched = false;
            for (const auto& fmt : zc.allowed_formats) {
                if (fmt.find('.') != std::string::npos) {
                    if (result.length() == 3 && result[0] == '0' && result[1] == '.') matched = true;
                } else {
                    if (result.find('.') == std::string::npos && result.length() == fmt.length()) matched = true;
                }
            }
            if (!matched) {
                if (show_debug && !result.empty()) std::cout << "[TEXT-DBG] Strict format rejection: '" << result << "'" << std::endl;
                return -1.0;
            }
        } 
        else {
            size_t dot_pos = result.find(".");
            while (dot_pos != std::string::npos) {
                if (dot_pos == 0 || (dot_pos == 1 && result[0] == '0')) {
                    break;
                } else {
                    result.erase(dot_pos, 1);
                    dot_pos = result.find(".");
                }
            }

            if (result.length() == 2 && result[0] == '.') {
                result = "0" + result;
            }

            bool valid = false;
            
            // Check against user-defined formats if they exist
            if (!zc.allowed_formats.empty()) {
                for (const auto& fmt : zc.allowed_formats) {
                    if (result.length() != fmt.length()) continue;
                    bool match = true;
                    for (size_t i = 0; i < result.length(); ++i) {
                        if (fmt[i] == '#') {
                            if (result[i] < '0' || result[i] > '9') { match = false; break; }
                        } else {
                            if (result[i] != fmt[i]) { match = false; break; }
                        }
                    }
                    if (match) { valid = true; break; }
                }
            } else {
                // Default hardcoded formats
                if (result.length() == 1 && result[0] >= '0' && result[0] <= '9') valid = true;
                else if (result.length() == 2 && result[0] >= '0' && result[0] <= '9' && result[1] >= '0' && result[1] <= '9') valid = true;
                else if (result.length() == 3 && result[0] >= '0' && result[0] <= '9' && result[1] == '.' && result[2] >= '0' && result[2] <= '9') valid = true;
                else if (result.length() == 3 && result[0] >= '0' && result[0] <= '9' && result[1] >= '0' && result[1] <= '9' && result[2] >= '0' && result[2] <= '9') valid = true;
            }

            if (!valid) {
                //if (show_debug && !result.empty()) std::cout << "[TEXT-DBG] Security rejection, invalid format: '" << result << "'" << std::endl;
                return -1.0;
            }
        }

        try {
            double final_value = std::stod(result);
            if (zc.max_value >= 0 && final_value > zc.max_value) return -1.0; // Exceeds upper limit
            if (final_value < zc.min_value) return -1.0; // Below lower limit
            return final_value;
        } catch (...) {
            return 0;
        }
    }

    return 0;
}

// --- COMMUNICATION THREAD (NON-BLOCKING OUTPUT) ---
std::queue<std::string> output_queue;
std::mutex output_mutex;
std::condition_variable output_cv;

void communication_thread() {
    while (true) {
        std::string msg;
        {
            std::unique_lock<std::mutex> lock(output_mutex);
            output_cv.wait(lock, [] { return !output_queue.empty(); });
            msg = output_queue.front();
            output_queue.pop();
        }
        std::cout << msg << std::endl; // Flushed automatically
    }
}

void send_message(const std::string& msg) {
    std::lock_guard<std::mutex> lock(output_mutex);
    output_queue.push(msg);
    output_cv.notify_one();
}

void save_debug_screenshot(const cv::Mat& frame, const std::vector<ZoneConfig>& zones, const std::string& prefix, const std::string& agent_name) {
    if (frame.empty()) return;
    cv::Mat debug_frame = frame.clone(); // Clone the pixel buffer
    
    // Perform drawing and slow disk I/O in a background thread to avoid stuttering the main OCR loop
    std::thread([debug_frame, zones, prefix, agent_name]() mutable {
        for (const auto& zc : zones) {
            cv::rectangle(debug_frame, zc.coords, cv::Scalar(0, 0, 255, 255), 2); // Red rectangle (BGRA)
            cv::putText(debug_frame, zc.name, cv::Point(zc.coords.x, std::max(0, zc.coords.y - 5)), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(0, 0, 255, 255), 1);
        }
        auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        std::filesystem::create_directories("debug_images");
        std::string filename = "debug_images/" + prefix + "_" + agent_name + "_" + std::to_string(now_ms) + ".png";
        cv::imwrite(filename, debug_frame);
    }).detach();
}
// --------------------------------------------------

int main() {
    // BELOW_NORMAL priority: the game always takes precedence over SpectraCV
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    
    // DISABLE OpenCV's internal multithreading (TBB/OpenMP).
    // Because we already use std::async to parallelize the zones, if OpenCV ALSO spawns 
    // 16 threads for each matchTemplate, it creates a massive thread storm causing 80% CPU usage!
    cv::setNumThreads(1);

    ScreenCapture capture;
    if (!capture.Initialize(MONITOR_INDEX)) {
        std::cerr << "{\"error\": \"DXGI Screen Capture failed to initialize\"}" << std::endl;
        return 1;
    }

    screen_width = capture.GetWidth();
    screen_height = capture.GetHeight();
    
    load_templates(); // Load and resize images AFTER knowing the screen resolution
    
    if (DEBUG_MODE) {
        std::cout << "[DEBUG] DEBUG mode activated. Forced agent: " << DEBUG_AGENT_NAME << std::endl;
        std::cout << "[DEBUG] Spectra CV C++ started. Resolution: " << screen_width << "x" << screen_height << std::endl;
        load_config(DEBUG_AGENT_NAME);
    } else {
        std::cout << "{\"info\": \"Spectra CV C++ started. Resolution: " << screen_width << "x" << screen_height << "\"}" << std::endl;
        std::thread stdin_t(process_stdin);
        stdin_t.detach();
    }

    std::thread comms_t(communication_thread);
    comms_t.detach();

    std::map<std::string, int> suspicious_drop_counters;
    
    struct AgentPhysics {
        std::chrono::steady_clock::time_point last_accepted_time;
        double last_accepted_value = 0.0;
        double velocity = 0.0;
        double simulated_value = 0.0;
        std::chrono::steady_clock::time_point last_frame_time;
        bool initialized = false;
        double last_rejected_value = -1.0;
        int consecutive_rejections = 0;
    };
    std::map<std::string, AgentPhysics> physics_engine;
    auto last_dxgi_check = std::chrono::steady_clock::now();

    while (true) {
        auto frame_start = std::chrono::steady_clock::now();
        
        if (std::chrono::duration_cast<std::chrono::seconds>(frame_start - last_dxgi_check).count() >= 30) {
            last_dxgi_check = frame_start;
            int current_sys_w = GetSystemMetrics(SM_CXSCREEN);
            int current_sys_h = GetSystemMetrics(SM_CYSCREEN);
            if (current_sys_w != screen_width || current_sys_h != screen_height) {
                if (DEBUG_MODE) {
                    send_message("[DEBUG] Periodic DXGI check (30s)... Resolution mismatch detected (" + std::to_string(current_sys_w) + "x" + std::to_string(current_sys_h) + "), reinitializing capture.");
                }
                capture.Initialize(MONITOR_INDEX);
            }
        }

        std::vector<ZoneConfig> current_zones;
        {
            std::lock_guard<std::mutex> lock(config_mutex);
            current_zones = active_zones;
        }

        if (current_zones.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Sleep quietly when no agent is loaded
            continue;
        }

        cv::Mat frame = capture.GetLatestFrame();
        if (frame.empty() || frame.cols <= 100 || frame.rows <= 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        try {
            if (frame.cols != screen_width || frame.rows != screen_height) {
                screen_width = frame.cols;
                screen_height = frame.rows;
                if (DEBUG_MODE) {
                    std::ostringstream ss;
                    ss << "[DEBUG] Resolution change detected: " << screen_width << "x" << screen_height;
                    send_message(ss.str());
                }
                load_templates();
                if (!current_agent_name.empty()) {
                    load_config(current_agent_name);
                }
                continue;
            }

            // Run process_zone on different threads/cores simultaneously using std::async
            std::vector<std::pair<ZoneConfig, std::future<json>>> futures;
            for (const auto& zc : current_zones) {
                if (zc.coords.x < 0 || zc.coords.y < 0 || zc.coords.x + zc.coords.width > screen_width || zc.coords.y + zc.coords.height > screen_height)
                    continue;

                futures.push_back({zc, std::async(std::launch::async, [&frame, zc]() -> json {
                    try {
                        cv::Mat roi = frame(zc.coords);
                        return process_zone(roi, zc);
                    } catch (...) {
                        return -1.0;
                    }
                })});
            }

            json current_state;
            for (auto& fut_pair : futures) {
                const ZoneConfig& zc = fut_pair.first;
                json val;
                try {
                    val = fut_pair.second.get();
                } catch (...) {
                    val = -1.0;
                }
                auto now = std::chrono::steady_clock::now();
                
                if (!physics_engine.count(zc.name)) {
                    physics_engine[zc.name] = {now, 0.0, 0.0, 0.0, now, false, -1.0, 0};
                }
                AgentPhysics& phys = physics_engine[zc.name];
                double dt = std::chrono::duration<double>(now - phys.last_frame_time).count();
                phys.last_frame_time = now;

                if (val.is_number()) {
                    double v = val.get<double>();
                    
                    // Reject completely impossible values
                    if (v != -1.0 && zc.max_value >= 0.0 && v > zc.max_value) {
                        v = -1.0;
                    }
                    
                    if (v == -1.0) {
                        // OCR completely failed
                        if (phys.initialized && zc.type == "text") {
                            phys.simulated_value += phys.velocity * dt;
                            if (zc.max_value >= 0 && phys.simulated_value > zc.max_value) phys.simulated_value = zc.max_value;
                            if (phys.simulated_value < zc.min_value) phys.simulated_value = zc.min_value;
                            current_state[zc.name] = std::round(phys.simulated_value);
                        } else if (phys.initialized && zc.type == "text_timer") {
                            if (phys.simulated_value > 0.0) {
                                phys.simulated_value -= 1.0 * dt; // Continues to descend
                            }
                            if (phys.simulated_value < 0.0) phys.simulated_value = 0.0;
                            current_state[zc.name] = std::round(phys.simulated_value);
                        } else if (phys.initialized && zc.type == "text_bullet") {
                            phys.simulated_value = phys.last_accepted_value; // Freeze
                            current_state[zc.name] = std::round(phys.simulated_value);
                        } else {
                            if (last_state.contains(zc.name)) {
                                current_state[zc.name] = last_state[zc.name];
                            } else {
                                current_state[zc.name] = 0.0;
                            }
                        }
                    } else {
                        bool accepted = true;
                        if (phys.initialized) {
                            double elapsed = std::chrono::duration<double>(now - phys.last_accepted_time).count();
                            double last_v = phys.last_accepted_value;
                            
                            if (zc.type == "text" || zc.type == "text_bullet" || zc.type == "text_timer") {
                                double diff = v - last_v;
                                
                                // Allow jump from 0.0 for continuous gauges (Viper), but not for bullets/timers
                                if (last_v == 0.0 && zc.type == "text") {
                                    accepted = true;
                                } else {
                                    if (diff < 0) { // Drop
                                        if (zc.type == "text_bullet" && diff < -2.0) {
                                            accepted = false; // Security: impossible to shoot more than 2 bullets in a single frame
                                        } else if (zc.type == "text_timer" && diff < -3.0) {
                                            accepted = false; // Security: a timer cannot lose 3 seconds in 1 frame
                                        } else if (zc.max_drop_per_s >= 0) {
                                            double max_drop = (elapsed * zc.max_drop_per_s) + 1.5; // Reduced tolerance
                                            if (-diff > max_drop) accepted = false;
                                        } else {
                                            // No physical limit: buffer for weird drops
                                            if (v == 0.0 || -diff > 30.0) {
                                                accepted = false;
                                            }
                                        }
                                    } else if (diff > 0) { // Increase
                                        bool is_strict_zero = false;
                                        double refill_limit = zc.max_refill_per_s;
                                        
                                        if (zc.type == "text_bullet" && current_phase == "combat") {
                                            refill_limit = 0.0; // Impossible to gain bullets mid-round
                                            is_strict_zero = true;
                                        }

                                        if (is_strict_zero) {
                                            accepted = false; // Absolute prohibition to increase
                                        } else if (refill_limit >= 0) {
                                            double max_refill = (elapsed * refill_limit) + 1.5; // Reduced tolerance
                                            if (diff > max_refill) {
                                                if (zc.max_value > 0.0 && v == zc.max_value) {
                                                    accepted = true; // Authorize instant jump to max_value (e.g., Neon fuel resets to 100)
                                                } else {
                                                    accepted = false;
                                                }
                                            }
                                        } else if (refill_limit == -1.0) {
                                            // -1.0 means "infinite refill allowed" (e.g., timer reset or buy)
                                            accepted = true;
                                        }
                                    }
                                }
                                
                                // ANTI-DESYNC CORRECTION SYSTEM
                                if (!accepted) {
                                    bool anti_desync_allowed = true;
                                    // Never force an increase in combat if strictly forbidden
                                    if (diff > 0 && zc.type == "text_bullet" && current_phase == "combat") {
                                        anti_desync_allowed = false;
                                    }
                                    
                                    // NEVER force a massive drop that violates physical reality
                                    if (diff < 0) {
                                        if (zc.type == "text_timer" && diff < -3.0) anti_desync_allowed = false;
                                        if (zc.type == "text_bullet" && diff < -2.0) anti_desync_allowed = false;
                                        if (zc.type == "text" && zc.max_drop_per_s >= 0) {
                                            double max_drop = (elapsed * zc.max_drop_per_s) + 5.0; // 5.0 absolute margin for anti-desync
                                            if (-diff > max_drop) anti_desync_allowed = false;
                                        }
                                    }

                                    if (anti_desync_allowed) {
                                        // To recover drifting gauges (like regenerating fuel), we check if the OCR 
                                        // is consistently higher or lower than the simulation, rather than requiring exact equality.
                                        if ((v > phys.simulated_value + 3.0 && phys.last_rejected_value > phys.simulated_value + 3.0) ||
                                            (v < phys.simulated_value - 3.0 && phys.last_rejected_value < phys.simulated_value - 3.0)) {
                                            phys.consecutive_rejections++;
                                            if (phys.consecutive_rejections >= 15) {
                                                accepted = true; // Force acceptance
                                            }
                                        } else {
                                            phys.consecutive_rejections = 1;
                                        }
                                        phys.last_rejected_value = v;
                                    }
                                } else {
                                    phys.consecutive_rejections = 0;
                                }
                            }
                        }

                        if (accepted) {
                            if (phys.initialized && (zc.type == "text" || zc.type == "text_bullet" || zc.type == "text_timer") && v > 0.0) {
                                double elapsed = std::chrono::duration<double>(now - phys.last_accepted_time).count();
                                
                                if (zc.type == "text_timer") {
                                    phys.velocity = -1.0; // A timer strictly descends by 1 per second
                                } else if (v != phys.last_accepted_value) {
                                    double instant_velocity = (v - phys.last_accepted_value) / elapsed;
                                    if (zc.max_drop_per_s >= 0 && instant_velocity < -zc.max_drop_per_s) instant_velocity = -zc.max_drop_per_s;
                                    if (zc.max_refill_per_s >= 0 && instant_velocity > zc.max_refill_per_s) instant_velocity = zc.max_refill_per_s;
                                    
                                    if (phys.velocity == 0.0) phys.velocity = instant_velocity;
                                    else phys.velocity = 0.5 * phys.velocity + 0.5 * instant_velocity; // Exponential smoothing (EMA)
                                } else if (elapsed > 0.5) {
                                    phys.velocity = 0.0; // Stagnation
                                }
                            } else if (v == 0.0) {
                                phys.velocity = 0.0;
                            }

                            phys.last_accepted_value = v;
                            phys.last_accepted_time = now;
                            phys.simulated_value = v;
                            phys.initialized = true;
                            suspicious_drop_counters[zc.name] = 0;
                            
                            if (zc.type == "text" || zc.type == "text_bullet" || zc.type == "text_timer") current_state[zc.name] = std::round(v);
                            else current_state[zc.name] = v;
                        } else {
                            // Value ignored (aberrant frame) -> Interpolation or Freeze
                            if (phys.initialized && zc.type == "text") {
                                phys.simulated_value += phys.velocity * dt;
                                if (zc.max_value >= 0 && phys.simulated_value > zc.max_value) phys.simulated_value = zc.max_value;
                                if (phys.simulated_value < zc.min_value) phys.simulated_value = zc.min_value;
                                current_state[zc.name] = std::round(phys.simulated_value);
                            } else if (phys.initialized && zc.type == "text_timer") {
                                if (phys.simulated_value > 0.0) {
                                    phys.simulated_value -= 1.0 * dt; // Strictly descends by 1 per second
                                }
                                if (phys.simulated_value < 0.0) phys.simulated_value = 0.0;
                                current_state[zc.name] = std::round(phys.simulated_value);
                            } else if (phys.initialized && zc.type == "text_bullet") {
                                // For bullets, it's shot by shot. If the frame is lost, freeze the value.
                                phys.simulated_value = phys.last_accepted_value;
                                current_state[zc.name] = std::round(phys.simulated_value);
                            } else {
                                if (last_state.contains(zc.name)) current_state[zc.name] = last_state[zc.name];
                            }
                        }
                    }
                } else {
                    current_state[zc.name] = val; // Fallback for colors and non-number types
                }
            }
            
            if (force_debug_screenshot) {
                save_debug_screenshot(frame, current_zones, "launch", current_agent_name);
                force_debug_screenshot = false;
            }

            if (current_state != last_state) {
                if (DEBUG_MODE) {
                    save_debug_screenshot(frame, current_zones, "state_change", current_agent_name);
                    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                    std::time_t now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                    std::tm* now_tm = std::localtime(&now_c);
                    char time_buf[20];
                    std::strftime(time_buf, sizeof(time_buf), "%H:%M:%S", now_tm);
                    int ms_remainder = now_ms % 1000;

                    std::ostringstream ss;
                    ss << "[DEBUG] [" << time_buf << "." << std::setfill('0') << std::setw(3) << ms_remainder << "] New State (" << current_agent_name << "): " << current_state.dump();
                    send_message(ss.str());
                } else {
                    json payload;
                    payload["type"] = "state_update";
                    payload["agent"] = current_agent_name;
                    payload["data"] = current_state;
                    send_message(payload.dump());
                }
                last_state = current_state;
            }

            auto frame_end = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(frame_end - frame_start).count();
            int target_frame_time = 1000 / TARGET_FPS;
            if (elapsed < target_frame_time) {
                std::this_thread::sleep_for(std::chrono::milliseconds(target_frame_time - elapsed));
            } else {
                std::this_thread::sleep_until(frame_start + std::chrono::milliseconds(1000 / TARGET_FPS));
            }
        } catch (const std::exception& e) {
            if (DEBUG_MODE) {
                std::ostringstream ss;
                ss << "[DEBUG] Exception in main loop: " << e.what();
                send_message(ss.str());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        } catch (...) {
            if (DEBUG_MODE) {
                send_message("[DEBUG] Unknown exception in main loop");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    return 0;
}
