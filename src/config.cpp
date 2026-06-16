#include "config.h"
#include <fstream>
#include <sstream>
#include <windows.h>

namespace {
    std::wstring Utf8ToWide(const std::string& utf8) {
        if (utf8.empty()) return L"";
        int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
        if (size <= 0) return L"";
        std::wstring wide(size - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], size);
        return wide;
    }

    std::string WideToUtf8(const std::wstring& wide) {
        if (wide.empty()) return "";
        int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (size <= 0) return "";
        std::string utf8(size - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &utf8[0], size, nullptr, nullptr);
        return utf8;
    }

    void SetDefaultConfig(AppConfig& config) {
        config.monitorIndex = 0;
        config.captureWindowTitle = L"";
        config.warp.corners[0] = { 100.0f, 100.0f };   // Top-Left
        config.warp.corners[1] = { 1820.0f, 100.0f };  // Top-Right
        config.warp.corners[2] = { 1820.0f, 980.0f };  // Bottom-Right
        config.warp.corners[3] = { 100.0f, 980.0f };   // Bottom-Left
        config.showGrid = false;
        config.gridCols = 8;
        config.gridRows = 6;
        config.gridThickness = 2.0f;
        config.gridColor[0] = 0.0f;
        config.gridColor[1] = 1.0f;
        config.gridColor[2] = 0.0f;
        config.gridColor[3] = 0.8f;
    }

    void Trim(std::string& s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
            return !std::isspace(ch);
        }));
        s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
            return !std::isspace(ch);
        }).base(), s.end());
    }
}

bool LoadConfig(const std::string& filepath, AppConfig& outConfig) {
    SetDefaultConfig(outConfig);
    
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return false;
    }
    
    std::string line;
    while (std::getline(file, line)) {
        size_t equalPos = line.find('=');
        if (equalPos == std::string::npos) continue;
        
        std::string key = line.substr(0, equalPos);
        std::string val = line.substr(equalPos + 1);
        Trim(key);
        Trim(val);
        
        if (key == "monitorIndex") {
            outConfig.monitorIndex = std::stoi(val);
        } else if (key == "captureWindowTitle") {
            outConfig.captureWindowTitle = Utf8ToWide(val);
        } else if (key == "corner0_x") {
            outConfig.warp.corners[0].x = std::stof(val);
        } else if (key == "corner0_y") {
            outConfig.warp.corners[0].y = std::stof(val);
        } else if (key == "corner1_x") {
            outConfig.warp.corners[1].x = std::stof(val);
        } else if (key == "corner1_y") {
            outConfig.warp.corners[1].y = std::stof(val);
        } else if (key == "corner2_x") {
            outConfig.warp.corners[2].x = std::stof(val);
        } else if (key == "corner2_y") {
            outConfig.warp.corners[2].y = std::stof(val);
        } else if (key == "corner3_x") {
            outConfig.warp.corners[3].x = std::stof(val);
        } else if (key == "corner3_y") {
            outConfig.warp.corners[3].y = std::stof(val);
        } else if (key == "showGrid") {
            outConfig.showGrid = (std::stoi(val) != 0);
        } else if (key == "gridCols") {
            outConfig.gridCols = std::stoi(val);
        } else if (key == "gridRows") {
            outConfig.gridRows = std::stoi(val);
        } else if (key == "gridThickness") {
            outConfig.gridThickness = std::stof(val);
        } else if (key == "gridColor_r") {
            outConfig.gridColor[0] = std::stof(val);
        } else if (key == "gridColor_g") {
            outConfig.gridColor[1] = std::stof(val);
        } else if (key == "gridColor_b") {
            outConfig.gridColor[2] = std::stof(val);
        } else if (key == "gridColor_a") {
            outConfig.gridColor[3] = std::stof(val);
        } else if (key == "autoRestoreMinimized") {
            outConfig.autoRestoreMinimized = (std::stoi(val) != 0);
        } else if (key == "cropLeft") {
            outConfig.cropLeft = std::stof(val);
        } else if (key == "cropTop") {
            outConfig.cropTop = std::stof(val);
        } else if (key == "cropRight") {
            outConfig.cropRight = std::stof(val);
        } else if (key == "cropBottom") {
            outConfig.cropBottom = std::stof(val);
        }
    }
    
    return true;
}

bool SaveConfig(const std::string& filepath, const AppConfig& config) {
    std::ofstream file(filepath);
    if (!file.is_open()) {
        return false;
    }
    
    file << "monitorIndex=" << config.monitorIndex << "\n";
    file << "captureWindowTitle=" << WideToUtf8(config.captureWindowTitle) << "\n";
    
    file << "corner0_x=" << config.warp.corners[0].x << "\n";
    file << "corner0_y=" << config.warp.corners[0].y << "\n";
    
    file << "corner1_x=" << config.warp.corners[1].x << "\n";
    file << "corner1_y=" << config.warp.corners[1].y << "\n";
    
    file << "corner2_x=" << config.warp.corners[2].x << "\n";
    file << "corner2_y=" << config.warp.corners[2].y << "\n";
    
    file << "corner3_x=" << config.warp.corners[3].x << "\n";
    file << "corner3_y=" << config.warp.corners[3].y << "\n";
    
    file << "showGrid=" << (config.showGrid ? 1 : 0) << "\n";
    file << "gridCols=" << config.gridCols << "\n";
    file << "gridRows=" << config.gridRows << "\n";
    file << "gridThickness=" << config.gridThickness << "\n";
    
    file << "gridColor_r=" << config.gridColor[0] << "\n";
    file << "gridColor_g=" << config.gridColor[1] << "\n";
    file << "gridColor_b=" << config.gridColor[2] << "\n";
    file << "gridColor_a=" << config.gridColor[3] << "\n";
    file << "autoRestoreMinimized=" << (config.autoRestoreMinimized ? 1 : 0) << "\n";
    
    file << "cropLeft=" << config.cropLeft << "\n";
    file << "cropTop=" << config.cropTop << "\n";
    file << "cropRight=" << config.cropRight << "\n";
    file << "cropBottom=" << config.cropBottom << "\n";
    
    return true;
}
