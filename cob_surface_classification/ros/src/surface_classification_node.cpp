/*!
 *****************************************************************
 * \file
 *
 * \note
 * Copyright (c) 2013 \n+
 * Fraunhofer Institute for Manufacturing Engineering
 * and Automation (IPA) \n\n
 *
 *****************************************************************
 *
 * \note
 * Project name: Care-O-bot
 * \note
 * ROS stack name: cob_object_perception
 * \note
 * ROS package name: cob_surface_classification
 *
 * \author
 * Author: Richard Bormann
 * \author
 * Supervised by:
 *
 * \date Date of creation: 07.08.2012
 *
 * \brief
 * functions for display of people detections
 *
 *****************************************************************
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer. \n
 * - Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution. \n
 * - Neither the name of the Fraunhofer Institute for Manufacturing
 * Engineering and Automation (IPA) nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission. \n
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License LGPL as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License LGPL for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License LGPL along with this program.
 * If not, see <http://www.gnu.org/licenses/>.
 *
 ****************************************************************/

/*switches for execution of processing steps*/

#define RECORD_MODE					false		//save color image and cloud for usage in EVALUATION_OFFLINE_MODE
#define COMPUTATION_MODE			true		//computations without record
#define EVALUATION_OFFLINE_MODE		false		//evaluation of stored pointcloud and image
#define EVALUATION_ONLINE_MODE		false		//computations plus evaluation of current computations plus record of evaluation

//steps in computation/evaluation_online mode:

#define SEG 						false 	//segmentation + refinement
#define SEG_WITHOUT_EDGES 			false 	//segmentation without considering edge image (wie Steffen)
#define SEG_REFINE					false 	//segmentation refinement according to curvatures (outdated)
#define CLASSIFY 					false	//classification


#define NORMAL_VIS 					false 	//visualisation of normals
#define SEG_VIS 					false 	//visualisation of segmentation
#define SEG_WITHOUT_EDGES_VIS 		false 	//visualisation of segmentation without edge image
#define CLASS_VIS 					false 	//visualisation of classification




// ROS includes
#include <ros/ros.h>

// ROS message includes
#include <sensor_msgs/Image.h>
#include <sensor_msgs/PointCloud2.h>

// topics
#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <image_transport/image_transport.h>
#include <image_transport/subscriber_filter.h>

// opencv
#include <opencv/cv.h>
#include <opencv/highgui.h>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/image_encodings.h>

// boost
#include <boost/bind.hpp>

// point cloud
#include <pcl/point_types.h>
#include <pcl_ros/point_cloud.h>
#include <pcl/visualization/cloud_viewer.h>
#include <pcl/visualization/pcl_visualizer.h>
#include <pcl/io/io.h>
#include <pcl/io/pcd_io.h>


//internal includes
#include <cob_surface_classification/edge_detection.h>
//#include <cob_surface_classification/surface_classification.h>
#include <cob_surface_classification/organized_normal_estimation.h>
#include <cob_surface_classification/refine_segmentation.h>

//package includes
#include <cob_3d_segmentation/depth_segmentation.h>
#include <cob_3d_segmentation/cluster_classifier.h>
#include <cob_3d_mapping_common/point_types.h>


//records
#include "scene_recording.h"
//evaluation
#include "Evaluation.h"



class SurfaceClassificationNode
{
public:
	typedef cob_3d_segmentation::PredefinedSegmentationTypes ST;

	SurfaceClassificationNode(ros::NodeHandle nh)
	: node_handle_(nh)
	{
		it_ = 0;
		sync_input_ = 0;

		it_ = new image_transport::ImageTransport(node_handle_);
		colorimage_sub_.subscribe(*it_, "colorimage_in", 1);
		pointcloud_sub_.subscribe(node_handle_, "pointcloud_in", 1);

		sync_input_ = new message_filters::Synchronizer<message_filters::sync_policies::ApproximateTime<sensor_msgs::Image, sensor_msgs::PointCloud2> >(30);
		sync_input_->connectInput(colorimage_sub_, pointcloud_sub_);
		sync_input_->registerCallback(boost::bind(&SurfaceClassificationNode::inputCallback, this, _1, _2));
	}

	~SurfaceClassificationNode()
	{
		if (it_ != 0)
			delete it_;
		if (sync_input_ != 0)
			delete sync_input_;
	}


	// Converts a color image message to cv::Mat format.
	void convertColorImageMessageToMat(const sensor_msgs::Image::ConstPtr& image_msg, cv_bridge::CvImageConstPtr& image_ptr, cv::Mat& image)
	{
		try
		{
			image_ptr = cv_bridge::toCvShare(image_msg, sensor_msgs::image_encodings::BGR8);
		}
		catch (cv_bridge::Exception& e)
		{
			ROS_ERROR("PeopleDetection: cv_bridge exception: %s", e.what());
		}
		image = image_ptr->image;
	}

	// from https://gist.github.com/volkansalma/2972237
	//  or  http://lists.apple.com/archives/perfoptimization-dev/2005/Jan/msg00051.html
	#define PI_FLOAT     3.14159265f
	#define PIBY2_FLOAT  1.5707963f
	// |error| < 0.005
	float fast_atan2f_1(float y, float x)
	{
		if (x == 0.0f)
		{
			if (y > 0.0f) return PIBY2_FLOAT;
			if (y == 0.0f) return 0.0f;
			return -PIBY2_FLOAT;
		}
		float atan;
		float z = y/x;
		if (fabsf(z) < 1.0f)
		{
			atan = z/(1.0f + 0.28f*z*z);
			if (x < 0.0f)
			{
				if (y < 0.0f) return atan - PI_FLOAT;
				return atan + PI_FLOAT;
			}
		}
		else
		{
			atan = PIBY2_FLOAT - z/(z*z + 0.28f);
			if ( y < 0.0f ) return atan - PI_FLOAT;
		}
		return atan;
	}

	float fast_atan2f_2(float y, float x)
	{
		//http://pubs.opengroup.org/onlinepubs/009695399/functions/atan2.html
		//Volkan SALMA

		const float ONEQTR_PI = M_PI / 4.0;
		const float THRQTR_PI = 3.0 * M_PI / 4.0;
		float r, angle;
		float abs_y = fabs(y) + 1e-10f; // kludge to prevent 0/0 condition
		if (x < 0.0f)
		{
			r = (x + abs_y) / (abs_y - x);
			angle = THRQTR_PI;
		}
		else
		{
			r = (x - abs_y) / (x + abs_y);
			angle = ONEQTR_PI;
		}
		angle += (0.1963f * r * r - 0.9817f) * r;
		if (y < 0.0f)
			return (-angle); // negate if in quad III or IV
		else
			return (angle);
	}

	float fast_arccosf(float x)
	{
		float x2 = x*x;
		float x4 = x2*x2;
		return (CV_PI/2.0 - (x + 1./6.*x*x2 + 3./40.*x*x4));
	}

	void inputCallback(const sensor_msgs::Image::ConstPtr& color_image_msg, const sensor_msgs::PointCloud2::ConstPtr& pointcloud_msg)
	{

		ROS_INFO("Input Callback");

		// convert color image to cv::Mat
		cv_bridge::CvImageConstPtr color_image_ptr;
		cv::Mat color_image;
		convertColorImageMessageToMat(color_image_msg, color_image_ptr, color_image);

		pcl::PointCloud<pcl::PointXYZRGB>::Ptr cloud (new pcl::PointCloud<pcl::PointXYZRGB>);
		pcl::fromROSMsg(*pointcloud_msg, *cloud);
		if(cloud->height == 1 && cloud->points.size() == 307200)
		{
			cloud->height = 480;
			cloud->width = 640;
		}

		//compute depth_image: greyvalue represents depth z
		Timer tim;
		tim.start();
		cv::Mat x_image = cv::Mat::zeros(cloud->height, cloud->width, CV_32FC1);
		cv::Mat y_image = cv::Mat::zeros(cloud->height, cloud->width, CV_32FC1);
		cv::Mat z_image = cv::Mat::zeros(cloud->height, cloud->width, CV_32FC1);
		int i=0;
		for (unsigned int v=0; v<cloud->height; v++)
		{
			for (unsigned int u=0; u<cloud->width; u++, i++)
			{
				//matrix indices: row y, column x!
				pcl::PointXYZRGB& point = cloud->at(u,v);
				if(point.z == point.z)	//test nan
				{
					x_image.at<float>(v,u) = point.x;
					y_image.at<float>(v,u) = point.y;
					z_image.at<float>(v,u) = point.z;
				}
			}
		}
		std::cout << "Time for x/y/z images: " << tim.getElapsedTimeInMilliSec() << "\n";

		//visualization
		cv::Mat depth_im_scaled;
		cv::normalize(z_image, depth_im_scaled,0,1,cv::NORM_MINMAX);
		cv::imshow("depth_image", depth_im_scaled);
		cv::waitKey(10);

		//draw crossline
		//int lineLength = 20;
		//cv::line(color_image,cv::Point2f(2*depth_image.cols/3 -lineLength/2, 2*depth_image.rows/3),cv::Point2f(2*depth_image.cols/3 +lineLength/2, 2*depth_image.rows/3),CV_RGB(0,1,0),1);
		//cv::line(color_image,cv::Point2f(2*depth_image.cols/3 , 2*depth_image.rows/3 +lineLength/2),cv::Point2f(2*depth_image.cols/3 , 2*depth_image.rows/3 -lineLength/2),CV_RGB(0,1,0),1);



		//record scene
		//----------------------------------------
		if(RECORD_MODE)
		{
			cv::Mat im_flipped;
			cv::flip(color_image, im_flipped,-1);
			cv::imshow("image", im_flipped);
			int key = cv::waitKey(50);
			//record if "r" is pressed while "image"-window is activated
			if(key == 1048690)
			{
				rec_.saveImage(im_flipped,"color");
				rec_.saveCloud(cloud,"cloud");
			}

		}

		//----------------------------------------

		int key = 0;
		cv::imshow("image", color_image);
		if(!EVALUATION_ONLINE_MODE)
			cv::waitKey(10);
		//if(EVALUATION_ONLINE_MODE){ key = cv::waitKey(50);}

		tim.start();
		cv::Mat x_dx, y_dy, z_dx, z_dy;
		//cv::Mat average_dz_right = cv::Mat::zeros(z_image.rows, z_image.cols, CV_32FC1);
		//cv::Mat average_dz_left = cv::Mat::zeros(z_image.rows, z_image.cols, CV_32FC1);
		//cv::Mat average_dx_right = cv::Mat::zeros(z_image.rows, z_image.cols, CV_32FC1);
		//cv::Mat average_dx_left = cv::Mat::zeros(z_image.rows, z_image.cols, CV_32FC1);
		cv::Mat edge = cv::Mat::zeros(z_image.rows, z_image.cols, CV_8UC1);
		//cv::medianBlur(z_image, z_image, 5);
		cv::Sobel(x_image, x_dx, -1, 1, 0, 5, 1./(6.*16.));
		cv::Sobel(x_image, y_dy, -1, 0, 1, 5, 1./(6.*16.));
		cv::Sobel(z_image, z_dx, -1, 1, 0, 5, 1./(6.*16.));
		cv::Sobel(z_image, z_dy, -1, 0, 1, 5, 1./(6.*16.));
		std::cout << "Time for slope Sobel: " << tim.getElapsedTimeInMilliSec() << "\n";
		tim.start();
//		cv::Mat kx, ky;
//		cv::getDerivKernels(kx, ky, 1, 0, 5, false, CV_32F);
//		std::cout << "kx:\n";
//		for (int i=0; i<kx.rows; ++i)
//		{
//			for (int j=0; j<kx.cols; ++j)
//				std::cout << kx.at<float>(i,j) << "\t";
//			std::cout << std::endl;
//		}
//		std::cout << "\nky:\n";
//		for (int i=0; i<ky.rows; ++i)
//		{
//			for (int j=0; j<ky.cols; ++j)
//				std::cout << ky.at<float>(i,j) << "\t";
//			std::cout << std::endl;
//		}
		int max_line_width = 30;
		int line_width = 10; // 1px/0.10m
		int last_line_width = 10;
		for (int v = max_line_width; v < z_dx.rows - max_line_width - 1; ++v)
		{
			for (int u = max_line_width; u < z_dx.cols - max_line_width - 1; ++u)
			{
				if (z_dx.at<float>(v, u) <= -0.05 || z_dx.at<float>(v, u) >= 0.05)
					edge.at<uchar>(v, u) = 255;
				else
				{
					// depth dependent scan line width for slope computation (1px/0.10m)
					line_width = std::min(int(10 * z_image.at<float>(v, u)), max_line_width);
					if (line_width == 0)
						line_width = last_line_width;
					else
						last_line_width = line_width;

					double avg_dz_l = 0., avg_dx_l = 0.;
//					int number_values = 0;
					for (int i = -line_width; i < 0; ++i)
					{
						float x_val = x_dx.at<float>(v, u + i);
						float z_val = z_dx.at<float>(v, u + i);
						if (x_val > 0. && z_val > -0.05f && z_val < 0.05f)
						{
							avg_dz_l += z_val;
							avg_dx_l += x_val;
//							++number_values;
						}
						// else jump edge
					}
					//std::cout << avg_slope/(double)number_values << "\t";
//					if (number_values > 0)
//					{
//						//average_dz_left.at<float>(v, u) = avg_dz / (double)number_values;
//						//average_dx_left.at<float>(v, u) = avg_dx / (double)number_values;
//						avg_dz_l /= (double)number_values;
//						avg_dx_l /= (double)number_values;
//					}

					double avg_dz_r = 0.;
					double avg_dx_r = 0.;
//					number_values = 0;
					for (int i = 1; i <= line_width; ++i)
					{
						float x_val = x_dx.at<float>(v, u + i);
						float z_val = z_dx.at<float>(v, u + i);
						if (x_val > 0. && z_val > -0.05f && z_val < 0.05f)
						{
							avg_dz_r += z_val;
							avg_dx_r += x_val;
//							++number_values;
						}
						// else jump edge
					}
					//std::cout << avg_slope/(double)number_values << "\t";
//					if (number_values > 0)
//					{
//						//average_dz_right.at<float>(v, u) = avg_dz / (double)number_values;
//						//average_dx_right.at<float>(v, u) = avg_dx / (double)number_values;
//						avg_dz_r /= (double)number_values;
//						avg_dx_r /= (double)number_values;
//					}

					double alpha_left = fast_atan2f_1(-avg_dz_l, -avg_dx_l);
					double alpha_right = fast_atan2f_1(avg_dz_r, avg_dx_r);
					double diff = fabs(alpha_left - alpha_right);
					if (diff < 145. / 180. * CV_PI || diff > 215. / 180. * CV_PI)
						edge.at<uchar>(v, u) = 128;//64 + 64 * 2 * fabs(CV_PI - fabs(alpha_left - alpha_right)) / CV_PI;
				}
			}
		}
		std::cout << "Time for slope: " << tim.getElapsedTimeInMilliSec() << std::endl;
//		tim.start();
//		for (int v = max_line_width; v < average_dz_right.rows - max_line_width - 1; ++v)
//		{
//			for (int u = max_line_width; u < average_dz_right.cols - max_line_width - 1; ++u)
//			{
//				if (edge.at<uchar>(v, u) != 255)
//				{
//					//double alpha_left_acc = atan2(-average_dz_left.at<float>(v, u), -average_dx_left.at<float>(v, u));
//					//double alpha_right_acc = atan2(average_dz_right.at<float>(v, u), average_dx_right.at<float>(v, u));
//					double alpha_left = fast_atan2f_1(-average_dz_left.at<float>(v, u), -average_dx_left.at<float>(v, u));
//					double alpha_right = fast_atan2f_1(average_dz_right.at<float>(v, u), average_dx_right.at<float>(v, u));
//					double diff = fabs(alpha_left - alpha_right);
//					//if (fabs(alpha_left-alpha_left_acc)>0.011 || fabs(alpha_right-alpha_right_acc)>0.011)
//					//	std::cout << "Large error: " << alpha_left-alpha_left_acc  << "   " << alpha_right-alpha_right_acc << "\n";
//					if (diff < 145. / 180. * CV_PI || diff > 215. / 180. * CV_PI)
//						edge.at<uchar>(v, u) = 128;//64 + 64 * 2 * fabs(CV_PI - fabs(alpha_left - alpha_right)) / CV_PI;
//					//if (v==240 && u>310 && u<330)
//					//	std::cout << "al: " << alpha_left << " ar: " << alpha_right << " diff: " << fabs(alpha_left-alpha_right) << "<" << 160./180.*CV_PI << "?\t";
//				}
//			}
//		}
//		std::cout << "Time for edge image: " << tim.getElapsedTimeInMilliSec() << "\n";

		cv::imshow("z_dx", z_dx);
		cv::normalize(x_dx, x_dx, 0., 1., cv::NORM_MINMAX);
		cv::imshow("x_dx", x_dx);
		//cv::normalize(average_slope, average_slope, 0., 1., cv::NORM_MINMAX);
//		average_dz_right = average_dz_right * 15 + 0.5;
//		cv::imshow("average_slope", average_dz_right);
		cv::imshow("edge", edge);
		cv::waitKey(10);
		return;

//		cv::Mat dx2, dy2, dxy2;
//		cv::medianBlur(z_image, z_image, 3);
//		cv::Sobel(z_image, dx2, -1, 1, 0, 5);
//		cv::Sobel(z_image, dy2 -1, 0, 1, 5);
//		cv::magnitude(dx2, dy2, dxy2);
//		cv::normalize(dxy2, dxy2, 0.0, 40.0, cv::NORM_MINMAX);
//		cv::imshow("dxy2", dxy2);
//		cv::normalize(dx2, dx2, -39.0, 40.0, cv::NORM_MINMAX);
//		cv::imshow("dx", dx2);
//		cv::normalize(dy2, dy2, -39.0, 40.0, cv::NORM_MINMAX);
//		cv::imshow("dy", dy2);
//		cv::waitKey(10);
		return;


		//std::cout << key <<endl;
		//record if "e" is pressed while "image"-window is activated
		if(COMPUTATION_MODE || (EVALUATION_ONLINE_MODE && key == 1048677))
		{
			pcl::PointCloud<pcl::Normal>::Ptr normals(new pcl::PointCloud<pcl::Normal>);
			pcl::PointCloud<pcl::Normal>::Ptr normalsWithoutEdges(new pcl::PointCloud<pcl::Normal>);
			pcl::PointCloud<PointLabel>::Ptr labels(new pcl::PointCloud<PointLabel>);
			pcl::PointCloud<PointLabel>::Ptr labelsWithoutEdges(new pcl::PointCloud<PointLabel>);
			ST::Graph::Ptr graph(new ST::Graph);
			ST::Graph::Ptr graphWithoutEdges(new ST::Graph);


//		oneWithoutEdges_.setInputCloud(cloud);
//		oneWithoutEdges_.setPixelSearchRadius(8,1,1);
//		oneWithoutEdges_.setOutputLabels(labelsWithoutEdges);
//		oneWithoutEdges_.setSkipDistantPointThreshold(8);	//PUnkte mit einem Abstand in der Tiefe von 8 werden nicht mehr zur Nachbarschaft gezählt
//		oneWithoutEdges_.compute(*normalsWithoutEdges);

			cv::Mat edgeImage = cv::Mat::ones(z_image.rows,z_image.cols,CV_32FC1);
			edge_detection_.computeDepthEdges(z_image, cloud, edgeImage);

			//edge_detection_.sobelLaplace(color_image,depth_image);

			//cv::imshow("edge_image", edgeImage);
			//cv::waitKey(10);

			//Timer timer;
			//timer.start();
			//for(int i=0; i<10; i++)
			//{
/*bring it back
			one_.setInputCloud(cloud);
			one_.setPixelSearchRadius(8,1,1);	//call before calling computeMaskManually()!!!
			one_.computeMaskManually_increasing(cloud->width);
			one_.setEdgeImage(edgeImage);
			one_.setOutputLabels(labels);
			one_.setSameDirectionThres(0.94);
			one_.setSkipDistantPointThreshold(8);	//don't consider points in neighbourhood with depth distance larger than 8
			one_.compute(*normals);
*/
			//}timer.stop();
			//std::cout << timer.getElapsedTimeInMilliSec() << " ms for normalEstimation on the whole image, averaged over 10 iterations\n";



			if(NORMAL_VIS)
			{
				// visualize normals
				pcl::visualization::PCLVisualizer viewerNormals("Cloud and Normals");
				viewerNormals.setBackgroundColor (0.0, 0.0, 0);
				pcl::visualization::PointCloudColorHandlerRGBField<pcl::PointXYZRGB> rgbNormals(cloud);

				viewerNormals.addPointCloud<pcl::PointXYZRGB> (cloud, rgbNormals, "cloud");
				viewerNormals.addPointCloudNormals<pcl::PointXYZRGB,pcl::Normal>(cloud, normals,2,0.005,"normals");
				viewerNormals.setPointCloudRenderingProperties (pcl::visualization::PCL_VISUALIZER_POINT_SIZE, 3, "cloud");

				while (!viewerNormals.wasStopped ())
				{
					viewerNormals.spinOnce();
				}
				viewerNormals.removePointCloud("cloud");
			}

			if(SEG || EVALUATION_ONLINE_MODE)
			{
				seg_.setInputCloud(cloud);
				seg_.setNormalCloudIn(normals);
				seg_.setLabelCloudInOut(labels);
				seg_.setClusterGraphOut(graph);
				seg_.performInitialSegmentation();
				seg_.refineSegmentation();
			}
			if(SEG_WITHOUT_EDGES)
			{
				segWithoutEdges_.setInputCloud(cloud);
				segWithoutEdges_.setNormalCloudIn(normalsWithoutEdges);
				segWithoutEdges_.setLabelCloudInOut(labelsWithoutEdges);
				segWithoutEdges_.setClusterGraphOut(graphWithoutEdges);
				segWithoutEdges_.performInitialSegmentation();
			}

			if(SEG_VIS)
			{
				pcl::PointCloud<pcl::PointXYZRGB>::Ptr segmented(new pcl::PointCloud<pcl::PointXYZRGB>);
				*segmented = *cloud;
				graph->clusters()->mapClusterColor(segmented);

				// visualize segmentation
				pcl::visualization::PCLVisualizer viewer("segmentation");
				viewer.setBackgroundColor (0.0, 0.0, 0);
				pcl::visualization::PointCloudColorHandlerRGBField<pcl::PointXYZRGB> rgb(segmented);
				viewer.addPointCloud<pcl::PointXYZRGB> (segmented,rgb,"seg");
				while (!viewer.wasStopped ())
				{
					viewer.spinOnce();
				}
				viewer.removePointCloud("seg");
			}
			if(SEG_WITHOUT_EDGES_VIS)
			{
				pcl::PointCloud<pcl::PointXYZRGB>::Ptr segmentedWithoutEdges(new pcl::PointCloud<pcl::PointXYZRGB>);
				pcl::copyPointCloud<pcl::PointXYZRGB,pcl::PointXYZRGB>(*cloud, *segmentedWithoutEdges);
				graphWithoutEdges->clusters()->mapClusterColor(segmentedWithoutEdges);

				pcl::visualization::PCLVisualizer viewerWithoutEdges("segmentationWithoutEdges");

				viewerWithoutEdges.setBackgroundColor (0.0, 0.0, 0);
				pcl::visualization::PointCloudColorHandlerRGBField<pcl::PointXYZRGB> rgbWithoutEdges(segmentedWithoutEdges);
				viewerWithoutEdges.addPointCloud<pcl::PointXYZRGB> (segmentedWithoutEdges,rgbWithoutEdges,"segWithoutEdges");
				while (!viewerWithoutEdges.wasStopped ())
				{
					viewerWithoutEdges.spinOnce();
				}
			}
			if(SEG_REFINE)
			{
				//merge segments with similar curvature characteristics
				segRefined_.setInputCloud(cloud);
				segRefined_.setClusterGraphInOut(graph);
				segRefined_.setLabelCloudInOut(labels);
				segRefined_.setNormalCloudIn(normals);
				//segRefined_.setCurvThres()
				segRefined_.refineUsingCurvature();
				//segRefined_.printCurvature(color_image);

				pcl::PointCloud<pcl::PointXYZRGB>::Ptr segmentedRef(new pcl::PointCloud<pcl::PointXYZRGB>);
				*segmentedRef = *cloud;
				graph->clusters()->mapClusterColor(segmentedRef);

				// visualize refined segmentation
				pcl::visualization::PCLVisualizer viewerRef("segmentationRef");
				viewerRef.setBackgroundColor (0.0, 0.0, 0);
				pcl::visualization::PointCloudColorHandlerRGBField<pcl::PointXYZRGB> rgbRef(segmentedRef);
				viewerRef.addPointCloud<pcl::PointXYZRGB> (segmentedRef,rgbRef,"segRef");

				while (!viewerRef.wasStopped ())
				{
					viewerRef.spinOnce();
				}
				viewerRef.removePointCloud("segRef");
			}



			if(CLASSIFY|| EVALUATION_ONLINE_MODE)
			{
				//classification

				cc_.setClusterHandler(graph->clusters());
				cc_.setNormalCloudInOut(normals);
				cc_.setLabelCloudIn(labels);
				cc_.setPointCloudIn(cloud);
				//cc_.setMaskSizeSmooth(14);
				cc_.classify();
			}
			if(CLASS_VIS)
			{

				pcl::PointCloud<pcl::PointXYZRGB>::Ptr classified(new pcl::PointCloud<pcl::PointXYZRGB>);
				*classified = *cloud;
				graph->clusters()->mapTypeColor(classified);
				graph->clusters()->mapClusterBorders(classified);

				// visualize classification
				pcl::visualization::PCLVisualizer viewerClass("classification");
				viewerClass.setBackgroundColor (0.0, 0.0, 0);
				pcl::visualization::PointCloudColorHandlerRGBField<pcl::PointXYZRGB> rgbClass(classified);
				viewerClass.addPointCloud<pcl::PointXYZRGB> (classified,rgbClass,"class");

				while (!viewerClass.wasStopped ())
				{
					viewerClass.spinOnce();
				}
				viewerClass.removePointCloud("class");
			}

			if(EVALUATION_ONLINE_MODE)
			{
				eval_.setClusterHandler(graph->clusters());
				eval_.compareClassification(cloud,color_image);
			}


		}
//		if(EVALUATION_OFFLINE_MODE)
//		{
//			TODO
//			std::string gt_filename = ...; //path to ground truth cloud
//			eval_.compareClassification(gt_filename);
//		}

	}//inputCallback()


private:
	ros::NodeHandle node_handle_;

	// messages
	image_transport::ImageTransport* it_;
	image_transport::SubscriberFilter colorimage_sub_; ///< Color camera image topic
	message_filters::Subscriber<sensor_msgs::PointCloud2> pointcloud_sub_;
	message_filters::Synchronizer<message_filters::sync_policies::ApproximateTime<sensor_msgs::Image, sensor_msgs::PointCloud2> >* sync_input_;

	//records
	Scene_recording rec_;

	cob_features::OrganizedNormalEstimation<pcl::PointXYZRGB, pcl::Normal, PointLabel> one_;
	cob_features::OrganizedNormalEstimation<pcl::PointXYZRGB, pcl::Normal, PointLabel> oneWithoutEdges_;

	EdgeDetection<pcl::PointXYZRGB> edge_detection_;
	cob_3d_segmentation::DepthSegmentation<ST::Graph, ST::Point, ST::Normal, ST::Label> seg_;
	cob_3d_segmentation::RefineSegmentation<ST::Graph, ST::Point, ST::Normal, ST::Label> segRefined_;

	cob_3d_segmentation::DepthSegmentation<ST::Graph, ST::Point, ST::Normal, ST::Label> segWithoutEdges_;

	cob_3d_segmentation::ClusterClassifier<ST::CH, ST::Point, ST::Normal, ST::Label> cc_;

	//evaluation
	Evaluation eval_;

};

int main (int argc, char** argv)
{
	// Initialize ROS, specify name of node
	ros::init(argc, argv, "cob_surface_classification");

	// Create a handle for this node, initialize node
	ros::NodeHandle nh;

	// Create and initialize an instance of CameraDriver
	SurfaceClassificationNode surfaceClassification(nh);

	ros::spin();

	return (0);
}
