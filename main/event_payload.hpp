#pragma once
#include "motion_detector.hpp"
#include "motion_config.hpp"
#include "mini_json.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
constexpr const char* kProgramVersion = "2.1.2";
constexpr int kApiVersion = 2;
std::int64_t unixMsNow();
std::string base64Encode(const std::uint8_t* data, std::size_t size);
bool eventSelected(const NotifyConfig& notify, const std::string& type);
mini_json::Value eventPayload(const MotionEvent& event, const std::string& deviceIp, int httpPort, const NotifyConfig& notify);
