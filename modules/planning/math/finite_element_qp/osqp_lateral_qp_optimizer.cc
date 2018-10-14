/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#include "modules/planning/math/finite_element_qp/osqp_lateral_qp_optimizer.h"

#include <algorithm>

#include "cybertron/common/log.h"
#include "modules/common/math/matrix_operations.h"
#include "modules/planning/common/planning_gflags.h"

namespace apollo {
namespace planning {

using Eigen::MatrixXd;
using apollo::common::math::DenseToCSCMatrix;

bool OsqpLateralQPOptimizer::optimize(
    const std::array<double, 3>& d_state, const double delta_s,
    const std::vector<std::pair<double, double>>& d_bounds) {
  std::vector<c_float> P_data;
  std::vector<c_int> P_indices;
  std::vector<c_int> P_indptr;
  CalculateKernel(d_bounds, &P_data, &P_indices, &P_indptr);
  delta_s_ = delta_s;
  const int num_var = d_bounds.size();
  const int kNumParam = 3 * d_bounds.size();
  const int kNumConstraint = kNumParam + 3 * (num_var - 1) + 3;
  MatrixXd affine_constraint = MatrixXd::Zero(kNumConstraint, kNumParam);
  c_float lower_bounds[kNumConstraint];
  c_float upper_bounds[kNumConstraint];

  const int prime_offset = num_var;
  const int pprime_offset = 2 * num_var;
  int constraint_index = 0;

  // d_i+1'' - d_i''
  for (int i = 0; i + 1 < num_var; ++i) {
    const int row = constraint_index;
    const int col = pprime_offset + i;
    affine_constraint(row, col) = -1.0;
    affine_constraint(row, col + 1) = 1.0;

    lower_bounds[constraint_index] =
        -FLAGS_lateral_third_order_derivative_max * delta_s;
    upper_bounds[constraint_index] =
        FLAGS_lateral_third_order_derivative_max * delta_s;
    ++constraint_index;
  }

  // d_i+1' - d_i' - 0.5 * ds * (d_i'' + d_i+1'')
  for (int i = 0; i + 1 < num_var; ++i) {
    affine_constraint(constraint_index, prime_offset + i) = -1.0;
    affine_constraint(constraint_index, prime_offset + i + 1) = 1.0;

    affine_constraint(constraint_index, pprime_offset + i) = -0.5 * delta_s;
    affine_constraint(constraint_index, pprime_offset + i + 1) = -0.5 * delta_s;

    lower_bounds[constraint_index] = 0.0;
    upper_bounds[constraint_index] = 0.0;
    ++constraint_index;
  }

  // d_i+1 - d_i - d_i' * ds - 1/3 * d_i'' * ds^2 - 1/6 * d_i+1'' * ds^2
  for (int i = 0; i + 1 < num_var; ++i) {
    affine_constraint(constraint_index, i) = -1.0;
    affine_constraint(constraint_index, i + 1) = 1.0;

    affine_constraint(constraint_index, prime_offset + i) = -delta_s;

    affine_constraint(constraint_index, pprime_offset + i) =
        -delta_s * delta_s / 3.0;
    affine_constraint(constraint_index, pprime_offset + i + 1) =
        -delta_s * delta_s / 6.0;

    lower_bounds[constraint_index] = 0.0;
    upper_bounds[constraint_index] = 0.0;
    ++constraint_index;
  }

  affine_constraint(constraint_index, 0) = 1.0;
  lower_bounds[constraint_index] = d_state[0];
  upper_bounds[constraint_index] = d_state[0];
  ++constraint_index;

  affine_constraint(constraint_index, prime_offset) = 1.0;
  lower_bounds[constraint_index] = d_state[1];
  upper_bounds[constraint_index] = d_state[1];
  ++constraint_index;

  affine_constraint(constraint_index, pprime_offset) = 1.0;
  lower_bounds[constraint_index] = d_state[2];
  upper_bounds[constraint_index] = d_state[2];
  ++constraint_index;

  const double LARGE_VALUE = 2.0;
  for (int i = 0; i < kNumParam; ++i) {
    affine_constraint(constraint_index, i) = 1.0;
    if (i < num_var) {
      lower_bounds[constraint_index] = d_bounds[i].first;
      upper_bounds[constraint_index] = d_bounds[i].second;
    } else {
      lower_bounds[constraint_index] = -LARGE_VALUE;
      upper_bounds[constraint_index] = LARGE_VALUE;
    }
    ++constraint_index;
  }

  CHECK_EQ(constraint_index, kNumConstraint);

  // change affine_constraint to CSC format
  std::vector<c_float> A_data;
  std::vector<c_int> A_indices;
  std::vector<c_int> A_indptr;
  DenseToCSCMatrix(affine_constraint, &A_data, &A_indices, &A_indptr);

  // offset
  double q[kNumParam];
  for (int i = 0; i < kNumParam; ++i) {
    if (i < num_var) {
      q[i] = -2.0 * FLAGS_weight_lateral_obstacle_distance *
             (d_bounds[i].first + d_bounds[i].second);
    } else {
      q[i] = 0.0;
    }
  }

  // Problem settings
  OSQPSettings* settings =
      reinterpret_cast<OSQPSettings*>(c_malloc(sizeof(OSQPSettings)));

  // Define Solver settings as default
  osqp_set_default_settings(settings);
  settings->alpha = 1.0;  // Change alpha parameter
  settings->eps_abs = 1.0e-05;
  settings->eps_rel = 1.0e-05;
  settings->max_iter = 5000;
  settings->polish = true;
  settings->verbose = FLAGS_enable_osqp_debug;

  // Populate data
  OSQPData* data = reinterpret_cast<OSQPData*>(c_malloc(sizeof(OSQPData)));
  data->n = kNumParam;
  data->m = affine_constraint.rows();
  data->P = csc_matrix(data->n, data->n, P_data.size(), P_data.data(),
                       P_indices.data(), P_indptr.data());
  data->q = q;
  data->A = csc_matrix(data->m, data->n, A_data.size(), A_data.data(),
                       A_indices.data(), A_indptr.data());
  data->l = lower_bounds;
  data->u = upper_bounds;

  // Workspace
  OSQPWorkspace* work = osqp_setup(data, settings);

  // Solve Problem
  osqp_solve(work);

  // extract primal results
  for (int i = 0; i < num_var; ++i) {
    opt_d_.push_back(work->solution->x[i]);
    opt_d_prime_.push_back(work->solution->x[i + num_var]);
    opt_d_pprime_.push_back(work->solution->x[i + 2 * num_var]);
  }
  opt_d_prime_[num_var - 1] = 0.0;
  opt_d_pprime_[num_var - 1] = 0.0;

  // Cleanup
  osqp_cleanup(work);
  c_free(data->A);
  c_free(data->P);
  c_free(data);
  c_free(settings);

  return true;
}

void OsqpLateralQPOptimizer::CalculateKernel(
    const std::vector<std::pair<double, double>>& d_bounds,
    std::vector<c_float>* P_data, std::vector<c_int>* P_indices,
    std::vector<c_int>* P_indptr) {
  const int kNumParam = 3 * d_bounds.size();

  // const int kNumOfMatrixElement = kNumParam * kNumParam;
  MatrixXd kernel = MatrixXd::Zero(kNumParam, kNumParam);  // dense matrix

  for (int i = 0; i < kNumParam; ++i) {
    if (i < static_cast<int>(d_bounds.size())) {
      kernel(i, i) = 2.0 * FLAGS_weight_lateral_offset +
                     2.0 * FLAGS_weight_lateral_obstacle_distance;
    } else if (i < 2 * static_cast<int>(d_bounds.size())) {
      kernel(i, i) = 2.0 * FLAGS_weight_lateral_derivative;
    } else {
      kernel(i, i) = 2.0 * FLAGS_weight_lateral_second_order_derivative;
    }
  }

  DenseToCSCMatrix(kernel, P_data, P_indices, P_indptr);
}

}  // namespace planning
}  // namespace apollo
