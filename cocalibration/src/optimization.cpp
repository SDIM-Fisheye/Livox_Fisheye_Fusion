/** headings **/
#include <optimization.h>
#include <common_lib.h>

ofstream outfile;

struct QuaternionFunctor {
    template <typename T>
    bool operator()(const T *const q_, const T *const t_, const T *const intrinsic_, T *cost) const {
        Eigen::Quaternion<T> q{q_[3], q_[0], q_[1], q_[2]};
        Eigen::Matrix<T, 3, 3> R = q.toRotationMatrix();
        Eigen::Matrix<T, 3, 1> t(t_);
        Eigen::Matrix<T, K_INT, 1> intrinsic(intrinsic_);
        Eigen::Matrix<T, 3, 1> lidar_point = R * lid_point_.cast<T>() + t;
        Eigen::Matrix<T, 2, 1> projection = IntrinsicTransform(intrinsic, lidar_point);
        T res, val;
        kde_interpolator_.Evaluate(projection(0) * T(kde_scale_), projection(1) * T(kde_scale_), &val);
        res = T(weight_) * (T(kde_val_) - val);
        cost[0] = res;
        cost[1] = res;
        cost[2] = res;
        return true;
    }

    QuaternionFunctor(const Vec3D lid_point,
                    const double weight,
                    const double ref_val,
                    const double scale,
                    const ceres::BiCubicInterpolator<ceres::Grid2D<double>> &interpolator)
                    : lid_point_(std::move(lid_point)), kde_interpolator_(interpolator), weight_(std::move(weight)), kde_val_(std::move(ref_val)), kde_scale_(std::move(scale)) {}

    static ceres::CostFunction *Create(const Vec3D &lid_point,
                                       const double &weight,
                                       const double &kde_val,
                                       const double &kde_scale,
                                       const ceres::BiCubicInterpolator<ceres::Grid2D<double>> &interpolator) {
        return new ceres::AutoDiffCostFunction<QuaternionFunctor, 3, ((6+1)-3), 3, K_INT>(
                new QuaternionFunctor(lid_point, weight, kde_val, kde_scale, interpolator));
    }

    const Vec3D lid_point_;
    const double weight_;
    const double kde_val_;
    const double kde_scale_;
    const ceres::BiCubicInterpolator<ceres::Grid2D<double>> &kde_interpolator_;
};

double project2Image(OmniProcess &omni, LidarProcess &lidar, std::vector<double> &params, std::string record_path, double bandwidth) {
    ofstream outfile;
    Ext_D extrinsic = Eigen::Map<Param_D>(params.data()).head(6);
    Int_D intrinsic = Eigen::Map<Param_D>(params.data()).tail(K_INT);
    EdgeCloud::Ptr ocam_edge_cloud = omni.ocamEdgeCloud;
    EdgeCloud::Ptr lidar_edge_cloud (new EdgeCloud);
    Mat4D T_mat = transformMat(extrinsic);
    pcl::transformPointCloud(*lidar.lidarEdgeCloud, *lidar_edge_cloud, T_mat);

    Vec3D lidar_point;
    Vec2D projection;
    cv::Mat fusion_image = cv::imread(omni.cocalibImagePath, cv::IMREAD_UNCHANGED);
    for (auto &point : lidar_edge_cloud->points) {
        lidar_point << point.x, point.y, point.z;
        projection = IntrinsicTransform(intrinsic, lidar_point);
        int u = std::clamp((int)round(projection(0)), 0, omni.cocalibImage.rows - 1);
        int v = std::clamp((int)round(projection(1)), 0, omni.cocalibImage.cols - 1);
        point.x = projection(0);
        point.y = projection(1);
        point.z = 0;

        float radius = pow(u-intrinsic(0),2) + pow(v-intrinsic(1),2);
        Pair &bounds = omni.kEffectiveRadius;
        if (radius > bounds.first * bounds.first && radius < bounds.second * bounds.second) {
            fusion_image.at<cv::Vec3b>(u, v)[0] = 0;   // b
            fusion_image.at<cv::Vec3b>(u, v)[1] = 255; // g
            fusion_image.at<cv::Vec3b>(u, v)[2] = 0;   // r
            if (MESSAGE_EN) {outfile << u << "," << v << endl; }
        }
    }
    if (MESSAGE_EN) {outfile.close(); }
    double proj_error = lidar.getEdgeDistance(ocam_edge_cloud, lidar_edge_cloud, 30);
    /** generate fusion image **/
    cv::imwrite(record_path, fusion_image);
    return proj_error;
}

std::vector<double> QuaternionCalib(OmniProcess &omni,
                                    LidarProcess &lidar,
                                    double bandwidth,
                                    std::vector<int> spot_vec,
                                    std::vector<double> init_params_vec,
                                    std::vector<double> lb,
                                    std::vector<double> ub,
                                    bool lock_intrinsic) {
    Param_D init_params = Eigen::Map<Param_D>(init_params_vec.data());
    Ext_D extrinsic = init_params.head(6);
    MatD(K_INT+(6+1), 1) q_vector;
    Mat3D rotation_mat = transformMat(extrinsic).topLeftCorner(3, 3);
    Eigen::Quaterniond quaternion(rotation_mat);
    ceres::EigenQuaternionManifold *q_manifold = new ceres::EigenQuaternionManifold();
    
    const int kParams = q_vector.size();
    const double scale = KDE_SCALE;
    q_vector.tail(K_INT + 3) = init_params.tail(K_INT + 3);
    q_vector.head(4) << quaternion.x(), quaternion.y(), quaternion.z(), quaternion.w();
    double params[kParams];
    memcpy(params, &q_vector(0), q_vector.size() * sizeof(double));

    /********* Initialize Ceres Problem *********/
    ceres::Problem problem;
    problem.AddParameterBlock(params, ((6+1)-3), q_manifold);
    problem.AddParameterBlock(params+((6+1)-3), 3);
    problem.AddParameterBlock(params+(6+1), K_INT);
    ceres::LossFunction *loss_function = new ceres::HuberLoss(0.05);

    /********* Fisheye KDE *********/
    std::vector<double> fisheye_kde = omni.Kde(bandwidth, scale);
    double *kde_val = new double[fisheye_kde.size()];
    memcpy(kde_val, &fisheye_kde[0], fisheye_kde.size() * sizeof(double));
    ceres::Grid2D<double> grid(kde_val, 0, omni.kImageSize.first * scale, 0, omni.kImageSize.second * scale);
    double ref_val = *max_element(fisheye_kde.begin(), fisheye_kde.end());
    ceres::BiCubicInterpolator<ceres::Grid2D<double>> interpolator(grid);

    double weight = sqrt(50000.0f / lidar.lidarEdgeCloud->size());
    for (auto &point : lidar.lidarEdgeCloud->points) {
        Vec3D lid_point = {point.x, point.y, point.z};
        problem.AddResidualBlock(QuaternionFunctor::Create(lid_point, weight, ref_val, scale, interpolator),
                            loss_function,
                            params, params+((6+1)-3), params+(6+1));
    }

    if (lock_intrinsic) {
        problem.SetParameterBlockConstant(params + (6+1));
    }

    for (int i = 0; i < kParams; ++i) {
        if (i < ((6+1)-3)) {
            problem.SetParameterLowerBound(params, i, (q_vector[i]-Q_LIM));
            problem.SetParameterUpperBound(params, i, (q_vector[i]+Q_LIM));
        }
        if (i >= ((6+1)-3) && i < (6+1)) {
            problem.SetParameterLowerBound(params+((6+1)-3), i-((6+1)-3), lb[i-1]);
            problem.SetParameterUpperBound(params+((6+1)-3), i-((6+1)-3), ub[i-1]);
        }
        else if (i >= (6+1) && !lock_intrinsic) {
            problem.SetParameterLowerBound(params+(6+1), i-(6+1), lb[i-1]);
            problem.SetParameterUpperBound(params+(6+1), i-(6+1), ub[i-1]);
        }
    }

    /********* Initial Options *********/
    ceres::Solver::Options options;
    options.linear_solver_type = ceres::DENSE_SCHUR;
    options.trust_region_strategy_type = ceres::LEVENBERG_MARQUARDT;
    options.minimizer_progress_to_stdout = MESSAGE_EN;
    options.num_threads = std::thread::hardware_concurrency();
    options.max_num_iterations = 200;
    options.gradient_tolerance = 1e-6;
    options.function_tolerance = 1e-12;
    options.use_nonmonotonic_steps = true;

    ceres::Solver::Summary summary;
    ceres::Solve(options, &problem, &summary);
    std::cout << summary.FullReport() << "\n";

    /********* 2D Image Visualization *********/
    Param_D result = Eigen::Map<MatD(K_INT+(6+1), 1)>(params).tail(6 + K_INT);
    result.head(3) = Eigen::Quaterniond(params[3], params[0], params[1], params[2]).matrix().eulerAngles(2,1,0).reverse();
    std::vector<double> result_vec(&result[0], result.data()+result.cols()*result.rows());
    extrinsic = result.head(6);
    /** Save Results**/
    std::string fusion_image_path = omni.RESULT_PATH + "/fusion_image_" + std::to_string((int)bandwidth) + ".bmp";
    std::string cocalib_result_path= lidar.RESULT_PATH + "/cocalib_" + std::to_string((int)bandwidth) + ".txt";
    double proj_error = project2Image(omni, lidar, result_vec, fusion_image_path, bandwidth);
    saveResults(cocalib_result_path, result_vec, bandwidth, summary.initial_cost, summary.final_cost, proj_error);
    return result_vec;
}

void costAnalysis(OmniProcess &omni,
                  LidarProcess &lidar,
                   std::vector<int> spot_vec,
                  std::vector<double> init_params_vec,
                  std::vector<double> result_vec,
                  double bandwidth) {
    const double scale = KDE_SCALE;

    /********* Fisheye KDE *********/
    std::vector<double> fisheye_kde = omni.Kde(bandwidth, scale);
    double *kde_val = new double[fisheye_kde.size()];
    memcpy(kde_val, &fisheye_kde[0], fisheye_kde.size() * sizeof(double));
    ceres::Grid2D<double> grid(kde_val, 0, omni.kImageSize.first * scale, 0, omni.kImageSize.second * scale);
    double ref_val = *max_element(fisheye_kde.begin(), fisheye_kde.end());
    ceres::BiCubicInterpolator<ceres::Grid2D<double>> interpolator(grid);

    /***** Correlation Analysis *****/
    Param_D params_mat = Eigen::Map<Param_D>(result_vec.data());
    Ext_D extrinsic;
    Int_D intrinsic;
    std::vector<double> results;
    std::vector<double> input_x;
    std::vector<double> input_y;
    std::vector<const char*> name = {
            "rx", "ry", "rz",
            "tx", "ty", "tz",
            "u0", "v0",
            "a0", "a1", "a2", "a3", "a4",
            "c", "d", "e"};
    int steps[3] = {1, 1, 1};
    int param_idx[3] = {0, 3, 6};
    const double step_size[3] = {0.0002, 0.001, 0.01};
    const double deg2rad = M_PI / 180;
    double offset[3] = {0, 0, 0};

    /** update evaluate points in 2D grid **/
    for (int m = 0; m < 6; m++) {
        extrinsic = params_mat.head(6);
        intrinsic = params_mat.tail(K_INT);
        if (m < 3) {
            steps[0] = 201;
            steps[1] = 1;
            steps[2] = 1;
            param_idx[0] = m;
            param_idx[1] = 3;
            param_idx[2] = 6;
        }
        else if (m < 6){
            steps[0] = 1;
            steps[1] = 201;
            steps[2] = 1;
            param_idx[0] = 0;
            param_idx[1] = m;
            param_idx[2] = 6;
        }
        else {
            steps[0] = 1;
            steps[1] = 1;
            steps[2] = 201;
            param_idx[0] = 0;
            param_idx[1] = 3;
            param_idx[2] = m;
        }

        double normalize_weight = sqrt(1.0f / lidar.lidarEdgeCloud->size());

        /** Save & terminal output **/
        string analysis_filepath = omni.DATASET_PATH + "/log/";
        if (steps[0] > 1) {
            analysis_filepath = analysis_filepath + name[param_idx[0]] + "_";
        }
        if (steps[1] > 1) {
            analysis_filepath = analysis_filepath + name[param_idx[1]] + "_";
        }
        if (steps[2] > 1) {
            analysis_filepath = analysis_filepath + name[param_idx[2]] + "_";
        }
        outfile.open(analysis_filepath + "_bw_" + to_string(int(bandwidth)) + "_result.txt", ios::out);
        if (steps[0] > 1) {
            outfile << init_params_vec[param_idx[0]] << "\t" << result_vec[param_idx[0]] << endl;
        }
        if (steps[1] > 1) {
            outfile << init_params_vec[param_idx[1]] << "\t" << result_vec[param_idx[1]] << endl;
        }
        if (steps[2] > 1) {
            outfile << init_params_vec[param_idx[2]] << "\t" << result_vec[param_idx[2]] << endl;
        }
        
        for (int i = -int((steps[0]-1)/2); i < int((steps[0]-1)/2)+1; i++) {
            offset[0] = i * step_size[0];
            extrinsic(param_idx[0]) = params_mat(param_idx[0]) + offset[0];
            
            for (int j = -int((steps[1]-1)/2); j < int((steps[1]-1)/2)+1; j++) {
                offset[1] = j * step_size[1];
                extrinsic(param_idx[1]) = params_mat(param_idx[1]) + offset[1];

                for (int n = -int((steps[2]-1)/2); n < int((steps[2]-1)/2)+1; n++) {
                    offset[2] = n * step_size[2];
                    intrinsic(param_idx[2]-6) = params_mat(param_idx[2]) + offset[2];
                
                    double step_res = 0;
                    int num_valid = 0;
                    /** Evaluate cost funstion **/
                    for (auto &point : lidar.lidarEdgeCloud->points) {
                        double val;
                        double weight = normalize_weight;
                        Eigen::Vector4d lidar_point4 = {point.x, point.y, point.z, 1.0};
                        Mat4D T_mat = transformMat(extrinsic);
                        Vec3D lidar_point = (T_mat * lidar_point4).head(3);
                        Vec2D projection = IntrinsicTransform(intrinsic, lidar_point);
                        interpolator.Evaluate(projection(0) * scale, projection(1) * scale, &val);
                        Pair &bounds = omni.kEffectiveRadius;
                        if ((pow(projection(0) - intrinsic(0), 2) + pow(projection(1) - intrinsic(1), 2)) > pow(bounds.first, 2)
                            && (pow(projection(0) - intrinsic(0), 2) + pow(projection(1) - intrinsic(1), 2)) < pow(bounds.second, 2)) {
                            step_res += pow(weight * val, 2);
                        }
                    }
                    if (steps[0] > 1) {
                        outfile << offset[0] + params_mat(param_idx[0]) << "\t";
                    }
                    if (steps[1] > 1) {
                        outfile << offset[1] + params_mat(param_idx[1]) << "\t";
                    }
                    if (steps[2] > 1) {
                        outfile << offset[2] + params_mat(param_idx[2]) << "\t";
                    }
                    outfile << step_res << endl;
                }
            }
        }
        outfile.close();
    }
}
