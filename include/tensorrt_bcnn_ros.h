#include <ros/ros.h>

#include <cv_bridge/cv_bridge.h>
#include <nav_msgs/GetMap.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/image_encodings.h>

#include <autoware_msgs/DynamicObjectWithFeatureArray.h>

// STL
#include <chrono>
#include <memory>
#include <string>
#include <vector>

// local
#include "TrtNet.h"
#include "cluster2d.h"
#include "data_reader.h"
#include "feature_generator.h"

#define __APP_NAME__ "tensorrt_bcnn"

class TensorrtBcnnROS {
 private:
  ros::NodeHandle nh_;
  ros::NodeHandle pnh_;
  ros::Subscriber sub_image_;
  ros::Subscriber sub_points_;
  ros::Publisher pub_confidence_image_;
  ros::Publisher confidence_image_pub_;
  ros::Publisher confidence_map_pub_;
  ros::Publisher class_image_pub_;
  ros::Publisher points_pub_;
  ros::Publisher objects_pub_;
  ros::Publisher d_objects_pub_;

  std::unique_ptr<Tn::trtNet> net_ptr_;
  std::shared_ptr<Cluster2D> cluster2d_;
  std::string topic_src_;
  std::string trained_model_name_;
  float score_threshold_;

  void imageCallback(const sensor_msgs::Image::ConstPtr &in_image);
  void pointsCallback(const sensor_msgs::PointCloud2 &msg);
  void reset_in_feature();
  void pubColoredPoints(const autoware_msgs::DetectedObjectArray &objects);
  void convertDetected2Dynamic(
      const autoware_msgs::DetectedObjectArray &objects,
      autoware_msgs::DynamicObjectWithFeatureArray &d_objects);
  cv::Mat get_confidence_image(const float *output);
  cv::Mat get_class_image(const float *output);
  nav_msgs::OccupancyGrid get_confidence_map(cv::Mat confidence_image);

  std::shared_ptr<FeatureGenerator> feature_generator_;
  std_msgs::Header message_header_;
  int range_;
  int rows_;
  int cols_;
  int siz_;
  std::vector<float> in_feature;

 public:
  TensorrtBcnnROS(/* args */);
  ~TensorrtBcnnROS();
  bool init();

  void createROSPubSub();
};