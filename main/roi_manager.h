// roi_manager.h
#pragma once
#include "roi.h"
#include <mutex>
#include <vector>

class ROIManager {
public:
    void update(const std::vector<PolygonROI>& rois) {
        std::lock_guard<std::mutex> lock(mtx_);
        rois_ = rois;
    }

    std::vector<PolygonROI> get() {
        std::lock_guard<std::mutex> lock(mtx_);
        return rois_;
    }

private:
    std::vector<PolygonROI> rois_;
    std::mutex mtx_;
};