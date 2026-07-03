// roi.h
#pragma once
#include <vector>

struct Point {
    float x;
    float y;
};

struct PolygonROI {
    int id;
    std::vector<Point> points;
    bool whitelist;  // true = 忽略运动（白名单区域）
};