#include <iostream>
#include <opencv2/opencv.hpp>
int main() {
    cv::Mat img = cv::Mat::zeros(50, 50, CV_8UC1);
    cv::Mat tpl = cv::Mat::zeros(10, 10, CV_8UC1);
    tpl(cv::Rect(4,4,2,2)).setTo(255);
    cv::Mat res;
    cv::matchTemplate(img, tpl, res, cv::TM_CCOEFF_NORMED);
    double minV, maxV;
    cv::minMaxLoc(res, &minV, &maxV);
    std::cout << "Max: " << maxV << std::endl;
    return 0;
}
