/** basic **/
#include <thread>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <tuple>
#include <numeric>
#include "python3.6/Python.h"
/** ros **/
#include <ros/ros.h>
#include <ros/package.h>
#include <rosbag/bag.h>
#include <rosbag/view.h>
#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/PointCloud.h>
#include <sensor_msgs/point_cloud_conversion.h>

/** pcl **/
#include <pcl/common/common.h>
#include <pcl/common/time.h>
#include <pcl_conversions/pcl_conversions.h>

#include <pcl/point_types.h>
#include <pcl/filters/filter.h>
#include <pcl/filters/conditional_removal.h>
#include <pcl/filters/radius_outlier_removal.h>
#include <pcl/filters/uniform_sampling.h>
#include <pcl/filters/extract_indices.h>

#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/common/transforms.h>


#include <pcl/features/normal_3d.h>
#include <pcl/features/normal_3d_omp.h>
// #include <pcl/registration/icp.h>
#include <pcl/registration/gicp.h>
// #include <pcl/registration/ndt.h>
#include <pcl/visualization/pcl_visualizer.h>


#include <Eigen/Core>

/** opencv **/
#include <opencv2/opencv.hpp>


/** headings **/
#include <define.h>


/** namespace **/
using namespace std;

/** typedef **/
typedef pcl::PointXYZI PointI;
typedef pcl::PointXYZINormal PointIN;
typedef pcl::PointXYZRGB PointRGB;
typedef pcl::PointCloud<PointI> CloudI;
typedef pcl::PointCloud<PointIN> CloudIN;
typedef pcl::PointCloud<PointRGB> CloudRGB;
typedef pcl::PointCloud<pcl::Normal> CloudN;
typedef pcl::PointCloud<pcl::PointXYZ> EdgeCloud;
typedef pcl::PointCloud<pcl::PointXYZ> EdgePixels;

typedef std::vector< Eigen::Matrix3d, Eigen::aligned_allocator<Eigen::Matrix3d> > MatricesVector;
typedef boost::shared_ptr< MatricesVector > MatricesVectorPtr;

class LidarProcess{
public:
    /** essential params **/
    string topic_name;
    string kPkgPath = ros::package::getPath("calibration");
    string dataset_name;
    string kDatasetPath;
    int spot_idx = 0;
    int view_idx = 0;
    int num_spots = 1;
    int num_views = 1; /** note: each spot contains several view **/
    int view_angle_init = 0;
    int view_angle_step = 1;
    int fullview_idx = 0;

    /** const parameters - original data - images and point clouds **/
    const int kFlatRows = 2000;
    const int kFlatCols = 4000;
    const float kRadPerPix = (M_PI * 2) / kFlatCols;
    const bool kColorMap = false; /** enable edge cloud output in polar/3D space for visualization **/

    /** tags and maps **/
    typedef vector<int> Tags;
    typedef vector<vector<Tags>> TagsMap;
    vector<vector<TagsMap>> tags_map_vec; /** container of tagsMaps of each pose **/

    /** spatial coordinates of edge points (center of distribution) **/
    // extracted edges in original space
    vector<vector<EdgeCloud>> edge_cloud_vec; /** container of edgeClouds of each pose **/

    /** rigid transformation generated by ICP at different poses(vertical angle) **/
    vector<vector<Eigen::Matrix4f>> pose_trans_mat_vec;

    /***** Extrinsic Parameters *****/
    Ext_D ext_;

    /** File Path of the Specific Scene **/
    struct PoseFilePath {
        PoseFilePath()= default;
        PoseFilePath(string& spot_path, string& pose_path) {
            this->fullview_recon_folder_path = spot_path + "/fullview_recon";
            this->output_folder_path = pose_path + "/outputs/lidar_outputs";
            this->bag_folder_path = pose_path + "/bags";
            this->result_folder_path = pose_path + "/results";

            this->edge_img_path = pose_path + "/edges/lidar_edge.png";
            this->view_cloud_path = this->output_folder_path + "/view_cloud.pcd";
            this->pose_trans_mat_path = this->output_folder_path + "/pose_trans_mat.txt";
            this->flat_img_path = this->output_folder_path + "/flat_lidar_image.bmp";
            this->tags_map_path = this->output_folder_path + "/tags_map.txt";
            this->edge_cloud_path = this->output_folder_path + "/edge_lidar.pcd";
            this->edge_fisheye_projection_path = this->output_folder_path + "/lid_trans.txt";
            this->params_record_path = this->output_folder_path + "/params_record.txt";
            
            this->lio_spot_trans_mat_path = this->fullview_recon_folder_path + "/lio_spot_trans_mat.txt";
            this->icp_spot_trans_mat_path = this->fullview_recon_folder_path + "/icp_spot_trans_mat.txt";
            this->spot_cloud_path = this->fullview_recon_folder_path + "/spot_cloud.pcd";
            this->spot_rgb_cloud_path = this->fullview_recon_folder_path + "/spot_rgb_cloud.pcd";
        }
        /** pose **/
        string output_folder_path;
        string result_folder_path;
        string bag_folder_path;

        string edge_img_path;
        string view_cloud_path;
        string pose_trans_mat_path;
        string flat_img_path;
        string tags_map_path;
        string edge_cloud_path;
        string edge_fisheye_projection_path;
        string params_record_path;
        /** spot **/
        string fullview_recon_folder_path;
        string spot_cloud_path;
        string fullview_sparse_cloud_path;
        string spot_rgb_cloud_path;
        string lio_spot_trans_mat_path;
        string icp_spot_trans_mat_path;
    };
    vector<vector<string>> folder_path_vec;
    vector<vector<struct PoseFilePath>> file_path_vec;

    /** Degree Map **/
    std::map<int, int> degree_map;

public:
    /***** LiDAR Class *****/
    LidarProcess();
    void SetSpotIdx(int spot_idx) {
        this->spot_idx = spot_idx;
    }
    void SetViewIdx(int view_idx) {
        this->view_idx = view_idx;
    }

    /***** Point Cloud Generation *****/
    void BagToPcd(string filepath, CloudI &cloud);

    /***** LiDAR Pre-Processing *****/
    void LidarToSphere(CloudI::Ptr &cart_cloud, CloudI::Ptr &polar_cloud);
    void SphereToPlane(CloudI::Ptr &polar_cloud);
    void GenerateEdgeCloud(CloudI::Ptr &cart_cloud);

    /***** Edge Process *****/
    void EdgeExtraction();
    void ReadEdge();

    /***** Registration and Mapping *****/
    Mat4F Align(CloudI::Ptr cloud_tgt, CloudI::Ptr cloud_src, Mat4F init_trans_mat, int cloud_type, const bool kIcpViz);
    void CalcEdgeDistance(EdgeCloud::Ptr cloud_tgt, EdgeCloud::Ptr cloud_src, float max_range);
    void SpotRegAnalysis(int tgt_spot_idx, int src_spot_idx, bool kAnalysis);

    void CreateDensePcd();
    void ViewRegistration();
    void FullViewMapping();
    void SpotRegistration();
    void FineToCoarseReg();
    void GlobalColoredMapping(bool kGlobalUniformSampling);
    void GlobalMapping(bool kGlobalUniformSampling);

    double GetFitnessScore(CloudI::Ptr cloud_tgt, CloudI::Ptr cloud_src, float max_range);
    void RemoveInvalidPoints(CloudI::Ptr cloud);

    void computeCovariances(pcl::PointCloud<PointI>::ConstPtr cloud,
                            const pcl::search::KdTree<PointI>::Ptr kdtree,
                            MatricesVector& cloud_covariances);

};