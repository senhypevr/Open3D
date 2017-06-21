// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2017 Jaesik Park <syncle@gmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "Odometry.h"

#include <Eigen/Dense>
#include <Core/Geometry/Image.h>
#include <Core/Geometry/RGBDImage.h>
#include <Core/Odometry/RGBDOdometryJacobian.h>

namespace three {

namespace {

std::shared_ptr<CorrespondenceSetPixelWise> ComputeCorrespondence(
		const Eigen::Matrix3d intrinsic_matrix,
		const Eigen::Matrix4d &odo,
		const Image &depth_s, const Image &depth_t,
		const OdometryOption &option)
{
	const Eigen::Matrix3d K = intrinsic_matrix;
	const Eigen::Matrix3d K_inv = K.inverse();
	const Eigen::Matrix3d R = odo.block<3, 3>(0, 0);
	const Eigen::Matrix3d KRK_inv = K * R * K_inv;
	Eigen::Vector3d Kt = K * odo.block<3, 1>(0, 3);
	
	// initialization: filling with any (u,v) to (-1,-1)
	auto correspondence_map = std::make_shared<Image>();
	correspondence_map->PrepareImage(depth_t.width_, depth_t.height_, 2, 4);
	for (int v = 0; v < correspondence_map->height_; v++) {
		for (int u = 0; u < correspondence_map->width_; u++) {
			*PointerAt<int>(*correspondence_map, u, v, 0) = -1;
			*PointerAt<int>(*correspondence_map, u, v, 1) = -1;
		}
	}

#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
	for (int v_t = 0; v_t < depth_t.height_; v_t++) {
		for (int u_t = 0; u_t < depth_t.width_; u_t++) {
			double d_t = *PointerAt<float>(depth_t, u_t, v_t);
			if (!std::isnan(d_t)) {
				Eigen::Vector3d uv_in_t = 
						d_t * KRK_inv * Eigen::Vector3d(u_t, v_t, 1.0) + Kt;
				double transformed_d_t = uv_in_t(2);
				int u_s = (int)(uv_in_t(0) / transformed_d_t + 0.5);
				int v_s = (int)(uv_in_t(1) / transformed_d_t + 0.5);
				if (u_s >= 0 && u_s < depth_t.width_ &&
					v_s >= 0 && v_s < depth_t.height_) {
					double d_s = *PointerAt<float>(depth_s, u_s, v_s);
					if (!std::isnan(d_s) &&
						std::abs(transformed_d_t - d_s) <= option.max_depth_diff_) {
						int exist_u_t, exist_v_t;
						exist_u_t = 
								*PointerAt<int>(*correspondence_map, u_s, v_s, 0);
						exist_v_t = 
								*PointerAt<int>(*correspondence_map, u_s, v_s, 1);
						if (exist_u_t != -1 && exist_v_t != -1) {
							double exist_d_t = *PointerAt<float>
									(depth_t, exist_u_t, exist_v_t) * 
									(KRK_inv(2,0) * exist_u_t + KRK_inv(2,1)
									* exist_v_t + KRK_inv(2,2)) + Kt(2);
							if (transformed_d_t > exist_d_t)
								continue;
						}
						*PointerAt<int>(*correspondence_map, u_s, v_s, 0) = u_t;
						*PointerAt<int>(*correspondence_map, u_s, v_s, 1) = v_t;
					}
				}
			}
		}
	}
	auto correspondence = std::make_shared<CorrespondenceSetPixelWise>();
	correspondence->clear();
	for (int v_s = 0; v_s < correspondence_map->height_; v_s++) {
		for (int u_s = 0; u_s < correspondence_map->width_; u_s++) {
			int u_t = *PointerAt<int>(*correspondence_map, u_s, v_s, 0);
			int v_t = *PointerAt<int>(*correspondence_map, u_s, v_s, 1);
			if (u_t != -1 && v_t != -1) {
				Eigen::Vector4i pixel_correspondence(u_s, v_s, u_t, v_t);
				correspondence->push_back(pixel_correspondence);
			}
		}
	}
	return correspondence;
}

/// This might be worth moving to Core/Eigen.h. But we need a better name.
Eigen::Matrix4d Transform6DVecorto4x4Matrix(Eigen::VectorXd input)
{
	Eigen::Affine3d aff_mat;
	aff_mat.linear() = (Eigen::Matrix3d)
			Eigen::AngleAxisd(input(2), Eigen::Vector3d::UnitZ())
			* Eigen::AngleAxisd(input(1), Eigen::Vector3d::UnitY())
			* Eigen::AngleAxisd(input(0), Eigen::Vector3d::UnitX());
	aff_mat.translation() = Eigen::Vector3d(input(3), input(4), input(5));
	return aff_mat.matrix();
}

/// This might be worth moving to Core/Eigen.h.
std::tuple<bool, Eigen::Matrix4d> 
		SolveLinearSystem(const Eigen::MatrixXd &J, const Eigen::VectorXd &r) 
{
	Eigen::MatrixXd Jt = J.transpose();
	Eigen::MatrixXd JtJ = Jt * J;
	Eigen::MatrixXd Jtr = Jt * r;

	bool solution_exist = true;
	double det = JtJ.determinant();
	if (fabs(det) < 1e-6 || std::isnan(det) || std::isinf(det))
		solution_exist = false;

	if (solution_exist) {
		// Robust Cholesky decomposition of a matrix with pivoting.
		Eigen::MatrixXd x = -JtJ.ldlt().solve(Jtr);
		Eigen::Matrix4d M = Transform6DVecorto4x4Matrix(x);
		return std::make_tuple(solution_exist, M);
	} else {
		return std::make_tuple(solution_exist, Eigen::Matrix4d::Zero());
	}
}

std::shared_ptr<Image> ConvertDepthImageToXYZImage(
		const Image &depth, const Eigen::Matrix3d &intrinsic_matrix)
{
	auto image_xyz = std::make_shared<Image>();
	if (depth.num_of_channels_ != 1 || depth.bytes_per_channel_ != 4) {
		PrintDebug("[ConvertDepthImageToXYZImage] Unsupported image format.\n");
		return image_xyz;
	}
	const double inv_fx = 1.0 / intrinsic_matrix(0, 0);
	const double inv_fy = 1.0 / intrinsic_matrix(1, 1);
	const double ox = intrinsic_matrix(0, 2);
	const double oy = intrinsic_matrix(1, 2);
	image_xyz->PrepareImage(depth.width_, depth.height_, 3, 4);

	for (int y = 0; y < image_xyz->height_; y++) {
		for (int x = 0; x < image_xyz->width_; x++) {
			float *px = PointerAt<float>(*image_xyz, x, y, 0);
			float *py = PointerAt<float>(*image_xyz, x, y, 1);
			float *pz = PointerAt<float>(*image_xyz, x, y, 2);
			float z = *PointerAt<float>(depth, x, y);
			*px = (float)((x - ox) * z * inv_fx);
			*py = (float)((y - oy) * z * inv_fy);
			*pz = z;
		}
	}
	return image_xyz;
}

std::vector<Eigen::Matrix3d>
		CreateCameraMatrixPyramid(
		const PinholeCameraIntrinsic &camera_intrinsic, int levels)
{
	std::vector<Eigen::Matrix3d> pyramid_camera_matrix;
	pyramid_camera_matrix.reserve(levels);
	for (int i = 0; i < levels; i++) {
		Eigen::Matrix3d level_camera_matrix = i == 0 ?
				camera_intrinsic.intrinsic_matrix_ : 
				0.5 * pyramid_camera_matrix[i - 1];
		level_camera_matrix(2, 2) = 1.;
		pyramid_camera_matrix.push_back(level_camera_matrix);
	}
	return pyramid_camera_matrix;
}

Eigen::MatrixXd CreateInfomationMatrix(
		const Eigen::Matrix4d &odo,
		const PinholeCameraIntrinsic &camera_intrinsic,
		const Image &depth_s, const Image &depth_t,
		const OdometryOption &option)
{
	Eigen::Matrix4d odo_inv = odo.inverse();

	std::shared_ptr<CorrespondenceSetPixelWise> correspondence;
	correspondence = ComputeCorrespondence(camera_intrinsic.intrinsic_matrix_, 
			odo_inv, depth_s, depth_t, option);

	auto xyz_t = ConvertDepthImageToXYZImage(
			depth_t, camera_intrinsic.intrinsic_matrix_);

	// write q^*
	// see http://redwood-data.org/indoor/registration.html
	// note: I comes first and q_skew is scaled by factor 2.
	Eigen::MatrixXd GtG(6, 6);
	Eigen::MatrixXd G(3 * depth_s.height_ * depth_s.width_, 6);
	G.setConstant(0.0f);

	for (int row = 0; row < correspondence->size(); row++) {
		int u_s = (*correspondence)[row](0);
		int v_s = (*correspondence)[row](1);
		int u_t = (*correspondence)[row](2);
		int v_t = (*correspondence)[row](3);
		double x = *PointerAt<float>(*xyz_t, u_t, v_t, 0);
		double y = *PointerAt<float>(*xyz_t, u_t, v_t, 1);
		double z = *PointerAt<float>(*xyz_t, u_t, v_t, 2);
		G(3 * row + 0, 0) = 1.0;
		G(3 * row + 0, 4) = 2.0 * z;
		G(3 * row + 0, 5) = -2.0 * y;
		G(3 * row + 1, 1) = 1.0;
		G(3 * row + 1, 3) = -2.0 * z;
		G(3 * row + 1, 5) = 2.0 * x;
		G(3 * row + 2, 2) = 1.0;
		G(3 * row + 2, 3) = 2.0 * y;
		G(3 * row + 2, 4) = -2.0 * x;
	}
	GtG = G.transpose() * G;
	return GtG;
}

void NormalizeIntensity(Image &image_s, Image &image_t, 
		CorrespondenceSetPixelWise &correspondence)
{
	if (image_s.width_ != image_t.width_ ||
		image_s.height_ != image_t.height_) {
		PrintError("[NormalizeIntensity] Size of two input images should be same\n");
		return;
	}
	double mean_s = 0.0, mean_t = 0.0;
	for (int row = 0; row < correspondence.size(); row++) {
		int u_s = correspondence[row](0);
		int v_s = correspondence[row](1);
		int u_t = correspondence[row](2);
		int v_t = correspondence[row](3);
		mean_s += *PointerAt<float>(image_s, u_s, v_s);
		mean_t += *PointerAt<float>(image_t, u_t, v_t);
	}
	mean_s /= (double)correspondence.size();
	mean_t /= (double)correspondence.size();
	LinearTransformImage(image_s, 0.5 / mean_s, 0.0);
	LinearTransformImage(image_t, 0.5 / mean_t, 0.0);
}

inline std::shared_ptr<RGBDImage> PackRGBDImage(
	const Image &color, const Image &depth) {
	return std::make_shared<RGBDImage>(RGBDImage(color, depth));
}

std::shared_ptr<Image> PreprocessDepth(
		const Image &depth_orig, const OdometryOption &option) 
{
	std::shared_ptr<Image> depth_processed = std::make_shared<Image>();
	*depth_processed = depth_orig;
	for (int y = 0; y < depth_processed->height_; y++) {
		for (int x = 0; x < depth_processed->width_; x++) {
			float *p = PointerAt<float>(*depth_processed, x, y);
			if ((*p < option.min_depth_ || *p > option.max_depth_ || *p <= 0))
				*p = std::numeric_limits<float>::quiet_NaN();			
		}
	}
	return depth_processed;
}

inline bool CheckImagePair(const Image &image_s, const Image &image_t) 
{
	return (image_s.width_ == image_t.width_ && 
			image_s.height_ == image_t.height_);
}

inline bool CheckRGBDImagePair(const RGBDImage &source, const RGBDImage &target) 
{
	return (CheckImagePair(source.color_, target.color_) &&
			CheckImagePair(source.depth_, target.depth_) &&
			CheckImagePair(source.color_, source.depth_) &&
			CheckImagePair(target.color_, target.depth_) &&
			CheckImagePair(source.color_, target.color_) &&
			source.color_.num_of_channels_ == 1 &&
			source.depth_.num_of_channels_ == 1 &&
			target.color_.num_of_channels_ == 1 &&
			target.depth_.num_of_channels_ == 1 &&
			source.color_.bytes_per_channel_ == 4 && 
			target.color_.bytes_per_channel_ == 4 && 
			source.depth_.bytes_per_channel_ == 4 && 
			target.depth_.bytes_per_channel_ == 4);
}

std::tuple<std::shared_ptr<RGBDImage>, std::shared_ptr<RGBDImage>>
		InitializeRGBDOdometry(
		const RGBDImage &source, const RGBDImage &target,
		const PinholeCameraIntrinsic &camera_intrinsic,
		const Eigen::Matrix4d &odo_init,
		const OdometryOption &option) 
{
	auto source_gray = FilterImage(source.color_, FILTER_GAUSSIAN_3);
	auto target_gray = FilterImage(target.color_, FILTER_GAUSSIAN_3);
	auto source_depth_preprocessed = PreprocessDepth(source.depth_, option);
	auto target_depth_preprocessed = PreprocessDepth(target.depth_, option);
	auto source_depth = FilterImage(*source_depth_preprocessed, FILTER_GAUSSIAN_3);
	auto target_depth = FilterImage(*target_depth_preprocessed, FILTER_GAUSSIAN_3);

	std::shared_ptr<CorrespondenceSetPixelWise> correspondence;
	correspondence = ComputeCorrespondence(
			camera_intrinsic.intrinsic_matrix_, odo_init.inverse(), 
			*source_depth, *target_depth, option);
	PrintDebug("Number of correspondence is %d\n", correspondence->size());

	int corresps_count_required = (int)(source_gray->height_ *
			source_gray->width_ * option.minimum_correspondence_ratio_ + 0.5);
	if (correspondence->size() < corresps_count_required) {
		PrintWarning("[InitializeRGBDPair] Bad initial pose\n");
	}
	NormalizeIntensity(*source_gray, *target_gray, *correspondence);

	auto source_out = PackRGBDImage(*source_gray, *source_depth);
	auto target_out = PackRGBDImage(*target_gray, *target_depth);
	return std::make_tuple(source_out, target_out);
}

std::tuple<bool, Eigen::Matrix4d> DoSingleIteration(
	const RGBDImage &source, const RGBDImage &target, 
	const Image &source_xyz,
	const RGBDImage &target_dx, const RGBDImage &target_dy,
	const Eigen::Matrix3d camera_matrix,
	const Eigen::Matrix4d &init_odo,
	const RGBDOdometryJacobian &jacobian_method,
	const OdometryOption &option)
{
	std::shared_ptr<CorrespondenceSetPixelWise> correspondence;
	correspondence = ComputeCorrespondence(
			camera_matrix, init_odo.inverse(),
			source.depth_, target.depth_, option);
	int corresps_count_required = (int)(source.color_.height_ * 
			source.color_.width_ * option.minimum_correspondence_ratio_ + 0.5);
	if (correspondence->size() < corresps_count_required) {
		PrintError("[ComputeOdometry] %d is too fewer than mininum requirement %d\n",
				correspondence->size(), corresps_count_required);
		return std::make_tuple(false, Eigen::Matrix4d::Identity());
	}

	Eigen::MatrixXd J;
	Eigen::VectorXd r;
	std::tie(J, r) = jacobian_method.ComputeJacobian(
			source, target, source_xyz, target_dx, target_dy,
			init_odo, *correspondence,
			camera_matrix, option);
	
	bool is_success;
	Eigen::Matrix4d M;
	std::tie(is_success, M) = SolveLinearSystem(J, r);
	if (!is_success) {
		PrintError("[ComputeOdometry] no solution!\n");
		return std::make_tuple(false, Eigen::Matrix4d::Identity());
	} else {
		return std::make_tuple(true, M);
	}
}

std::tuple<bool, Eigen::Matrix4d> ComputeMultiscale(
		const RGBDImage &source, const RGBDImage &target,
		const PinholeCameraIntrinsic &camera_intrinsic,
		const Eigen::Matrix4d &init_odo,
		const RGBDOdometryJacobian &jacobian_method,
		const OdometryOption &option)
{
	std::vector<int> iter_counts = option.iteration_number_per_pyramid_level_;
	int num_levels = (int)iter_counts.size();

	auto source_pyramid = CreateRGBDImagePyramid(source, num_levels);
	auto target_pyramid = CreateRGBDImagePyramid(target, num_levels);
	auto target_pyramid_dx = FilterRGBDImagePyramid
			(target_pyramid, FILTER_SOBEL_3_DX);
	auto target_pyramid_dy = FilterRGBDImagePyramid
			(target_pyramid, FILTER_SOBEL_3_DY);

	Eigen::Matrix4d result_odo = init_odo.isZero() ?
			Eigen::Matrix4d::Identity() : init_odo;	

	std::vector<Eigen::Matrix3d> pyramid_camera_matrix =
			CreateCameraMatrixPyramid(camera_intrinsic, (int)iter_counts.size());

	for (int level = num_levels - 1; level >= 0; level--) {
		const Eigen::Matrix3d level_camera_matrix = pyramid_camera_matrix[level];

		auto source_xyz_level = ConvertDepthImageToXYZImage(
				source_pyramid[level]->depth_, level_camera_matrix);
		auto source_level = PackRGBDImage(source_pyramid[level]->color_,
				source_pyramid[level]->depth_);
		auto target_level = PackRGBDImage(target_pyramid[level]->color_, 
				target_pyramid[level]->depth_);
		auto target_dx_level = PackRGBDImage(target_pyramid_dx[level]->color_,
				target_pyramid_dx[level]->depth_);
		auto target_dy_level = PackRGBDImage(target_pyramid_dy[level]->color_,
				target_pyramid_dy[level]->depth_);
		
		for (int iter = 0; iter < iter_counts[num_levels - level - 1]; iter++) {
			PrintDebug("Iter : %d, Level : %d, ", iter, level);
			Eigen::Matrix4d curr_odo;
			bool is_success;
			std::tie(is_success, curr_odo) = DoSingleIteration(
				*source_level, *target_level, *source_xyz_level,
				*target_dx_level, *target_dy_level, level_camera_matrix,
				result_odo, jacobian_method, option);
			result_odo = curr_odo * result_odo;

			if (!is_success) {
				PrintError("[ComputeOdometry] no solution!\n");
				return std::make_tuple(false, Eigen::Matrix4d::Identity());
			}
		}
	}
	return std::make_tuple(true, result_odo);
}

}	// unnamed namespace

std::tuple<bool, Eigen::Matrix4d, Eigen::Matrix6d>
		ComputeRGBDOdometry(const RGBDImage &source, const RGBDImage &target,
		const PinholeCameraIntrinsic &camera_intrinsic /*= PinholeCameraIntrinsic()*/,
		const Eigen::Matrix4d &odo_init /*= Eigen::Matrix4d::Identity()*/,
		const RGBDOdometryJacobian &jacobian_method
		/*=RGBDOdometryJacobianfromHybridTerm*/,
		const OdometryOption &option /*= OdometryOption()*/)
{
	if (!CheckRGBDImagePair(source, target)) {
		PrintError("[RGBDOdometry] Two RGBD pairs should be same in size.\n");
		return std::make_tuple(false,
				Eigen::Matrix4d::Identity(), Eigen::Matrix6d::Zero());
	}

	std::shared_ptr<RGBDImage> source_processed, target_processed;
	std::tie(source_processed, target_processed) =
			InitializeRGBDOdometry(source, target, camera_intrinsic, odo_init, option);

	Eigen::Matrix4d odo;
	bool is_success;
	std::tie(is_success, odo) = ComputeMultiscale(
			*source_processed, *target_processed,
			camera_intrinsic, odo_init, jacobian_method, option);

	if (is_success) {
		Eigen::Matrix4d trans_output = odo.inverse();
		Eigen::MatrixXd info_output = CreateInfomationMatrix(odo, camera_intrinsic,
				source_processed->depth_, target_processed->depth_, option);
		return std::make_tuple(true, trans_output, info_output);
	}
	else {
		return std::make_tuple(false,
				Eigen::Matrix4d::Identity(), Eigen::Matrix6d::Zero());
	}
}

}	// namespace three
