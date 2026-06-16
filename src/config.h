#pragma once
#include <string>
#include "warp.h"

struct AppConfig {
    int monitorIndex = 0;
    std::wstring captureWindowTitle = L"";
    WarpSettings warp;
    
    // Grid settings
    bool showGrid = false;
    int gridCols = 8;
    int gridRows = 6;
    float gridThickness = 2.0f;
    float gridColor[4] = { 0.0f, 1.0f, 0.0f, 0.8f }; // green grid by default

    // Behaviour
    bool autoRestoreMinimized = true; // Move minimized windows offscreen so WGC keeps capturing

    // Crop settings (fractions 0.0 to 1.0)
    float cropLeft = 0.0f;
    float cropTop = 0.0f;
    float cropRight = 0.0f;
    float cropBottom = 0.0f;
};

bool LoadConfig(const std::string& filepath, AppConfig& outConfig);
bool SaveConfig(const std::string& filepath, const AppConfig& config);
