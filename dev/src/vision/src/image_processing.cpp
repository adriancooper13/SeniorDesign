#include <cv_bridge/cv_bridge.h>
#include <opencv2/opencv.hpp>

#include "custom_interfaces/msg/image_data.hpp"
#include "custom_interfaces/msg/threshold_adjustment.hpp"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/msg/image.hpp"
#include "std_msgs/msg/int32.hpp"

#define WHITE 255
#define WIDTH 360
#define HEIGHT 240
#define NO_EDGE_FOUND INT_MAX
#define NO_BALL_FOUND -180
#define DEBUG true

const auto RED = cv::Scalar(0, 0, 255);
const auto GREEN = cv::Scalar(0, 255, 0);
const auto BLUE = cv::Scalar(255, 0, 0);

class ImageProcessing : public rclcpp::Node
{
    private:
        int middle_pos, lower_threshold, lower_red_value;
        std::vector<int> histogram_lane;
        cv::Mat frame, frame_copy, frame_final, frame_red;
        rclcpp::Publisher<custom_interfaces::msg::ImageData>::SharedPtr image_data_publisher;
        rclcpp::Subscription<sensor_msgs::msg::Image>::SharedPtr image_subscription;
        rclcpp::Subscription<custom_interfaces::msg::ThresholdAdjustment>::SharedPtr threshold_subscription;
        rclcpp::TimerBase::SharedPtr timer;

    public:
        ImageProcessing() : Node("image_processing")
        {
            lower_threshold = 180;
            lower_red_value = 195;

            image_data_publisher = create_publisher<custom_interfaces::msg::ImageData>("image_data", 10);
            image_subscription = create_subscription<sensor_msgs::msg::Image>(
                "camera/image_raw",
                10,
                std::bind(&ImageProcessing::process_image, this, std::placeholders::_1)
            );
            threshold_subscription = create_subscription<custom_interfaces::msg::ThresholdAdjustment>(
                "vision_threshold_adjustment",
                10,
                std::bind(&ImageProcessing::adjust_thresholds, this, std::placeholders::_1)
            );

            if (DEBUG)
            {
                using namespace std::chrono_literals;
                timer = create_wall_timer(1ms, std::bind(&ImageProcessing::image_show, this));
            }

            RCLCPP_INFO(get_logger(), "%s node has started.", get_name());
        }

    private:
        void process_image(const sensor_msgs::msg::Image::SharedPtr message)
        {
            frame = cv_bridge::toCvCopy(message, message->encoding)->image;
            frame_copy = frame.clone();
            cv::cvtColor(frame, frame_red, cv::COLOR_BGR2HSV);
            
            int edge_result = NO_EDGE_FOUND;            
            auto edge_thread = std::thread(&ImageProcessing::check_corners, this, std::ref(edge_result));
	        
            int line_width = 2;
	        // create a frame of reference... adjust these as needed. They represent the 4 corners of the box.
            cv::Point2f source[] = {
                cv::Point2f(30, HEIGHT / 2),
                cv::Point2f(WIDTH - 30, HEIGHT / 2),
                cv::Point2f(0, HEIGHT),
                cv::Point2f(WIDTH, HEIGHT)
            };
            
            // goes from top left to top right
            line(frame, source[0], source[1], RED, line_width);
            // goes from top right to bottom right
            line(frame, source[1], source[3], RED, line_width);
            // goes from bottom right to bottom left
            line(frame, source[3], source[2], RED, line_width);
            // goes from bottom left to top left
            line(frame, source[2], source[0], RED, line_width);
            
            threshold();
            histogram();
            find_largest_ball();
            int ball_result = lane_center();

            // We do not see a golf ball. We need the corner information. Wait for it.
            if (ball_result == NO_BALL_FOUND)
                edge_thread.join();

            publish_image_data(ball_result, edge_result);
            if (edge_thread.joinable())
                edge_thread.join();
        }

        void check_corners(int &edge_result)
        {
            int box_width = 40, divisions = 255;
            int pixels_from_top = 160, pixel_from_bottom = 0;

            cv::Mat mask1, mask2, red_frame_copy;
            // first digit in Scalar is it's Hue... (red goes from 175 to 5 (it wraps around 180 and back to 0))
            // Second digit is for Saturation... The higher the saturation value, the deeper the red... a low saturation is a lighter red
            // the third value represents value... a value of 0 is black. Darker read means a lower value 
            inRange(frame_red, cv::Scalar(0, 120, lower_red_value), cv::Scalar(10, 255, 255), mask1);
            inRange(frame_red, cv::Scalar(170, 120, lower_red_value), cv::Scalar(180, 255, 255), mask2);
            add(mask1, mask2, frame_red);
            
            try
            {
                cv::cvtColor(frame_red, red_frame_copy, cv::COLOR_GRAY2BGR);
                int res = histogram(red_frame_copy, 0, box_width, pixels_from_top, pixel_from_bottom, divisions);
                if (res != -1)
                {
                    RCLCPP_DEBUG(get_logger(), "Should turn right");
                    edge_result = WIDTH - res - (WIDTH / 2);
                    return;
                }

                cv::cvtColor(frame_red, red_frame_copy, cv::COLOR_GRAY2BGR);
                res = histogram(red_frame_copy, WIDTH - box_width, WIDTH, pixels_from_top, pixel_from_bottom, divisions);
                if (res != -1)
                {
                    RCLCPP_DEBUG(get_logger(), "Should turn left");
                    edge_result = WIDTH - res - (WIDTH / 2);
                }
            }
            catch (const cv::Exception &e)
            {
                RCLCPP_WARN(get_logger(), "Could not convert image from gray to BGR format. Exception: %s", e.what());
            }
        }

        void threshold()
        {
            cv::Mat frame_thresh, frame_gray;
            cvtColor(frame_copy, frame_gray, cv::COLOR_BGR2GRAY);
            // frame input name, min threshold for white, max threshold for white, frame output name. Tweak these as necessary, but min threshold may want to go down if indoors.
            // find the white in the image.
            inRange(frame_gray, lower_threshold, WHITE, frame_thresh); // 137 looked good indoors at night, 165 looked good indoors during the day
            
            frame_final = frame_thresh.clone();
            cvtColor(frame_final, frame_final, cv::COLOR_GRAY2RGB);
        }

        void histogram()
        {
            // resize to the size of the lane.
            histogram_lane.resize(frame.size().width);
            histogram_lane.clear();
            
            // How far down from the top red line are we looking.
            int pixels_from_top = (HEIGHT / 2) + 10;
            cv::Mat roi_lane, frame_final_bgr;
            cvtColor(frame_final, frame_final_bgr, cv::COLOR_RGB2BGR);
            for (int i = 0; i < frame.size().width; i++)
            {
                // reason of interest strip
                roi_lane = frame_final_bgr(cv::Rect(i, pixels_from_top, 1, HEIGHT - pixels_from_top));
                divide(255, roi_lane, roi_lane);
                histogram_lane.push_back((int)(sum(roi_lane)[0]));
            }
        }

        // Assumes frame is in RGB format
        int histogram(
            cv::Mat &localframe,
            int start,
            int end,
            int pixels_from_top,
            int pixels_from_bottom = 0,
            int divisions = 255)
        {
            cv::Mat roi_lane;
            for (int i = start; i < end; i++)
            {
                // reason of interest strip
                roi_lane = localframe(cv::Rect(i, pixels_from_top, 1, HEIGHT - pixels_from_top - pixels_from_bottom));
                divide(divisions, roi_lane, roi_lane);
                if ((int)(sum(roi_lane)[0]) > 5)
                    return i;
            }

            return -1;
        }

        int lane_center()
        {
            int frame_center = WIDTH / 2;
            
            line(frame_final, cv::Point2f(frame_center, 0), cv::Point2f(frame_center, HEIGHT), BLUE, 3);
            
            // difference between true center and center ball...
            return middle_pos - frame_center;
        }

        void find_middle_ball()
        {
            // iterator to point to max intensity spot
            std::vector<int>::iterator left_ptr;
            // scans from left-most pixel to left-middle pixel
            left_ptr = max_element(histogram_lane.begin(), histogram_lane.begin() + 120);
            auto left_lane_pos = distance(histogram_lane.begin(), left_ptr);
            
            // iterator to point to max intensity spot
            std::vector<int>::iterator right_ptr;
            // scans from right-middle pixel to right-most pixel
            right_ptr = max_element(histogram_lane.end() - 119, histogram_lane.end());
            auto right_lane_pos = distance(histogram_lane.begin(), right_ptr);
            
            // scans from left-middle pixel to right-middle pixel
            std::vector<int>::iterator middle_ptr;
            middle_ptr = max_element(histogram_lane.begin() + 121, histogram_lane.end() - 120);
            middle_pos = distance(histogram_lane.begin(), middle_ptr);
            
            // middle is at pixel column 180
            int mid_dist = abs(180 - middle_pos);
            int left_dist = abs(180 - left_lane_pos);
            int right_dist = abs(180 - right_lane_pos);
            
            if (mid_dist <= left_dist && mid_dist <= right_dist) {
                middle_pos = middle_pos;
            } else if (left_dist <= mid_dist && left_dist <= right_dist) {
                middle_pos = left_lane_pos;
            } else {
                middle_pos = right_lane_pos;
            }
            
            line(frame_final, cv::Point2f(middle_pos, 0), cv::Point2f(middle_pos, HEIGHT), GREEN, 2);
        }

        void find_largest_ball()
        {
            // iterator to point to max intensity spot
            std::vector<int>::iterator whitest_ptr;
            // scans from left-most pixel to left-middle pixel
            whitest_ptr = max_element(histogram_lane.begin(), histogram_lane.end());
            middle_pos = distance(histogram_lane.begin(), whitest_ptr);
            
            line(frame_final, cv::Point2f(middle_pos, 0), cv::Point2f(middle_pos, HEIGHT), GREEN, 2);
        }

        void publish_image_data(int ball_result, int &corner_result)
        {
            // Publish result
            auto message = custom_interfaces::msg::ImageData();
            message.ball_position = ball_result;
            message.corner_position = corner_result;
            image_data_publisher->publish(message);
        }

        void adjust_thresholds(const custom_interfaces::msg::ThresholdAdjustment::SharedPtr threshold_adjustment)
        {
            int lower_adj = threshold_adjustment->lower_adjustment;
            int red_adj = threshold_adjustment->red_adjustment;

            if (lower_adj != 0 && lower_threshold + lower_adj >= 0 && lower_threshold + lower_adj <= 255)
            {
                lower_threshold += lower_adj;
                RCLCPP_INFO(get_logger(), "Lower Threshold: %d", lower_threshold);
            }
            if (red_adj != 0 && lower_red_value + red_adj >= 0 && lower_red_value + red_adj <= 255)
            {
                lower_red_value += red_adj;
                RCLCPP_INFO(get_logger(), "Red Value: %d", lower_red_value);
            }
        }

        void image_show()
        {
            if (!frame.empty())
            {
                cv::imshow("raw_image", frame);
                cv::waitKey(1);
            }

            if (!frame_final.empty())
            {
                cv::imshow("final_image", frame_final);
                cv::waitKey(1);
            }

            if (!frame_red.empty())
            {
                cv::imshow("frame_red", frame_red);
                cv::waitKey(1);
            }
            
        }
};

int main(int argc, char **argv)
{
    rclcpp::init(argc, argv);
    rclcpp::spin(std::make_shared<ImageProcessing>());
    rclcpp::shutdown();
    return 0;
}
