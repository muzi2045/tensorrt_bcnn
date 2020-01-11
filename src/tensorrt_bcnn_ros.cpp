#include <ros/package.h>
#include <boost/filesystem.hpp>

#include "tensorrt_bcnn_ros.h"
#include <string>
#include <algorithm>

TensorrtBcnnROS::TensorrtBcnnROS(/* args */):
pnh_("~")
{
}
bool TensorrtBcnnROS::init()
{
  std::string package_path = ros::package::getPath("tensorrt_bcnn");
  // std::string engine_path = package_path +  "/data/bcnnv3_416_fp32.engine";
  // std::string engine_path = package_path +  "/data/holecnn.engine";
  std::string engine_path = package_path +  "/data/bcnn_0111.engine";

  std::ifstream fs(engine_path);
  if(fs.is_open())
  {
    ROS_INFO("load %s", engine_path.c_str());
    net_ptr_.reset(new Tn::trtNet(engine_path));
  }
  else
  {
    ROS_INFO("Could not find %s, try making TensorRT engine from caffemodel and prototxt",
    engine_path.c_str());
    // boost::filesystem::create_directories(package_path + "/data");
    // std::string prototxt_file;
    // std::string caffemodel_file;    // pnh_.param<std::string>("prototxt_file", prototxt_file, "");
    // pnh_.param<std::string>("caffemodel_file", caffemodel_file, "");
    // std::string output_node = "bcnn-det";
    // std::vector<std::string> output_name;
    // output_name.push_back(output_node);
    // std::vector<std::vector<float>> calib_data;
    // Tn::RUN_MODE run_mode = Tn::RUN_MODE::FLOAT32;
    // net_ptr_.reset(new Tn::trtNet(prototxt_file, caffemodel_file, output_name, calib_data, run_mode));
    // net_ptr_->saveEngine(engine_path);
  }
  feature_generator_.reset(new FeatureGenerator());
  if (!feature_generator_->init())
  {
    ROS_ERROR("[%s] Fail to Initialize feature generator for CNNSegmentation", __APP_NAME__);
    return false;
  }

  ros::NodeHandle private_node_handle("~");//to receive args
  private_node_handle.param<std::string>("points_src", topic_src_, "/points_raw");
  ROS_INFO("[%s] points_src: %s", __APP_NAME__, topic_src_.c_str());
}

TensorrtBcnnROS::~TensorrtBcnnROS()
{
}


void TensorrtBcnnROS::createROSPubSub()
{
  sub_points_ = nh_.subscribe(topic_src_, 1, &TensorrtBcnnROS::pointsCallback, this);

  pub_objects_ = nh_.advertise<autoware_msgs::DynamicObjectWithFeatureArray>("rois", 1);
  pub_image_ = nh_.advertise<sensor_msgs::Image>("/perception/tensorrt_bcnn/classified_image", 1);
  confidence_pub_ = nh_.advertise<sensor_msgs::Image>("confidence_image", 1);
  confidence_map_pub_ = nh_.advertise<nav_msgs::OccupancyGrid>("confidence_map", 1);
}

void TensorrtBcnnROS::pointsCallback(const sensor_msgs::PointCloud2 &msg)
{
  int rows_ = 640;
  int cols_ = 640;
  // ROS_INFO("points callback called");
  std::chrono::system_clock::time_point start, end;
  start = std::chrono::system_clock::now();

  pcl::PointCloud<pcl::PointXYZI>::Ptr in_pc_ptr(new pcl::PointCloud<pcl::PointXYZI>);
  pcl::fromROSMsg(msg, *in_pc_ptr);
  pcl::PointIndices valid_idx;
  auto &indices = valid_idx.indices;
  indices.resize(in_pc_ptr->size());
  std::iota(indices.begin(), indices.end(), 0);
  message_header_ = msg.header;
  std::vector<float> input_data = feature_generator_->generate(in_pc_ptr);

  int outputCount = net_ptr_->getOutputSize()/sizeof(float);
  std::unique_ptr<float[]> output_data(new float[outputCount]);

  net_ptr_->doInference(input_data.data(), output_data.get());

  auto output = output_data.get();

  cv::Mat confidence_image(640, 640, CV_8UC1);
  for (int row = 0; row < 640; ++row)
  {
    unsigned char *src = confidence_image.ptr<unsigned char>(row);
    for (int col = 0; col < 640; ++col)
    {
      int grid = row + col * 640;
      if (output[grid] > 0.5)
      {
        src[cols_ - col - 1] = 255;
      }
      else
      {
        src[cols_ - col - 1] = 0;
      }
    }
  }
  confidence_pub_.publish(cv_bridge::CvImage(message_header_,
                                             sensor_msgs::image_encodings::MONO8,
                                             confidence_image).toImageMsg());

  nav_msgs::OccupancyGrid confidence_map;
  confidence_map.header = message_header_;
  confidence_map.header.frame_id = "velodyne";
  confidence_map.info.width = 640;
  confidence_map.info.height = 640;
  confidence_map.info.origin.orientation.w = 1;
  double resolution = 120. /640.;
  confidence_map.info.resolution = resolution;
  confidence_map.info.origin.position.x = -((640 + 1) * resolution * 0.5f);
  confidence_map.info.origin.position.y = -((640 + 1) * resolution * 0.5f);

  //read the pixels of the image and fill the confidence_map table
  int data;
  for(int i=640 -1; i>=0; i--)
  {
    for (unsigned int j=0; j<640; j++)
    {
      data = confidence_image.data[i*640 + j];
      if (data >=123 && data <= 131)
      {
        confidence_map.data.push_back(-1);
      }
      else if (data >=251 && data <= 259)
      {
        confidence_map.data.push_back(0);
      }
      else
        confidence_map.data.push_back(100);
    }
  }
  confidence_map_pub_.publish(confidence_map);
}
