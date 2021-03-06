/*
 * Copyright (C) 2016, Australian Centre for Robotic Vision, ACRV
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Osnabrück University nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 *  Created on: 11.07.2016
 *
 *      Authors:
 *         Trung T. Pham <trung.pham@adelaide.edu.au>
 *         Markus Eich <markus.eich@qut.edu.au>
 *
 *
 */

#include <pcl/filters/filter.h>
#include <pcl_conversions/pcl_conversions.h>
#include <ros/ros.h>
#include <sensor_msgs/PointCloud2.h>
#include <string>

#include <segmentation/segmentation.hpp>
#include <cpf_segmentation_ros/DoSegmentation.h>
#include <cpf_segmentation_ros/DoSegmentationAction.h>
#include <cpf_segmentation_ros/EnablePublisher.h>

#include <actionlib/server/simple_action_server.h>

#include <boost/thread.hpp>

/**
*Node for segmentation of 3D points using segmentation library
*/

using namespace std;
using namespace pcl;
using namespace ros;
using namespace APC;

class SegmentationNode {
   public:
    SegmentationNode(ros::NodeHandle nh)
        : nh_(nh),
          publisherEnabled_(true),
          actionServer_(nh_, "segmentation", false) {
        std::string pc_topic;
        nh_.getParam("pointcloud_topic", pc_topic);

        if (pc_topic.empty()) {
            pc_topic = "/realsense/points_aligned";
        }
        // pub sub
        pcl_sub_ =
            nh_.subscribe(pc_topic, 1, &SegmentationNode::scan_callback, this);
        segmented_pub_ =
            nh_.advertise<sensor_msgs::PointCloud2>("segmented_pointcloud", 1);

        // register services
        doSegmentationSrv_ = nh_.advertiseService(
            "do_segmentation", &SegmentationNode::doSegmentation, this);
        enablePublisherSrv_ = nh_.advertiseService(
            "enable_publisher", &SegmentationNode::enablePublisher, this);

        // register action server
        actionServer_.registerGoalCallback(
            boost::bind(&SegmentationNode::goalCB, this));

        actionServer_.start();

        Config config = segmentation.getConfig();

        // Initialize params from the param server
        nh_.param<float>("voxel_resolution", config.voxel_resolution, 0.003f);
        nh_.param<float>("seed_resolution", config.seed_resolution, 0.05f);

        nh_.param<float>("color_importance", config.color_importance, 1.0f);
        nh_.param<float>("spatial_importance", config.spatial_importance, 0.4f);
        nh_.param<float>("normal_importance", config.normal_importance, 1.0f);

        nh_.param<bool>("use_single_cam_transform",
                        config.use_single_cam_transform, false);
        nh_.param<bool>("use_supervoxel_refinement",
                        config.use_supervoxel_refinement, false);

        nh_.param<bool>("use_random_sampling", config.use_random_sampling,
                        false);
        nh_.param<float>("outlier_cost", config.outlier_cost, 0.02f);
        nh_.param<float>("smooth_cost", config.smooth_cost,
                         config.outlier_cost * 0.01);

        nh_.param<int>("min_inliers_per_plane", config.min_inliers_per_plane,
                       10);
        nh_.param<float>(
            "label_cost", config.label_cost,
            config.min_inliers_per_plane * 0.5 * config.outlier_cost);

        nh_.param<int>("max_num_iterations", config.max_num_iterations, 25);
        nh_.param<float>("max_curvature", config.max_curvature, 0.001f);
        nh_.param<int>("gc_scale", config.gc_scale, 1e4);

        segmentation.setConfig(config);
    }

    /**
    * @brief goalCB callback funtion for the new goal
    */
    void goalCB() {
        actionServer_.acceptNewGoal();
        publisherEnabled_ = false;
        segmentation_thread = new boost::thread(
            boost::bind(&SegmentationNode::doSegmentation, this));
    }

    /**
    * @brief doSegmentation thread for doing the segmentation
    * @param cloud_ptr
    * @return
    */
    void doSegmentation() {
        cpf_segmentation_ros::DoSegmentationResult result;
        pcl::PointCloud<pcl::PointXYZL>::Ptr segmented_pc_ptr;

        std::vector<int> removedIndices;
        pcl::removeNaNFromPointCloud(cloud_, cloud_, removedIndices);

        if (cloud_.size() > 0) {
            segmentation.setPointCloud(cloud_.makeShared());
            segmentation.doSegmentation();
            segmented_pc_ptr = segmentation.getSegmentedPointCloud();

            pcl::PointCloud<PointXYZL> cloud2_;

            pcl::copyPointCloud(*segmented_pc_ptr, cloud2_);
            cloud2_.clear();

            BOOST_FOREACH (pcl::PointXYZL point, *segmented_pc_ptr) {
                if (point.label == 0) continue;
                cloud2_.push_back(point);
            }

            pcl::toROSMsg(cloud2_, result.segmented_cloud);

            actionServer_.setSucceeded(result);
        } else {
            ROS_ERROR("No points in cloud. Aborting..\n");
            actionServer_.setAborted(result);
        }
    }

    /**
    * @brief enablePublisher
    * @param req
    * @param resp
    * @return
    */
    bool enablePublisher(cpf_segmentation_ros::EnablePublisher::Request &req,
                         cpf_segmentation_ros::EnablePublisher::Response &resp) {
        ROS_INFO("Publisher set to %d\n", req.enable);
        publisherEnabled_ = req.enable;
        return true;
    }

    /**
    * @brief doSegmentation service callback for synchronized processing
    * @param req the input point cloud
    * @param res the output point cloud
    * @return true if the service succeeded
    */
    bool doSegmentation(cpf_segmentation_ros::DoSegmentation::Request &req,
                        cpf_segmentation_ros::DoSegmentation::Response &res) {
        sensor_msgs::PointCloud2 seg_msg;
        pcl::PointCloud<pcl::PointXYZL>::Ptr segmented_pc_ptr;

        pcl::fromROSMsg(req.input_cloud, cloud_);

        std::vector<int> removedIndices;
        pcl::removeNaNFromPointCloud(cloud_, cloud_, removedIndices);

        ROS_INFO("Got cloud %lu points\n", cloud_.size());

        segmentation.setPointCloud(cloud_.makeShared());

        segmentation.doSegmentation();

        segmented_pc_ptr = segmentation.getSegmentedPointCloud();

        pcl::PointCloud<PointXYZL> cloud2_;
        if (cloud_.size() > 0) {
            pcl::copyPointCloud(*segmented_pc_ptr, cloud2_);
            cloud2_.clear();

            BOOST_FOREACH (pcl::PointXYZL point, *segmented_pc_ptr) {
                if (point.label == 0) continue;
                cloud2_.push_back(point);
            }
            pcl::toROSMsg(cloud2_, res.segmented_cloud);
            res.segmented_cloud.header = req.input_cloud.header;
            segmented_pub_.publish(res.segmented_cloud);
        } else {
            res.segmented_cloud.header = req.input_cloud.header;
        }
        return true;
    }
    /**
    * @brief scan_callback
    * @param cloud_ptr
    */
    void scan_callback(const sensor_msgs::PointCloud2ConstPtr &cloud_ptr) {
        pcl::fromROSMsg(*cloud_ptr, cloud_);

        if (publisherEnabled_ && cloud_.size() > 0) {
            sensor_msgs::PointCloud2 seg_msg;
            pcl::PointCloud<pcl::PointXYZL>::Ptr segmented_pc_ptr;

            std::vector<int> removedIndices;
            pcl::removeNaNFromPointCloud(cloud_, cloud_, removedIndices);

            segmentation.setPointCloud(cloud_.makeShared());

            segmentation.doSegmentation();

            segmented_pc_ptr = segmentation.getSegmentedPointCloud();

            if (cloud_.size() > 0) {
                pcl::PointCloud<PointXYZL> cloud2_;

                pcl::copyPointCloud(*segmented_pc_ptr, cloud2_);
                cloud2_.clear();

                BOOST_FOREACH (pcl::PointXYZL point, *segmented_pc_ptr) {
                    if (point.label == 0) continue;
                    cloud2_.push_back(point);
                }

                pcl::toROSMsg(cloud2_, seg_msg);
                segmented_pub_.publish(seg_msg);
            }
        }
    }

   private:
    ros::NodeHandle nh_;

    // pub sub
    ros::Publisher segmented_pub_;
    ros::Subscriber pcl_sub_;

    // services
    ros::ServiceServer doSegmentationSrv_;
    ros::ServiceServer enablePublisherSrv_;

    // action libs
    actionlib::SimpleActionServer<cpf_segmentation_ros::DoSegmentationAction> actionServer_;

    boost::thread *segmentation_thread;

    pcl::PointCloud<PointT> cloud_;
    Segmentation segmentation;

    // enable/desable publisher
    bool publisherEnabled_;
};

int main(int argc, char **argv) {
    ros::init(argc, argv, "segmentation_node");
    ros::NodeHandle nh("~");
    SegmentationNode node(nh);

    ros::spin();

    ROS_INFO("Terminating node...");

    return 0;
}
