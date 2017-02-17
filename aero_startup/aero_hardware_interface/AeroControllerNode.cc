#include "aero_hardware_interface/AeroControllerNode.hh"

using namespace aero;
using namespace controller;

//////////////////////////////////////////////////
AeroControllerNode::AeroControllerNode(const ros::NodeHandle& _nh,
                                       const std::string& _port_upper,
                                       const std::string& _port_lower) :
  nh_(_nh), upper_(_port_upper), lower_(_port_lower), wheel_spinner_(1, &wheel_queue_),
  jointtraj_spinner_(1, &jointtraj_queue_)
{
  ROS_INFO("starting aero_hardware_interface");

  ROS_INFO(" create publisher");
  state_pub_ =
      nh_.advertise<pr2_controllers_msgs::JointTrajectoryControllerState>(
          "state", 10);

  ROS_INFO(" create stroke publisher");
  stroke_state_pub_ =
      nh_.advertise<pr2_controllers_msgs::JointTrajectoryControllerState>(
          "stroke_state", 10);

  ROS_INFO(" create error publisher");
  status_pub_ = nh_.advertise<std_msgs::Bool>("error", 10);

  // ROS_INFO(" create cmdvel sub");
  // cmdvel_sub_ =
  //     nh_.subscribe(
  //         "cmd_vel",
  //         10,
  //         &AeroControllerNode::GoVelocityCallback,
  //         this);

  // must be asynchronous or msgs will be dropped while running upper thread
  ROS_INFO(" create command sub");
  jointtraj_ops_ =
    ros::SubscribeOptions::create<trajectory_msgs::JointTrajectory>(
         "command",
         10,
         boost::bind(&AeroControllerNode::JointTrajectoryCallback, this, _1),
         ros::VoidPtr(),
         &jointtraj_queue_);
  jointtraj_sub_ = nh_.subscribe(jointtraj_ops_);    
  jointtraj_spinner_.start();

  ROS_INFO(" create wheel servo sub");
  wheel_servo_sub_ =
      nh_.subscribe(
          "wheel_servo",
          10,
          &AeroControllerNode::WheelServoCallback,
          this);

  ROS_INFO(" create wheel command sub");
  wheel_ops_ =
    ros::SubscribeOptions::create<trajectory_msgs::JointTrajectory>(
         "wheel_command",
         10,
         boost::bind(&AeroControllerNode::WheelCommandCallback, this, _1),
         ros::VoidPtr(),
         &wheel_queue_);
  wheel_sub_ = nh_.subscribe(wheel_ops_);
  wheel_spinner_.start();

  ROS_INFO(" create utility servo sub");
  util_sub_ =
      nh_.subscribe(
          "util_servo",
          10,
          &AeroControllerNode::UtilServoCallback,
          this);

  ROS_INFO(" create interpolation service");
  interpolation_server_ =
    nh_.advertiseService(
        "interpolation",
        &AeroControllerNode::InterpolationCallback,
        this);

  ROS_INFO(" create hand_control sub");
  util_sub_ =
      nh_.subscribe(
          "hand_control",
          10,
          &AeroControllerNode::HandScriptCallback,
          this);

  ROS_INFO(" create send joints service");
  send_joints_server_ =
    nh_.advertiseService(
        "send_joints",
        &AeroControllerNode::SendJointsCallback,
        this);

  ROS_INFO(" create get joints service");
  get_joints_server_ =
    nh_.advertiseService(
        "get_joints",
        &AeroControllerNode::GetJointsCallback,
        this);

  ROS_INFO(" create status reset sub");
  status_reset_sub_ =
      nh_.subscribe(
          "reset_status",
          10,
          &AeroControllerNode::StatusResetCallback,
          this);

  bool get_state = true;
  nh_.param<bool> ("get_state", get_state, true);

  if (get_state) {
    ROS_INFO(" create timer sub");
	//shiigi 0.1 -> 0.01
    timer_ =
        nh_.createTimer(ros::Duration(0.01),
                            &AeroControllerNode::JointStateCallback, this);
  } else {
    ROS_INFO(" controller DO NOT return state.");
    this->JointStateOnce();
  }

  global_thread_cnt_ = 0;

  ROS_INFO(" done");
}

//////////////////////////////////////////////////
AeroControllerNode::~AeroControllerNode()
{
}

//////////////////////////////////////////////////
// void AeroControllerNode::GoVelocityCallback(
//     const geometry_msgs::Twist::ConstPtr& _msg)
// {
//   boost::mutex::scoped_lock lock(mtx_);
//   lower_.flush();
//   usleep(1000 * 10);
// }

//////////////////////////////////////////////////
void AeroControllerNode::JointTrajectoryThread(
    std::vector<aero::interpolation::InterpolationPtr> _interpolation,
    std::vector<std::pair<std::vector<int16_t>, uint16_t> > _stroke_trajectory,
    int _trajectory_start_from,
    int _split_start_from)
{
  uint16_t csec_per_frame = 10; // 10 fps
  uint this_id;

  // register current thread
  mtx_threads_.lock();
  std::vector<int> joints;
  for (size_t i = 0; i < _stroke_trajectory[0].first.size(); ++i)
    if (_stroke_trajectory[0].first.at(i) != 0x7fff)
      joints.push_back(static_cast<int>(i));
  registered_threads_.push_back({global_thread_cnt_, joints, false});
  this_id = global_thread_cnt_++;
  mtx_threads_.unlock();

  // main process starts here

  for (auto it = _stroke_trajectory.begin() + _trajectory_start_from;
       it != _stroke_trajectory.end(); ++it) {
    // check if any kill signal was provided to current thread
    mtx_threads_.lock();
    for (auto th = registered_threads_.begin(); th != registered_threads_.end(); ++th)
      if (th->id == this_id) {
        if (th->kill) {
          // save info of killing thread
          mtx_thread_graveyard_.lock();
          thread_graveyard_.push_back({_stroke_trajectory, _interpolation,
                it - _stroke_trajectory.begin(), 1, this_id});
          mtx_thread_graveyard_.unlock();
          // remove this thread info
          registered_threads_.erase(th);
          mtx_threads_.unlock();
          return;
        }
        break;
      }
    mtx_threads_.unlock();

    int k = static_cast<int>(it - _stroke_trajectory.begin());

    // from here, main process for trajectory it
    // any movement faster than 100ms(10cs) will not interpolate = linear
    if (it->second < 10
	|| _interpolation.at(k)->is(aero::interpolation::i_constant)
	) {
      mtx_upper_.lock();
      upper_.set_position(it->first, it->second - (it-1)->second);
      mtx_upper_.unlock();
      continue;
    }
    // find number of splits in this trajectory
    // time becomes slightly faster if not cleanly dividable
    int splits = static_cast<int>((it->second - (it-1)->second) / csec_per_frame);
    // send splitted stroke
    for (size_t j = _split_start_from; j <= splits; ++j) {
      // check if any kill signal was provided to current thread
      mtx_threads_.lock();
      for (auto th = registered_threads_.begin();
           th != registered_threads_.end(); ++th)
        if (th->id == this_id) {
          if (th->kill) {
            // save info of killing thread
            mtx_thread_graveyard_.lock();
            thread_graveyard_.push_back({_stroke_trajectory,
                  _interpolation, k, j, this_id});
            mtx_thread_graveyard_.unlock();
            // remove this thread info
            registered_threads_.erase(th);
            mtx_threads_.unlock();
            return;
          }
          break;
        }
      mtx_threads_.unlock();

      // from here, main process for split j
      float t_param = _interpolation.at(k)->interpolate(
          static_cast<float>(j) / splits);
      // calculate stroke in this split
      std::vector<int16_t> stroke(it->first.size(), 0x7fff);
      for (size_t i = 0; i < it->first.size(); ++i) {
        if (it->first[i] == 0x7fff) continue; // skip non-send joints
        stroke[i] =
          (1 - t_param) * (it - 1)->first[i] + t_param * it->first[i];
      }
      // slightly longer time added for trajectory smoothness
      mtx_upper_.lock();
      upper_.set_position(stroke, csec_per_frame + 10);
      mtx_upper_.unlock();
      // 20ms sleep in set_position, subtract
      usleep(static_cast<int32_t>(csec_per_frame * 10.0 * 1000.0 - 20000.0));
    } // splits
    _split_start_from = 1;
  } // _stroke_trajectory

  // remove finished thread
  mtx_threads_.lock();
  for (auto th = registered_threads_.begin(); th != registered_threads_.end(); ++th)
    if (th->id == this_id) {
      registered_threads_.erase(th);
      break;
    }
  mtx_threads_.unlock();
}

//////////////////////////////////////////////////
void AeroControllerNode::JointTrajectoryCallback(
    const trajectory_msgs::JointTrajectory::ConstPtr& _msg)
{
  mtx_upper_.lock();
  mtx_lower_.lock();

  int number_of_angle_joints =
      upper_.get_number_of_angle_joints() +
      lower_.get_number_of_angle_joints();

  if (_msg->joint_names.size() > number_of_angle_joints) {
    // invalid number of joints from _msg
    mtx_upper_.unlock();
    mtx_lower_.unlock();
    return;
  }

  // positions in _msg are not ordered
  std::vector<int32_t> id_in_msg_to_ordered_id;
  id_in_msg_to_ordered_id.resize(_msg->joint_names.size());

  // if count is 0, commands will not go to robot
  //   used for occasions such as controlling only the upper body
  //   this way, controlling is safer
  size_t upper_count = 0;
  size_t lower_count = 0;

  // check for unused joints
  std::vector<bool> send_true(number_of_angle_joints, false);

  for (size_t i = 0; i < _msg->joint_names.size(); ++i) {
    // try finding name from upper_
    id_in_msg_to_ordered_id[i] =
        upper_.get_ordered_angle_id(_msg->joint_names[i]);

    // if finding name from upper_ failed
    if (id_in_msg_to_ordered_id[i] < 0) {
      // try finding name from lower_
      id_in_msg_to_ordered_id[i] =
        lower_.get_ordered_angle_id(_msg->joint_names[i]);
    } else {
      ++upper_count;
      send_true[id_in_msg_to_ordered_id[i]] = true;
      continue;
    }

    if (id_in_msg_to_ordered_id[i] < 0) {
      mtx_upper_.unlock();
      mtx_lower_.unlock();
      return; // invalid name
    }

    ++lower_count;
    send_true[id_in_msg_to_ordered_id[i]] = true;
  }

  // initiate trajectories

  std::vector<std::pair<std::vector<int16_t>, uint16_t> > upper_stroke_trajectory;

  // note: only upper will have interpolation
  if (upper_count > 0) {
    upper_stroke_trajectory.reserve(_msg->points.size() + 1);
    // get current stroke values, use reference for safety
    std::vector<int16_t> ref_strokes = upper_.get_reference_stroke_vector();
    // fill in unused joints to no-send
    common::UnusedAngle2Stroke(ref_strokes, send_true);
    upper_stroke_trajectory.push_back({ref_strokes, 0});
  }

  // from here, get ready to handle the _msg positions

  // this is a tmp variable that is reused
  std::vector<double> ordered_positions(number_of_angle_joints);

  // for each trajectory points,
  for (size_t i = 0; i < _msg->points.size(); ++i) {
    std::fill(ordered_positions.begin(), ordered_positions.end(), 0.0);

    // check for cancelling joints (NaN values)
    std::vector<bool> send_true_with_cancel;
    send_true_with_cancel.assign(send_true.begin(), send_true.end());

    for (size_t j = 0; j < _msg->points[i].positions.size(); ++j)
      if (!std::isnan(_msg->points[i].positions[j]))
        ordered_positions[id_in_msg_to_ordered_id[j]] =
          _msg->points[i].positions[j];
      else
        send_true_with_cancel[id_in_msg_to_ordered_id[j]] = false;

    std::vector<int16_t> strokes(AERO_DOF);
    common::Angle2Stroke(strokes, ordered_positions);

    // fill in unused joints to no-send
    common::UnusedAngle2Stroke(strokes, send_true_with_cancel);

    // split strokes into upper and lower
    std::vector<int16_t> upper_stroke_vector(
	strokes.begin(), strokes.begin() + AERO_DOF_UPPER);
    std::vector<int16_t> lower_stroke_vector(
	strokes.begin() + AERO_DOF_UPPER, strokes.end());

    double time_sec = _msg->points[i].time_from_start.toSec();
    uint16_t time_csec = static_cast<uint16_t>(time_sec * 100.0);

    if (upper_count > 0) {
      upper_stroke_trajectory.push_back({upper_stroke_vector, time_csec});
    }

    // only the first lower trajectory point is valid
    if (lower_count > 0 && i == 0) {
      lower_.set_position(lower_stroke_vector, time_csec);
    }
  }

  mtx_upper_.unlock();
  mtx_lower_.unlock();

  if (upper_count <= 0) return; // nothing more to do

  std::vector<aero::interpolation::InterpolationPtr> interpolation;
  interpolation.reserve(upper_stroke_trajectory.size());
  // fill in dummy setup in head
  interpolation.push_back(std::shared_ptr<aero::interpolation::Interpolation>(
        new aero::interpolation::Interpolation(aero::interpolation::i_constant)));
  // copy interpolation setup
  mtx_intrpl_.lock();
  for (auto it = interpolation_.begin(); it != interpolation_.end(); ++it)
    interpolation.push_back(*it);
  // fillin rest with linear if not specified
  for (size_t i = interpolation_.size(); i < upper_stroke_trajectory.size(); ++i)
    interpolation.push_back(std::shared_ptr<aero::interpolation::Interpolation>(
        new aero::interpolation::Interpolation(aero::interpolation::i_linear)));
  mtx_intrpl_.unlock();

  // start new thread if no other thread is running
  mtx_threads_.lock();
  if (registered_threads_.size() == 0) {
    mtx_threads_.unlock();
    std::thread send_av([&](
        std::vector<aero::interpolation::InterpolationPtr> _interpolation,
        std::vector<std::pair<std::vector<int16_t>, uint16_t> > _stroke_trajectory)
    {
      JointTrajectoryThread(_interpolation, _stroke_trajectory, 1, 1);
    }, interpolation, upper_stroke_trajectory);
    send_av.detach();
    return;
  }
  mtx_threads_.unlock();

  // check if any of current joint is already running in one of the threads
  std::vector<std::pair<int, uint> > conflict_joints;
  mtx_threads_.lock();
  for (size_t i = 0; i < upper_stroke_trajectory[1].first.size(); ++i) {
    if (upper_stroke_trajectory[1].first.at(i) == 0x7fff &&
        upper_stroke_trajectory[0].first.at(i) == 0x7fff) // **
      continue;
    // ** if [1] == 0x7fff but [0] != 0x7fff : the 0x7fff in [1] means cancel
    //    this distinguishes unused joints to cancelled joints
    for (auto th = registered_threads_.begin(); th != registered_threads_.end(); ++th)
      for (auto j = th->joints.begin(); j != th->joints.end(); ++j)
        if (i == *j) conflict_joints.push_back({static_cast<int>(i), th->id});
  }
  mtx_threads_.unlock();

  // start new thread if there is no conflict
  if (conflict_joints.size() == 0) {
    std::thread send_av([&](
        std::vector<aero::interpolation::InterpolationPtr> _interpolation,
        std::vector<std::pair<std::vector<int16_t>, uint16_t> > _stroke_trajectory)
    {
      JointTrajectoryThread(_interpolation, _stroke_trajectory, 1, 1);
    }, interpolation, upper_stroke_trajectory);
    send_av.detach();
    return;
  }

  // if there is a thread conflict, thread remapping must be conducted
  // find and count the threads to be killed
  std::vector<int> kill_thread_id;
  for (auto it = conflict_joints.begin(); it != conflict_joints.end(); ++it) {
    auto in_list = std::find(kill_thread_id.begin(), kill_thread_id.end(), it->second);
    if (in_list == kill_thread_id.end()) { // kill thread if not killed yet
      mtx_threads_.lock();
      for (auto th = registered_threads_.begin(); th != registered_threads_.end(); ++th)
        if (it->second == th->id) {
          th->kill = true; // register as kill
          kill_thread_id.push_back(it->second); // save already set to kill thread
          mtx_threads_.unlock();
          break;
        }
      mtx_threads_.unlock();
    }
  }

  auto time_begin = std::chrono::high_resolution_clock::now();

  // wait till corresponding threads are killed
  while (1) {
    mtx_thread_graveyard_.lock();
    if (thread_graveyard_.size() == kill_thread_id.size() ||
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - time_begin).count()
        > 500) { // a thread might have finished before kill
      mtx_thread_graveyard_.unlock();
      break;
    }
    mtx_thread_graveyard_.unlock();
    usleep(10 * 1000); // sleep 10 ms and wait for update
  }

  mtx_thread_graveyard_.lock();
  // remap use joint info
  for (auto it = conflict_joints.begin(); it != conflict_joints.end(); ++it)
    for (auto th = thread_graveyard_.begin(); th != thread_graveyard_.end(); ++th)
      if (it->second == th->thread_id) {
        for (auto traj = th->trajectories.begin();
             traj != th->trajectories.end(); ++traj)
          traj->first.at(it->first) = 0x7fff;
        break;
      }
  // create new threads
  std::vector<std::thread> new_threads;
  // add this msg
  new_threads.push_back(std::thread([&](
        std::vector<aero::interpolation::InterpolationPtr> _interpolation,
        std::vector<std::pair<std::vector<int16_t>, uint16_t> > _stroke_trajectory)
    {
      JointTrajectoryThread(_interpolation, _stroke_trajectory, 1, 1);
    }, interpolation, upper_stroke_trajectory));
  // add previously running threads that were remapped
  for (auto th = thread_graveyard_.begin(); th != thread_graveyard_.end(); ++th) {
    int unused_joint_num = 0;
    for (auto j = th->trajectories[0].first.begin();
         j != th->trajectories[0].first.end(); ++j)
      if (*j == 0x7fff) ++unused_joint_num;
    // if remapped trajectory is still valid (= has moving joints)
    if (unused_joint_num != th->trajectories[0].first.size()) {
      new_threads.push_back(std::thread([&](
        std::vector<aero::interpolation::InterpolationPtr> _interpolation,
        std::vector<std::pair<std::vector<int16_t>, uint16_t> > _stroke_trajectory,
        int _trajectory_start_from,
        int _split_start_from)
      {
        JointTrajectoryThread(_interpolation, _stroke_trajectory,
                              _trajectory_start_from, _split_start_from);
      }, th->interpolation, th->trajectories, th->at_trajectory_num, th->at_split_num));
    }
  }
  // free graveyard
  thread_graveyard_.clear();
  mtx_thread_graveyard_.unlock();

  // start threads
  std::for_each(
      new_threads.begin(), new_threads.end(), [](std::thread& t){ t.detach(); });
}

//////////////////////////////////////////////////
void AeroControllerNode::JointStateCallback(const ros::TimerEvent& event)
{
  this->JointStateOnce();
}

//////////////////////////////////////////////////
void AeroControllerNode::JointStateOnce()
{
  mtx_upper_.lock();
  mtx_lower_.lock();

  // get desired positions
  std::vector<int16_t> upper_ref_vector =
      upper_.get_reference_stroke_vector();
  std::vector<int16_t> lower_ref_vector =
      lower_.get_reference_stroke_vector();

  // update current position when upper body is not being controlled
  // when upper body is controlled, current position is auto-updated
  mtx_threads_.lock();
  if (registered_threads_.size() == 0) {
    // commands take 20ms sleep, threading to save time
    std::thread t1([&](){
        upper_.update_position();
      });
    std::thread t2([&](){
        lower_.update_position();
      });
    t1.join();
    t2.join();
  }
  mtx_threads_.unlock();

  // get upper actual positions
  std::vector<int16_t> upper_stroke_vector =
    upper_.get_actual_stroke_vector();

  // get lower actual positions
  std::vector<int16_t> lower_stroke_vector =
    lower_.get_actual_stroke_vector();

  // first handle the stroke publisher
  // in this way, we don't have to concatenate new vectors
  pr2_controllers_msgs::JointTrajectoryControllerState stroke_state;
  stroke_state.header.stamp = ros::Time::now();
  stroke_state.joint_names.resize(AERO_DOF);
  stroke_state.desired.positions.resize(AERO_DOF);
  stroke_state.actual.positions.resize(AERO_DOF);

  if (upper_ref_vector.size() < AERO_DOF_UPPER || upper_stroke_vector.size() < AERO_DOF_UPPER)
    for (size_t i = 0; i < AERO_DOF_UPPER; ++i) {
      stroke_state.joint_names[i] = upper_.get_stroke_joint_name(i);
      stroke_state.desired.positions[i] = 0.0;
      stroke_state.actual.positions[i] = 0.0;
    }
  else // usually should enter else, enters if when port is not activated
    for (size_t i = 0; i < AERO_DOF_UPPER; ++i) {
      stroke_state.joint_names[i] = upper_.get_stroke_joint_name(i);
      stroke_state.desired.positions[i] =
          static_cast<double>(upper_ref_vector[i]);
      stroke_state.actual.positions[i] =
          static_cast<double>(upper_stroke_vector[i]);
    }

  if (lower_ref_vector.size() < AERO_DOF_LOWER || lower_stroke_vector.size() < AERO_DOF_LOWER)
    for (size_t i = 0; i < AERO_DOF_LOWER; ++i) {
      stroke_state.joint_names[i + AERO_DOF_UPPER] =
          lower_.get_stroke_joint_name(i);
      stroke_state.desired.positions[i + AERO_DOF_UPPER] = 0.0;
      stroke_state.actual.positions[i + AERO_DOF_UPPER] = 0.0;
    }
  else // usually should enter else, enters if when port is not activated
    for (size_t i = 0; i < AERO_DOF_LOWER; ++i) {
      stroke_state.joint_names[i + AERO_DOF_UPPER] =
          lower_.get_stroke_joint_name(i);
      stroke_state.desired.positions[i + AERO_DOF_UPPER] =
          static_cast<double>(lower_ref_vector[i]);
      stroke_state.actual.positions[i + AERO_DOF_UPPER] =
          static_cast<double>(lower_stroke_vector[i]);
    }

  int number_of_angle_joints =
      upper_.get_number_of_angle_joints() +
      lower_.get_number_of_angle_joints();

  pr2_controllers_msgs::JointTrajectoryControllerState state;
  state.header.stamp = ros::Time::now();
  state.joint_names.resize(number_of_angle_joints);
  state.desired.positions.resize(number_of_angle_joints);
  state.actual.positions.resize(number_of_angle_joints);

  // convert to desired angle positions
  std::vector<int16_t> desired_strokes(stroke_state.desired.positions.begin(),
                                       stroke_state.desired.positions.end());
  common::Stroke2Angle(state.desired.positions, desired_strokes);

  // convert to actual angle positions
  std::vector<int16_t> actual_strokes(stroke_state.actual.positions.begin(),
                                      stroke_state.actual.positions.end());
  common::Stroke2Angle(state.actual.positions, actual_strokes);

  // get joint names (auto-generated function)
  common::AngleJointNames(state.joint_names);

  // get status
  std_msgs::Bool status_flag;
  status_flag.data = upper_.get_status() || lower_.get_status();
  status_pub_.publish(status_flag);

  state_pub_.publish(state);
  stroke_state_pub_.publish(stroke_state);

  mtx_upper_.unlock();
  mtx_lower_.unlock();
}

//////////////////////////////////////////////////
void AeroControllerNode::WheelServoCallback(
    const std_msgs::Bool::ConstPtr& _msg)
{
  mtx_lower_.lock();

  if (_msg->data) {
    // wheel_on sets all joints and wheels to servo on
    lower_.wheel_on();
  } else {
    // servo_on joints only, and servo off wheels
    lower_.servo_on();
  }

  mtx_lower_.unlock();
}

//////////////////////////////////////////////////
void AeroControllerNode::StatusResetCallback(
    const std_msgs::Empty::ConstPtr& _msg)
{
  mtx_upper_.lock();
  mtx_lower_.lock();

  upper_.reset_status();
  lower_.reset_status();

  mtx_upper_.unlock();
  mtx_lower_.unlock();
}

//////////////////////////////////////////////////
void AeroControllerNode::WheelCommandCallback(
    const trajectory_msgs::JointTrajectory::ConstPtr& _msg)
{
  mtx_lower_.lock();

  // wheel name to indices, if not exist, then return -1
  std::vector<int32_t> joint_to_wheel_indices(AERO_DOF_WHEEL);
  for (size_t i = 0; i < _msg->joint_names.size(); ++i) {
    std::string joint_name = _msg->joint_names[i];
    joint_to_wheel_indices[i] =
        lower_.get_wheel_id(joint_name);
  }

  // set previous velocity
  std::vector<int16_t> wheel_vector;
  std::vector<int16_t>& ref_vector =
      lower_.get_reference_wheel_vector();
  wheel_vector.assign(ref_vector.begin(), ref_vector.end());

  // for each trajectory points,
  for (size_t i = 0; i < _msg->points.size(); ++i) {
    // convert positions to stroke vector
    for (size_t j = 0; j < _msg->points[i].positions.size(); ++j) {
      if (joint_to_wheel_indices[j] >= 0) {
        wheel_vector[
            static_cast<size_t>(joint_to_wheel_indices[j])] =
            static_cast<int16_t>(_msg->points[i].positions[j]);
      }
    }

    double time_sec = _msg->points[i].time_from_start.toSec();
    uint16_t time_csec = static_cast<uint16_t>(time_sec * 100.0);
    lower_.set_wheel_velocity(wheel_vector, time_csec);
    // usleep(static_cast<int32_t>(time_sec * 1000.0 * 1000.0));
  }

  mtx_lower_.unlock();
}

//////////////////////////////////////////////////
void AeroControllerNode::UtilServoCallback(
    const std_msgs::Int32::ConstPtr& _msg)
{
  mtx_upper_.lock();

  if (_msg->data == 0) {
    usleep(static_cast<int32_t>(200.0 * 1000.0));
    upper_.util_servo_off();
    usleep(static_cast<int32_t>(200.0 * 1000.0));
  } else {
    usleep(static_cast<int32_t>(200.0 * 1000.0));
    upper_.util_servo_on();
    usleep(static_cast<int32_t>(200.0 * 1000.0));
  }

  mtx_upper_.unlock();
}

//////////////////////////////////////////////////
bool AeroControllerNode::InterpolationCallback(
    aero_startup::AeroInterpolation::Request &_req,
    aero_startup::AeroInterpolation::Response &_res)
{
  _res.status = true;

  mtx_intrpl_.lock();
  interpolation_.clear();

  for (auto it = _req.type.begin(); it != _req.type.end(); ++it) {
    interpolation_.push_back(std::shared_ptr<aero::interpolation::Interpolation>(
        new aero::interpolation::Interpolation(*it)));
  }

  int i = 0;
  for (auto it = _req.p.begin(); it != _req.p.end(); ++it) {
    if (i > interpolation_.size()) {
      _res.status = false;
      return true;
    }
    int j = 0;
    for (auto itt = it->points.begin(); itt != it->points.end(); ++itt) {
      interpolation_.at(i)->set_points({itt->x, itt->y}, j);
      ++j;
    }
    ++i;
  }
  mtx_intrpl_.unlock();

  return true;
}

//////////////////////////////////////////////////
void AeroControllerNode::HandScriptCallback(
    const std_msgs::Int16MultiArray::ConstPtr& _msg)
{
  mtx_upper_.lock();
    upper_.Hand_Script(_msg->data[0],_msg->data[1]);
  mtx_upper_.unlock();
}

//////////////////////////////////////////////////
bool AeroControllerNode::SendJointsCallback(
    aero_startup::AeroSendJoints::Request &_req,
    aero_startup::AeroSendJoints::Response &_res)
{
  mtx_upper_.lock();
  mtx_lower_.lock();

  int number_of_angle_joints =
      upper_.get_number_of_angle_joints() +
      lower_.get_number_of_angle_joints();

  if (_req.joint_names.size() > number_of_angle_joints) {
    // invalid number of joints from _req
    mtx_upper_.unlock();
    mtx_lower_.unlock();
    return false;
  }

  // positions in _req are not ordered
  std::vector<int32_t> id_in_req_to_ordered_id;
  id_in_req_to_ordered_id.resize(_req.joint_names.size());

  // if count is 0, commands will not go to robot
  //   used for occasions such as controlling only the upper body
  //   this way, controlling is safer
  size_t upper_count = 0;
  size_t lower_count = 0;

  // check for unused joints
  std::vector<bool> send_true(number_of_angle_joints, false);

  for (size_t i = 0; i < _req.joint_names.size(); ++i) {
    // try finding name from upper_
    id_in_req_to_ordered_id[i] =
        upper_.get_ordered_angle_id(_req.joint_names[i]);

    // if finding name from upper_ failed
    if (id_in_req_to_ordered_id[i] < 0) {
      // try finding name from lower_
      id_in_req_to_ordered_id[i] =
        lower_.get_ordered_angle_id(_req.joint_names[i]);
    } else {
      ++upper_count;
      send_true[id_in_req_to_ordered_id[i]] = true;
      continue;
    }

    if (id_in_req_to_ordered_id[i] < 0) {
      mtx_upper_.unlock();
      mtx_lower_.unlock();
      return false; // invalid name
    }

    ++lower_count;
    send_true[id_in_req_to_ordered_id[i]] = true;
  }

  // from here, get ready to handle the _req positions

  // this is a tmp variable that is reused
  std::vector<double> ordered_positions(number_of_angle_joints);

  // for each trajectory points,
  std::fill(ordered_positions.begin(), ordered_positions.end(), 0.0);

  // check for cancelling joints (NaN values)
  std::vector<bool> send_true_with_cancel;
  send_true_with_cancel.assign(send_true.begin(), send_true.end());

  for (size_t j = 0; j < _req.points.positions.size(); ++j)
    if (!std::isnan(_req.points.positions[j]))
      ordered_positions[id_in_req_to_ordered_id[j]] =
        _req.points.positions[j];
    else
      send_true_with_cancel[id_in_req_to_ordered_id[j]] = false;

  std::vector<int16_t> strokes(AERO_DOF);
  common::Angle2Stroke(strokes, ordered_positions);

  // fill in unused joints to no-send
  common::UnusedAngle2Stroke(strokes, send_true_with_cancel);

  // split strokes into upper and lower
  std::vector<int16_t> upper_stroke_vector(
      strokes.begin(), strokes.begin() + AERO_DOF_UPPER);
  std::vector<int16_t> lower_stroke_vector(
      strokes.begin() + AERO_DOF_UPPER, strokes.end());

  double time_sec = _req.points.time_from_start.toSec();
  uint16_t time_csec = static_cast<uint16_t>(time_sec * 100.0);

  std::thread t1([&](){
      if (_req.reset_status) { // reset status if flag
        upper_.reset_status();
        usleep(20000); // 20ms sleep before next command
      }
      if (upper_count > 0) {
        upper_.set_position(upper_stroke_vector, time_csec);
      }
    });

  std::thread t2([&](){
      if (_req.reset_status) { // reset status if flag
        lower_.reset_status();
        usleep(20000); // 20ms sleep before next command
      }
      if (lower_count > 0) {
        lower_.set_position(lower_stroke_vector, time_csec);
      }
    });

  t1.join();
  t2.join();

  usleep(20000); // prevent service call overlap

  mtx_upper_.unlock();
  mtx_lower_.unlock();

  // wait for action to finish
  usleep(time_csec * 10 * 1000 - 60000); // **
  // ** 20ms sleep in set_position + 20ms sleep before/after mtx lock

  mtx_upper_.lock();
  mtx_lower_.lock();

  usleep(20000); // prevent update failures

  // commands take 20ms sleep, threading to save time
  std::thread t3([&](){
      upper_.update_position();
    });
  std::thread t4([&](){
      lower_.update_position();
    });
  t3.join();
  t4.join();

  // get upper actual positions
  std::vector<int16_t> upper_stroke_vector_ret =
    upper_.get_actual_stroke_vector();

  // get lower actual positions
  std::vector<int16_t> lower_stroke_vector_ret =
    lower_.get_actual_stroke_vector();

  std::vector<double> actual_stroke_state(AERO_DOF);

  if (upper_stroke_vector_ret.size() < AERO_DOF_UPPER)
    for (size_t i = 0; i < AERO_DOF_UPPER; ++i)
      actual_stroke_state[i] = 0.0;
  else // usually should enter else, enters if when port is not activated
    for (size_t i = 0; i < AERO_DOF_UPPER; ++i)
      actual_stroke_state[i] =
        static_cast<double>(upper_stroke_vector_ret[i]);

  if (lower_stroke_vector_ret.size() < AERO_DOF_LOWER)
    for (size_t i = 0; i < AERO_DOF_LOWER; ++i)
      actual_stroke_state[i + AERO_DOF_UPPER] = 0.0;
  else // usually should enter else, enters if when port is not activated
    for (size_t i = 0; i < AERO_DOF_LOWER; ++i)
      actual_stroke_state[i + AERO_DOF_UPPER] =
        static_cast<double>(lower_stroke_vector_ret[i]);

  _res.joint_names.resize(number_of_angle_joints);
  _res.points.positions.resize(number_of_angle_joints);

  // convert to actual angle positions
  std::vector<int16_t> actual_strokes(actual_stroke_state.begin(),
                                      actual_stroke_state.end());
  common::Stroke2Angle(_res.points.positions, actual_strokes);

  // get joint names (auto-generated function)
  common::AngleJointNames(_res.joint_names);

  // get status
  std_msgs::Bool status_flag;
  _res.status = upper_.get_status() || lower_.get_status();

  mtx_upper_.unlock();
  mtx_lower_.unlock();

  return true;
}

//////////////////////////////////////////////////
bool AeroControllerNode::GetJointsCallback(
    aero_startup::AeroSendJoints::Request &_req,
    aero_startup::AeroSendJoints::Response &_res)
{
  mtx_upper_.lock();
  mtx_lower_.lock();

  int number_of_angle_joints =
      upper_.get_number_of_angle_joints() +
      lower_.get_number_of_angle_joints();

  std::thread t1([&](){
      if (_req.reset_status) { // reset status if flag
        upper_.reset_status();
        usleep(20000); // 20ms sleep before next command
      }
    });

  std::thread t2([&](){
      if (_req.reset_status) { // reset status if flag
        lower_.reset_status();
        usleep(20000); // 20ms sleep before next command
      }
    });

  t1.join();
  t2.join();

  // commands take 20ms sleep, threading to save time
  std::thread t3([&](){
      upper_.update_position();
    });
  std::thread t4([&](){
      lower_.update_position();
    });
  t3.join();
  t4.join();

  // get upper actual positions
  std::vector<int16_t> upper_stroke_vector_ret =
    upper_.get_actual_stroke_vector();

  // get lower actual positions
  std::vector<int16_t> lower_stroke_vector_ret =
    lower_.get_actual_stroke_vector();

  std::vector<double> actual_stroke_state(AERO_DOF);

  if (upper_stroke_vector_ret.size() < AERO_DOF_UPPER)
    for (size_t i = 0; i < AERO_DOF_UPPER; ++i)
      actual_stroke_state[i] = 0.0;
  else // usually should enter else, enters if when port is not activated
    for (size_t i = 0; i < AERO_DOF_UPPER; ++i)
      actual_stroke_state[i] =
        static_cast<double>(upper_stroke_vector_ret[i]);

  if (lower_stroke_vector_ret.size() < AERO_DOF_LOWER)
    for (size_t i = 0; i < AERO_DOF_LOWER; ++i)
      actual_stroke_state[i + AERO_DOF_UPPER] = 0.0;
  else // usually should enter else, enters if when port is not activated
    for (size_t i = 0; i < AERO_DOF_LOWER; ++i)
      actual_stroke_state[i + AERO_DOF_UPPER] =
        static_cast<double>(lower_stroke_vector_ret[i]);

  _res.joint_names.resize(number_of_angle_joints);
  _res.points.positions.resize(number_of_angle_joints);

  // convert to actual angle positions
  std::vector<int16_t> actual_strokes(actual_stroke_state.begin(),
                                      actual_stroke_state.end());
  common::Stroke2Angle(_res.points.positions, actual_strokes);

  // get joint names (auto-generated function)
  common::AngleJointNames(_res.joint_names);

  // get status
  std_msgs::Bool status_flag;
  _res.status = upper_.get_status() || lower_.get_status();

  mtx_upper_.unlock();
  mtx_lower_.unlock();

  return true;
}
