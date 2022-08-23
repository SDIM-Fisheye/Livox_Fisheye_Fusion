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
/** pcl **/
#include <pcl/point_cloud.h>
#include <pcl_conversions/pcl_conversions.h>
#include <pcl/filters/voxel_grid.h>
#include <pcl/filters/uniform_sampling.h>
#include <pcl/common/common.h>
#include <pcl/io/pcd_io.h>
#include <pcl/point_types.h>
#include <pcl/filters/radius_outlier_removal.h>
#include <pcl/filters/passthrough.h>
#include <pcl/kdtree/kdtree_flann.h>
#include <pcl/common/transforms.h>
#include <Eigen/Core>
#include <pcl/registration/icp.h>
#include <pcl/registration/gicp.h>
#include <pcl/registration/ndt.h>
#include <pcl/visualization/pcl_visualizer.h>
#include <pcl/filters/filter.h>
#include <pcl/filters/conditional_removal.h>
#include <pcl/common/time.h>
#include <pcl/filters/extract_indices.h>
/** opencv **/
#include <opencv2/opencv.hpp>

/** namespace **/
using namespace std;

/** typedef **/
typedef pcl::PointXYZI PointI;
typedef pcl::PointXYZRGB PointRGB;
typedef pcl::PointCloud<PointI> CloudI;
typedef pcl::PointCloud<PointRGB> CloudRGB;

class LidarProcess{
public:
    /** essential params **/
    string topic_name;
    string kPkgPath = ros::package::getPath("calibration");
    string dataset_name;
    string kDatasetPath;
    int spot_idx = 0;
    int view_idx = 0;
    int num_spots;
    int num_views; /** note: each spot contains several view **/
    int view_angle_init;
    int view_angle_step;
    int fullview_idx;
    vector<vector<string>> poses_folder_path_vec;

    /** const parameters - original data - images and point clouds **/
    const bool kDenseCloud = true; /** true means merge the dense cloud and create fullview dense cloud, 
                                       otherwise it will create icp sparse cloud and fullview sparse cloud to be used in visualization **/
    const bool kProjByIntensity = true;
    static const int kNumRecPcds = 250; /** dense point cloud used for reconstruction **/
    static const int kNumIcpPcds = 20; /** sparse point cloud used for ICP registration **/
    const int kFlatRows = 2000;
    const int kFlatCols = 4000;
    const float kRadPerPix = (M_PI * 2) / 4000;
    const bool kEdgeAnalysis = true; /** enable edge cloud output in polar/3D space for visualization **/

    /** tags and maps **/
    typedef struct Tags {
        int num_pts = 0; /** number of points **/
        vector<int> pts_indices = {};
    }Tags; /** "Tags" here is a struct type, equals to "struct Tags", LidarProcess::Tags **/
    typedef vector<vector<Tags>> TagsMap;
    vector<vector<TagsMap>> tags_map_vec; /** container of tagsMaps of each pose **/

    /** coordinates of edge pixels (which are considered as the edge) **/
    typedef vector<vector<int>> EdgePixels;
    vector<vector<EdgePixels>> edge_pixels_vec;

    /** spatial coordinates of edge points (center of distribution) **/
    typedef vector<vector<double>> EdgePts;
    vector<vector<EdgePts>> edge_pts_vec;

    /** mean position of the lidar pts in a specific pixel space **/
    vector<vector<CloudI::Ptr>> edge_cloud_vec; /** container of edgeClouds of each pose **/

    /** rigid transformation generated by ICP at different poses(vertical angle) **/
    vector<vector<Eigen::Matrix4f>> pose_trans_mat_vec;

    /***** Extrinsic Parameters *****/
    struct Extrinsic {
        double rx = 0, ry = 0, rz = 0,
               tx = 0, ty = 0, tz = 0;
    } extrinsic;

    /** File Path of the Specific Scene **/
    struct PoseFilePath {
        PoseFilePath()= default;
        PoseFilePath(string& spot_path, string& pose_path) {
            this->fullview_recon_folder_path = spot_path + "/fullview_recon";
            this->output_folder_path = pose_path + "/outputs/lidar_outputs";
            // this->dense_pcds_folder_path = pose_path + "/dense_pcds";
            this->bag_folder_path = pose_path + "/bags";
            this->result_folder_path = pose_path + "/results";
            
            this->lio_spot_trans_mat_path = this->fullview_recon_folder_path + "/lio_spot_trans_mat.txt";
            this->icp_spot_trans_mat_path = this->fullview_recon_folder_path + "/icp_spot_trans_mat.txt";
            this->fullview_dense_cloud_path = this->fullview_recon_folder_path + "/fullview_dense_cloud.pcd";
            this->fullview_sparse_cloud_path = this->fullview_recon_folder_path + "/fullview_sparse_cloud.pcd";
            this->fullview_rgb_cloud_path = this->fullview_recon_folder_path + "/fullview_rgb_cloud.pcd";
            this->edge_polar_pcd_path = this->fullview_recon_folder_path + "/edge_polar.pcd";
            this->edge_cart_pcd_path = this->fullview_recon_folder_path + "/edge_cart.pcd";
            this->edge_img_path = pose_path + "/edges/lidEdge.png";
            this->dense_pcd_path = this->output_folder_path + "/lidDense" + to_string(kNumRecPcds) + ".pcd";
            // this->icp_pcd_path = this->output_folder_path + "/icp_cloud.pcd";
            this->pose_trans_mat_path = this->output_folder_path + "/pose_trans_mat.txt";
            this->flat_img_path = this->output_folder_path + "/flatLidarImage.bmp";
            this->tags_map_path = this->output_folder_path + "/tags_map.txt";
            this->edge_pts_coordinates_path = this->output_folder_path + "/lid3dOut.txt";
            this->edge_fisheye_projection_path = this->output_folder_path + "/lidTrans.txt";
            this->params_record_path = this->output_folder_path + "/ParamsRecord.txt";
        }
        /** pose **/
        string output_folder_path;
        // string dense_pcds_folder_path;
        string result_folder_path;
        string bag_folder_path;
        string edge_img_path;
        string dense_pcd_path;
        // string icp_pcd_path;
        string pose_trans_mat_path;
        string flat_img_path;
        string edge_polar_pcd_path;
        string edge_cart_pcd_path;
        string tags_map_path;
        string edge_pts_coordinates_path;
        string edge_fisheye_projection_path;
        string params_record_path;
        /** spot **/
        string fullview_recon_folder_path;
        string fullview_dense_cloud_path;
        string fullview_sparse_cloud_path;
        string fullview_rgb_cloud_path;
        string lio_spot_trans_mat_path;
        string icp_spot_trans_mat_path;
    };
    vector<vector<struct PoseFilePath>> poses_files_path_vec;

    /** Degree Map **/
    std::map<int, int> degree_map;

public:
    /***** LiDAR Class *****/
    LidarProcess();
    void SetExtrinsic(vector<double> &parameters) {
        this->extrinsic.rx = parameters[0];
        this->extrinsic.ry = parameters[1];
        this->extrinsic.rz = parameters[2];
        this->extrinsic.tx = parameters[3];
        this->extrinsic.ty = parameters[4];
        this->extrinsic.tz = parameters[5];
    }
    void SetSpotIdx(int spot_idx) {
        this->spot_idx = spot_idx;
    }
    void SetViewIdx(int view_idx) {
        this->view_idx = view_idx;
    }

    /***** Point Cloud Generation *****/
    static int ReadFileList(const string &folder_path, vector<string> &file_list);
    void BagToPcd(string filepath, CloudI &cloud);

    /***** LiDAR Pre-Processing *****/
    void LidarToSphere(CloudI::Ptr& cart_cloud, CloudI::Ptr& polar_cloud);
    void SphereToPlane(CloudI::Ptr& cart_cloud, CloudI::Ptr& polar_cloud);
    void PixLookUp(CloudI::Ptr& cart_cloud, CloudI::Ptr& polar_cloud);

    /***** Edge Process *****/
    void EdgeExtraction();
    void EdgeToPixel();
    void ReadEdge();
    vector<double> Kde(vector<vector<double>> edge_pixels, int row_samples, int col_samples);

    /***** Registration and Mapping *****/
    tuple<Eigen::Matrix4f, CloudI::Ptr> ICP(CloudI::Ptr cloud_tgt, CloudI::Ptr cloud_src, Eigen::Matrix4f init_trans_mat, int cloud_type, const bool kIcpViz);
    void DistanceAnalysis(CloudI::Ptr cloud_tgt, CloudI::Ptr cloud_src, float uniform_radius, float max_range);
    double GetIcpFitnessScore(CloudI::Ptr cloud_tgt, CloudI::Ptr cloud_src, double max_range);
    void CreateDensePcd();
    void ViewRegistration();
    void FullViewMapping();
    void SpotRegistration();
    void FineToCoarseReg();
    void GlobalColoredMapping();
    void GlobalMapping();
    void MappingEval();

    template <typename PointType>
    void LoadPcd(string filepath, pcl::PointCloud<PointType> &cloud, const char* name = "");
};