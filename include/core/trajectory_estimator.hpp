// rewrite b-spline computing using basalt
// libaorun 24.1.6
#ifndef TRAJECTORY_ESTIMATOR_HPP
#define TRAJECTORY_ESTIMATOR_HPP

#include <thread>

#include <ceres/ceres.h>
#include <ceres/covariance.h>

#include "utils/dataset_reader.h"
#include "core/basalt_traj.hpp"
#include "factor/auto_diff/imu_factor.hpp"
#include "factor/auto_diff/surfel_factor.hpp"
#include "core/calibration.hpp"
#include <basalt/spline/ceres_local_param.hpp>
#include "core/surfel_association.h"


namespace licalib {
  
template <int N>
class TrajEstimator{
  static ceres::Problem::Options DefaultProblemOptions(){
    ceres::Problem::Options options;
    options.loss_function_ownership = ceres::DO_NOT_TAKE_OWNERSHIP;
    options.local_parameterization_ownership = ceres::DO_NOT_TAKE_OWNERSHIP;
    return options;
  }

public:
  // TrajEstimator(std::shared_ptr<TrajSE3<N>> trajectory,
  //                     CalibParamManager::Ptr calib_param,
  //                     // std::shared_ptr<Lidar> lidar
  //               )
  //     : TrajPtr(trajectory), calib_param_(calib_param), lidar_(lidar) {
  //   problem_ = std::make_shared<ceres::Problem>(DefaultProblemOptions());
  //   local_parameterization = new basalt::LieLocalParameterization<Sophus::SO3d>();
  // }
  TrajEstimator(std::shared_ptr<TrajSE3<N>> trajectory,
                      CalibParamManager::Ptr calib_param
                      )
      : TrajPtr(trajectory), calib_param_(calib_param){
    problem_ = std::make_shared<ceres::Problem>(DefaultProblemOptions());
    local_parameterization = new basalt::LieLocalParameterization<Sophus::SO3d>();
    // lidar_ = Lidar(0);
  }
  void addConstBiasGyroMeasurements(const std::vector<IO::IMUData>& imu_datas,
                                    double gyro_weight){
    for(auto& imu_data : imu_datas){
      basalt::SplineMeta<N> spline_meta;
      
      Sophus::SE3d last_kont = TrajPtr->getLastKnot();
      TrajPtr->extendKnotsTo(imu_data.timestamp, last_kont);
      TrajPtr->CalculateSplineMeta({{imu_data.timestamp, imu_data.timestamp}},
                                  spline_meta);
      using Functor = gyroSO3ConstBiasFactor<N>;
      // Functor* functor = new Functor(imu_data, spline_meta, gyro_weight);
      // auto* cost_function = new ceres::DynamicAutoDiffCostFunction<Functor>(functor);
      Functor* functor = new Functor(imu_data, spline_meta, gyro_weight);
      auto* cost_function =
        new ceres::DynamicAutoDiffCostFunction<Functor>(functor);

    // add so3 knots
  // b /root/li_calib/src/lidar_IMU_calib/include/core/trajectory_estimator.hpp:49
      for (int i = 0; i < spline_meta.NumParameters(); i++) {
        cost_function->AddParameterBlock(4);
      }

      cost_function->AddParameterBlock(3);  // gyro bias

      cost_function->SetNumResiduals(3);

      std::vector<double*> vec;
      
      AddControlPoints(spline_meta, vec);
      // AddControlPoints(spline_meta, vec, true);

      vec.emplace_back(calib_param_->gyro_bias.data());
      // problem_->AddParameterBlock(calib_param_->gyro_bias.data(), 3);
      // problem_->SetParameterBlockConstant(calib_param_->gyro_bias.data());

      problem_->AddResidualBlock(cost_function, NULL, vec);

    }
    problem_->SetParameterBlockConstant(calib_param_->gyro_bias.data());
    // double startIdx = imu_datas[0].timestamp;
    problem_->SetParameterBlockConstant(TrajPtr->getKnotSO3(0+1e-9).data());
  }
  
  void addAccelerometerMeasurement(const std::vector<IO::IMUData>& imu_datas,
                                   double gyro_weight,
                                   double accel_weight) {
    for(auto& imu_data : imu_datas) {
      basalt::SplineMeta<N> spline_meta;

      TrajPtr->CalculateSplineMeta({{imu_data.timestamp, imu_data.timestamp}},
                                   spline_meta);
      using Functor = GyroAcceWithBiasFactor<N>;
      Functor* functor = new Functor(imu_data, spline_meta, gyro_weight, accel_weight);
      auto* cost_function = new ceres::DynamicAutoDiffCostFunction<Functor>(functor);

      // add so3 knots
      for(int i = 0; i < spline_meta.NumParameters(); i++){
        cost_function->AddParameterBlock(4);
      }

      // add pos knots
      for(int i = 0; i < spline_meta.NumParameters(); i++){
        cost_function->AddParameterBlock(3);
      }

      cost_function->AddParameterBlock(3);  // gyro bias
      cost_function->AddParameterBlock(3);  // acce bias
      // cost_function->AddParameterBlock(3);  // g_refine

      cost_function->SetNumResiduals(6);

      std::vector<double*> vec;
      AddControlPoints(spline_meta, vec, false);
      AddControlPoints(spline_meta, vec, true);
        
      vec.emplace_back(calib_param_->gyro_bias.data());
      vec.emplace_back(calib_param_->acce_bias.data());
      // vec.emplace_back(calib_param_->gravity.data());
      problem_->AddParameterBlock(calib_param_->gyro_bias.data(), 3);
      problem_->AddParameterBlock(calib_param_->acce_bias.data(), 3);
      // problem_->AddParameterBlock(calib_param_->gravity.data(), 3);

      problem_->AddResidualBlock(cost_function, nullptr, vec);

    }
    problem_->SetParameterBlockConstant(TrajPtr->getKnotPos(0+1e-9).data());
    problem_->SetParameterBlockConstant(TrajPtr->getKnotSO3(0+1e-9).data());
  }

  void addSurfMeasurement(const SurfelAssociation::Ptr surfel_association) {
    using Functor = surfelFactorWithRelative<N>;
    closest_point_vec_.clear();
    for(auto const& v : surfel_association->get_surfel_planes()) {
      closest_point_vec_.push_back(v.Pi);
    }
    int numbers = 0;
    for(auto const& spoint: surfel_association->get_surfel_points()){
      numbers++;
      basalt::SplineMeta<N> spline_meta;
      double lidar_time = spoint.timestamp;
      size_t plane_id = spoint.plane_id;
      // auto knotsize = TrajPtr->getKnots().size();
      auto weight = calib_param_->global_opt_lidar_weight;
      auto maptime = surfel_association->get_maptime();
      TrajPtr->CalculateSplineMeta({{maptime, lidar_time}}, spline_meta);
      
      Functor* functor = new Functor(
          maptime, lidar_time, weight, spoint.point,
          spline_meta);
      auto* cost_function = new ceres::DynamicAutoDiffCostFunction<Functor>(functor);
      for(int i = 0; i < spline_meta.NumParameters(); i++) {
        cost_function->AddParameterBlock(4);
      }
      for(int i = 0; i < spline_meta.NumParameters(); i++) {
        cost_function->AddParameterBlock(3);
      }
      cost_function->AddParameterBlock(3); // vector3
      cost_function->AddParameterBlock(4); // quaternion
      // cost_function->AddParameterBlock(1); // time_offset

      cost_function->AddParameterBlock(3); // plane param
      
      cost_function->SetNumResiduals(1);

      std::vector<double*> vec;
      AddControlPoints(spline_meta, vec, false);
      AddControlPoints(spline_meta, vec, true);
      // addSurfParams(closest_point_vec_.at(plane_id), vec);
      vec.emplace_back(calib_param_->p_LinI.data());
      vec.emplace_back(calib_param_->q_LtoI.coeffs().data());
      // vec.emplace_back(calib_param_->time_offset);
      vec.emplace_back(closest_point_vec_.at(plane_id).data());

      problem_->AddParameterBlock(calib_param_->p_LinI.data(), 3);
      problem_->AddParameterBlock(calib_param_->q_LtoI.coeffs().data(), 4);
      // problem_->AddParameterBlock(calib_param_->time_offset, 1);
      problem_->AddParameterBlock(closest_point_vec_.at(plane_id).data(), 3);

      problem_->AddResidualBlock(cost_function, nullptr, vec);
    }
    problem_->SetParameterBlockConstant(TrajPtr->getKnotPos(0+1e-9).data());
    problem_->SetParameterBlockConstant(TrajPtr->getKnotSO3(0+1e-9).data());
    std::cout << "\033[32m" << "surfel numbers " << numbers <<  "\033[0m" << std::endl;
  }

  void AddControlPoints(
    const basalt::SplineMeta<N>& spline_meta, std::vector<double*>& vec,
    bool addPosKont = false) {
    for (auto const& seg : spline_meta.segments) {
      size_t start_idx = TrajPtr->computeTIndex(seg.t0 + 1e-9).second;
      for (size_t i = start_idx; i < (start_idx + seg.NumParameters()); ++i) {
        if (addPosKont) {
          vec.emplace_back(TrajPtr->getKnotPos(i).data());
          problem_->AddParameterBlock(TrajPtr->getKnotPos(i).data(), 3);
        } else {
          vec.emplace_back(TrajPtr->getKnotSO3(i).data());
          problem_->AddParameterBlock(TrajPtr->getKnotSO3(i).data(), 4,
                                      local_parameterization);
        }
        // if (IsLocked() || (fixed_control_point_index_ >= 0 &&
        //                    i <= fixed_control_point_index_)) {
        //   problem_->SetParameterBlockConstant(vec.back());
        // }
      }
    }
  }

  void addSurfParams(Eigen::Vector3d plane, std::vector<double*>& vec) {
    vec.emplace_back(calib_param_->p_LinI.data());
    vec.emplace_back(calib_param_->q_LtoI.coeffs().data());
    // vec.emplace_back(calib_param_->time_offset);
    vec.emplace_back(plane.data());

    problem_->AddParameterBlock(calib_param_->p_LinI.data(), 3);
    problem_->AddParameterBlock(calib_param_->q_LtoI.coeffs().data(), 4);
    // problem_->AddParameterBlock(calib_param_->time_offset, 1);
    problem_->AddParameterBlock(plane.data(), 3);
  }

  ceres::Solver::Summary Solve(int max_iterations,
                               bool progress,
                               int num_threads) {
  ceres::Solver::Options options;

  options.minimizer_type = ceres::TRUST_REGION;
  //  options.gradient_tolerance = 0.01 * Sophus::Constants<double>::epsilon();
  //  options.function_tolerance = 0.01 * Sophus::Constants<double>::epsilon();
  options.linear_solver_type = ceres::SPARSE_NORMAL_CHOLESKY;
  //    options.trust_region_strategy_type = ceres::DOGLEG;
  //    options.dogleg_type = ceres::SUBSPACE_DOGLEG;

  //    options.linear_solver_type = ceres::SPARSE_SCHUR;
  options.trust_region_strategy_type = ceres::LEVENBERG_MARQUARDT;

  options.minimizer_progress_to_stdout = progress;

  if (num_threads < 1) {
    num_threads = std::thread::hardware_concurrency();
  }
  options.num_threads = num_threads;
  options.max_num_iterations = max_iterations;

  if (callbacks_.size() > 0) {
    for (auto& cb : callbacks_) {
      options.callbacks.push_back(cb.get());
    }

    if (callback_needs_state_) options.update_state_every_iteration = true;
  }

  ceres::Solver::Summary summary;
  ceres::Solve(options, problem_.get(), &summary);

  //  getCovariance();
  return summary;
}
private:
  std::shared_ptr<TrajSE3<N>> TrajPtr;
  std::shared_ptr<ceres::Problem> problem_;
  std::shared_ptr<CalibParamManager> calib_param_;
  ceres::LocalParameterization* local_parameterization;

  std::vector<std::unique_ptr<ceres::IterationCallback>> callbacks_;
  bool callback_needs_state_;
  // std::shared_ptr<Lidar> lidar_;
  Eigen::aligned_vector<Eigen::Vector3d> closest_point_vec_;
};

}

#endif