#include <ros/package.h>
#include <boost/filesystem.hpp>

#include <algorithm>
#include <string>
#include "tensorrt_bcnn_ros.h"

// #include <cuda.h>
// #include <chrono>

TensorrtBcnnROS::TensorrtBcnnROS(/* args */) : pnh_("~") {}
bool TensorrtBcnnROS::init() {
  ros::NodeHandle private_node_handle("~");  // to receive args
  private_node_handle.param<std::string>("points_src", topic_src_,
                                         "/points_raw");
  private_node_handle.param<std::string>("trained_model", trained_model_name_,
                                         "/bcnn_0111.engine");
  private_node_handle.param<float>("score_threshold", score_threshold_, 0.5);

  range_ = 60;
  rows_ = 640;
  cols_ = 640;
  siz_ = rows_ * cols_;
  in_feature.resize(siz_ * 8);

  std::string package_path = ros::package::getPath("tensorrt_bcnn");
  std::string engine_path = package_path + "/data/" + trained_model_name_;

  std::ifstream fs(engine_path);
  if (fs.is_open()) {
    ROS_INFO("load %s", engine_path.c_str());
    net_ptr_.reset(new Tn::trtNet(engine_path));
  } else {
    ROS_INFO("Could not find %s.", engine_path.c_str());
  }

  cluster2d_.reset(new Cluster2D());
  if (!cluster2d_->init(rows_, cols_, range_)) {
    ROS_ERROR("[%s] Fail to Initialize cluster2d for CNNSegmentation",
              __APP_NAME__);
    return false;
  }

  feature_generator_.reset(new FeatureGenerator());
  if (!feature_generator_->init(&in_feature[0])) {
    ROS_ERROR("[%s] Fail to Initialize feature generator for CNNSegmentation",
              __APP_NAME__);
    return false;
  }

  ROS_INFO("[%s] points_src: %s", __APP_NAME__, topic_src_.c_str());
}

TensorrtBcnnROS::~TensorrtBcnnROS() {}

void TensorrtBcnnROS::createROSPubSub() {
  sub_points_ =
      nh_.subscribe(topic_src_, 1, &TensorrtBcnnROS::pointsCallback, this);
  confidence_image_pub_ =
      nh_.advertise<sensor_msgs::Image>("confidence_image", 1);
  confidence_map_pub_ =
      nh_.advertise<nav_msgs::OccupancyGrid>("confidence_map", 1);
  class_image_pub_ = nh_.advertise<sensor_msgs::Image>("class_image", 1);
  points_pub_ = nh_.advertise<sensor_msgs::PointCloud2>(
      "/detection/lidar_detector/points_cluster", 1);
  objects_pub_ = nh_.advertise<autoware_msgs::DetectedObjectArray>(
      "/detection/lidar_detector/objects", 1);
  d_objects_pub_ = nh_.advertise<autoware_msgs::DynamicObjectWithFeatureArray>(
      "labeled_clusters", 1);
}

void TensorrtBcnnROS::reset_in_feature() {
  for (int i = 0; i < siz_ * 8; ++i) {
    if (i < siz_) {
      in_feature[i] = -5;
    } else if (siz_ <= i && i < siz_ * 3) {
      in_feature[i] = 0;
    } else if (siz_ * 4 <= i && i < siz_ * 6) {
      in_feature[i] = 0;
    } else if (siz_ * 7 <= i && i < siz_ * 8) {
      in_feature[i] = 0;
    }
  }
}

cv::Mat TensorrtBcnnROS::get_confidence_image(const float *output) {
  cv::Mat confidence_image(rows_, cols_, CV_8UC1);
  for (int row = 0; row < rows_; ++row) {
    unsigned char *src = confidence_image.ptr<unsigned char>(row);
    for (int col = 0; col < cols_; ++col) {
      int grid = row + col * 640;
      if (output[grid + siz_ * 3] > score_threshold_) {
        src[cols_ - col - 1] = 255;
      } else {
        src[cols_ - col - 1] = 0;
      }
    }
  }
  return confidence_image;
}

cv::Mat TensorrtBcnnROS::get_class_image(const float *output) {
  cv::Mat class_image(rows_, cols_, CV_8UC3);

  for (int row = 0; row < rows_; ++row) {
    cv::Vec3b *src = class_image.ptr<cv::Vec3b>(row);
    for (int col = 0; col < cols_; ++col) {
      int grid = row + col * cols_;
      std::vector<float> class_vec{
          output[grid + siz_ * 4], output[grid + siz_ * 5],
          output[grid + siz_ * 6], output[grid + siz_ * 7],
          output[grid + siz_ * 8], output[grid + siz_ * 9]};
      std::vector<float>::iterator maxIt =
          std::max_element(class_vec.begin(), class_vec.end());
      size_t pred_class = std::distance(class_vec.begin(), maxIt);
      if (pred_class == 1) {
        src[cols_ - col - 1] = cv::Vec3b(255, 0, 0);
      } else if (pred_class == 2) {
        src[cols_ - col - 1] = cv::Vec3b(255, 160, 0);
      } else if (pred_class == 3) {
        src[cols_ - col - 1] = cv::Vec3b(0, 255, 0);
      } else if (pred_class == 4) {
        src[cols_ - col - 1] = cv::Vec3b(0, 0, 255);
      } else {
        src[cols_ - col - 1] = cv::Vec3b(0, 0, 0);
      }
    }
  }
  return class_image;
}

nav_msgs::OccupancyGrid TensorrtBcnnROS::get_confidence_map(
    cv::Mat confidence_image) {
  nav_msgs::OccupancyGrid confidence_map;
  confidence_map.header = message_header_;
  confidence_map.info.width = 640;
  confidence_map.info.height = 640;
  confidence_map.info.origin.orientation.w = 1;
  float resolution = 120. / 640.;
  confidence_map.info.resolution = resolution;
  confidence_map.info.origin.position.x = -((640 + 1) * resolution * 0.5f);
  confidence_map.info.origin.position.y = -((640 + 1) * resolution * 0.5f);
  int data;
  for (int i = 640 - 1; i >= 0; i--) {
    for (unsigned int j = 0; j < 640; j++) {
      data = confidence_image.data[i * 640 + j];
      if (data >= 123 && data <= 131) {
        confidence_map.data.push_back(-1);
      } else if (data >= 251 && data <= 259) {
        confidence_map.data.push_back(0);
      } else
        confidence_map.data.push_back(100);
    }
  }
  return confidence_map;
}

void TensorrtBcnnROS::pubColoredPoints(
    const autoware_msgs::DetectedObjectArray &objects_array) {
  pcl::PointCloud<pcl::PointXYZRGB> colored_cloud;
  for (size_t object_i = 0; object_i < objects_array.objects.size();
       object_i++) {
    pcl::PointCloud<pcl::PointXYZI> object_cloud;
    pcl::fromROSMsg(objects_array.objects[object_i].pointcloud, object_cloud);
    int red = (object_i) % 256;
    int green = (object_i * 7) % 256;
    int blue = (object_i * 13) % 256;

    for (size_t i = 0; i < object_cloud.size(); i++) {
      pcl::PointXYZRGB colored_point;
      colored_point.x = object_cloud[i].x;
      colored_point.y = object_cloud[i].y;
      colored_point.z = object_cloud[i].z;
      colored_point.r = red;
      colored_point.g = green;
      colored_point.b = blue;
      colored_cloud.push_back(colored_point);
    }
  }
  sensor_msgs::PointCloud2 output_colored_cloud;
  pcl::toROSMsg(colored_cloud, output_colored_cloud);
  output_colored_cloud.header = message_header_;
  points_pub_.publish(output_colored_cloud);
}

void TensorrtBcnnROS::convertDetected2Dynamic(
    const autoware_msgs::DetectedObjectArray &objects,
    autoware_msgs::DynamicObjectWithFeatureArray &d_objects) {
  d_objects.header = objects.header;
  for (const auto &object : objects.objects) {
    autoware_msgs::DynamicObjectWithFeature d_object;
    if (object.label == "person" || object.label == "bike" ||
        object.label == "bicycle") {
      d_object.object.semantic.type = d_object.object.semantic.PEDESTRIAN;
    } else if (object.label == "car" || object.label == "bus" ||
               object.label == "track") {
      d_object.object.semantic.type = d_object.object.semantic.CAR;
    } else {
      d_object.object.semantic.type = d_object.object.semantic.PEDESTRIAN;
    }
    d_object.object.semantic.confidence = 1.0;
    d_object.feature.cluster = object.pointcloud;
    d_objects.feature_objects.push_back(d_object);
  }
}

void TensorrtBcnnROS::pointsCallback(const sensor_msgs::PointCloud2 &msg) {
  pcl::PointCloud<pcl::PointXYZI>::Ptr in_pc_ptr(
      new pcl::PointCloud<pcl::PointXYZI>);
  pcl::fromROSMsg(msg, *in_pc_ptr);
  pcl::PointIndices valid_idx;
  auto &indices = valid_idx.indices;
  indices.resize(in_pc_ptr->size());
  std::iota(indices.begin(), indices.end(), 0);
  message_header_ = msg.header;

  this->reset_in_feature();

  feature_generator_->generate(in_pc_ptr, &in_feature[0]);

  int outputCount = net_ptr_->getOutputSize() / sizeof(float);
  std::unique_ptr<float[]> output_data(new float[outputCount]);

  net_ptr_->doInference(in_feature.data(), output_data.get());
  float *output = output_data.get();

  cv::Mat confidence_image = this->get_confidence_image(output);
  nav_msgs::OccupancyGrid confidence_map =
      this->get_confidence_map(confidence_image);
  cv::Mat class_image = this->get_class_image(output);

  float objectness_thresh = 0.5;
  bool use_all_grids_for_clustering = true;

  cluster2d_->cluster(output, in_pc_ptr, valid_idx, objectness_thresh,
                      use_all_grids_for_clustering);
  cluster2d_->filter(output);
  cluster2d_->classify(output);

  float confidence_thresh = score_threshold_;
  float height_thresh = 0.5;
  int min_pts_num = 3;

  autoware_msgs::DetectedObjectArray objects;
  objects.header = message_header_;
  cluster2d_->getObjects(confidence_thresh, height_thresh, min_pts_num, objects,
                         message_header_);

  autoware_msgs::DynamicObjectWithFeatureArray d_objects;

  convertDetected2Dynamic(objects, d_objects);

  pubColoredPoints(objects);

  objects_pub_.publish(objects);
  d_objects_pub_.publish(d_objects);

  confidence_image_pub_.publish(
      cv_bridge::CvImage(message_header_, sensor_msgs::image_encodings::MONO8,
                         confidence_image)
          .toImageMsg());
  confidence_map_pub_.publish(confidence_map);
  class_image_pub_.publish(
      cv_bridge::CvImage(message_header_, sensor_msgs::image_encodings::RGB8,
                         class_image)
          .toImageMsg());
}
