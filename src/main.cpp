#include <iostream>
#include <dirent.h>
#include <sys/stat.h>
#include <fstream>
#include <cmath>

#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/photo/photo.hpp"
#include "opencv2/imgcodecs.hpp"


#define DEBUG_FLAG              1     // Debug flag for image channels
#define BIN_AREA                40    // Bin area
#define NUM_BINS                11    // Number of bins
#define MIN_ARC_LENGTH          20    // Min arc length
#define PI                      3.14  // Approximate value of pi

/* Channel type */
enum class ChannelType : unsigned char {
    BLUE = 0,
    GREEN,
    RED,
    WHITE
};

/* Hierarchy type */
enum class HierarchyType : unsigned char {
    INVALID_CNTR = 0,
    CHILD_CNTR,
    PARENT_CNTR
};

/* Enhance the image */
bool enhanceImage(  cv::Mat src,
                    ChannelType channel_type,
                    cv::Mat *norm,
                    cv::Mat *dst    ) {

    // Split the image
    std::vector<cv::Mat> channel(3);
    cv::split(src, channel);
    cv::Mat img = channel[0];

    // Normalize the image
    cv::Mat normalized;
    cv::normalize(img, normalized, 0, 255, cv::NORM_MINMAX, CV_8UC1);

    // Enhance the image using Gaussian blur and thresholding
    cv::Mat enhanced;
    switch(channel_type) {
        case ChannelType::GREEN: {
            // Enhance the green channel
            cv::threshold(normalized, enhanced, 15, 255, cv::THRESH_BINARY);
        } break;

        case ChannelType::RED: {
            // Enhance the red channel
            cv::threshold(normalized, enhanced, 35, 255, cv::THRESH_BINARY);
        } break;

        case ChannelType::BLUE: {
            // Enhance the white channel
            cv::threshold(normalized, enhanced, 35, 255, cv::THRESH_BINARY);
        } break;

        default: {
            std::cerr << "Invalid channel type" << std::endl;
            return false;
        }
    }
    *norm = normalized;
    *dst = enhanced;
    return true;
}

/* Find the contours in the image */
void contourCalc(   cv::Mat src, ChannelType channel_type, 
                    double min_area, cv::Mat *dst, 
                    std::vector<std::vector<cv::Point>> *contours, 
                    std::vector<cv::Vec4i> *hierarchy, 
                    std::vector<HierarchyType> *validity_mask, 
                    std::vector<double> *parent_area    ) {

    cv::Mat temp_src;
    src.copyTo(temp_src);
    switch(channel_type) {
        case ChannelType::GREEN : {
            findContours(temp_src, *contours, *hierarchy, cv::RETR_EXTERNAL, 
                                                        cv::CHAIN_APPROX_SIMPLE);
        } break;

        case ChannelType::RED :
        case ChannelType::WHITE : {
            findContours(temp_src, *contours, *hierarchy, cv::RETR_CCOMP, 
                                                        cv::CHAIN_APPROX_SIMPLE);
        } break;

        default: return;
    }

    *dst = cv::Mat::zeros(temp_src.size(), CV_8UC3);
    if (!contours->size()) return;
    validity_mask->assign(contours->size(), HierarchyType::INVALID_CNTR);
    parent_area->assign(contours->size(), 0.0);

    // Keep the contours whose size is >= than min_area
    cv::RNG rng(12345);
    for (int index = 0 ; index < (int)contours->size(); index++) {
        if ((*hierarchy)[index][3] > -1) continue; // ignore child
        auto cntr_external = (*contours)[index];
        double area_external = fabs(contourArea(cv::Mat(cntr_external)));
        if (area_external < min_area) continue;

        std::vector<int> cntr_list;
        cntr_list.push_back(index);

        int index_hole = (*hierarchy)[index][2];
        double area_hole = 0.0;
        while (index_hole > -1) {
            std::vector<cv::Point> cntr_hole = (*contours)[index_hole];
            double temp_area_hole = fabs(contourArea(cv::Mat(cntr_hole)));
            if (temp_area_hole) {
                cntr_list.push_back(index_hole);
                area_hole += temp_area_hole;
            }
            index_hole = (*hierarchy)[index_hole][0];
        }
        double area_contour = area_external - area_hole;
        if (area_contour >= min_area) {
            (*validity_mask)[cntr_list[0]] = HierarchyType::PARENT_CNTR;
            (*parent_area)[cntr_list[0]] = area_contour;
            for (unsigned int i = 1; i < cntr_list.size(); i++) {
                (*validity_mask)[cntr_list[i]] = HierarchyType::CHILD_CNTR;
            }
            cv::Scalar color = cv::Scalar(rng.uniform(0, 255), rng.uniform(0,255), 
                                            rng.uniform(0,255));
            drawContours(*dst, *contours, index, color, cv::FILLED, cv::LINE_8, *hierarchy);
        }
    }
}

/* Filter out ill-formed or small cells */
void filterCells(   std::vector<std::vector<cv::Point>> contours,
                    std::vector<HierarchyType> contour_mask,
                    std::vector<double> contours_area,
                    std::vector<std::vector<cv::Point>> *filtered_contours,
                    std::vector<HierarchyType> *filtered_contour_mask,
                    std::vector<double> *filtered_contours_area     ) {

    for (size_t i = 0; i < contours.size(); i++) {
        if (contour_mask[i] != HierarchyType::PARENT_CNTR) continue;

        // Eliminate invalid contours
        if (contours[i].size() < 5) continue;

        // Eliminate small contours via contour arc calculation
        if (arcLength(contours[i], true) >= MIN_ARC_LENGTH) {
            filtered_contours->push_back(contours[i]);
            filtered_contour_mask->push_back(contour_mask[i]);
            filtered_contours_area->push_back(contours_area[i]);
        }
    }
}

/* Separation metrics */
std::string separationMetrics(
                std::vector<std::vector<cv::Point>> contours) {

    float aggregate_diameter = 0;
    float aggregate_aspect_ratio = 0;
    std::vector<unsigned int> count(NUM_BINS, 0);

    for (size_t i = 0; i < contours.size(); i++) {
        auto min_area_rect = minAreaRect(cv::Mat(contours[i]));
        float aspect_ratio = float(min_area_rect.size.width)/min_area_rect.size.height;
        if (aspect_ratio > 1.0) aspect_ratio = 1.0/aspect_ratio;
        aggregate_aspect_ratio += aspect_ratio;

        float area = contourArea(contours[i]);
        aggregate_diameter += 2 * sqrt(area / PI);
        unsigned int bin_index = (area/BIN_AREA < NUM_BINS) ? 
                                            area/BIN_AREA : NUM_BINS-1;
        count[bin_index]++;
    }

    std::string result =    std::to_string(contours.size())     + "," +
                            std::to_string(aggregate_diameter)  + "," +
                            std::to_string(aggregate_aspect_ratio);
    for (size_t i = 0; i < count.size(); i++) {
        result += "," + std::to_string(count[i]);
    }

    return result;
}

/* Process each image */
bool processImage(std::string path, std::string image_name, std::string *result) {

    *result = image_name + ",";

    // Create the output directory
    std::string out_directory = path + "result/";
    struct stat st = {0};
    if (stat(out_directory.c_str(), &st) == -1) {
        mkdir(out_directory.c_str(), 0700);
    }

    // Extract the pixel map from the input image
    std::string image_path = path + "original/" + image_name;
    std::string cmd = "convert -quiet -quality 100 " + image_path + " /tmp/img.jpg";
    system(cmd.c_str());
    cv::Mat image = cv::imread("/tmp/img.jpg", cv::IMREAD_COLOR | cv::IMREAD_ANYDEPTH);
    if (image.empty()) {
        std::cerr << "Invalid input file" << std::endl;
        return false;
    }
    system("rm /tmp/img.jpg");

    // Split the image
    std::vector<cv::Mat> channel(3);
    cv::split(image, channel);
    cv::Mat blue  = channel[0];
    cv::Mat green = channel[0];
    cv::Mat red   = channel[0];

    /** Gather BGR channel information needed for feature extraction **/

    // Green channel
    cv::Mat green_normalized, green_enhanced;
    if(!enhanceImage(green, ChannelType::GREEN, &green_normalized, &green_enhanced)) {
        return false;
    }
    cv::Mat green_segmented;
    std::vector<std::vector<cv::Point>> contours_green;
    std::vector<cv::Vec4i> hierarchy_green;
    std::vector<HierarchyType> green_contour_mask;
    std::vector<double> green_contour_area;
    contourCalc(green_enhanced, ChannelType::GREEN, 1.0, 
                &green_segmented, &contours_green, 
                &hierarchy_green, &green_contour_mask, 
                &green_contour_area);

    // Red channel
    cv::Mat red_normalized, red_enhanced;
    if(!enhanceImage(red, ChannelType::RED, &red_normalized, &red_enhanced)) {
        return false;
    }
    cv::Mat red_segmented;
    std::vector<std::vector<cv::Point>> contours_red;
    std::vector<cv::Vec4i> hierarchy_red;
    std::vector<HierarchyType> red_contour_mask;
    std::vector<double> red_contour_area;
    contourCalc(red_enhanced, ChannelType::RED, 1.0, 
                &red_segmented, &contours_red, 
                &hierarchy_red, &red_contour_mask, 
                &red_contour_area);

    // White channel
    cv::Mat blue_normalized, blue_enhanced;
    if(!enhanceImage(blue, ChannelType::BLUE, &blue_normalized, &blue_enhanced)) {
        return false;
    }
    cv::Mat white_enhanced;
    bitwise_and(blue_enhanced, green_enhanced, white_enhanced);
    bitwise_and(white_enhanced, red_enhanced, white_enhanced);
    cv::Mat white_segmented;
    std::vector<std::vector<cv::Point>> contours_white;
    std::vector<cv::Vec4i> hierarchy_white;
    std::vector<HierarchyType> white_contour_mask;
    std::vector<double> white_contour_area;
    contourCalc(white_enhanced, ChannelType::WHITE, 1.0, 
                &white_segmented, &contours_white, 
                &hierarchy_white, &white_contour_mask, 
                &white_contour_area);


    /** Extract multi-dimensional features for analysis **/

    /* Characterize the green channel */
    std::vector<std::vector<cv::Point>> contours_green_filtered;
    std::vector<HierarchyType> green_filtered_contour_mask;
    std::vector<double> green_filtered_contours_area;
    filterCells(    contours_green,
                    green_contour_mask,
                    green_contour_area,
                    &contours_green_filtered,
                    &green_filtered_contour_mask,
                    &green_filtered_contours_area    );
    *result += separationMetrics(contours_green_filtered) + ",";

    /* Characterize the red channel */
    std::vector<std::vector<cv::Point>> contours_red_filtered;
    std::vector<HierarchyType> red_filtered_contour_mask;
    std::vector<double> red_filtered_contours_area;
    filterCells(    contours_red,
                    red_contour_mask,
                    red_contour_area,
                    &contours_red_filtered,
                    &red_filtered_contour_mask,
                    &red_filtered_contours_area    );
    *result += separationMetrics(contours_red_filtered) + ",";

    /* Characterize the white channel */
    std::vector<std::vector<cv::Point>> contours_white_filtered;
    std::vector<HierarchyType> white_filtered_contour_mask;
    std::vector<double> white_filtered_contours_area;
    filterCells(    contours_white,
                    white_contour_mask,
                    white_contour_area,
                    &contours_white_filtered,
                    &white_filtered_contour_mask,
                    &white_filtered_contours_area    );
    *result += separationMetrics(contours_white_filtered);


    /** Draw the required images **/

    /* Normalized image */
    std::vector<cv::Mat> merge_normalized;
    merge_normalized.push_back(blue_normalized);
    merge_normalized.push_back(green_normalized);
    merge_normalized.push_back(red_normalized);
    cv::Mat color_normalized;
    cv::merge(merge_normalized, color_normalized);
    std::string out_normalized = out_directory + image_name;
    out_normalized.insert(out_normalized.find_last_of("."), "_a_normalized", 13);
    if (DEBUG_FLAG) {
        std::vector<int> compression_params;
        compression_params.push_back(CV_IMWRITE_JPEG_QUALITY);
        compression_params.push_back(101);
        cv::imwrite("/tmp/img.jpg", color_normalized, compression_params);
        cmd = "convert -quiet /tmp/img.jpg " + out_normalized;
        system(cmd.c_str());
        system("rm /tmp/img.jpg");
    }

    /* Enhanced image */
    std::vector<cv::Mat> merge_enhanced;
    merge_enhanced.push_back(blue_enhanced);
    merge_enhanced.push_back(green_enhanced);
    merge_enhanced.push_back(red_enhanced);
    cv::Mat color_enhanced;
    cv::merge(merge_enhanced, color_enhanced);
    std::string out_enhanced = out_directory + image_name;
    out_enhanced.insert(out_enhanced.find_last_of("."), "_b_enhanced", 11);
    if (DEBUG_FLAG) {
        std::vector<int> compression_params;
        compression_params.push_back(CV_IMWRITE_JPEG_QUALITY);
        compression_params.push_back(101);
        cv::imwrite("/tmp/img.jpg", color_enhanced, compression_params);
        cmd = "convert -quiet /tmp/img.jpg " + out_enhanced;
        system(cmd.c_str());
        system("rm /tmp/img.jpg");
    }

    /* Analyzed image */
    cv::Mat drawing_blue  = blue_normalized;
    cv::Mat drawing_green = green_normalized;
    cv::Mat drawing_red   = red_normalized;

    // Draw green boundaries
    for (size_t i = 0; i < contours_green_filtered.size(); i++) {
        if (green_filtered_contour_mask[i] != HierarchyType::PARENT_CNTR) continue;
        drawContours(drawing_blue, contours_green_filtered, i, 0, 1, 8);
        drawContours(drawing_green, contours_green_filtered, i, 255, 1, 8);
        drawContours(drawing_red, contours_green_filtered, i, 255, 1, 8);
    }

    // Draw white boundaries
    for (size_t i = 0; i < contours_white_filtered.size(); i++) {
        if (white_filtered_contour_mask[i] != HierarchyType::PARENT_CNTR) continue;
        drawContours(drawing_blue, contours_white_filtered, i, 255, 1, 8);
        drawContours(drawing_green, contours_white_filtered, i, 0, 1, 8);
        drawContours(drawing_red, contours_white_filtered, i, 255, 1, 8);
    }

    // Merge the modified red, blue and green layers
    std::vector<cv::Mat> merge_analyzed;
    merge_analyzed.push_back(drawing_blue);
    merge_analyzed.push_back(drawing_green);
    merge_analyzed.push_back(drawing_red);
    cv::Mat color_analyzed;
    cv::merge(merge_analyzed, color_analyzed);
    std::string out_analyzed = out_directory + image_name;
    if (DEBUG_FLAG) out_analyzed.insert(out_analyzed.find_last_of("."), "_c_analyzed", 11);
    std::vector<int> compression_params;
    compression_params.push_back(CV_IMWRITE_JPEG_QUALITY);
    compression_params.push_back(101);
    cv::imwrite("/tmp/img.jpg", color_analyzed, compression_params);
    cmd = "convert -quiet /tmp/img.jpg " + out_analyzed;
    system(cmd.c_str());
    system("rm /tmp/img.jpg");

    return true;
}

/* Main - create the threads and start the processing */
int main(int argc, char *argv[]) {

    /* Check for argument count */
    if (argc != 2) {
        std::cerr << "Invalid number of arguments." << std::endl;
        return -1;
    }

    /* Read the path to the data */
    std::string path(argv[1]);

    /* Read the list of directories to process */
    std::string image_list_filename = path + "image_list.dat";
    std::vector<std::string> input_images;
    FILE *file = fopen(image_list_filename.c_str(), "r");
    if (!file) {
        std::cerr << "Invalid raw image" << std::endl;
        return -1;
    }

    char line[128];
    while (fgets(line, sizeof(line), file) != NULL) {
        line[strlen(line)-1] = 0;
        std::string temp_str(line);
        input_images.push_back(temp_str);
    }
    fclose(file);

    /* Create and prepare the file for metrics */
    std::string metrics_file = path + "computed_metrics.csv";
    std::ofstream data_stream;
    data_stream.open(metrics_file, std::ios::out);
    if (!data_stream.is_open()) {
        std::cerr << "Could not create the metrics file." << std::endl;
        return -1;
    }

    data_stream << "Image_Name,";

    // Green channel
    data_stream << "Green_Contour_Count,";
    data_stream << "Green_Contour_Diameter_(mean),";
    data_stream << "Green_Contour_Aspect_Ratio_(mean),";
    for (unsigned int i = 0; i < NUM_BINS-1; i++) {
        data_stream << i*BIN_AREA << " <= Green_Contour_Area < " << (i+1)*BIN_AREA << ",";
    }
    data_stream << "Green_Contour_Area >= " << (NUM_BINS-1)*BIN_AREA << ",";

    // Red channel
    data_stream << "Red_Contour_Count,";
    data_stream << "Red_Contour_Diameter_(mean),";
    data_stream << "Red_Contour_Aspect_Ratio_(mean),";
    for (unsigned int i = 0; i < NUM_BINS-1; i++) {
        data_stream << i*BIN_AREA << " <= Red_Contour_Area < " << (i+1)*BIN_AREA << ",";
    }
    data_stream << "Red_Contour_Area >= " << (NUM_BINS-1)*BIN_AREA << ",";

    // White channel
    data_stream << "White_Contour_Count,";
    data_stream << "White_Contour_Diameter_(mean),";
    data_stream << "White_Contour_Aspect_Ratio_(mean),";
    for (unsigned int i = 0; i < NUM_BINS-1; i++) {
        data_stream << i*BIN_AREA << " <= White_Contour_Area < " << (i+1)*BIN_AREA << ",";
    }
    data_stream << "White_Contour_Area >= " << (NUM_BINS-1)*BIN_AREA;

    data_stream << std::endl;


    /* Process the image set */
    for (unsigned int index = 0; index < input_images.size(); index++) {
        std::cout << "Processing " << input_images[index] << std::endl;
        std::string result;
        if (!processImage(path, input_images[index], &result)) {
            std::cerr << "ERROR !!!" << std::endl;
            return -1;
        }
        data_stream << result << std::endl;
    }
    data_stream.close();

    return 0;
}

