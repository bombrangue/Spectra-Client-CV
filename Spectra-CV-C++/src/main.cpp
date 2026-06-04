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
#include <opencv2/core/utils/logger.hpp>
#include "ScreenCapture.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>
#include <wrl/client.h>

using json = nlohmann::json;

int auto_detect_valorant_monitor(int current_index) {
    HWND hwnd = FindWindowA("UnrealWindow", "VALORANT  ");
    if (!hwnd) hwnd = FindWindowA(NULL, "VALORANT  ");
    if (!hwnd) hwnd = FindWindowA(NULL, "VALORANT");
    
    if (!hwnd) return current_index; // Default to current if game not found
    
    HMONITOR hMonitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!hMonitor) return current_index;

    Microsoft::WRL::ComPtr<IDXGIFactory1> dxgiFactory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&dxgiFactory))) return current_index;

    int totalOutputIndex = 0;
    for (UINT adapterIdx = 0; ; adapterIdx++) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (dxgiFactory->EnumAdapters1(adapterIdx, &adapter) == DXGI_ERROR_NOT_FOUND) break;

        for (UINT outputIdx = 0; ; outputIdx++) {
            Microsoft::WRL::ComPtr<IDXGIOutput> dxgiOutput;
            if (adapter->EnumOutputs(outputIdx, &dxgiOutput) == DXGI_ERROR_NOT_FOUND) break;

            DXGI_OUTPUT_DESC desc;
            dxgiOutput->GetDesc(&desc);
            if (!desc.AttachedToDesktop) {
                totalOutputIndex++;
                continue;
            }

            if (desc.Monitor == hMonitor) {
                return totalOutputIndex;
            }
            totalOutputIndex++;
        }
    }
    return current_index;
}

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
    bool allow_jump_to_max = false;
    std::vector<std::string> allowed_formats;
    
    // New features:
    std::string icon_prefix;
    std::vector<cv::Vec3b> game_bg_colors; // Stored in BGR
    cv::Vec3b template_bg_color; // Stored in BGR
    bool has_template_bg = false;
    json constant_value;
};

// --- MODE DEBUG ---
bool DEBUG_MODE = true;
std::string DEBUG_AGENT_NAME = "admin_view"; // Modify to test another agent

// --- CV MODE ---
std::string current_cv_mode = "OFF";

// --- SCREEN SELECTION ---
int MONITOR_INDEX = 0; // 0 = Primary Screen, 1 = Secondary Screen, etc.

// --- FRAME RATE LIMITER (FPS) ---
int TARGET_FPS = 10; // Max 30 FPS (drastically reduces CPU usage)

std::vector<ZoneConfig> active_zones;
std::string current_agent_name = "";
std::string current_phase = "buy_phase"; // "buy_phase" or "combat"
std::mutex config_mutex;
int screen_width = 2560;
int screen_height = 1440;
json last_state;
json last_nested_state;
bool force_debug_screenshot = true;

std::map<std::string, cv::Mat> raw_templates;

std::string get_exe_dir() {
    char buffer[MAX_PATH];
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::string exe_path(buffer);
    return exe_path.substr(0, exe_path.find_last_of("\\/"));
}

void load_templates(const std::string& agent_name = "") {
    raw_templates.clear();
    
    std::string base_template_dir = get_exe_dir() + "/templates";
    bool skip_resolution_scan = false;
    int closest_width = 7680;
    int closest_height = 4320; // Default fallback height if no folders are found
    std::string closest_folder = "";
    int min_diff = 999999;
    
    if (!agent_name.empty() && std::filesystem::exists(base_template_dir + "/" + agent_name)) {
        base_template_dir += "/" + agent_name;
        if (agent_name == "admin_view") {
            skip_resolution_scan = true;
            closest_width = 1920;
            closest_height = 1080;
            if (DEBUG_MODE) std::cout << "[DEBUG] Using flat template structure for " << agent_name << " (assuming 1920x1080)." << std::endl;
        }
    }

    // 1. Scan the templates directory for resolution folders (e.g., "1920x1080", "2560x1440")
    if (!skip_resolution_scan && std::filesystem::exists(base_template_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(base_template_dir)) {
            if (entry.is_directory()) {
                std::string folder_name = entry.path().filename().string();
                size_t x_pos = folder_name.find('x');
                if (x_pos != std::string::npos) {
                    try {
                        int w = std::stoi(folder_name.substr(0, x_pos));
                        int h = std::stoi(folder_name.substr(x_pos + 1));
                        int diff = std::abs(w - screen_width) + std::abs(h - screen_height);
                        if (diff < min_diff) {
                            min_diff = diff;
                            closest_width = w;
                            closest_height = h;
                            closest_folder = folder_name;
                        }
                    } catch (...) {
                        // Ignore folders that don't match the expected format
                    }
                }
            }
        }
    }

    std::string target_dir = base_template_dir;
    if (!skip_resolution_scan) {
        if (!closest_folder.empty()) {
            target_dir += "/" + closest_folder;
            if (DEBUG_MODE) std::cout << "[DEBUG] Selected template resolution folder: " << closest_folder << " (W:" << closest_width << " H:" << closest_height << ")" << std::endl;
        } else {
            if (DEBUG_MODE) std::cout << "[DEBUG] No resolution folders found, defaulting to base templates directory (assuming 8K/7680x4320)." << std::endl;
        }
    }

    // 3. Load dynamically
    if (std::filesystem::exists(target_dir)) {
        for (const auto& entry : std::filesystem::directory_iterator(target_dir)) {
            if (entry.is_regular_file() && entry.path().extension() == ".png") {
                std::string path = entry.path().string();
                std::string name = entry.path().stem().string(); // get filename without extension
                
                cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);
                if (!img.empty()) {
                    raw_templates[name] = img;
                    if (DEBUG_MODE) std::cout << "[DEBUG] Template loaded: " << name << " from " << path << std::endl;
                }
            }
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

std::vector<std::pair<std::string, json>> gather_leaves(const json& node, const std::string& current_path) {
    std::vector<std::pair<std::string, json>> leaves;
    if (node.is_object()) {
        if (node.contains("type")) {
            leaves.push_back({current_path, node});
        } else {
            for (auto& [key, value] : node.items()) {
                std::string new_path = current_path.empty() ? key : current_path + "." + key;
                auto sub_leaves = gather_leaves(value, new_path);
                leaves.insert(leaves.end(), sub_leaves.begin(), sub_leaves.end());
            }
        }
    }
    return leaves;
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
    last_nested_state = json::object();

    auto agent_config = config[agent_name];
    auto leaves = gather_leaves(agent_config, "");
    
    for (auto& [key, zone] : leaves) {
        ZoneConfig zc;
        zc.name = key;
        zc.type = zone.value("type", "color");
        
        if (zc.type == "constant") {
            if (zone.contains("value")) zc.constant_value = zone["value"];
            active_zones.push_back(zc);
            continue; // Skip image processing for constants
        }
        
        zc.icon_prefix = zone.value("icon_prefix", "");
        
        if (zone.contains("game_bg_colors")) {
            for (auto& color : zone["game_bg_colors"]) {
                if (color.is_string()) {
                    auto rgb = hex_to_rgb(color.get<std::string>());
                    zc.game_bg_colors.push_back(cv::Vec3b(rgb[2], rgb[1], rgb[0]));
                }
            }
        }
        
        if (zone.contains("template_bg_color") && zone["template_bg_color"].is_string()) {
            auto rgb = hex_to_rgb(zone["template_bg_color"].get<std::string>());
            zc.template_bg_color = cv::Vec3b(rgb[2], rgb[1], rgb[0]);
            zc.has_template_bg = true;
        }
        
        double pct_x = (double)zone["x_pct"] / 100.0;
        double pct_y = (double)zone["y_pct"] / 100.0;
        double pct_w = (double)zone["w_pct"] / 100.0;
        double pct_h = (double)zone["h_pct"] / 100.0;

        // Unreal Engine UI Canvas Scaling Logic
        // Valorant's UI logic is universal for all resolutions:
        // The HUD is drawn on a virtual 16:9 canvas that perfectly scales to the physical screen height.
        // Elements are anchored relative to the center of this 16:9 canvas.
        double canvas_height = screen_height;
        double canvas_width = screen_height * (16.0 / 9.0);

        // X is anchored from the center of the UI canvas
        double x_offset = (pct_x - 0.5) * canvas_width;
        int x = (int)std::round((screen_width / 2.0) + x_offset);
        
        // Y is anchored from the bottom of the UI canvas
        double dist_from_bottom = (1.0 - pct_y) * canvas_height;
        int y = (int)std::round(screen_height - dist_from_bottom);
        
        // Width and Height scale proportionally
        int base_w = std::max(1, (int)std::round(pct_w * canvas_width));
        int base_h = std::max(1, (int)std::round(pct_h * canvas_height));

        // No safety margin. The ROI is strictly bound to the exact mathematical percentage dimensions.
        int margin_x = 0;
        int margin_y = 0;

        x -= margin_x;
        y -= margin_y;
        int w = base_w + (margin_x * 2);
        int h = base_h + (margin_y * 2);

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
        if (zone.contains("allow_jump_to_max")) zc.allow_jump_to_max = zone["allow_jump_to_max"].get<bool>();
        
        if (zone.contains("allowed_formats")) {
            for (auto& fmt : zone["allowed_formats"]) {
                zc.allowed_formats.push_back(fmt.get<std::string>());
            }
        }

        if ((zc.type == "color" || zc.type == "text" || zc.type == "text_bullet" || zc.type == "text_timer" || zc.type == "fuel") && zone.contains("expected_color_rgb")) {
            int h_tol = zone.value("h_tol", (zc.type == "text" || zc.type == "text_bullet" || zc.type == "text_timer" || zc.type == "fuel") ? 15 : 5);
            int s_tol = zone.value("s_tol", (zc.type == "text" || zc.type == "text_bullet" || zc.type == "text_timer" || zc.type == "fuel") ? 40 : 10);
            int v_tol = zone.value("v_tol", (zc.type == "text" || zc.type == "text_bullet" || zc.type == "text_timer" || zc.type == "fuel") ? 200 : 150);

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
        if (zc.type == "text" || zc.type == "text_bullet" || zc.type == "text_timer" || zc.type == "fuel" || zc.type == "text_raw" || zc.type == "ult_fraction") {
            for (const auto& [name, temp_img] : raw_templates) {
                if (!zc.icon_prefix.empty()) {
                    if (name.find(zc.icon_prefix) != 0) continue;
                } else {
                    // Filter templates by type
                    if ((zc.type == "text" || zc.type == "text_timer" || zc.type == "fuel") && name.find("a_bullet") != std::string::npos) continue;
                    if (zc.type == "text_bullet" && name.find("a_bullet") == std::string::npos) continue;
                    
                    // Fuel specific filtering
                    if ((zc.type == "text" || zc.type == "text_timer" || zc.type == "text_bullet" || zc.type == "text_raw" || zc.type == "ult_fraction") && name.find("fuel_") != std::string::npos) continue;
                    if (zc.type == "fuel" && name.find("fuel_") == std::string::npos) continue;
                }

                cv::Mat gray, temp_mask;
                
                // Extracting text: A simple threshold at 130 perfectly removes the dark HUD background 
                // while fully preserving the shape and anti-aliased borders of low-resolution text.
                if (temp_img.channels() == 3 || temp_img.channels() == 4) {
                    cv::cvtColor(temp_img, gray, cv::COLOR_BGR2GRAY);
                } else {
                    gray = temp_img.clone();
                }
                
                if (zc.has_template_bg) {
                    int tolerance = 15;
                    cv::Mat temp_bgr = temp_img.clone();
                    for (int y = 0; y < temp_bgr.rows; ++y) {
                        for (int x = 0; x < temp_bgr.cols; ++x) {
                            cv::Vec3b& pixel = temp_bgr.at<cv::Vec3b>(y, x);
                            if (std::abs(pixel[0] - zc.template_bg_color[0]) <= tolerance &&
                                std::abs(pixel[1] - zc.template_bg_color[1]) <= tolerance &&
                                std::abs(pixel[2] - zc.template_bg_color[2]) <= tolerance) {
                                gray.at<uchar>(y, x) = 0; // Force background to black
                            }
                        }
                    }
                }

                if (zc.type == "text_raw" || zc.type == "ult_fraction") {
                    temp_mask = gray.clone();
                } else {
                    cv::threshold(gray, temp_mask, 130, 255, cv::THRESH_BINARY);
                }
                
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
                    
                    // The templates in `raw_templates` are already pre-scaled to the native screen resolution inside `load_templates`.
                    // Therefore, the mathematically perfect scale for this resolution is exactly 1.0.
                    double base_scale = 1.0;
                    
                    // We only need 3 scales around the mathematical perfect scale instead of 7 for standard fonts
                    std::vector<double> scales = {base_scale * 0.9, base_scale, base_scale * 1.1};
                    
                    // Fuel gauges (type "text" or "fuel") are physically larger on the HUD than standard timers
                    if (zc.type == "text" || zc.type == "fuel") {
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

            if (command["action"] == "set_mode") {
                current_cv_mode = command["mode"].get<std::string>();
                ss << "[DEBUG] [" << time_buf << "." << std::setfill('0') << std::setw(3) << ms_remainder << "] Spectra CV Mode switched to: " << current_cv_mode;
                send_message(ss.str());
                if (current_cv_mode == "MAIN") {
                    load_templates("admin_view");
                    load_config("admin_view");
                } else if (current_cv_mode == "OFF") {
                    std::lock_guard<std::mutex> lock(config_mutex);
                    active_zones.clear();
                    current_agent_name = "";
                }
            } else if (command["action"] == "set_agent") {
                if (current_cv_mode != "MAIN") {
                    ss << "[DEBUG] [" << time_buf << "." << std::setfill('0') << std::setw(3) << ms_remainder << "] Spectra CV received input (set_agent): " << command["agent"].get<std::string>();
                    send_message(ss.str());
                    load_templates(command["agent"]);
                    load_config(command["agent"]);
                }
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

void apply_background_masking(cv::Mat& bgr, const ZoneConfig& zc) {
    if (bgr.empty() || zc.game_bg_colors.empty()) return;
    int tolerance = 15; // Small tolerance for compression artifacts
    for (int y = 0; y < bgr.rows; ++y) {
        for (int x = 0; x < bgr.cols; ++x) {
            cv::Vec3b& pixel = bgr.at<cv::Vec3b>(y, x);
            for (const auto& bg_color : zc.game_bg_colors) {
                if (std::abs(pixel[0] - bg_color[0]) <= tolerance &&
                    std::abs(pixel[1] - bg_color[1]) <= tolerance &&
                    std::abs(pixel[2] - bg_color[2]) <= tolerance) {
                    pixel = cv::Vec3b(0, 0, 0); // Force to pure black
                    break;
                }
            }
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
    else if (zc.type == "icon") {
        cv::Mat bgr;
        cv::cvtColor(roi, bgr, cv::COLOR_BGRA2BGR);
        
        apply_background_masking(bgr, zc);
        
        std::string best_match = "";
        double best_score = 0.60; // Minimum threshold
        
        for (const auto& [name, temp_img] : raw_templates) {
            if (!zc.icon_prefix.empty() && name.find(zc.icon_prefix) != 0) continue;
            
            cv::Mat temp_bgr = temp_img.clone();
            
            // Mask template background if provided
            if (zc.has_template_bg) {
                int tolerance = 15;
                for (int y = 0; y < temp_bgr.rows; ++y) {
                    for (int x = 0; x < temp_bgr.cols; ++x) {
                        cv::Vec3b& pixel = temp_bgr.at<cv::Vec3b>(y, x);
                        if (std::abs(pixel[0] - zc.template_bg_color[0]) <= tolerance &&
                            std::abs(pixel[1] - zc.template_bg_color[1]) <= tolerance &&
                            std::abs(pixel[2] - zc.template_bg_color[2]) <= tolerance) {
                            pixel = cv::Vec3b(0, 0, 0);
                        }
                    }
                }
            }
            
            if (bgr.rows < temp_bgr.rows || bgr.cols < temp_bgr.cols) continue;
            
            cv::Mat res;
            cv::matchTemplate(bgr, temp_bgr, res, cv::TM_CCOEFF_NORMED);
            double minVal, maxVal;
            cv::minMaxLoc(res, &minVal, &maxVal);
            
            if (maxVal > best_score) {
                best_score = maxVal;
                if (!zc.icon_prefix.empty() && name.find(zc.icon_prefix) == 0) {
                    best_match = name.substr(zc.icon_prefix.length());
                } else {
                    best_match = name;
                }
            }
        }
        
        if (best_match.empty()) return -1.0;
        return best_match; // returns string natively via json
    }
    else if (zc.type == "text" || zc.type == "text_bullet" || zc.type == "text_timer" || zc.type == "fuel" || zc.type == "text_raw" || zc.type == "ult_fraction") {
        if (zc.binarized_templates.empty()) return 0;

        cv::Mat bgr, gray, mask, clean_gray;
        cv::cvtColor(roi, bgr, cv::COLOR_BGRA2BGR);
        
        apply_background_masking(bgr, zc);
        
        cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);

        if (zc.type == "text_raw" || zc.type == "ult_fraction") {
            mask = gray.clone();
        } else {
            // Extracting text: A simple threshold at 130 perfectly removes the dark HUD background
            // and preserves low-resolution anti-aliasing without the destructive effects of inRange/dilate.
            cv::threshold(gray, mask, 130, 255, cv::THRESH_BINARY);
        }

        // If the zone contains no valid text
        if (cv::countNonZero(mask) < 3) {
            return -1.0; // Explicitly signal OCR failure to activate the physics fallback for ALL text types
        }

        clean_gray = cv::Mat::zeros(gray.size(), CV_8UC1);
        gray.copyTo(clean_gray, mask);

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

            if (!zc.icon_prefix.empty() && real_name.find(zc.icon_prefix) == 0) {
                real_name = real_name.substr(zc.icon_prefix.length());
            } else {
                if (zc.type == "text_bullet" && real_name.find("a_bullet_") != std::string::npos) {
                    real_name = real_name.substr(9, 1);
                }
                
                if (zc.type == "fuel" && real_name.find("fuel_") != std::string::npos) {
                    real_name = real_name.substr(5, 1);
                }
            }

            if (clean_gray.rows < temp_mask.rows || clean_gray.cols < temp_mask.cols) {
                continue; // No log here to avoid spamming with the 7 scales
            }

            cv::Mat res;
            // Template matching on Masked Grayscale
            cv::matchTemplate(clean_gray, temp_mask, res, cv::TM_CCOEFF_NORMED);
            
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
                            std::string digit = real_name;
                            if (real_name == "dot") digit = ".";
                            else if (real_name == "comma") digit = ",";
                            else if (real_name == "slash") digit = "/";
                            
                            matches.push_back({digit, x, score, temp_mask.cols});
                        }
                    }
                }
            }
        }

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
                return -1.0;
            }
        }

        try {
            if (zc.type == "text_raw" || zc.type == "ult_fraction") {
                return result; // Return string natively for non-physical attributes (like kills, money with commas)
            }
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
    _putenv_s("OPENCV_LOG_LEVEL", "SILENT");
    _putenv_s("OPENCV_LOG_LEVEL", "FATAL"); // Just in case SILENT is not fully respected
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_SILENT);

    // BELOW_NORMAL priority: the game always takes precedence over SpectraCV
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    
    // DISABLE OpenCV's internal multithreading (TBB/OpenMP).
    // Because we already use std::async to parallelize the zones, if OpenCV ALSO spawns 
    // 16 threads for each matchTemplate, it creates a massive thread storm causing 80% CPU usage!
    cv::setNumThreads(1);

    // Auto-detect Valorant monitor at startup
    MONITOR_INDEX = auto_detect_valorant_monitor(MONITOR_INDEX);

    ScreenCapture capture;
    if (!capture.Initialize(MONITOR_INDEX)) {
        std::cerr << "{\"error\": \"DXGI Screen Capture failed to initialize\"}" << std::endl;
        return 1;
    }

    screen_width = capture.GetWidth();
    screen_height = capture.GetHeight();
    
    load_templates(DEBUG_MODE ? DEBUG_AGENT_NAME : ""); // Load and resize images AFTER knowing the screen resolution
    
    if (DEBUG_MODE) {
        current_cv_mode = "MAIN";
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
        double last_ult_max = 0.0;
    };
    std::map<std::string, AgentPhysics> physics_engine;
    auto last_dxgi_check = std::chrono::steady_clock::now();

    while (true) {
        if (current_cv_mode == "OFF") {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        auto frame_start = std::chrono::steady_clock::now();
        
        if (std::chrono::duration_cast<std::chrono::seconds>(frame_start - last_dxgi_check).count() >= 10) {
            last_dxgi_check = frame_start;
            
            // Auto-detect if Valorant is on a different monitor
            int val_monitor = auto_detect_valorant_monitor(MONITOR_INDEX);
            if (val_monitor != MONITOR_INDEX) {
                if (DEBUG_MODE) {
                    send_message("[DEBUG] Valorant window moved to monitor " + std::to_string(val_monitor) + ", reinitializing capture.");
                }
                MONITOR_INDEX = val_monitor;
                capture.Initialize(MONITOR_INDEX);
                continue; // Skip the rest to avoid using an invalid frame
            }
            int current_sys_w = screen_width;
            int current_sys_h = screen_height;
            
            HWND hwnd = FindWindowA("UnrealWindow", "VALORANT  ");
            if (!hwnd) hwnd = FindWindowA(NULL, "VALORANT  ");
            if (hwnd) {
                HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi;
                mi.cbSize = sizeof(MONITORINFO);
                if (GetMonitorInfo(hMon, &mi)) {
                    current_sys_w = mi.rcMonitor.right - mi.rcMonitor.left;
                    current_sys_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
                }
            } else {
                current_sys_w = GetSystemMetrics(SM_CXSCREEN);
                current_sys_h = GetSystemMetrics(SM_CYSCREEN);
            }

            if (current_sys_w != screen_width || current_sys_h != screen_height) {
                if (DEBUG_MODE) {
                    send_message("[DEBUG] Periodic DXGI check (10s)... Resolution mismatch detected (" + std::to_string(current_sys_w) + "x" + std::to_string(current_sys_h) + "), reinitializing capture.");
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
                load_templates(current_agent_name);
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
                        if (phys.initialized && (zc.type == "text" || zc.type == "fuel")) {
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
                            
                            if (zc.type == "text" || zc.type == "text_bullet" || zc.type == "text_timer" || zc.type == "fuel") {
                                double diff = v - last_v;
                                
                                // Allow jump from 0.0 for continuous gauges (Viper), but not for bullets/timers
                                if (last_v == 0.0 && (zc.type == "text" || zc.type == "fuel")) {
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
                                                if (zc.allow_jump_to_max && zc.max_value > 0.0 && v == zc.max_value) {
                                                    accepted = true; // Authorize instant jump to max_value explicitly permitted by user
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
                                        if ((zc.type == "text" || zc.type == "fuel") && zc.max_drop_per_s >= 0) {
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
                            if (phys.initialized && (zc.type == "text" || zc.type == "text_bullet" || zc.type == "text_timer" || zc.type == "fuel") && v > 0.0) {
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
                            
                            if (zc.type == "text" || zc.type == "text_bullet" || zc.type == "text_timer" || zc.type == "fuel") current_state[zc.name] = std::round(v);
                            else current_state[zc.name] = v;
                        } else {
                            // Value ignored (aberrant frame) -> Interpolation or Freeze
                            if (phys.initialized && (zc.type == "text" || zc.type == "fuel")) {
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
                } else if (zc.type == "ult_fraction" && val.is_string()) {
                    std::string s = val.get<std::string>();
                    std::string parent_path = zc.name;
                    size_t last_dot = zc.name.find_last_of('.');
                    if (last_dot != std::string::npos) {
                        parent_path = zc.name.substr(0, last_dot);
                    }
                    
                    if (s == "READY" || s == "ready") {
                        current_state[parent_path + ".ult_points"] = phys.last_ult_max;
                        current_state[parent_path + ".ult_maximum"] = phys.last_ult_max;
                    } else {
                        size_t slash_pos = s.find('/');
                        if (slash_pos != std::string::npos) {
                            try {
                                double pts = std::stod(s.substr(0, slash_pos));
                                double max_pts = std::stod(s.substr(slash_pos + 1));
                                current_state[parent_path + ".ult_points"] = pts;
                                current_state[parent_path + ".ult_maximum"] = max_pts;
                                phys.last_ult_max = max_pts;
                            } catch (...) {}
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

            json nested_state = json::object();
            for (auto& [k, v] : current_state.items()) {
                std::string jp_str = "/" + k;
                std::replace(jp_str.begin(), jp_str.end(), '.', '/');
                try {
                    nested_state[json::json_pointer(jp_str)] = v;
                } catch (...) {
                    // Ignore JSON pointer parsing errors
                }
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
                    ss << "[DEBUG] [" << time_buf << "." << std::setfill('0') << std::setw(3) << ms_remainder << "] New State (" << current_agent_name << "): " << nested_state.dump();
                    send_message(ss.str());
                } else {
                    if (current_cv_mode == "MAIN") {
                        for (auto& [k, v] : nested_state.items()) {
                            if (!last_nested_state.contains(k) || last_nested_state[k] != v) {
                                json payload;
                                payload["type"] = "gep_info";
                                json data_block;
                                data_block["gameId"] = 21640;
                                data_block["key"] = k; // the sub-category string like "Scoreboard"
                                
                                // GEP wraps values inside a stringified JSON
                                json value_obj;
                                if (v.is_number() || v.is_string() || v.is_boolean()) {
                                    value_obj["value"] = v;
                                } else {
                                    value_obj = v;
                                }
                                data_block["value"] = value_obj.dump();
                                
                                payload["data"] = data_block;
                                send_message(payload.dump());
                            }
                        }
                    } else {
                        json payload;
                        payload["type"] = "state_update";
                        payload["agent"] = current_agent_name;
                        payload["data"] = nested_state;
                        send_message(payload.dump());
                    }
                }
                last_state = current_state;
                last_nested_state = nested_state;
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
