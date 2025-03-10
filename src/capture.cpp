// Copyright [2015] Takashi Ogura<t.ogura@gmail.com>

#include "cv_camera/capture.h"
#include <sstream>
#include <string>

namespace cv_camera
{

namespace enc = sensor_msgs::image_encodings;

Capture::Capture(ros::NodeHandle &node, const std::string &topic_name,
                 int32_t buffer_size, const std::string &frame_id,
                 const std::string& camera_name,
                 double rect, double raw_factor,
                 int rotate, int compress_height)
    : node_(node),
      it_(node_),
      topic_name_(topic_name),
      buffer_size_(buffer_size),
      frame_id_(frame_id),
      info_manager_(node_, camera_name),
      rect_(rect),
      raw_factor_(raw_factor),
      rotate_(rotate),
      compress_height_(compress_height),
      capture_delay_(ros::Duration(node_.param("capture_delay", 0.0)))
{
}

void Capture::loadCameraInfo()
{
  std::string url;
  if (node_.getParam("camera_info_url", url))
  {
    if (info_manager_.validateURL(url))
    {
      info_manager_.loadCameraInfo(url);
    }
  }

  rescale_camera_info_ = node_.param<bool>("rescale_camera_info", false);

  for (int i = 0;; ++i)
  {
    int code = 0;
    double value = 0.0;
    std::stringstream stream;
    stream << "property_" << i << "_code";
    const std::string param_for_code = stream.str();
    stream.str("");
    stream << "property_" << i << "_value";
    const std::string param_for_value = stream.str();
    if (!node_.getParam(param_for_code, code) || !node_.getParam(param_for_value, value))
    {
      break;
    }
    if (!cap_.set(code, value))
    {
      ROS_ERROR_STREAM("Setting with code " << code << " and value " << value << " failed"
                                            << std::endl);
    }
  }
}

void Capture::rescaleCameraInfo(int width, int height)
{
  double width_coeff = static_cast<double>(width) / info_.width;
  double height_coeff = static_cast<double>(height) / info_.height;
  info_.width = width;
  info_.height = height;

  // See http://docs.ros.org/api/sensor_msgs/html/msg/CameraInfo.html for clarification
  info_.K[0] *= width_coeff;
  info_.K[2] *= width_coeff;
  info_.K[4] *= height_coeff;
  info_.K[5] *= height_coeff;

  info_.P[0] *= width_coeff;
  info_.P[2] *= width_coeff;
  info_.P[5] *= height_coeff;
  info_.P[6] *= height_coeff;
}

void Capture::open(int32_t device_id)
{
  cap_.open(device_id);
  if (!cap_.isOpened())
  {
    std::stringstream stream;
    stream << "device_id" << device_id << " cannot be opened";
    throw DeviceError(stream.str());
  }
  pub_ = it_.advertiseCamera(topic_name_, buffer_size_);
  if (rect_ > 1.01)
  {
    rectpub_ = it_.advertiseCamera("image_rect", buffer_size_);
  }

  loadCameraInfo();
}

void Capture::open(const std::string &device_path)
{
  cap_.open(device_path, cv::CAP_V4L);
  if (!cap_.isOpened())
  {
    throw DeviceError("device_path " + device_path + " cannot be opened");
  }
  pub_ = it_.advertiseCamera(topic_name_, buffer_size_);
  if (rect_ > 1.01)
  {
    rectpub_ = it_.advertiseCamera("image_rect", buffer_size_);
  }

  loadCameraInfo();
}

void Capture::open()
{
  open(0);
}

void Capture::openFile(const std::string &file_path)
{
  cap_.open(file_path);
  if (!cap_.isOpened())
  {
    std::stringstream stream;
    stream << "file " << file_path << " cannot be opened";
    throw DeviceError(stream.str());
  }
  pub_ = it_.advertiseCamera(topic_name_, buffer_size_);
  if (rect_ > 1.01)
  {
    rectpub_ = it_.advertiseCamera("image_rect", buffer_size_);
  }
  std::string url;
  if (node_.getParam("camera_info_url", url))
  {
    if (info_manager_.validateURL(url))
    {
      info_manager_.loadCameraInfo(url);
    }
  }
}

bool Capture::capture()
{
  if (cap_.read(bridge_.image))
  {
    ros::Time stamp = ros::Time::now() - capture_delay_;
    bridge_.encoding = bridge_.image.channels() == 3 ? enc::BGR8 : enc::MONO8;
    bridge_.header.stamp = stamp;
    bridge_.header.frame_id = frame_id_;

    info_ = info_manager_.getCameraInfo();
    if (info_.height == 0 && info_.width == 0)
    {
      info_.height = bridge_.image.rows;
      info_.width = bridge_.image.cols;
    }
    else if (info_.height != bridge_.image.rows || info_.width != bridge_.image.cols)
    {
      if (rescale_camera_info_)
      {
        int old_width = info_.width;
        int old_height = info_.height;
        rescaleCameraInfo(bridge_.image.cols, bridge_.image.rows);
        ROS_INFO_ONCE("Camera calibration automatically rescaled from %dx%d to %dx%d",
                      old_width, old_height, bridge_.image.cols, bridge_.image.rows);
      }
      else
      {
        ROS_WARN_ONCE("Calibration resolution %dx%d does not match camera resolution %dx%d. "
                      "Use rescale_camera_info param for rescaling",
                      info_.width, info_.height, bridge_.image.cols, bridge_.image.rows);
      }
    }
    info_.header.stamp = stamp;
    info_.header.frame_id = frame_id_;

    return true;
  }
  return false;
}

void Capture::publish()
{
  /*
  pub_.publish(*getImageMsgPtr(), info_);
  */
  
  // rotate -> [rect] OR [compress] ->finish
  sensor_msgs::ImagePtr msg = boost::make_shared<sensor_msgs::Image>();
  cv::Mat rotatedMat;
  if (rotate_ != 0)
  {
    cv::rotate(bridge_.image, rotatedMat, 1);//180 degree rotate
  }
  else
  {
    rotatedMat = bridge_.image.clone();
  }
  
  
  if (rect_ > 1.01)
  {
    // In order to save bandwidth, when rect function is active,
    // the original image topic will compress by the 'raw_factor'
    int new_width = bridge_.image.cols / raw_factor_;
    int new_height = bridge_.image.rows / raw_factor_;
    cv::Size dsize = cv::Size(new_width, new_height);
    cv::Mat resizedMat;
    cv::resize(rotatedMat, resizedMat, dsize, 0, 0, CV_INTER_AREA);
    msg = cv_bridge::CvImage(bridge_.header, bridge_.encoding, resizedMat).toImageMsg();
    info_.width = new_width;
    info_.height = new_height;
    pub_.publish(*msg, info_);

    info_.width = bridge_.image.cols;
    info_.height = bridge_.image.rows;
    publishRect();
  }
  else if (compress_height_ > 0)
  {
    if (compress_height_ >= bridge_.image.rows)
    {
      //No need to compress
      ROS_WARN("compress_height need to be smaller than original height. Please Check.");
      msg = cv_bridge::CvImage(bridge_.header, bridge_.encoding, rotatedMat).toImageMsg();
      pub_.publish(*msg, info_);
    }
    else
    {
      int new_height = compress_height_;
      int new_width = new_height * bridge_.image.cols / bridge_.image.rows;
      cv::Size dsize = cv::Size(new_width, new_height);
      cv::Mat compressedMat;
      cv::resize(rotatedMat, compressedMat, dsize, 0, 0, CV_INTER_AREA);
      msg = cv_bridge::CvImage(bridge_.header, bridge_.encoding, rotatedMat).toImageMsg();
      info_.width = new_width;
      info_.height = new_height;
      pub_.publish(*msg, info_);
    }
  }
  else
  {
    msg = cv_bridge::CvImage(bridge_.header, bridge_.encoding, compressedMat).toImageMsg();
    pub_.publish(*msg, info_);
  }
}

void Capture::publishRect()
{
  int new_width = bridge_.image.cols / rect_;
  int new_height = bridge_.image.rows / rect_;
  cv::Rect rect((bridge_.image.cols - new_width) / 2, (bridge_.image.rows - new_height) / 2, new_width, new_height);
  cv::Mat rected = bridge_.image(rect);
  sensor_msgs::ImagePtr msg = cv_bridge::CvImage(bridge_.header, bridge_.encoding, rected).toImageMsg();
  info_.width = new_width;
  info_.height = new_height;
  
  rectpub_.publish(*msg, info_);
  info_.width = bridge_.image.cols;
  info_.height = bridge_.image.rows;
}

bool Capture::setPropertyFromParam(int property_id, const std::string &param_name)
{
  if (cap_.isOpened())
  {
    double value = 0.0;
    if (node_.getParam(param_name, value))
    {
      ROS_INFO("setting property %s = %lf", param_name.c_str(), value);
      return cap_.set(property_id, value);
    }
  }
  return true;
}

} // namespace cv_camera
