/*
 * Copyright (c) 2013, Willow Garage, Inc.
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
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
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
 */

/* 
 * Author: Chad Rockey
 */

 #include <graft/GraftUKFAttitude.h>
 #include <ros/console.h>

 GraftUKFAttitude::GraftUKFAttitude() : diverged_(false){
	graft_state_.setZero();
	graft_state_(0,0) = 1.0; // Normalize quaternion
	graft_control_.setZero();
	graft_covariance_.setIdentity();
	Q_.setZero();
 }

GraftUKFAttitude::~GraftUKFAttitude(){

}

VectorXd verticalConcatenate(VectorXd& m, VectorXd& n){
	if(m.rows() == 0){
		return n;
	}
	VectorXd out;
	out.resize(m.rows()+n.rows(), m.cols());
	out << m,n;
	return out;
}

MatrixXd verticalConcatenate(MatrixXd& m, MatrixXd& n){
	if(m.rows() == 0){
		return n;
	}
	MatrixXd out;
	out.resize(m.rows()+n.rows(), m.cols());
	out << m,n;
	return out;
}

MatrixXd matrixSqrt(MatrixXd matrix){ ///< @TODO Make a reference?  MatrixXd vs templated....
	// Use LLT Cholesky decomposiion to create stable Matrix Sqrt
	return Eigen::LLT<Eigen::MatrixXd>(matrix).matrixL();
}

std::vector<MatrixXd > generateSigmaPoints(MatrixXd state, MatrixXd covariance, double lambda){
	std::vector<MatrixXd > out;

	double gamma = std::sqrt((state.rows()+lambda));
	MatrixXd sig_sqrt = gamma*matrixSqrt(covariance);

	// i = 0, push back state as is
	out.push_back(state);

	// i = 1,...,n
	for(size_t i = 1; i <= state.rows(); i++){
		out.push_back(state + sig_sqrt.col(i-1));
	}

	// i = n + 1,...,2n
	for(size_t i = state.rows() + 1; i <= 2*state.rows(); i++){
		out.push_back(state - sig_sqrt.col(i-(state.rows()+1)));
	}

	return out;
}

MatrixXd meanFromSigmaPoints(std::vector<MatrixXd >& sigma_points, double n, double lambda){
	double weight_zero = lambda / (n + lambda);
	MatrixXd out = weight_zero * sigma_points[0];
	double weight_i = 1.0/(2*(n + lambda));
	for(size_t i = 1; i <= 2*n; i++){
		out = out + weight_i * sigma_points[i];
	}
	return out;
}

///< @TODO Combined covariancesFromSigmaPoints with crossCovariance
MatrixXd covarianceFromSigmaPoints(std::vector<MatrixXd >& sigma_points, MatrixXd& mean, MatrixXd process_noise, double n, double alpha, double beta, double lambda){
	double cov_weight_zero = lambda / (n + lambda) + (1 - alpha*alpha + beta);
	MatrixXd out = cov_weight_zero * (sigma_points[0] - mean) * (sigma_points[0] - mean).transpose();
	double weight_i = 1.0/(2*(n + lambda));
	for(size_t i = 1; i <= 2*n; i++){
		out = out + weight_i  * (sigma_points[i] - mean) * (sigma_points[i] - mean).transpose();
	}
	return out+process_noise;
}

MatrixXd crossCovariance(std::vector<MatrixXd >& sigma_points, MatrixXd& mean, std::vector<MatrixXd >& meas_sigma_points, MatrixXd& meas_mean, double alpha, double beta, double lambda){
	double n = sigma_points[0].rows();
	double cov_weight_zero = lambda / (n + lambda) + (1 - alpha*alpha + beta);
	MatrixXd out = cov_weight_zero * (sigma_points[0] - mean) * (meas_sigma_points[0] - meas_mean).transpose();
	double weight_i = 1.0/(2*(n + lambda));
	for(size_t i = 1; i <= 2*n; i++){
		out = out + weight_i  * (sigma_points[i] - mean) * (meas_sigma_points[i] - meas_mean).transpose();
	}
	return out;
}

Matrix<double, 4, 4> quaternionUpdateMatrix(const double wx, const double wy, const double wz){
	Matrix<double, 4, 4> out;
	out <<   0,  wx,  wy,  wz,
	       -wx,   0, -wz,  wy,
	       -wy,  wz,   0, -wx,
	       -wz, -wy,  wx,   0;
   return out;
}

Matrix<double, 4, 1> unitQuaternion(const Matrix<double, 4, 1>& q){
	double q_mag = std::sqrt(q(0)*q(0) + q(1)*q(1) + q(2)*q(2) + q(3)*q(3));
	return q / q_mag;
}

Matrix<double, 4, 1> updatedQuaternion(const Matrix<double, 4, 1>& q, const double wx, const double wy, const double wz, double dt){
	Matrix<double, 4, 1> out;
	Matrix<double, 4, 4> I = Matrix<double, 4, 4>::Identity();
	double s = 1.0/2.0 * std::sqrt(wx*wx*dt*dt + wy*wy*dt*dt + wz*wz*dt*dt);
	double k = 0.0;
	double q_mag = std::sqrt(q(0)*q(0) + q(1)*q(1) + q(2)*q(2) + q(3)*q(3));
	double err = 1.0 - q_mag*q_mag;
	
	double correction_factor = 1 - 1.0/2.0*s*s + 1.0/24.0*s*s*s*s + k * dt * err; // Cosine taylor series
	MatrixXd correction = I*(correction_factor);
	double update_factor = 1.0/2.0*dt*(1.0 - 1.0/6.0*s*s + 1.0/120.0*s*s*s*s); // Sinc taylor series
	Matrix<double, 4, 4> quaterion_update_matrix = quaternionUpdateMatrix(wx, wy, wz);
	MatrixXd update = update_factor*quaterion_update_matrix;
	out = (correction-update)*q;


	double out_mag = std::sqrt(out(0)*out(0) + out(1)*out(1) + out(2)*out(2) + out(3)*out(3));
	double out_err = 1.0 - out_mag*out_mag;

	return out;
}

MatrixXd GraftUKFAttitude::f(MatrixXd x, double dt){
	Matrix<double, SIZE, 1> out;
	out.setZero();
	Matrix<double, 4, 1> new_q = updatedQuaternion(x.block(0, 0, 4, 1), x(4), x(5), x(6), dt);
	out.block(0, 0, 4, 1) = new_q;
	out(4) = x(4); // wx
	out(5) = x(5); // wy
	out(6) = x(6); // wz
	return out;
}

graft::GraftState::ConstPtr stateMsgFromMatrix(const MatrixXd& state){
	graft::GraftState::Ptr out(new graft::GraftState());
	out->pose.orientation.w = state(0);
	out->pose.orientation.x = state(1);
	out->pose.orientation.y = state(2);
	out->pose.orientation.z = state(3);
	out->twist.angular.x = state(4);
	out->twist.angular.y = state(5);
	out->twist.angular.z = state(6);
	return out;
}

std::vector<MatrixXd > GraftUKFAttitude::predict_sigma_points(std::vector<MatrixXd >& sigma_points, double dt){
	std::vector<MatrixXd > out;
	for(size_t i = 0; i < sigma_points.size(); i++){
		out.push_back(f(sigma_points[i], dt));
	}
	return out;
}

graft::GraftStatePtr GraftUKFAttitude::getMessageFromState(){
	return GraftUKFAttitude::getMessageFromState(graft_state_, graft_covariance_);
}

graft::GraftStatePtr GraftUKFAttitude::getMessageFromState(Matrix<double, SIZE, 1>& state, Matrix<double, SIZE, SIZE>& covariance){
	graft::GraftStatePtr msg(new graft::GraftState());
	msg->pose.orientation.w = state(0);
	msg->pose.orientation.x = state(1);
	msg->pose.orientation.y = state(2);
	msg->pose.orientation.z = state(3);
	msg->twist.angular.x = state(4);
	msg->twist.angular.y = state(5);
	msg->twist.angular.z = state(6);

	for(size_t i = 0; i < SIZE*SIZE; i++){
		msg->covariance[i] = covariance(i);
	}
	return msg;
}

VectorXd addElementToVector(const VectorXd& vec, const double element){
	if(vec.size() == 0){
		VectorXd out(1);
		out(0) = element;
		return out;
	}
	VectorXd out(vec.size() + 1);
	out << vec, element;
	return out;
}

MatrixXd addElementToColumnMatrix(const MatrixXd& mat, const double element){
	MatrixXd out(mat.rows() + 1, 1);
	MatrixXd small(1, 1);
	small(0,0) = element;
	if(mat.rows() == 0){
		return small;
	}
	out << mat, small;
	return out;
}

geometry_msgs::Vector3 normalized_acceleration(const geometry_msgs::Vector3& accel){
	geometry_msgs::Vector3 out;
	double mag = std::sqrt(accel.x*accel.x + accel.y*accel.y + accel.z*accel.z);
	out.x = accel.x / mag;
	out.y = accel.y / mag;
	out.z = accel.z / mag;
	return out;
}

// Returns measurement vector
VectorXd getMeasurements(const std::vector<boost::shared_ptr<GraftSensor> >& topics, const std::vector<MatrixXd>& predicted_sigma_points, std::vector<MatrixXd>& output_measurement_sigmas, MatrixXd& output_innovation_covariance){
	VectorXd actual_measurement;
	output_measurement_sigmas.clear();
	VectorXd innovation_covariance_diagonal;
	innovation_covariance_diagonal.resize(0);
	// Convert the predicted_sigma_points into messages
	std::vector<graft::GraftState::ConstPtr> predicted_sigma_msgs;
	for(size_t i = 0; i < predicted_sigma_points.size(); i++){
		predicted_sigma_msgs.push_back(stateMsgFromMatrix(predicted_sigma_points[i]));
		output_measurement_sigmas.push_back(MatrixXd());
	}

	
	// For each topic
	for(size_t i = 0; i < topics.size(); i++){
		// Get the measurement msg and covariance
		graft::GraftSensorResidual::ConstPtr meas = topics[i]->z();
		// Get the predicted measurements
		std::vector<graft::GraftSensorResidual::ConstPtr> residuals_msgs;
		for(size_t j = 0; j < predicted_sigma_msgs.size(); j++){
			graft::GraftSensorResidual::Ptr msg = topics[i]->h(*predicted_sigma_msgs[j]);
			residuals_msgs.push_back(msg);

		}
		// Assemble outputs for this topic
		if(meas == NULL){ // Timeout or not received or invalid, skip
			continue;
		}
		// Angular Velocity X
		if(meas->twist_covariance[21] > 1e-20){
			actual_measurement = addElementToVector(actual_measurement, meas->twist.angular.x);
			innovation_covariance_diagonal = addElementToVector(innovation_covariance_diagonal, meas->twist_covariance[21]);
			for(size_t j = 0; j < residuals_msgs.size(); j++){
				output_measurement_sigmas[j] = addElementToColumnMatrix(output_measurement_sigmas[j], residuals_msgs[j]->twist.angular.x);
			}
		}
		// Angular Velocity Y
		if(meas->twist_covariance[28] > 1e-20){
			actual_measurement = addElementToVector(actual_measurement, meas->twist.angular.y);
			innovation_covariance_diagonal = addElementToVector(innovation_covariance_diagonal, meas->twist_covariance[28]);
			for(size_t j = 0; j < residuals_msgs.size(); j++){
				output_measurement_sigmas[j] = addElementToColumnMatrix(output_measurement_sigmas[j], residuals_msgs[j]->twist.angular.y);
			}
		}
		// Angular Velocity Z
		if(meas->twist_covariance[35] > 1e-20){
			actual_measurement = addElementToVector(actual_measurement, meas->twist.angular.z);
			innovation_covariance_diagonal = addElementToVector(innovation_covariance_diagonal, meas->twist_covariance[35]);
			for(size_t j = 0; j < residuals_msgs.size(); j++){
				output_measurement_sigmas[j] = addElementToColumnMatrix(output_measurement_sigmas[j], residuals_msgs[j]->twist.angular.z);
			}
		}
		// Linear Acceleration
		if(meas->accel_covariance[0] > 1e-20 && meas->accel_covariance[4] > 1e-20 && meas->accel_covariance[8] > 1e-20){
			geometry_msgs::Vector3 accel_meas_norm = normalized_acceleration(meas->accel);
			actual_measurement = addElementToVector(actual_measurement, accel_meas_norm.x);
			actual_measurement = addElementToVector(actual_measurement, accel_meas_norm.y);
			actual_measurement = addElementToVector(actual_measurement, accel_meas_norm.z);
			innovation_covariance_diagonal = addElementToVector(innovation_covariance_diagonal, meas->accel_covariance[0]);
			innovation_covariance_diagonal = addElementToVector(innovation_covariance_diagonal, meas->accel_covariance[4]);
			innovation_covariance_diagonal = addElementToVector(innovation_covariance_diagonal, meas->accel_covariance[8]);
			for(size_t j = 0; j < residuals_msgs.size(); j++){
				geometry_msgs::Vector3 res_meas_norm = normalized_acceleration(residuals_msgs[j]->accel);
				output_measurement_sigmas[j] = addElementToColumnMatrix(output_measurement_sigmas[j], res_meas_norm.x);
				output_measurement_sigmas[j] = addElementToColumnMatrix(output_measurement_sigmas[j], res_meas_norm.y);
				output_measurement_sigmas[j] = addElementToColumnMatrix(output_measurement_sigmas[j], res_meas_norm.z);
			}
		}
	}

	// Convert covariance vector to matrix
	output_innovation_covariance = innovation_covariance_diagonal.asDiagonal();
	
	return actual_measurement;
}

void clearMessages(std::vector<boost::shared_ptr<GraftSensor> >& topics){
	for(size_t i = 0; i < topics.size(); i++){
		topics[i]->clearMessage();
	}
}

double GraftUKFAttitude::predictAndUpdate(){
	if(topics_.size() == 0 || topics_[0] == NULL){
		return 0;
	}
	if( diverged_ ) {
		return 0;
	}
	ros::Time t = ros::Time::now();
	double dt = (t - last_update_time_).toSec();
	if(last_update_time_.toSec() < 0.0001){ // No previous updates
		ROS_WARN("Negative dt, skipping update.");
		last_update_time_ = t;
		return 0.0;
	}
	last_update_time_ = t;

	// Prediction
	double lambda = alpha_*alpha_*(SIZE + kappa_) - SIZE;
	std::vector<MatrixXd > previous_sigma_points = generateSigmaPoints(graft_state_, graft_covariance_, lambda);
	std::vector<MatrixXd > predicted_sigma_points = predict_sigma_points(previous_sigma_points, dt);
	MatrixXd predicted_mean = meanFromSigmaPoints(predicted_sigma_points, graft_state_.rows(), lambda);
	MatrixXd predicted_covariance = covarianceFromSigmaPoints(predicted_sigma_points, predicted_mean, Q_, graft_state_.rows(), alpha_, beta_, lambda);

	// Update
	std::vector<MatrixXd> observation_sigma_points = generateSigmaPoints(predicted_mean, predicted_covariance, lambda);
	std::vector<MatrixXd> predicted_observation_sigma_points;
	MatrixXd measurement_noise;
	MatrixXd z = getMeasurements(topics_, observation_sigma_points, predicted_observation_sigma_points, measurement_noise);
	if(z.size() == 0){ // No measurements
		return 0.0;
	}
	MatrixXd predicted_measurement = meanFromSigmaPoints(predicted_observation_sigma_points, graft_state_.rows(), lambda);
	MatrixXd predicted_measurement_uncertainty = covarianceFromSigmaPoints(predicted_observation_sigma_points, predicted_measurement, measurement_noise, graft_state_.rows(), alpha_, beta_, lambda);
	MatrixXd cross_covariance = crossCovariance(observation_sigma_points, predicted_mean, predicted_observation_sigma_points, predicted_measurement, alpha_, beta_, lambda);
	MatrixXd K = cross_covariance * predicted_measurement_uncertainty.partialPivLu().inverse();
	graft_state_ = predicted_mean + K*(z - predicted_measurement);
	graft_state_.block(0, 0, 4, 1) = unitQuaternion(graft_state_.block(0, 0, 4, 1));
	graft_covariance_ = predicted_covariance - K*predicted_measurement_uncertainty*K.transpose();

	for( int i=0; i<SIZE; i++ ) {
		for( int j=0; j<SIZE; j++ ) {
			if( !std::isfinite(graft_covariance_(i, j)) ) {
				diverged_ = true;
			}
		}
	}
	if( diverged_ ) {
		// print offending messages
		std::stringstream errmsg;
		errmsg << "Covariance diverged! Offending topics are: ";

		// For each topic
		for(size_t i = 0; i < topics_.size(); i++){
			// Get the measurement msg and covariance
			graft::GraftSensorResidual::ConstPtr meas = topics_[i]->z();
			if( meas ) {
				if( i>0 ) errmsg << ", ";
				errmsg << topics_[i]->getName() << "(";
				errmsg << *meas << ")";
			}
		}

		ROS_ERROR_STREAM_THROTTLE(5.0, errmsg.str());
	}

	clearMessages(topics_);
	return dt;
}

void GraftUKFAttitude::setTopics(std::vector<boost::shared_ptr<GraftSensor> >& topics){
	topics_ = topics;
}

void GraftUKFAttitude::setInitialCovariance(std::vector<double>& P){
	graft_covariance_.setZero();
	size_t diagonal_size = std::sqrt(graft_covariance_.size());
	if(P.size() == graft_covariance_.size()){ // Full matrix
		for(size_t i = 0; i < P.size(); i++){
			graft_covariance_(i) = P[i];
		}
	} else if(P.size() == diagonal_size){ // Diagonal matrix
		for(size_t i = 0; i < P.size(); i++){
			graft_covariance_(i*(diagonal_size+1)) = P[i];
		}
	} else { // Not specified correctly
		ROS_ERROR("initial_covariance is size %zu, expected %zu.\nUsing Identity.\nThis probably won't work well.", P.size(), graft_covariance_.size());
		graft_covariance_.setIdentity();
	}
	std::cout << "cov:\n" << graft_covariance_ << std::endl;
}

void GraftUKFAttitude::setProcessNoise(std::vector<double>& Q){
	Q_.setZero();
	size_t diagonal_size = std::sqrt(Q_.size());
	if(Q.size() == Q_.size()){ // Full process nosie matrix
		for(size_t i = 0; i < Q.size(); i++){
			Q_(i) = Q[i];
		}
	} else if(Q.size() == diagonal_size){ // Diagonal matrix
		for(size_t i = 0; i < Q.size(); i++){
			Q_(i*(diagonal_size+1)) = Q[i];
		}
	} else { // Not specified correctly
		ROS_ERROR("Process noise parameter 'Q' is size %zu, expected %zu.\nUsing 0.1*Identity.\nThis probably won't work well.", Q.size(), Q_.size());
		Q_.setIdentity();
		Q_ = 0.1 * Q_;
	}
}

void GraftUKFAttitude::setAlpha(const double alpha){
	alpha_ = alpha;
}

void GraftUKFAttitude::setKappa(const double kappa){
	kappa_ = kappa;
}

void GraftUKFAttitude::setBeta(const double beta){
	beta_ = beta;
}
