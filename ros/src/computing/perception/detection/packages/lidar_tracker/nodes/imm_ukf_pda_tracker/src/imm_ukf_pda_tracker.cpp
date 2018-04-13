//
// Created by kosuke on 12/23/17.
//

// #include <ros/ros.h>

// #include <iostream>
// #include <fstream>
// #include <vector>
// #include <cmath>
// #include <math.h>

// #include <limits.h>

// #include <tf/tf.h>
// #include <tf/transform_listener.h>
// #include <tf2/LinearMath/Quaternion.h>

#include "ukf.h"
#include "imm_ukf_pda_tracker.h"


using std::cout;
using std::endl;

namespace States
{
  enum TrackingState: int
  {
     Die = 0,
     Init = 1,
     Stable = 4,
     Lost = 10,

  };
}
typedef States::TrackingState TrackingState;

namespace Matches
{
  enum IsMatch: int
  {
     False = 0,
     True = 1,
  };
}
typedef Matches::IsMatch IsMatch;


//for print out iteration
int g_count = 0;

ImmUkfPda::ImmUkfPda()
{


  ros::NodeHandle private_nh_("~");
  private_nh_.param<bool>("init_", init_, false);
  private_nh_.param<double>("gamma_g_", gamma_g_, 9.22);
  private_nh_.param<double>("p_g_", p_g_, 0.99);
  private_nh_.param<double>("p_d_", p_d_, 0.9);
  private_nh_.param<double>("distance_thres_", distance_thres_, 99);
  private_nh_.param<int>("life_time_thres_", life_time_thres_, 8);
  private_nh_.param<double>("bb_yaw_change_thres_", bb_yaw_change_thres_, 0.3);
  private_nh_.param<double>("dist_from_init_thres_", dist_from_init_thres_, 3.0);
  private_nh_.param<double>("init_yaw_", init_yaw_, 100);
  private_nh_.param<std::string>("input_topic_", input_topic_, "/cloud_clusters");
  private_nh_.param<std::string>("output_topic_", output_topic_, "/tracking_cluster_array");

  tf::TransformListener *lr (new  tf::TransformListener);
  tran_=lr;

  sub_cloud_array_   = node_handle_.subscribe (input_topic_, 1, &ImmUkfPda::callBack, this);
  pub_cloud_array_  = node_handle_.advertise<autoware_msgs::CloudClusterArray> (output_topic_, 1);

}

void ImmUkfPda::callBack(autoware_msgs::CloudClusterArray input)
{
    autoware_msgs::CloudClusterArray output;

    // only transform pose(clusteArray.clusters.bouding_box.pose)
    transformPoseToGlobal(input);
    tracker(input, output);
    transformPoseToLocal(output);
    // output = input;
    pub_cloud_array_.publish(output);
    g_count ++;
    // std::cout << "Frame " <<g_count<< "------------------------------------------------"<<std::endl;
    // std::cout << "target size "<<targets_.size() << std::endl;
}

void ImmUkfPda::transformPoseToGlobal(autoware_msgs::CloudClusterArray& input)
{
    for(size_t i = 0; i < input.clusters.size(); i ++)
    {
        geometry_msgs::PoseStamped pose_in, pose_out;

        pose_in.header = input.header;
        pose_in.pose = input.clusters[i].bounding_box.pose;
        tran_->waitForTransform("/velodyne", "/world",input.header.stamp, ros::Duration(3.0));
        tran_->transformPose("world", ros::Time(0), pose_in, input.header.frame_id, pose_out);
        input.clusters[i].bounding_box.pose = pose_out.pose;
    }
}

void ImmUkfPda::transformPoseToLocal(autoware_msgs::CloudClusterArray& output)
{
    for(size_t i = 0; i < output.clusters.size(); i ++)
    {
        geometry_msgs::PoseStamped pose_in, pose_out;

        pose_in.header = output.header;
        pose_in.header.frame_id = "world";
        pose_in.pose = output.clusters[i].bounding_box.pose;

        tran_->waitForTransform("/world", "/velodyne",ros::Time(0), ros::Duration(3.0));
        tran_->transformPose("velodyne", ros::Time(0), pose_in, "world", pose_out);
        pose_out.header.frame_id = output.header.frame_id = "velodyne";
        output.clusters[i].bounding_box.pose = pose_out.pose;
    }
}


void ImmUkfPda::findMaxZandS(const UKF target, Eigen::VectorXd& max_det_z, Eigen::MatrixXd& max_det_s)
{
    double cv_det   = target.s_cv_.determinant();
    double ctrv_det = target.s_ctrv_.determinant();
    double rm_det   = target.s_rm_.determinant();

    if(cv_det > ctrv_det)
    {
        if(cv_det > rm_det)
        {
            max_det_z = target.z_pred_cv_;
            max_det_s = target.s_cv_;
        }
        else
        {
            max_det_z = target.z_pred_rm_;
            max_det_s = target.s_rm_;
        }
    }
    else{
        if(ctrv_det > rm_det)
        {
            max_det_z = target.z_pred_ctrv_;
            max_det_s = target.s_ctrv_;
        }
        else
        {
            max_det_z = target.z_pred_rm_;
            max_det_s = target.s_rm_;
        }
    }


}

void ImmUkfPda::measurementValidation(const autoware_msgs::CloudClusterArray input, UKF& target,
                                     const bool second_init,
                                     const Eigen::VectorXd max_det_z, const Eigen::MatrixXd max_det_s,
                                     std::vector<autoware_msgs::CloudCluster>& cluster_vec,
                                     std::vector<int>& matching_vec)
{

    int count = 0;
    bool second_init_done = false;
    // double smallest_nis = 999;
    double smallest_nis = std::numeric_limits<double>::max();
    autoware_msgs::CloudCluster smallest_meas_cluster;
    for(size_t i = 0; i < input.clusters.size(); i++)
    {
        double x = input.clusters[i].bounding_box.pose.position.x;
        double y = input.clusters[i].bounding_box.pose.position.y;
        Eigen::VectorXd meas = Eigen::VectorXd(2);
        meas << x, y;

        Eigen::VectorXd diff = meas - max_det_z;
        double nis = diff.transpose()*max_det_s.inverse()*diff;
        // cout <<"nis: " <<nis << endl;
        if(nis < gamma_g_)
        { // x^2 99% range
            count ++;
            if(matching_vec[i] == IsMatch::False) target.lifetime_ ++;

            // pick one meas with smallest nis
            if(second_init)
            {
                if(nis < smallest_nis)
                {
                smallest_nis = nis;
                smallest_meas_cluster = input.clusters[i];
                // measVec.push_back(meas);
                matching_vec[i] = IsMatch::True;
                second_init_done = true;
                }
            }
            else
            {
                // cout << "check cp "<<input.clusters[i].bounding_box.pose.position.x << " "<< input.clusters[i].bounding_box.pose.position.y << endl;
                cluster_vec.push_back(input.clusters[i]);
                matching_vec[i] = IsMatch::True;
            }
        }
    }
    if(second_init_done) cluster_vec.push_back(smallest_meas_cluster);
}

void ImmUkfPda::filterPDA(UKF& target,
                          const std::vector<autoware_msgs::CloudCluster> cluster_vec,
                          std::vector<double>& lambda_vec)
{
    // calculating association probability
    double num_meas = cluster_vec.size();
    double b = 2*num_meas*(1-p_d_*p_g_)/(gamma_g_*p_d_);
    double e_cv_sum   = 0;
    double e_ctrv_sum = 0;
    double e_rm_sum   = 0;

    std::vector<double> e_cv_vec;
    std::vector<double> e_ctrv_vec;
    std::vector<double> e_rm_vec;

    std::vector<Eigen::VectorXd> diff_cv_vec;
    std::vector<Eigen::VectorXd> diff_ctrv_vec;
    std::vector<Eigen::VectorXd> diff_rm_vec;

    for(size_t i = 0; i < num_meas; i++)
    {
        Eigen::VectorXd meas_vec = Eigen::VectorXd(2);
        meas_vec(0) = cluster_vec[i].bounding_box.pose.position.x;
        meas_vec(1) = cluster_vec[i].bounding_box.pose.position.y;

        Eigen::VectorXd diff_cv   = meas_vec - target.z_pred_cv_;
        Eigen::VectorXd diff_ctrv = meas_vec - target.z_pred_ctrv_;
        Eigen::VectorXd diff_rm   = meas_vec - target.z_pred_rm_;

        diff_cv_vec.push_back(diff_cv);
        diff_ctrv_vec.push_back(diff_ctrv);
        diff_rm_vec.push_back(diff_rm);

        double e_cv   = exp(  -0.5*diff_cv.transpose()*  target.s_cv_.inverse()  *diff_cv);
        double e_ctrv = exp(-0.5*diff_ctrv.transpose()*  target.s_ctrv_.inverse()*diff_ctrv);
        double e_rm   = exp(  -0.5*diff_rm.transpose()*  target.s_rm_.inverse()  *diff_rm);

        e_cv_vec.push_back(e_cv);
        e_ctrv_vec.push_back(e_ctrv);
        e_rm_vec.push_back(e_rm);

        e_cv_sum   += e_cv;
        e_ctrv_sum += e_ctrv;
        e_rm_sum   += e_rm;
    }
    double beta_cv_zero   = b/(b+e_cv_sum);
    double beta_ctrv_zero = b/(b+e_ctrv_sum);
    double beta_rm_zero   = b/(b+e_rm_sum);

    std::vector<double> beta_cv;
    std::vector<double> beta_ctrv;
    std::vector<double> beta_rm;


    for(size_t i = 0; i < num_meas; i++)
    {
        double temp_cv   = e_cv_vec[i]/(b+e_cv_sum);
        double temp_ctrv = e_ctrv_vec[i]/(b+e_ctrv_sum);
        double temp_rm   = e_rm_vec[i]/(b+e_rm_sum);

        beta_cv.push_back(temp_cv);
        beta_ctrv.push_back(temp_ctrv);
        beta_rm.push_back(temp_rm);
    }
    Eigen::VectorXd sigma_x_cv;
    Eigen::VectorXd sigma_x_ctrv;
    Eigen::VectorXd sigma_x_rm;
    sigma_x_cv.setZero(2);
    sigma_x_ctrv.setZero(2);
    sigma_x_rm.setZero(2);

    for(size_t i = 0; i < num_meas; i++)
    {
        sigma_x_cv   += beta_cv[i]*diff_cv_vec[i];
        sigma_x_ctrv += beta_ctrv[i]*diff_ctrv_vec[i];
        sigma_x_rm   += beta_rm[i]*diff_rm_vec[i];
    }

    Eigen::MatrixXd sigma_p_cv;
    Eigen::MatrixXd sigma_p_ctrv;
    Eigen::MatrixXd sigma_p_rm;
    sigma_p_cv.setZero(2,2);
    sigma_p_ctrv.setZero(2,2);
    sigma_p_rm.setZero(2,2);
    for(size_t i = 0; i < num_meas; i++)

    {
        sigma_p_cv   += (beta_cv[i]  *diff_cv_vec[i]  *diff_cv_vec[i].transpose()     - sigma_x_cv*sigma_x_cv.transpose());
        sigma_p_ctrv += (beta_ctrv[i]*diff_ctrv_vec[i]*diff_ctrv_vec[i].transpose()   - sigma_x_ctrv*sigma_x_ctrv.transpose());
        sigma_p_rm   += (beta_rm[i]  *diff_rm_vec[i]  *diff_rm_vec[i].transpose()     - sigma_x_rm*sigma_x_rm.transpose());
    }

    // update x and P
    target.x_cv_   = target.x_cv_   + target.k_cv_*sigma_x_cv;
    target.x_ctrv_ = target.x_ctrv_ + target.k_ctrv_*sigma_x_ctrv;
    target.x_rm_   = target.x_rm_   + target.k_rm_*sigma_x_rm;

    while (target.x_cv_(3)> M_PI) target.x_cv_(3) -= 2.*M_PI;
    while (target.x_cv_(3)<-M_PI) target.x_cv_(3) += 2.*M_PI;
    while (target.x_ctrv_(3)> M_PI) target.x_ctrv_(3) -= 2.*M_PI;
    while (target.x_ctrv_(3)<-M_PI) target.x_ctrv_(3) += 2.*M_PI;
    while (target.x_rm_(3)> M_PI) target.x_rm_(3) -= 2.*M_PI;
    while (target.x_rm_(3)<-M_PI) target.x_rm_(3) += 2.*M_PI;

    if(num_meas != 0)
    {
    	target.p_cv_   = beta_cv_zero*target.p_cv_ +
                  (1-beta_cv_zero)*(target.p_cv_ - target.k_cv_*target.s_cv_*target.k_cv_.transpose()) +
                  target.k_cv_*sigma_p_cv*target.k_cv_.transpose();
	    target.p_ctrv_ = beta_ctrv_zero*target.p_ctrv_ +
	                  (1-beta_ctrv_zero)*(target.p_ctrv_ - target.k_ctrv_*target.s_ctrv_*target.k_ctrv_.transpose()) +
	                  target.k_ctrv_*sigma_p_ctrv*target.k_ctrv_.transpose();
	    target.p_rm_   = beta_rm_zero*target.p_rm_ +
	                  (1-beta_rm_zero)*(target.p_rm_ - target.k_rm_*target.s_rm_*target.k_rm_.transpose()) +
	                  target.k_rm_*sigma_p_rm*target.k_rm_.transpose();
    }
    else
    {
    	target.p_cv_   = target.p_cv_   - target.k_cv_  *target.s_cv_  *target.k_cv_.transpose();
	    target.p_ctrv_ = target.p_ctrv_ - target.k_ctrv_*target.s_ctrv_*target.k_ctrv_.transpose();
	    target.p_rm_   = target.p_rm_   - target.k_rm_  *target.s_rm_  *target.k_rm_.transpose();
    }

    Eigen::VectorXd max_det_z;
    Eigen::MatrixXd max_det_s;

    findMaxZandS(target, max_det_z, max_det_s);
    double Vk =  M_PI *sqrt(gamma_g_ * max_det_s.determinant());

    double lambda_cv, lambda_ctrv, lambda_rm;
    if(num_meas != 0)
    {
    	lambda_cv   = (1 - p_g_*p_d_)/pow(Vk, num_meas) +
	                        p_d_*pow(Vk, 1-num_meas)*e_cv_sum/(num_meas*sqrt(2*M_PI*target.s_cv_.determinant()));
	    lambda_ctrv = (1 - p_g_*p_d_)/pow(Vk, num_meas) +
	                        p_d_*pow(Vk, 1-num_meas)*e_ctrv_sum/(num_meas*sqrt(2*M_PI*target.s_ctrv_.determinant()));
	    lambda_rm   = (1 - p_g_*p_d_)/pow(Vk, num_meas) +
	                        p_d_*pow(Vk, 1-num_meas)*e_rm_sum/(num_meas*sqrt(2*M_PI*target.s_rm_.determinant()));
    }
    else
    {
    	lambda_cv   = (1 - p_g_*p_d_)/pow(Vk, num_meas);
	    lambda_ctrv = (1 - p_g_*p_d_)/pow(Vk, num_meas);
	    lambda_rm   = (1 - p_g_*p_d_)/pow(Vk, num_meas);
    }
    // cout <<endl<< "lambda: "<<endl<<lambdaCV << " "<< lambdaCTRV<<" "<< lambdaRM << endl;
    lambda_vec.push_back(lambda_cv);
    lambda_vec.push_back(lambda_ctrv);
    lambda_vec.push_back(lambda_rm);
}

void ImmUkfPda::getNearestEuclidCluster(const UKF target,
                                        const std::vector<autoware_msgs::CloudCluster> cluster_vec,
                                        autoware_msgs::CloudCluster& cluster, double& min_dist){
    int min_ind = 0;
    double px = target.x_merge_(0);
    double py = target.x_merge_(1);
    // cout << "px py"<< px << " "<< py << endl;
    for (size_t i = 0; i < cluster_vec.size(); i++)
    {
        double meas_x = cluster_vec[i].bounding_box.pose.position.x;
        double meas_y = cluster_vec[i].bounding_box.pose.position.y;
        // cout << "measx measY "<< measX << " "<< measY << endl;
        double dist = sqrt((px-meas_x)*(px-meas_x)+(py-meas_y)*(py-meas_y));
        if(dist < min_dist)
        {
            min_dist = dist;
            min_ind = i;
        }
    }
    // assert(clusterVec[minInd].rows() == 2);
    cluster = cluster_vec[min_ind];
}


void ImmUkfPda::associateBB(const std::vector<autoware_msgs::CloudCluster> cluster_vec,
                            UKF& target)
{
    //skip if no validated measurement
    if(cluster_vec.size() == 0)
    {
        return;
    }
    // cout << target.tracking_num_ << " "<< target.lifetime_ << endl;
    if(target.tracking_num_ == TrackingState::Stable && target.lifetime_ >= life_time_thres_)
    {
        // cout << "--------------" << endl;
        autoware_msgs::CloudCluster nearest_cluster;
        // double min_dist = 999;
        double min_dist = std::numeric_limits<double>::max();
        getNearestEuclidCluster(target, cluster_vec, nearest_cluster, min_dist);
        // std::cout << "minDist "<< min_dist << std::endl;
        if(min_dist < distance_thres_)
        {
            target.is_vis_bb_ = true;
            // target.BBox_    = bbox;
            // cout <<"nearestCluster "<< nearestCluster.bounding_box<<endl;
            target.jsk_bb_   = nearest_cluster.bounding_box;
        }
    }
}



double ImmUkfPda::getBboxArea(const pcl::PointCloud<pcl::PointXYZ> bbox)
{
    pcl::PointXYZ p1 = bbox[0];
    pcl::PointXYZ p2 = bbox[1];
    pcl::PointXYZ p3 = bbox[2];
    pcl::PointXYZ p4 = bbox[3];

    //S=tri(p1,p2,p3) + tri(p1, p3, p4)
    //s(triangle) = 1/2*|(x1−x3)(y2−y3)−(x2−x3)(y1−y3)|
    double tri1 = 0.5*abs((p1.x - p3.x)*(p2.y - p3.y) - (p2.x - p3.x)*(p1.y - p3.y));
    double tri2 = 0.5*abs((p1.x - p4.x)*(p3.y - p4.y) - (p3.x - p4.x)*(p1.y - p4.y));
    double S = tri1 + tri2;
    return S;
}




double ImmUkfPda::getJskBBoxYaw(const jsk_recognition_msgs::BoundingBox jsk_bb)
{
    tf::Quaternion q(jsk_bb.pose.orientation.x, jsk_bb.pose.orientation.y,
                    jsk_bb.pose.orientation.z, jsk_bb.pose.orientation.w);
    double roll, pitch, yaw;
    tf::Matrix3x3(q).getRPY(roll, pitch, yaw);
    return yaw;
}

double ImmUkfPda::getJskBBoxArea(const jsk_recognition_msgs::BoundingBox jsk_bb)
{
    double area = jsk_bb.dimensions.x*jsk_bb.dimensions.y;
    return area;
}

void ImmUkfPda::updateBB(UKF& target)
{
    //to do: initialize target.BBox_ somewhere else

    // skip to prevent memory leak by accessing empty target.bbox_

    if(!target.is_vis_bb_)
    {
        return;
    }
    double yaw = getJskBBoxYaw(target.jsk_bb_);

    // skip the rest of process if it is first bbox associaiton
    // todo: wanna check if bestJskBBox is empty or not, there should be better method
    // if(target.bestBBox_.empty()){
    if(target.best_yaw_ == init_yaw_)
    {
        target.best_jsk_bb_ = target.jsk_bb_;
        target.best_yaw_   = yaw;
        return;
    }

    // restricting yaw movement
    // double diff_yaw = yaw - target.best_yaw_;

    // // diffYaw is within the threshold, apply the diffYaw chamge
    // if(abs(diff_yaw) < bb_yaw_change_thres_)
    // {
    //     target.best_jsk_bb_.pose.orientation = target.jsk_bb_.pose.orientation;
    //     target.best_yaw_ = yaw;
    // }
    // else
    // {
    //     target.jsk_bb_.pose.orientation = target.best_jsk_bb_.pose.orientation;
    // }

    // // bbox area
    double area     = getJskBBoxArea(target.jsk_bb_);
    double best_area = getJskBBoxArea(target.best_jsk_bb_);
    // double bestArea = getBboxArea(target.bestBBox_);

    // start updating bbox params
    double delta_area = area - best_area;

    // when the delta area is under 0, keep best area and relocate(slide) it for current cp
    if( delta_area < 0 )
    {
        // updateVisBoxArea(target, dtCP);
        target.jsk_bb_.dimensions = target.best_jsk_bb_.dimensions;
        // for  mergeSegmentation, area comparison
        target.bb_area_ = best_area;
    }
    else if(delta_area > 0)
    {
        // target.bestBBox_ = target.BBox_;
        target.best_jsk_bb_.dimensions = target.jsk_bb_.dimensions;
        // for mergeSegmentation, area comparison
        target.bb_area_  = area;
    }
}

double ImmUkfPda::getIntersectCoef(const double vec1x, const double vec1y, const double vec2x, const double vec2y,
                                  const double px, const double py, const double cpx, const double cpy)
{
    double intersect_coef = (((vec1x-vec2x)*(py - vec1y) + (vec1y - vec2y)*(vec1x - px)) *
        ((vec1x - vec2x)*(cpy - vec1y) + (vec1y - vec2y)*(vec1x - cpx)));
    return intersect_coef;

}

//naive method
// void ImmUkfPda::mergeOverSegmentation(const std::vector<UKF> targets){
//     // cout << "mergeOverSegmentation"<<endl;
//     for(size_t i = 0; i < targets.size(); i++){
//         if(targets[i].is_vis_bb_ == true){
//             double vec1x = targets[i].BBox_[0].x;
//             double vec1y = targets[i].BBox_[0].y;
//             double vec2x = targets[i].BBox_[1].x;
//             double vec2y = targets[i].BBox_[1].y;
//             double vec3x = targets[i].BBox_[2].x;
//             double vec3y = targets[i].BBox_[2].y;
//             double vec4x = targets[i].BBox_[3].x;
//             double vec4y = targets[i].BBox_[3].y;
//             double cp1x  = (vec1x+vec2x+vec3x)/3;
//             double cp1y  = (vec1y+vec2y+vec3y)/3;
//             double cp2x  = (vec1x+vec4x+vec3x)/3;
//             double cp2y  = (vec1y+vec4y+vec3y)/3;
//             for (int j = 0; j < targets.size(); j++){
//                 if(i == j) continue;
//                 // make sure that merged area(i) is larger than compared area(j)
//                 if(targets_[i].bb_area_ < targets_[j].bb_area_){
//                     double px = targets[j].x_merge_(0);
//                     double py = targets[j].x_merge_(1);
//                     // check if px, py are in triangle vec1, vec2, vec3
//                     double cross1 = getIntersectCoef(vec1x, vec1y, vec2x, vec2y, px, py, cp1x, cp1y);
//                     double cross2 = getIntersectCoef(vec1x, vec1y, vec3x, vec3y, px, py, cp1x, cp1y);
//                     double cross3 = getIntersectCoef(vec3x, vec3y, vec2x, vec2y, px, py, cp1x, cp1y);
//                     // check if px, py are in triangle vec1, vec3, vec4
//                     double cross4 = getIntersectCoef(vec1x, vec1y, vec4x, vec4y, px, py, cp2x, cp2y);
//                     double cross5 = getIntersectCoef(vec1x, vec1y, vec3x, vec3y, px, py, cp2x, cp2y);
//                     double cross6 = getIntersectCoef(vec3x, vec3y, vec4x, vec4y, px, py, cp2x, cp2y);
//                     // check if inside or not
//                     if((cross1 > 0 && cross2>0&&cross3>0)||(cross4>0 && cross5 > 0 && cross6>0)){
//                         trackNumVec_[i] = 5;
//                         trackNumVec_[j] = 0;
//                     }
//                 }
//             }
//         }
//     }
// }

void ImmUkfPda::updateLabel(UKF target, autoware_msgs::CloudCluster& cc)
{
    int tracking_num = target.tracking_num_;
    // cout << "trackingnum "<< trackingNum << endl;
    if(target.is_static_)
    {
        cc.label = "Static";
    }
    else if(tracking_num > TrackingState::Die && tracking_num < TrackingState::Stable)
    {
        cc.label = "Initialized";
    }
    else if(tracking_num == TrackingState::Stable)
    {
        cc.label = "Stable";
    }
    else if(tracking_num > TrackingState::Stable && tracking_num <= TrackingState::Lost)
    {
        cc.label = "Lost";
    }
    else
    {
        cc.label = "None";
    }
}

void ImmUkfPda::tracker(autoware_msgs::CloudClusterArray input,
                        autoware_msgs::CloudClusterArray& output)
{

    int in_num_cluster = input.clusters.size();
    double timestamp = input.header.stamp.toSec();

    double det_explode_param = 10;
    double cov_explode_param = 1000;

    if(!init_)
    {
    	for(int i = 0; i < in_num_cluster; i++)
        {
            double px = input.clusters[i].bounding_box.pose.position.x;
            double py = input.clusters[i].bounding_box.pose.position.y;
    		Eigen::VectorXd init_meas = Eigen::VectorXd(2);
    		init_meas << px, py;

            UKF ukf;
            ukf.initialize(init_meas, timestamp);
            targets_.push_back(ukf);

    	}
        timestamp_ = timestamp;
        // egoPreYaw_ = egoYaw_;
        init_ = true;

        // assert(targets_.size() == trackNumVec_.size());
        // assert(targets_.size() == targetPoints.size());
        // assert(targetPoints.size()== targetVandYaw.size());
        return;
    }

    // assert (targets_.size() == trackNumVec_.size());

    // // used for making new target with no data association
    std::vector<int> matching_vec(in_num_cluster); // make 0 vector

    double dt = (timestamp - timestamp_);
    timestamp_ = timestamp;


    // start UKF process
    for(size_t i = 0; i < targets_.size(); i++)
    {
        //reset is_vis_bb_ to false
        targets_[i].is_vis_bb_ = false;

    	//todo: modify here. This skips irregular measurement and nan
        if(targets_[i].tracking_num_ == TrackingState::Die) continue;
        // prevent ukf not to explode
        if(targets_[i].p_merge_.determinant() > det_explode_param || targets_[i].p_merge_(4,4) > cov_explode_param)
        {
            targets_[i].tracking_num_ = TrackingState::Die;
            continue;
        }
        // immukf prediction step
        targets_[i].predictionIMMUKF(dt);


        Eigen::VectorXd max_det_z;
        Eigen::MatrixXd max_det_s;
    	std::vector<autoware_msgs::CloudCluster> cluster_vec;
        // std::vector<Eigen::VectorXd> bboxVec;
    	std::vector<double> lambda_vec;
        // find maxDetS associated with predZ
        findMaxZandS(targets_[i], max_det_z, max_det_s);
        // to do: might modufy here: this code ensures that measurement is incorporated
        max_det_s = max_det_s*4;
        double det_s = max_det_s.determinant();

        // prevent ukf not to explode
        if(std::isnan(det_s)|| det_s > det_explode_param)
        {
            targets_[i].tracking_num_ = TrackingState::Die;
            continue;
        }

        bool second_init;
        if(targets_[i].tracking_num_ == TrackingState::Init)
        {
            second_init = true;
        }
        else
        {
            second_init = false;
        }

        // measurement gating, get measVec, bboxVec, matchingVec through reference
        measurementValidation(input, targets_[i], second_init, max_det_z, max_det_s, cluster_vec,matching_vec);

        // bounding box association if target is stable :plus, right angle correction if its needed
        // input: track number, bbox measurements, &target
        // cout << "cluster vec size "<<clusterVec.size()<<endl;
        associateBB(cluster_vec, targets_[i]);

        // bounding box validation
        // updateBB(targets_[i]);

        // cout << "validated meas "<<measVec[0][0]<<" "<<measVec[0][1]<<endl;

        // second detection for a target: update v and yaw
        if(second_init)
        {
            if(cluster_vec.size() == 0)
            {
                targets_[i].tracking_num_ = TrackingState::Die;
                continue;
            }

            // assert(measVec.size() == 1);
            // record init measurement for env classification
            targets_[i].init_meas_ << targets_[i].x_merge_(0), targets_[i].x_merge_(1);

            // abs update
            double target_x = cluster_vec[0].bounding_box.pose.position.x;
            double target_y = cluster_vec[0].bounding_box.pose.position.y;
            double target_diff_x = target_x - targets_[i].x_merge_(0);
            double target_diff_y = target_y - targets_[i].x_merge_(1);
            double target_yaw = atan2(target_diff_y, target_diff_x);
            double dist      = sqrt(target_diff_x*target_diff_x + target_diff_y* target_diff_y);
            double target_v   = dist/dt;
            // double targetV   = 2;

            while (target_yaw> M_PI) target_yaw -= 2.*M_PI;
            while (target_yaw<-M_PI) target_yaw += 2.*M_PI;

            targets_[i].x_merge_(0) = targets_[i].x_cv_(0) = targets_[i].x_ctrv_(0) = targets_[i].x_rm_(0) = target_x;
            targets_[i].x_merge_(1) = targets_[i].x_cv_(1) = targets_[i].x_ctrv_(1) = targets_[i].x_rm_(1) = target_y;
            targets_[i].x_merge_(2) = targets_[i].x_cv_(2) = targets_[i].x_ctrv_(2) = targets_[i].x_rm_(2) = target_v;
            targets_[i].x_merge_(3) = targets_[i].x_cv_(3) = targets_[i].x_ctrv_(3) = targets_[i].x_rm_(3) = target_yaw;

            targets_[i].tracking_num_++;
            continue;
        }

    	// update tracking number
    	if(cluster_vec.size() > 0)
        {
            if(targets_[i].tracking_num_ < TrackingState::Stable)
            {
    			targets_[i].tracking_num_++;
    		}
            else if(targets_[i].tracking_num_ == TrackingState::Stable)
            {
                targets_[i].tracking_num_ = TrackingState::Stable;
    		}
            else if(targets_[i].tracking_num_ >= TrackingState::Stable && targets_[i].tracking_num_ < TrackingState::Lost)
            {
                targets_[i].tracking_num_ = TrackingState::Stable;
    		}
            else if(targets_[i].tracking_num_ == TrackingState::Lost)
            {
                targets_[i].tracking_num_ = TrackingState::Die;
            }
    	}else{
            if(targets_[i].tracking_num_ < TrackingState::Stable)
            {
                targets_[i].tracking_num_ = TrackingState::Die;
            }
            else if(targets_[i].tracking_num_ >= TrackingState::Stable && targets_[i].tracking_num_ < TrackingState::Lost)
            {
    			targets_[i].tracking_num_++;
    		}
            else if(targets_[i].tracking_num_ == TrackingState::Lost)
            {
                targets_[i].tracking_num_ = TrackingState::Die;
    		}
    	}

        if(targets_[i].tracking_num_ == TrackingState::Die) continue;


	    filterPDA(targets_[i], cluster_vec, lambda_vec);

	    targets_[i].postProcessIMMUKF(lambda_vec);
    }
    // // end UKF process


    // // deling with over segmentation, update trackNumVec_
    // mergeOverSegmentation(targets_);


    // making new ukf target for no data association clusters
    int added_num = 0;
    // size_t target_num = targets_.size();

    for(size_t i = 0; i < matching_vec.size(); i ++)
    {
        if(matching_vec[i] == IsMatch::False)
        {
            double px = input.clusters[i].bounding_box.pose.position.x;
            double py = input.clusters[i].bounding_box.pose.position.y;

            Eigen::VectorXd init_meas = Eigen::VectorXd(2);
            init_meas << px, py;

            UKF ukf;
            ukf.initialize(init_meas, timestamp);
            targets_.push_back(ukf);

            added_num ++;
        }
    }
    // assert(targets_.size() == (added_num + target_num));

    // static dynamic classification
    for (size_t i = 0; i < targets_.size(); i++)
    {
        if(!targets_[i].is_static_ &&
         targets_[i].tracking_num_ == TrackingState::Stable &&
         targets_[i].lifetime_ > life_time_thres_ )
        {
            // double dist_thres = 3.0;
            if((targets_[i].dist_from_init_ < dist_from_init_thres_)&&
                    (targets_[i].mode_prob_rm_ > targets_[i].mode_prob_cv_ ||
                     targets_[i].mode_prob_rm_ > targets_[i].mode_prob_ctrv_ ))
            {
                targets_[i].is_static_    = true;
            }
        }
    }



    // making output(CludClusterArray) for visualization
    tf::StampedTransform transform;
    tran_->lookupTransform("/world", "/velodyne", ros::Time(0), transform);

    // get yaw angle from "world" to "velodyne" for direction(arrow) visualization
    tf::Matrix3x3 m(transform.getRotation());
    double roll, pitch, yaw;
    m.getRPY(roll, pitch, yaw);

    output.header = input.header;
    // int target_num_count = 0;
    for(size_t i = 0; i < targets_.size(); i++)
    {
        if(targets_[i].is_vis_bb_)
        {
            double tx = targets_[i].x_merge_(0);
            double ty = targets_[i].x_merge_(1);
            double mx = targets_[i].init_meas_(0);
            double my = targets_[i].init_meas_(1);

            //for static classification
            targets_[i].dist_from_init_ = sqrt((tx - mx)*(tx - mx) + (ty - my)*(ty - my));
            std::vector<double> cp;
            cp.push_back(tx);
            cp.push_back(ty);

            double tv = targets_[i].x_merge_(2);
            double tyaw = targets_[i].x_merge_(3) - yaw;

            // tyaw += egoPoints_[0][2];
            while (tyaw> M_PI) tyaw -= 2.*M_PI;
            while (tyaw<-M_PI) tyaw += 2.*M_PI;
            // cout << "testing yaw off "<< tyaw << endl;

            // cout << "inside tracker ----------------------------------------------------------------------------------------------"<<targets_[i].jskBB_.pose << endl;

            autoware_msgs::CloudCluster cc;
            cc.header = input.header;
            cc.bounding_box = targets_[i].jsk_bb_;
            cc.bounding_box.header = input.header;
            cc.score           = tv;
            cc.estimated_angle = tyaw;
            updateLabel(targets_[i], cc);
            output.clusters.push_back(cc);
        }

    }
    // cout << output.clusters.size()<< endl;

    assert(matching_vec.size() == input.clusters.size());
}
