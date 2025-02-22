// Copyright 2023 Autoware Foundation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This source code is derived from the https://github.com/pal-robotics/aruco_ros.
// Here is the license statement.
/*****************************
 Copyright 2011 Rafael Muñoz Salinas. All rights reserved.

 Redistribution and use in source and binary forms, with or without modification, are
 permitted provided that the following conditions are met:

 1. Redistributions of source code must retain the above copyright notice, this list of
 conditions and the following disclaimer.

 2. Redistributions in binary form must reproduce the above copyright notice, this list
 of conditions and the following disclaimer in the documentation and/or other materials
 provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY Rafael Muñoz Salinas ''AS IS'' AND ANY EXPRESS OR IMPLIED
 WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL Rafael Muñoz Salinas OR
 CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 The views and conclusions contained in the software and documentation are those of the
 authors and should not be interpreted as representing official policies, either expressed
 or implied, of Rafael Muñoz Salinas.
 ********************************/

#include "ar_tag_based_localizer.hpp"

#include "localization_util/util_func.hpp"

#include <Eigen/Core>
#include <Eigen/Geometry>
#include <opencv4/opencv2/calib3d.hpp>

#include <cv_bridge/cv_bridge.h>
#include <tf2/LinearMath/Transform.h>

#include <algorithm>
#ifdef ROS_DISTRO_GALACTIC
#include <tf2_eigen/tf2_eigen.h>
#else
#include <tf2_eigen/tf2_eigen.hpp>
#endif

ArTagBasedLocalizer::ArTagBasedLocalizer(const rclcpp::NodeOptions & options)
: Node("ar_tag_based_localizer", options), cam_info_received_(false)
{
}

bool ArTagBasedLocalizer::setup()
{
  /*
    Declare node parameters
  */
  marker_size_ = static_cast<float>(this->declare_parameter<double>("marker_size"));
  target_tag_ids_ = this->declare_parameter<std::vector<std::string>>("target_tag_ids");
  base_covariance_ = this->declare_parameter<std::vector<double>>("base_covariance");
  distance_threshold_squared_ = std::pow(this->declare_parameter<double>("distance_threshold"), 2);
  ekf_time_tolerance_ = this->declare_parameter<double>("ekf_time_tolerance");
  ekf_position_tolerance_ = this->declare_parameter<double>("ekf_position_tolerance");
  std::string detection_mode = this->declare_parameter<std::string>("detection_mode");
  float min_marker_size = static_cast<float>(this->declare_parameter<double>("min_marker_size"));
  if (detection_mode == "DM_NORMAL") {
    detector_.setDetectionMode(aruco::DM_NORMAL, min_marker_size);
  } else if (detection_mode == "DM_FAST") {
    detector_.setDetectionMode(aruco::DM_FAST, min_marker_size);
  } else if (detection_mode == "DM_VIDEO_FAST") {
    detector_.setDetectionMode(aruco::DM_VIDEO_FAST, min_marker_size);
  } else {
    // Error
    RCLCPP_ERROR_STREAM(this->get_logger(), "Invalid detection_mode: " << detection_mode);
    return false;
  }

  /*
    Log parameter info
  */
  RCLCPP_INFO_STREAM(this->get_logger(), "min_marker_size: " << min_marker_size);
  RCLCPP_INFO_STREAM(this->get_logger(), "detection_mode: " << detection_mode);
  RCLCPP_INFO_STREAM(this->get_logger(), "thresMethod: " << detector_.getParameters().thresMethod);
  RCLCPP_INFO_STREAM(this->get_logger(), "marker_size_: " << marker_size_);

  /*
    tf
  */
  tf_buffer_ = std::make_unique<tf2_ros::Buffer>(this->get_clock());
  tf_listener_ = std::make_unique<tf2_ros::TransformListener>(*tf_buffer_);
  tf_broadcaster_ = std::make_unique<tf2_ros::TransformBroadcaster>(this);

  /*
    Initialize image transport
  */
  it_ = std::make_unique<image_transport::ImageTransport>(shared_from_this());

  /*
    Subscribers
  */
  map_bin_sub_ = this->create_subscription<HADMapBin>(
    "~/input/lanelet2_map", rclcpp::QoS(10).durability(rclcpp::DurabilityPolicy::TransientLocal),
    std::bind(&ArTagBasedLocalizer::map_bin_callback, this, std::placeholders::_1));

  rclcpp::QoS qos_sub(rclcpp::QoSInitialization::from_rmw(rmw_qos_profile_default));
  qos_sub.best_effort();
  image_sub_ = this->create_subscription<Image>(
    "~/input/image", qos_sub,
    std::bind(&ArTagBasedLocalizer::image_callback, this, std::placeholders::_1));
  cam_info_sub_ = this->create_subscription<CameraInfo>(
    "~/input/camera_info", qos_sub,
    std::bind(&ArTagBasedLocalizer::cam_info_callback, this, std::placeholders::_1));
  ekf_pose_sub_ = this->create_subscription<PoseWithCovarianceStamped>(
    "~/input/ekf_pose", qos_sub,
    std::bind(&ArTagBasedLocalizer::ekf_pose_callback, this, std::placeholders::_1));

  /*
    Publishers
  */
  rclcpp::QoS qos_marker = rclcpp::QoS(rclcpp::KeepLast(10));
  qos_marker.transient_local();
  qos_marker.reliable();
  marker_pub_ = this->create_publisher<MarkerArray>("~/debug/marker", qos_marker);
  rclcpp::QoS qos_pub(rclcpp::QoSInitialization::from_rmw(rmw_qos_profile_default));
  image_pub_ = it_->advertise("~/debug/result", 1);
  pose_pub_ =
    this->create_publisher<PoseWithCovarianceStamped>("~/output/pose_with_covariance", qos_pub);
  diag_pub_ = this->create_publisher<DiagnosticArray>("/diagnostics", qos_pub);

  RCLCPP_INFO(this->get_logger(), "Setup of ar_tag_based_localizer node is successful!");
  return true;
}

void ArTagBasedLocalizer::map_bin_callback(const HADMapBin::ConstSharedPtr & msg)
{
  landmark_map_ = parse_landmark(msg, "apriltag_16h5", this->get_logger());
  const MarkerArray marker_msg = convert_to_marker_array_msg(landmark_map_);
  marker_pub_->publish(marker_msg);
}

void ArTagBasedLocalizer::image_callback(const Image::ConstSharedPtr & msg)
{
  if ((image_pub_.getNumSubscribers() == 0) && (pose_pub_->get_subscription_count() == 0)) {
    RCLCPP_DEBUG(this->get_logger(), "No subscribers, not looking for ArUco markers");
    return;
  }

  if (!cam_info_received_) {
    RCLCPP_DEBUG(this->get_logger(), "No cam_info has been received.");
    return;
  }

  const builtin_interfaces::msg::Time curr_stamp = msg->header.stamp;
  cv_bridge::CvImagePtr cv_ptr;
  try {
    cv_ptr = cv_bridge::toCvCopy(*msg, sensor_msgs::image_encodings::RGB8);
  } catch (cv_bridge::Exception & e) {
    RCLCPP_ERROR(this->get_logger(), "cv_bridge exception: %s", e.what());
    return;
  }

  cv::Mat in_image = cv_ptr->image;

  // detection results will go into "markers"
  std::vector<aruco::Marker> markers;

  // ok, let's detect
  detector_.detect(in_image, markers, cam_param_, marker_size_, false);

  // for each marker, draw info and its boundaries in the image
  for (const aruco::Marker & marker : markers) {
    const tf2::Transform tf_cam_to_marker = aruco_marker_to_tf2(marker);

    TransformStamped tf_cam_to_marker_stamped;
    tf2::toMsg(tf_cam_to_marker, tf_cam_to_marker_stamped.transform);
    tf_cam_to_marker_stamped.header.stamp = curr_stamp;
    tf_cam_to_marker_stamped.header.frame_id = msg->header.frame_id;
    tf_cam_to_marker_stamped.child_frame_id = "detected_marker_" + std::to_string(marker.id);
    tf_broadcaster_->sendTransform(tf_cam_to_marker_stamped);

    PoseStamped pose_cam_to_marker;
    tf2::toMsg(tf_cam_to_marker, pose_cam_to_marker.pose);
    pose_cam_to_marker.header.stamp = curr_stamp;
    pose_cam_to_marker.header.frame_id = msg->header.frame_id;
    publish_pose_as_base_link(pose_cam_to_marker, std::to_string(marker.id));

    // drawing the detected markers
    marker.draw(in_image, cv::Scalar(0, 0, 255), 2);
  }

  // draw a 3d cube in each marker if there is 3d info
  if (cam_param_.isValid()) {
    for (aruco::Marker & marker : markers) {
      aruco::CvDrawingUtils::draw3dAxis(in_image, marker, cam_param_);
    }
  }

  if (image_pub_.getNumSubscribers() > 0) {
    // show input with augmented information
    cv_bridge::CvImage out_msg;
    out_msg.header.stamp = curr_stamp;
    out_msg.encoding = sensor_msgs::image_encodings::RGB8;
    out_msg.image = in_image;
    image_pub_.publish(out_msg.toImageMsg());
  }

  const int detected_tags = static_cast<int>(markers.size());

  diagnostic_msgs::msg::DiagnosticStatus diag_status;

  if (detected_tags > 0) {
    diag_status.level = diagnostic_msgs::msg::DiagnosticStatus::OK;
    diag_status.message = "AR tags detected. The number of tags: " + std::to_string(detected_tags);
  } else {
    diag_status.level = diagnostic_msgs::msg::DiagnosticStatus::WARN;
    diag_status.message = "No AR tags detected.";
  }

  diag_status.name = "localization: " + std::string(this->get_name());
  diag_status.hardware_id = this->get_name();

  diagnostic_msgs::msg::KeyValue key_value;
  key_value.key = "Number of Detected AR Tags";
  key_value.value = std::to_string(detected_tags);
  diag_status.values.push_back(key_value);

  DiagnosticArray diag_msg;
  diag_msg.header.stamp = this->now();
  diag_msg.status.push_back(diag_status);

  diag_pub_->publish(diag_msg);
}

// wait for one camera info, then shut down that subscriber
void ArTagBasedLocalizer::cam_info_callback(const CameraInfo & msg)
{
  if (cam_info_received_) {
    return;
  }

  cv::Mat camera_matrix(3, 4, CV_64FC1, 0.0);
  camera_matrix.at<double>(0, 0) = msg.p[0];
  camera_matrix.at<double>(0, 1) = msg.p[1];
  camera_matrix.at<double>(0, 2) = msg.p[2];
  camera_matrix.at<double>(0, 3) = msg.p[3];
  camera_matrix.at<double>(1, 0) = msg.p[4];
  camera_matrix.at<double>(1, 1) = msg.p[5];
  camera_matrix.at<double>(1, 2) = msg.p[6];
  camera_matrix.at<double>(1, 3) = msg.p[7];
  camera_matrix.at<double>(2, 0) = msg.p[8];
  camera_matrix.at<double>(2, 1) = msg.p[9];
  camera_matrix.at<double>(2, 2) = msg.p[10];
  camera_matrix.at<double>(2, 3) = msg.p[11];

  cv::Mat distortion_coeff(4, 1, CV_64FC1);
  for (int i = 0; i < 4; ++i) {
    distortion_coeff.at<double>(i, 0) = 0;
  }

  const cv::Size size(static_cast<int>(msg.width), static_cast<int>(msg.height));

  cam_param_ = aruco::CameraParameters(camera_matrix, distortion_coeff, size);

  cam_info_received_ = true;
}

void ArTagBasedLocalizer::ekf_pose_callback(const PoseWithCovarianceStamped & msg)
{
  latest_ekf_pose_ = msg;
}

void ArTagBasedLocalizer::publish_pose_as_base_link(
  const PoseStamped & sensor_to_tag, const std::string & tag_id)
{
  // Check tag_id
  if (std::find(target_tag_ids_.begin(), target_tag_ids_.end(), tag_id) == target_tag_ids_.end()) {
    RCLCPP_INFO_STREAM(this->get_logger(), "tag_id(" << tag_id << ") is not in target_tag_ids");
    return;
  }
  if (landmark_map_.count(tag_id) == 0) {
    RCLCPP_INFO_STREAM(this->get_logger(), "tag_id(" << tag_id << ") is not in landmark_map_");
    return;
  }

  // Range filter
  const double distance_squared = sensor_to_tag.pose.position.x * sensor_to_tag.pose.position.x +
                                  sensor_to_tag.pose.position.y * sensor_to_tag.pose.position.y +
                                  sensor_to_tag.pose.position.z * sensor_to_tag.pose.position.z;
  if (distance_threshold_squared_ < distance_squared) {
    return;
  }

  // Transform to base_link
  PoseStamped base_link_to_tag;
  try {
    const TransformStamped transform =
      tf_buffer_->lookupTransform("base_link", sensor_to_tag.header.frame_id, tf2::TimePointZero);
    tf2::doTransform(sensor_to_tag, base_link_to_tag, transform);
    base_link_to_tag.header.frame_id = "base_link";
  } catch (tf2::TransformException & ex) {
    RCLCPP_INFO(this->get_logger(), "Could not transform base_link to camera: %s", ex.what());
    return;
  }

  // (1) map_to_tag
  const Pose & map_to_tag = landmark_map_.at(tag_id);
  const Eigen::Affine3d map_to_tag_affine = pose_to_affine3d(map_to_tag);

  // (2) tag_to_base_link
  const Eigen::Affine3d base_link_to_tag_affine = pose_to_affine3d(base_link_to_tag.pose);
  const Eigen::Affine3d tag_to_base_link_affine = base_link_to_tag_affine.inverse();

  // calculate map_to_base_link
  const Eigen::Affine3d map_to_base_link_affine = map_to_tag_affine * tag_to_base_link_affine;
  const Pose map_to_base_link = tf2::toMsg(map_to_base_link_affine);

  // If latest_ekf_pose_ is older than <ekf_time_tolerance_> seconds compared to current frame, it
  // will not be published.
  const rclcpp::Duration diff_time =
    rclcpp::Time(sensor_to_tag.header.stamp) - rclcpp::Time(latest_ekf_pose_.header.stamp);
  if (diff_time.seconds() > ekf_time_tolerance_) {
    RCLCPP_INFO(
      this->get_logger(),
      "latest_ekf_pose_ is older than %f seconds compared to current frame. "
      "latest_ekf_pose_.header.stamp: %d.%d, sensor_to_tag.header.stamp: %d.%d",
      ekf_time_tolerance_, latest_ekf_pose_.header.stamp.sec, latest_ekf_pose_.header.stamp.nanosec,
      sensor_to_tag.header.stamp.sec, sensor_to_tag.header.stamp.nanosec);
    return;
  }

  // If curr_pose differs from latest_ekf_pose_ by more than <ekf_position_tolerance_>, it will not
  // be published.
  const Pose curr_pose = map_to_base_link;
  const Pose latest_ekf_pose = latest_ekf_pose_.pose.pose;
  const double diff_position = norm(curr_pose.position, latest_ekf_pose.position);
  if (diff_position > ekf_position_tolerance_) {
    RCLCPP_INFO(
      this->get_logger(),
      "curr_pose differs from latest_ekf_pose_ by more than %f m. "
      "curr_pose: (%f, %f, %f), latest_ekf_pose: (%f, %f, %f)",
      ekf_position_tolerance_, curr_pose.position.x, curr_pose.position.y, curr_pose.position.z,
      latest_ekf_pose.position.x, latest_ekf_pose.position.y, latest_ekf_pose.position.z);
    return;
  }

  // Construct output message
  PoseWithCovarianceStamped pose_with_covariance_stamped;
  pose_with_covariance_stamped.header.stamp = sensor_to_tag.header.stamp;
  pose_with_covariance_stamped.header.frame_id = "map";
  pose_with_covariance_stamped.pose.pose = curr_pose;

  // ~5[m]: base_covariance
  // 5~[m]: scaling base_covariance by std::pow(distance/5, 3)
  const double distance = std::sqrt(distance_squared);
  const double scale = distance / 5;
  const double coeff = std::max(1.0, std::pow(scale, 3));
  for (int i = 0; i < 36; i++) {
    pose_with_covariance_stamped.pose.covariance[i] = coeff * base_covariance_[i];
  }

  pose_pub_->publish(pose_with_covariance_stamped);
}

tf2::Transform ArTagBasedLocalizer::aruco_marker_to_tf2(const aruco::Marker & marker)
{
  cv::Mat rot(3, 3, CV_64FC1);
  cv::Mat r_vec64;
  marker.Rvec.convertTo(r_vec64, CV_64FC1);
  cv::Rodrigues(r_vec64, rot);
  cv::Mat tran64;
  marker.Tvec.convertTo(tran64, CV_64FC1);

  tf2::Matrix3x3 tf_rot(
    rot.at<double>(0, 0), rot.at<double>(0, 1), rot.at<double>(0, 2), rot.at<double>(1, 0),
    rot.at<double>(1, 1), rot.at<double>(1, 2), rot.at<double>(2, 0), rot.at<double>(2, 1),
    rot.at<double>(2, 2));

  tf2::Vector3 tf_orig(tran64.at<double>(0, 0), tran64.at<double>(1, 0), tran64.at<double>(2, 0));

  return tf2::Transform(tf_rot, tf_orig);
}
