#ifndef VISION_MODULE_HPP
#define VISION_MODULE_HPP

#include <opencv2/opencv.hpp>
#include <vector>
#include <string>

struct DetectionResult
{
    cv::Rect box;
    int classId;
    float confidence;
    bool isValid;
};

bool Vision_Init(const std::string& modelPath);
void Vision_Start(void);
void Vision_Stop(void);
void Vision_SubmitFrame(const cv::Mat& frame);
void Vision_GetOverlay(std::vector<DetectionResult>& outDetections);

#endif /* VISION_MODULE_HPP */