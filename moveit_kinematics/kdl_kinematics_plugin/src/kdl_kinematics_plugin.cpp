/*********************************************************************
 * Software License Agreement (BSD License)
 *
 *  Copyright (c) 2012, Willow Garage, Inc.
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions
 *  are met:
 *
 *   * Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above
 *     copyright notice, this list of conditions and the following
 *     disclaimer in the documentation and/or other materials provided
 *     with the distribution.
 *   * Neither the name of Willow Garage nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 *  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 *  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 *  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 *  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *********************************************************************/

/* Author: Sachin Chitta, David Lu!!, Ugo Cupcic */

#include <moveit/kdl_kinematics_plugin/kdl_kinematics_plugin.h>
#include <moveit/kdl_kinematics_plugin/chainiksolver_vel_pinv_mimic.hpp>

#include <tf2_kdl/tf2_kdl.h>
#include <tf2/transform_datatypes.h>

#include <kdl_parser/kdl_parser.hpp>
#include <kdl/chainfksolverpos_recursive.hpp>
#include <kdl/frames_io.hpp>
#include <kdl/kinfam_io.hpp>

// URDF, SRDF
#include <urdf_model/model.h>
#include <srdfdom/model.h>

// register KDLKinematics as a KinematicsBase implementation
#include <class_loader/class_loader.hpp>
CLASS_LOADER_REGISTER_CLASS(kdl_kinematics_plugin::KDLKinematicsPlugin, kinematics::KinematicsBase)

namespace kdl_kinematics_plugin
{
KDLKinematicsPlugin::KDLKinematicsPlugin() : initialized_(false)
{
}

void KDLKinematicsPlugin::getRandomConfiguration(Eigen::VectorXd& jnt_array) const
{
  state_->setToRandomPositions(joint_model_group_);
  state_->copyJointGroupPositions(joint_model_group_, &jnt_array[0]);
}

void KDLKinematicsPlugin::getRandomConfiguration(const Eigen::VectorXd& seed_state,
                                                 const std::vector<double>& consistency_limits,
                                                 Eigen::VectorXd& jnt_array) const
{
  joint_model_group_->getVariableRandomPositionsNearBy(state_->getRandomNumberGenerator(), &jnt_array[0],
                                                       &seed_state[0], consistency_limits);
}

bool KDLKinematicsPlugin::checkConsistency(const Eigen::VectorXd& seed_state,
                                           const std::vector<double>& consistency_limits,
                                           const Eigen::VectorXd& solution) const
{
  for (std::size_t i = 0; i < dimension_; ++i)
    if (fabs(seed_state(i) - solution(i)) > consistency_limits[i])
      return false;
  return true;
}

bool KDLKinematicsPlugin::initialize(const moveit::core::RobotModel& robot_model, const std::string& group_name,
                                     const std::string& base_frame, const std::vector<std::string>& tip_frames,
                                     double search_discretization)
{
  storeValues(robot_model, group_name, base_frame, tip_frames, search_discretization);
  joint_model_group_ = robot_model_->getJointModelGroup(group_name);
  if (!joint_model_group_)
    return false;

  if (!joint_model_group_->isChain())
  {
    ROS_ERROR_NAMED("kdl", "Group '%s' is not a chain", group_name.c_str());
    return false;
  }
  if (!joint_model_group_->isSingleDOFJoints())
  {
    ROS_ERROR_NAMED("kdl", "Group '%s' includes joints that have more than 1 DOF", group_name.c_str());
    return false;
  }

  KDL::Tree kdl_tree;

  if (!kdl_parser::treeFromUrdfModel(*robot_model.getURDF(), kdl_tree))
  {
    ROS_ERROR_NAMED("kdl", "Could not initialize tree object");
    return false;
  }
  if (!kdl_tree.getChain(base_frame_, getTipFrame(), kdl_chain_))
  {
    ROS_ERROR_NAMED("kdl", "Could not initialize chain object");
    return false;
  }

  dimension_ = joint_model_group_->getActiveJointModels().size() + joint_model_group_->getMimicJointModels().size();
  for (std::size_t i = 0; i < joint_model_group_->getJointModels().size(); ++i)
  {
    if (joint_model_group_->getJointModels()[i]->getType() == moveit::core::JointModel::REVOLUTE ||
        joint_model_group_->getJointModels()[i]->getType() == moveit::core::JointModel::PRISMATIC)
    {
      solver_info_.joint_names.push_back(joint_model_group_->getJointModelNames()[i]);
      const std::vector<moveit_msgs::JointLimits>& jvec =
          joint_model_group_->getJointModels()[i]->getVariableBoundsMsg();
      solver_info_.limits.insert(solver_info_.limits.end(), jvec.begin(), jvec.end());
    }
  }

  if (!joint_model_group_->hasLinkModel(getTipFrame()))
  {
    ROS_ERROR_NAMED("kdl", "Could not find tip name in joint group '%s'", group_name.c_str());
    return false;
  }
  solver_info_.link_names.push_back(getTipFrame());

  joint_min_.resize(solver_info_.limits.size());
  joint_max_.resize(solver_info_.limits.size());

  for (unsigned int i = 0; i < solver_info_.limits.size(); i++)
  {
    joint_min_(i) = solver_info_.limits[i].min_position;
    joint_max_(i) = solver_info_.limits[i].max_position;
  }

  // Get Solver Parameters
  lookupParam("max_solver_iterations", max_solver_iterations_, 500);
  lookupParam("epsilon", epsilon_, 1e-5);
  lookupParam("position_only_ik", position_ik_, false);

  if (position_ik_)
    ROS_INFO_NAMED("kdl", "Using position only ik");

  // Check for mimic joints
  unsigned int joint_counter = 0;
  for (std::size_t i = 0; i < kdl_chain_.getNrOfSegments(); ++i)
  {
    const robot_model::JointModel* jm = robot_model_->getJointModel(kdl_chain_.segments[i].getJoint().getName());

    // first check whether it belongs to the set of active joints in the group
    if (jm->getMimic() == NULL && jm->getVariableCount() > 0)
    {
      JointMimic mimic_joint;
      mimic_joint.reset(joint_counter);
      mimic_joint.joint_name = kdl_chain_.segments[i].getJoint().getName();
      mimic_joint.active = true;
      mimic_joints_.push_back(mimic_joint);
      ++joint_counter;
      continue;
    }
    if (joint_model_group_->hasJointModel(jm->getName()))
    {
      if (jm->getMimic() && joint_model_group_->hasJointModel(jm->getMimic()->getName()))
      {
        JointMimic mimic_joint;
        mimic_joint.joint_name = kdl_chain_.segments[i].getJoint().getName();
        mimic_joint.offset = jm->getMimicOffset();
        mimic_joint.multiplier = jm->getMimicFactor();
        mimic_joints_.push_back(mimic_joint);
        continue;
      }
    }
  }
  for (std::size_t i = 0; i < mimic_joints_.size(); ++i)
  {
    if (!mimic_joints_[i].active)
    {
      const robot_model::JointModel* joint_model =
          joint_model_group_->getJointModel(mimic_joints_[i].joint_name)->getMimic();
      for (std::size_t j = 0; j < mimic_joints_.size(); ++j)
      {
        if (mimic_joints_[j].joint_name == joint_model->getName())
        {
          mimic_joints_[i].map_index = mimic_joints_[j].map_index;
        }
      }
    }
  }

  // Setup the joint state groups that we need
  state_.reset(new robot_state::RobotState(robot_model_));

  fk_solver_.reset(new KDL::ChainFkSolverPos_recursive(kdl_chain_));

  initialized_ = true;
  ROS_DEBUG_NAMED("kdl", "KDL solver initialized");
  return true;
}

bool KDLKinematicsPlugin::timedOut(const ros::WallTime& start_time, double duration) const
{
  return ((ros::WallTime::now() - start_time).toSec() >= duration);
}

bool KDLKinematicsPlugin::getPositionIK(const geometry_msgs::Pose& ik_pose, const std::vector<double>& ik_seed_state,
                                        std::vector<double>& solution, moveit_msgs::MoveItErrorCodes& error_code,
                                        const kinematics::KinematicsQueryOptions& options) const
{
  std::vector<double> consistency_limits;

  // limit search to a single attempt by setting a timeout of zero
  return searchPositionIK(ik_pose, ik_seed_state, 0.0, solution, IKCallbackFn(), error_code, consistency_limits,
                          options);
}

bool KDLKinematicsPlugin::searchPositionIK(const geometry_msgs::Pose& ik_pose, const std::vector<double>& ik_seed_state,
                                           double timeout, std::vector<double>& solution,
                                           moveit_msgs::MoveItErrorCodes& error_code,
                                           const kinematics::KinematicsQueryOptions& options) const
{
  std::vector<double> consistency_limits;

  return searchPositionIK(ik_pose, ik_seed_state, timeout, solution, IKCallbackFn(), error_code, consistency_limits,
                          options);
}

bool KDLKinematicsPlugin::searchPositionIK(const geometry_msgs::Pose& ik_pose, const std::vector<double>& ik_seed_state,
                                           double timeout, const std::vector<double>& consistency_limits,
                                           std::vector<double>& solution, moveit_msgs::MoveItErrorCodes& error_code,
                                           const kinematics::KinematicsQueryOptions& options) const
{
  return searchPositionIK(ik_pose, ik_seed_state, timeout, solution, IKCallbackFn(), error_code, consistency_limits,
                          options);
}

bool KDLKinematicsPlugin::searchPositionIK(const geometry_msgs::Pose& ik_pose, const std::vector<double>& ik_seed_state,
                                           double timeout, std::vector<double>& solution,
                                           const IKCallbackFn& solution_callback,
                                           moveit_msgs::MoveItErrorCodes& error_code,
                                           const kinematics::KinematicsQueryOptions& options) const
{
  std::vector<double> consistency_limits;
  return searchPositionIK(ik_pose, ik_seed_state, timeout, solution, solution_callback, error_code, consistency_limits,
                          options);
}

bool KDLKinematicsPlugin::searchPositionIK(const geometry_msgs::Pose& ik_pose, const std::vector<double>& ik_seed_state,
                                           double timeout, const std::vector<double>& consistency_limits,
                                           std::vector<double>& solution, const IKCallbackFn& solution_callback,
                                           moveit_msgs::MoveItErrorCodes& error_code,
                                           const kinematics::KinematicsQueryOptions& options) const
{
  return searchPositionIK(ik_pose, ik_seed_state, timeout, solution, solution_callback, error_code, consistency_limits,
                          options);
}

bool KDLKinematicsPlugin::searchPositionIK(const geometry_msgs::Pose& ik_pose, const std::vector<double>& ik_seed_state,
                                           double timeout, std::vector<double>& solution,
                                           const IKCallbackFn& solution_callback,
                                           moveit_msgs::MoveItErrorCodes& error_code,
                                           const std::vector<double>& consistency_limits,
                                           const kinematics::KinematicsQueryOptions& options) const
{
  ros::WallTime start_time = ros::WallTime::now();
  if (!initialized_)
  {
    ROS_ERROR_NAMED("kdl", "kinematics solver not initialized");
    error_code.val = error_code.NO_IK_SOLUTION;
    return false;
  }

  if (ik_seed_state.size() != dimension_)
  {
    ROS_ERROR_STREAM_NAMED("kdl", "Seed state must have size " << dimension_ << " instead of size "
                                                               << ik_seed_state.size());
    error_code.val = error_code.NO_IK_SOLUTION;
    return false;
  }

  if (!consistency_limits.empty() && consistency_limits.size() != dimension_)
  {
    ROS_ERROR_STREAM_NAMED("kdl", "Consistency limits be empty or must have size " << dimension_ << " instead of size "
                                                                                   << consistency_limits.size());
    error_code.val = error_code.NO_IK_SOLUTION;
    return false;
  }
  // Resize consistency limits to remove mimic joints
  std::vector<double> consistency_limits_mimic;
  if (!consistency_limits.empty())
  {
    for (std::size_t i = 0; i < dimension_; ++i)
    {
      if (!mimic_joints_[i].active)
        continue;
      consistency_limits_mimic.push_back(consistency_limits[i]);
    }
  }

  KDL::JntArray jnt_seed_state(dimension_);
  KDL::JntArray jnt_pos_in(dimension_);
  KDL::JntArray jnt_pos_out(dimension_);
  jnt_seed_state.data = Eigen::Map<const Eigen::VectorXd>(ik_seed_state.data(), ik_seed_state.size());
  jnt_pos_in = jnt_seed_state;

  KDL::ChainIkSolverVel_pinv_mimic ik_solver_vel(kdl_chain_, mimic_joints_, position_ik_);
  solution.resize(dimension_);

  KDL::Frame pose_desired;
  tf2::fromMsg(ik_pose, pose_desired);

  ROS_DEBUG_STREAM_NAMED("kdl", "searchPositionIK: Position request pose is "
                                    << ik_pose.position.x << " " << ik_pose.position.y << " " << ik_pose.position.z
                                    << " " << ik_pose.orientation.x << " " << ik_pose.orientation.y << " "
                                    << ik_pose.orientation.z << " " << ik_pose.orientation.w);

  unsigned int counter(0);
  while (true)
  {
    ++counter;
    if (counter > 1 && timedOut(start_time, timeout))  // timeout after first attempt only
    {
      ROS_DEBUG_STREAM_NAMED("kdl", "IK timed out after " << (ros::WallTime::now() - start_time).toSec() << " > "
                                                          << timeout << "s and " << counter - 1 << " attempts");
      error_code.val = error_code.TIMED_OUT;
      return false;
    }
    if (counter > 1)
      ROS_DEBUG_STREAM_NAMED("kdl", "New random configuration (" << counter << "): " << jnt_pos_in);

    int ik_valid = CartToJnt(ik_solver_vel, jnt_pos_in, pose_desired, jnt_pos_out, max_solver_iterations_);
    ROS_DEBUG_NAMED("kdl", "IK valid: %d", ik_valid);
    if (!consistency_limits_mimic.empty())
    {
      if ((ik_valid < 0 && !options.return_approximate_solution) ||
          !checkConsistency(jnt_seed_state.data, consistency_limits_mimic, jnt_pos_out.data))
      {
        ROS_DEBUG_STREAM_NAMED("kdl", "Could not find IK solution"
                                          << ((ik_valid < 0 && !options.return_approximate_solution) ?
                                                  "" :
                                                  ": does not match consistency limits"));
        getRandomConfiguration(jnt_seed_state.data, consistency_limits_mimic, jnt_pos_in.data);
        continue;
      }
    }
    else
    {
      if (ik_valid < 0 && !options.return_approximate_solution)
      {
        ROS_DEBUG_NAMED("kdl", "Could not find IK solution");
        getRandomConfiguration(jnt_pos_in.data);
        continue;
      }
    }
    ROS_DEBUG_NAMED("kdl", "Found IK solution");
    for (unsigned int j = 0; j < dimension_; j++)
      solution[j] = jnt_pos_out(j);
    if (!solution_callback.empty())
      solution_callback(ik_pose, solution, error_code);
    else
      error_code.val = error_code.SUCCESS;

    if (error_code.val == error_code.SUCCESS)
    {
      ROS_DEBUG_STREAM_NAMED("kdl", "Solved after " << (ros::WallTime::now() - start_time).toSec() << " < " << timeout
                                                    << "s and " << counter << " attempts");
      return true;
    }
  }
  ROS_DEBUG_NAMED("kdl", "An IK that satisifes the constraints and is collision free could not be found");
  error_code.val = error_code.NO_IK_SOLUTION;
  return false;
}

int KDLKinematicsPlugin::CartToJnt(KDL::ChainIkSolverVel& ik_solver, const KDL::JntArray& q_init,
                                   const KDL::Frame& p_in, KDL::JntArray& q_out, const unsigned int max_iter) const
{
  KDL::Frame f;
  KDL::Twist delta_twist;
  KDL::JntArray delta_q(q_out.rows());

  q_out = q_init;
  ROS_DEBUG_STREAM_NAMED("kdl", "Input: " << q_init);

  unsigned int i;
  for (i = 0; i < max_iter; ++i)
  {
    fk_solver_->JntToCart(q_out, f);
    delta_twist = diff(f, p_in);
    ROS_DEBUG_STREAM_NAMED("kdl", "delta_twist: " << delta_twist);

    if (position_ik_)
    {
      if (fabs(delta_twist(0)) < epsilon_ && fabs(delta_twist(1)) < epsilon_ && fabs(delta_twist(2)) < epsilon_)
        break;
    }
    else
    {
      if (KDL::Equal(delta_twist, KDL::Twist::Zero(), epsilon_))
        break;
    }

    ik_solver.CartToJnt(q_out, delta_twist, delta_q);
    KDL::Add(q_out, delta_q, q_out);

    ROS_DEBUG_STREAM_NAMED("kdl", "delta_q: " << delta_q);
    ROS_DEBUG_STREAM_NAMED("kdl", "q: " << q_out);

    for (std::size_t j = 0; j < joint_min_.rows(); ++j)
    {
      if (q_out(j) < joint_min_(j))
        q_out(j) = joint_min_(j);
    }
    for (std::size_t j = 0; j < joint_max_.rows(); ++j)
    {
      if (q_out(j) > joint_max_(j))
        q_out(j) = joint_max_(j);
    }
  }

  ROS_DEBUG_STREAM_NAMED("kdl", "Actual Solution (" << i << "): " << q_out);

  if (i != max_iter)
    return 0;
  else
    return -3;
}

bool KDLKinematicsPlugin::getPositionFK(const std::vector<std::string>& link_names,
                                        const std::vector<double>& joint_angles,
                                        std::vector<geometry_msgs::Pose>& poses) const
{
  if (!initialized_)
  {
    ROS_ERROR_NAMED("kdl", "kinematics solver not initialized");
    return false;
  }
  poses.resize(link_names.size());
  if (joint_angles.size() != dimension_)
  {
    ROS_ERROR_NAMED("kdl", "Joint angles vector must have size: %d", dimension_);
    return false;
  }

  KDL::Frame p_out;
  KDL::JntArray jnt_pos_in(dimension_);
  jnt_pos_in.data = Eigen::Map<const Eigen::VectorXd>(joint_angles.data(), joint_angles.size());

  bool valid = true;
  for (unsigned int i = 0; i < poses.size(); i++)
  {
    if (fk_solver_->JntToCart(jnt_pos_in, p_out) >= 0)
    {
      poses[i] = tf2::toMsg(p_out);
    }
    else
    {
      ROS_ERROR_NAMED("kdl", "Could not compute FK for %s", link_names[i].c_str());
      valid = false;
    }
  }
  return valid;
}

const std::vector<std::string>& KDLKinematicsPlugin::getJointNames() const
{
  return solver_info_.joint_names;
}

const std::vector<std::string>& KDLKinematicsPlugin::getLinkNames() const
{
  return solver_info_.link_names;
}

}  // namespace
