//==============================================================================
// Copyright 2019-2020 Kitware, Inc., Kitware SAS
// Author: Julia Sanchez (Kitware SAS)
// Creation date: 2021-03-15
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//==============================================================================

#pragma once

#include "LidarSlam/CeresCostFunctions.h" // for residual structure + ceres
#include "LidarSlam/Utilities.h"
#include <list>
#include <cfloat>
#include <mutex>
#ifdef USE_GTSAM
#include <gtsam/navigation/ImuFactor.h>
#endif

namespace LidarSlam
{

#define SetSensorMacro(name,type) void Set##name (type _arg) { this->name = _arg; }
#define GetSensorMacro(name,type) type Get##name () const { return this->name; }

namespace ExternalSensors
{

// ---------------------------------------------------------------------------
// MEASUREMENTS
// ---------------------------------------------------------------------------

struct LandmarkMeasurement
{
  double Time = 0.;
  // Relative transform between the detector and the tag
  Eigen::Isometry3d TransfoRelative = Eigen::Isometry3d::Identity();
  Eigen::Matrix6d Covariance = Eigen::Matrix6d::Identity();
  // No covariance is attached to pose measurement for now
  // Constant one can be used for pose graph optimizations
};

// ---------------------------------------------------------------------------
struct WheelOdomMeasurement
{
  double Time = 0.;
  double Distance = 0.;
};

// ---------------------------------------------------------------------------
struct GravityMeasurement
{
  double Time = 0.;
  Eigen::Vector3d Acceleration = Eigen::Vector3d::Zero();
};

// ---------------------------------------------------------------------------
struct GpsMeasurement
{
  double Time = 0.;
  Eigen::Vector3d Position = Eigen::Vector3d::Zero();
  Eigen::Matrix3d Covariance = Eigen::Matrix3d::Identity();
};

// ---------------------------------------------------------------------------
struct PoseMeasurement
{
  double Time = 0.;
  Eigen::Isometry3d Pose = Eigen::Isometry3d::Identity();
  Eigen::Matrix6d Covariance = 0.05 * Eigen::Matrix6d::Identity();
};

// ---------------------------------------------------------------------------
struct ImuMeasurement
{
  double Time = 0.;
  Eigen::Vector3d Acceleration  = Eigen::Vector3d::Zero();
  Eigen::Vector3d AngleVelocity = Eigen::Vector3d::Zero();
  // NOTE : Imu preintegrator makes the assumption the
  // covariance is fixed for all IMU measurements so we
  // attach the covariances to the IMU manager
};

// ---------------------------------------------------------------------------
// SENSOR MANAGERS
// ---------------------------------------------------------------------------

// Base class to derive all external sensors
// Contains some tools for time synchronization
// data management and general parameters

// ---------------------------------------------------------------------------
template <typename T>
class SensorManager
{
public:
  SensorManager(const std::string& name = "BaseSensor")
  : SensorName(name), PreviousIt(Measures.begin()) {}

  SensorManager(double timeOffset, double timeThreshold, unsigned int maxMeas,
                bool verbose = false, const std::string& name = "BaseSensor")
  : TimeOffset(timeOffset),
    TimeThreshold(timeThreshold),
    MaxMeasures(maxMeas),
    Verbose(verbose),
    SensorName(name),
    PreviousIt(Measures.begin())
  {}

  // -----------------Setters/Getters-----------------
  GetSensorMacro(SensorName, std::string)
  SetSensorMacro(SensorName, std::string)

  GetSensorMacro(Weight, double)
  SetSensorMacro(Weight, double)

  GetSensorMacro(Calibration, Eigen::Isometry3d)
  SetSensorMacro(Calibration, const Eigen::Isometry3d&)

  GetSensorMacro(TimeOffset, double)
  SetSensorMacro(TimeOffset, double)

  GetSensorMacro(TimeThreshold, double)
  SetSensorMacro(TimeThreshold, double)

  GetSensorMacro(SaturationDistance, float)
  SetSensorMacro(SaturationDistance, float)

  GetSensorMacro(Verbose, bool)
  SetSensorMacro(Verbose, bool)

  GetSensorMacro(MaxMeasures, unsigned int)
  void SetMaxMeasures(unsigned int maxMeas)
  {
    std::lock_guard<std::mutex> lock(this->Mtx);
    this->MaxMeasures = maxMeas;
    while (this->Measures.size() > this->MaxMeasures)
    {
      if (this->PreviousIt == this->Measures.begin())
        ++this->PreviousIt;
      this->Measures.pop_front();
    }
  }

  std::list<T> GetMeasures() const
  {
    std::lock_guard<std::mutex> lock(this->Mtx);
    return this->Measures;
  }

  GetSensorMacro(Residual, CeresTools::Residual)

  // -----------------Basic functions-----------------

  // ------------------
  // Add one measure at a time in measures list
  void AddMeasurement(const T& m)
  {
    std::lock_guard<std::mutex> lock(this->Mtx);
    this->Measures.emplace_back(m);
    if (this->Measures.size() > this->MaxMeasures)
    {
      if (this->PreviousIt == this->Measures.begin())
        ++this->PreviousIt;
      this->Measures.pop_front();
    }
  }

  // ------------------
  void Reset(bool resetMeas = false)
  {
    this->ResetResidual();
    std::lock_guard<std::mutex> lock(this->Mtx);
    if (resetMeas)
      this->Measures.clear();
    this->PreviousIt = this->Measures.begin();
  }

  // ------------------
  // Check if sensor can be used in tight SLAM optimization
  // The weight must be not null and the measures list must contain
  // at leat 2 elements to be able to interpolate
  bool CanBeUsedLocally() const
  {
    std::lock_guard<std::mutex> lock(this->Mtx);
    return this->Weight > 1e-6 && this->Measures.size() > 1;
  }

  // ------------------
  // Check if sensor has enough data to be interpolated
  // (the measures list must contain at leat 2 elements)
  bool HasData() const
  {
    std::lock_guard<std::mutex> lock(this->Mtx);
    return this->Measures.size() > 1;
  }

  // Compute the interpolated measure to be synchronized with SLAM output (at lidarTime)
  // 'trackTime' allows to keep a time track and to speed up multiple searches
  // when following chronological order
  virtual bool ComputeSynchronizedMeasure(double lidarTime, T& synchMeas, bool trackTime = true) = 0;
  // Compute the constraint associated to the measurement
  virtual bool ComputeConstraint(double lidarTime) = 0;

protected:
  // ------------------
  // Reset the current residual
  void ResetResidual()
  {
    this->Residual.Cost.reset();
    this->Residual.Robustifier.reset();
  }

  // ------------------
  std::pair<typename std::list<T>::iterator, typename std::list<T>::iterator> GetMeasureBounds(double lidarTime, bool trackTime = true)
  {
    // Check if the measurements can be interpolated (or slightly extrapolated)
    if (lidarTime < this->Measures.front().Time || lidarTime > this->Measures.back().Time + this->TimeThreshold)
    {
      if (this->Verbose)
        PRINT_INFO(std::fixed << std::setprecision(9)
                   << "\t Measures contained in : [" << this->Measures.front().Time << ","
                   << this->Measures.back().Time <<"]\n"
                   << "\t -> " << this->SensorName << " not used"
                   << std::scientific)
      return std::make_pair(this->Measures.begin(), this->Measures.begin());
    }

    auto prevIt = this->PreviousIt;
    // Reset if the timeline has been modified (and if there is memory of a previous pose)
    if (prevIt == this->Measures.end() || prevIt->Time > lidarTime)
      prevIt = this->Measures.begin();

    auto postIt = prevIt;
    // Get iterator pointing to the first measurement after LiDAR time
    if (prevIt == this->Measures.begin())
    {
      // If after reset or for first search, use upper_bound function
      postIt = std::upper_bound(prevIt,
                                this->Measures.end(),
                                lidarTime,
                                [&](double time, const T& measure) {return time < measure.Time;});
    }
    else
    {
      // If in the continuity of search, directly look for closest measurements
      while (postIt->Time < lidarTime && postIt != this->Measures.end())
        ++postIt;
    }

    // If the last measure was taken before Lidar points
    // extract the two last measures (for extrapolation)
    if (postIt == this->Measures.end())
      --postIt;

    // Get iterator pointing to the last measurement before LiDAR time
    prevIt = postIt;
    --prevIt;

    // Update the previous iterator for next call
    if (trackTime)
      this->PreviousIt = prevIt;

    // Check the time between the 2 measurements
    // Do not interpolate if the time is too long
    if (!this->CheckBounds(prevIt,postIt))
      return std::make_pair(this->Measures.begin(), this->Measures.begin());

    return std::make_pair(prevIt, postIt);
  }

  // ------------------
  virtual bool CheckBounds(typename std::list<T>::iterator& prevIt, typename std::list<T>::iterator& postIt)
  {
    // If the time between the 2 measurements is too long
    // Do not use the current measures
    if (postIt->Time - prevIt->Time > this->TimeThreshold)
    {
      if (this->Verbose)
        PRINT_INFO("\t The two last " << this->SensorName << " measures can not be interpolated (too much time difference)"
                   << "-> " << this->SensorName << " not used")
      return false;
    }
    return true;
  }

protected:
  // Measures stored
  std::list<T> Measures;
  // Weight to apply to sensor info when used in local optimization
  double Weight = 0.;
  // Calibration transform with base_link and the sensor
  Eigen::Isometry3d Calibration = Eigen::Isometry3d::Identity();
  // Time offset to make external sensors/Lidar correspondance
  double TimeOffset = 0.;
  // Time threshold between 2 measures to consider they can be interpolated
  double TimeThreshold = 0.5;
  // Threshold distance to not take into account the constraint
  // This distance is used in the constraint robustifier
  float SaturationDistance = 5.f;
  // Measures length limit
  // The oldest measures are forgotten
  unsigned int MaxMeasures = 1e6;
  // Verbose boolean to enable/disable debug info
  bool Verbose = false;
  // Sensor name for output
  std::string SensorName;
  // Iterator pointing to the last measure used
  // This allows to keep a time track
  typename std::list<T>::iterator PreviousIt;
  // Resulting residual
  CeresTools::Residual Residual;
  // Mutex to handle the data from outside the library
  mutable std::mutex Mtx;
};

// ---------------------------------------------------------------------------
// A wheel odometer allows to get a translation information
// For now, this manager is designed for cases where there is no rotation
// in the whole trajectory (e.g. Lidar following a cable)
// It builds a constraint comparing translation of base between
// two successive poses or from a reference pose
class WheelOdometryManager : public SensorManager<WheelOdomMeasurement>
{
public:
  WheelOdometryManager(const std::string& name = "Wheel odometer"): SensorManager(name){}
  WheelOdometryManager(double w, double timeOffset, double timeThresh, unsigned int maxMeas,
                       bool verbose = false, const std::string& name = "Wheel odometer")
  : SensorManager(timeOffset, timeThresh, maxMeas, verbose, name) {this->Weight = w;}

  void Reset(bool resetMeas = false);

  //Setters/Getters
  GetSensorMacro(PreviousPose, Eigen::Isometry3d)
  SetSensorMacro(PreviousPose, const Eigen::Isometry3d&)

  GetSensorMacro(Relative, bool)
  SetSensorMacro(Relative, bool)

  GetSensorMacro(RefDistance, double)
  SetSensorMacro(RefDistance, double)

  // Compute the interpolated measure to be synchronized with SLAM output (at lidarTime)
  bool ComputeSynchronizedMeasure(double lidarTime, WheelOdomMeasurement& synchMeas, bool trackTime = true) override;
  // Wheel odometry constraint (unoriented)
  // Can be relative since last frame or absolute since first pose
  bool ComputeConstraint(double lidarTime) override;

private:
  // Members used when using the relative distance with last estimated pose
  Eigen::Isometry3d PreviousPose = Eigen::Isometry3d::Identity();
  double RefDistance = FLT_MAX;
  // Boolean to indicate whether to compute an absolute constraint (since first frame)
  // or relative constraint (since last acquired frame)
  bool Relative = false;
};

// ---------------------------------------------------------------------------
// An IMU can supply the gravity direction when measuring the whole acceleration
// when velocity is constant.
// This manager allows to create a local constraint to align gravity vectors between SLAM poses
class ImuGravityManager : public SensorManager<GravityMeasurement>
{
public:
  ImuGravityManager(const std::string& name = "IMU"): SensorManager(name){}
  ImuGravityManager(double w, double timeOffset, double timeThresh, unsigned int maxMeas,
                    bool verbose = false, const std::string& name = "IMU")
  : SensorManager(timeOffset, timeThresh, maxMeas, verbose, name) {this->Weight = w;}

  void Reset(bool resetMeas = false);

  //Setters/Getters
  GetSensorMacro(GravityRef, Eigen::Vector3d)
  SetSensorMacro(GravityRef, const Eigen::Vector3d&)

  // Compute the interpolated measure to be synchronized with SLAM output (at lidarTime)
  bool ComputeSynchronizedMeasure(double lidarTime, GravityMeasurement& synchMeas, bool trackTime = true) override;

  // Compute the interpolated measure to be synchronized with SLAM output (at lidarTime) in base frame
  bool ComputeSynchronizedMeasureBase(double lidarTime, GravityMeasurement& synchMeas, bool trackTime = true);

  // IMU constraint (gravity)
  bool ComputeConstraint(double lidarTime) override;
  // Compute Reference gravity vector from IMU measurements
  void ComputeGravityRef(double deltaAngle);

private:
  Eigen::Vector3d GravityRef = Eigen::Vector3d::Zero();
};

// ---------------------------------------------------------------------------
// Landmarks can be detected by an external sensor (e.g. camera)
// This manager allows to create a local constraint with a reference absolute pose for the landmark
// or with previous observed poses of the landmark (like a usual keypoint)
class LandmarkManager: public SensorManager<LandmarkMeasurement>
{
public:
  LandmarkManager(const std::string& name = "Tag detector") : SensorManager(name){}
  LandmarkManager(const LandmarkManager& lmManager);
  LandmarkManager(double timeOffset, double timeThresh, unsigned int maxMeas, bool positionOnly = true,
                  bool verbose = false, const std::string& name = "Tag detector");

  void operator=(const LandmarkManager& lmManager);

  void Reset(bool resetMeas = false);

  // Setters/Getters
  // The absolute pose can be set from outside the lib
  // or will be detected online, averaging the previous detections
  GetSensorMacro(AbsolutePose, Eigen::Vector6d)
  GetSensorMacro(AbsolutePoseCovariance, Eigen::Matrix6d)

  GetSensorMacro(PositionOnly, bool)
  SetSensorMacro(PositionOnly, bool)

  GetSensorMacro(CovarianceRotation, bool)
  SetSensorMacro(CovarianceRotation, bool)
  // Set the initial absolute pose
  // NOTE : the absolute pose can be updated if UpdateAbsolutePose is called
  void SetAbsolutePose(const Eigen::Vector6d& pose, const Eigen::Matrix6d& cov);

  // Compute the interpolated measure (landmark pose) to be synchronized with SLAM output at lidarTime
  bool ComputeSynchronizedMeasure(double lidarTime, LandmarkMeasurement& synchMeas, bool trackTime = true) override;

  // Compute the interpolated measure (landmark pose)
  // to be synchronized with SLAM output at lidarTimenfrom base frame
  bool ComputeSynchronizedMeasureBase(double lidarTime, LandmarkMeasurement& synchMeas, bool trackTime = true);

  // Landmark constraint
  bool ComputeConstraint(double lidarTime) override;

  // Update the absolute pose in case the tags are used as relative constraints
  // (i.e, no absolute poses of the tags are supplied)
  bool UpdateAbsolutePose(const Eigen::Isometry3d& baseTransform, double lidarTime);
  bool NeedsReferencePoseRefresh(double lidarTime);

private:
  bool HasBeenUsed(double lidarTime);

private:
  // Absolute pose of the landmark in the global frame
  Eigen::Vector6d AbsolutePose = Eigen::Vector6d::Zero();
  Eigen::Matrix6d AbsolutePoseCovariance = Eigen::Matrix6d::Zero();
  // Relative transform (detector/landmark) stored to be used when updating the absolute pose
  // It represents the transform between the detector and the landmark
  // i.e. detector to landmark, no calibration.
  Eigen::Isometry3d RelativeTransform = Eigen::Isometry3d::Identity();
  // Boolean to check the absolute pose has been loaded
  // or if the tag has already been seen
  bool HasAbsolutePose = false;
  std::pair<double, double> LastUpdateTimes = {FLT_MAX, FLT_MAX};
  // Counter to check how many frames the tag was seen on
  // This is used to average the pose in case the absolute poses
  // were not supplied initially and are updated (cf. UpdateAbsolutePose)
  int Count = 0;
  // The constraint created can use the whole position (orientation + position) -> false
  // or only the position -> true (if the orientation is not reliable enough)
  bool PositionOnly = true;
  // Allow to rotate the covariance
  // Can be disabled if the covariance is fixed or not used (e.g. for local constraint)
  bool CovarianceRotation = false;
};

// ---------------------------------------------------------------------------
// GPS manager contains GPS sensor positions
// This manager can be used to build a pose graph
// GPS measurements are represented in a specific world referential frame (e.g. ENU)
// An offset transform links the GPS referential and the Lidar SLAM referential frame (e.g. first pose)
// This offset must be set from outside this library and can be computed using the GPS data and some lidar SLAM poses
class GpsManager: public SensorManager<GpsMeasurement>
{
public:
  GpsManager(const std::string& name = "GPS") : SensorManager(name){}
  GpsManager(const GpsManager& gpsManager);
  GpsManager(double timeOffset, double timeThresh, unsigned int maxMeas,
             bool verbose = false, const std::string& name = "GPS")
  : SensorManager(timeOffset, timeThresh, maxMeas, verbose, name) {}

  void operator=(const GpsManager& gpsManager);

  // Setters/Getters
  GetSensorMacro(Offset, Eigen::Isometry3d)
  SetSensorMacro(Offset, const Eigen::Isometry3d&)

  // Compute the interpolated measure (GPS position in GPS referential) to be synchronized with SLAM output at lidarTime
  bool ComputeSynchronizedMeasure(double lidarTime, GpsMeasurement& synchMeas, bool trackTime = true) override;

  // Compute the interpolated measure (GPS position in SLAM referential) to be synchronized with SLAM output at lidarTime
  // The measures data track GPS sensor frame (not base frame) but are represented in the same referential
  bool ComputeSynchronizedMeasureOffset(double lidarTime, GpsMeasurement& synchMeas, bool trackTime = true);

  bool ComputeConstraint(double lidarTime) override;

  bool CanBeUsedLocally() const {return false;}

private:
  // Offset transform to link GPS global frame and Lidar SLAM global frame
  // GPS referential to base referential
  Eigen::Isometry3d Offset = Eigen::Isometry3d::Identity();
};

// ---------------------------------------------------------------------------
class PoseManager: public SensorManager<PoseMeasurement>
{
public:
  PoseManager(const std::string& name = "Pose sensor") : SensorManager(name){}

  PoseManager(double w, double timeOffset, double timeThresh, unsigned int maxMeas,
              bool verbose = false, const std::string& name = "Pose sensor")
  : SensorManager(timeOffset, timeThresh, maxMeas, verbose, name) {this->Weight = w;}

  void Reset(bool resetMeas = false);

  // Setters/Getters
  GetSensorMacro(PrevLidarTime, double)
  SetSensorMacro(PrevLidarTime, double)

  GetSensorMacro(PrevPoseTransform, Eigen::Isometry3d)
  SetSensorMacro(PrevPoseTransform, const Eigen::Isometry3d&)

  GetSensorMacro(CovarianceRotation, bool)
  SetSensorMacro(CovarianceRotation, bool)

  GetSensorMacro(DistanceThreshold, double)
  SetSensorMacro(DistanceThreshold, double)

  // Compute the interpolated measure (pose of the external pose sensor's)
  // to be synchronized with SLAM output at lidarTime
  bool ComputeSynchronizedMeasure(double lidarTime, PoseMeasurement& synchMeas, bool trackTime = true) override;

  // Compute the interpolated measure (base pose)
  // to be synchronized with SLAM output at lidarTime : calibration is applied
  bool ComputeSynchronizedMeasureBase(double lidarTime, PoseMeasurement& synchMeas, bool trackTime = true);

  bool ComputeConstraint(double lidarTime) override;

protected:
  double PrevLidarTime = -1.;
  Eigen::Isometry3d PrevPoseTransform = Eigen::Isometry3d::Identity();
  // Allow to rotate the covariance
  // Can be disabled if the covariance is fixed or not used (e.g. for local constraint)
  bool CovarianceRotation = false;
  // Distance threshold [m] between 2 measures to consider they can be interpolated
  double DistanceThreshold = 0.;

  // Check time and motion difference of bounds
  // Do not use the 2 measures if time difference is too long and motion difference is too large and return false
  // Otherwise return true
  bool CheckBounds(std::list<PoseMeasurement>::iterator& prevIt, std::list<PoseMeasurement>::iterator& postIt) override;
};

// ---------------------------------------------------------------------------
// IMU raw data (accelerations and angular rates) must be preprocessed to be able to integrate them into the SLAM optimization
// This manager allows to preintegrate them. The main problem is to correctly estimate the bias which is a slowly varying error
// made on raw measurements. This bias can be corrected at each new SLAM pose output using this manager.
// This manager is derived from PoseManager so that once the data are preintegrated, the pose manager
// can be used to compute synchronized data and a local constraint
// NOTE : init values and useful constants were found here : https://github.com/TixiaoShan/LIO-SAM/
class ImuManager: public PoseManager
{
public:
  ImuManager(const std::string& name = "IMU")
  : PoseManager(name){this->Reset();}

  ImuManager(double w, double timeOffset, double timeThresh, unsigned int maxMeas,
             bool verbose = false, const std::string& name = "IMU")
  : PoseManager(w, timeOffset, timeThresh, maxMeas, verbose, name) {this->Reset();}

  // ---------------------------------------------------------------------------

  // Setters/Getters
  GetSensorMacro(Gravity, Eigen::Vector3d)
  SetSensorMacro(Gravity, const Eigen::Vector3d&)

  GetSensorMacro(Frequency, float)
  SetSensorMacro(Frequency, float)

  // ---------------------------------------------------------------------------
  void Reset(bool resetMeas = false)
  {
    this->SensorManager::Reset(resetMeas);
    if (resetMeas)
    {
      std::lock_guard<std::mutex> lock(this->Mtx);
      this->RawMeasures.clear();
    }
    // Initialize preintegrator
    // Set measurement noise and gravity (in world reference frame)
    // boost smart pointer mandatory to initialize preintegrators
    boost::shared_ptr<gtsam::PreintegrationParams> p = boost::make_shared<gtsam::PreintegrationParams>(-this->Gravity);
    p->accelerometerCovariance = this->AccCovariance;
    p->gyroscopeCovariance     = this->GyrCovariance;
    // Set integration noise (from velocities to positions)
    p->integrationCovariance = std::pow(1e-4, 2) * Eigen::Matrix3d::Identity();
    // Initialize a null bias
    this->Bias = gtsam::imuBias::ConstantBias(Eigen::Vector6d::Zero());
    // Initialize preintegrators with noise and bias
    this->PreintegratorNewData = std::make_shared<gtsam::PreintegratedImuMeasurements>(p, this->Bias);
  }

  // --------------------------------------------------------------------------
  // Add one measure at a time in measures list
  // The measure added (acc, vel) is not of the same type as the one used in optimization (pose)
  // so we overload the function AddMeasurement
  // WARNING for postprocess : preintegration can be heavy,
  // resulting poses are only used in local SLAM optimization,
  // we advice to not add measures that are far away from studied SLAM time
  using PoseManager::AddMeasurement;
  void AddMeasurement(const ImuMeasurement& m)
  {
    #ifdef USE_GTSAM
    std::lock_guard<std::mutex> lock(this->Mtx);
    // Update preintegration with new measurement
    // If this is the first measurement,
    // dt is approximated using frequency set externally
    double dt = this->RawMeasures.empty()? 1. / this->Frequency : m.Time - this->RawMeasures.back().Time;
    this->PreintegratorNewData->integrateMeasurement(m.Acceleration, m.AngleVelocity, dt);
    // Store raw measurement
    this->RawMeasures.emplace_back(m);
    // Use gravity, new data, previously optimized SLAM pose, previously optimized SLAM velocity
    // and previously optimized bias to derive new IMU pose
    // and store it into the measures list
    gtsam::NavState imuState = this->PreintegratorNewData->predict(this->PrevState, this->Bias);
    PoseMeasurement imuPose;
    imuPose.Pose.linear() = imuState.R();
    imuPose.Pose.translation() = imuState.t();
    imuPose.Time = m.Time;
    this->Measures.emplace_back(imuPose);
    if (this->Measures.size() > this->MaxMeasures)
    {
      if (this->PreviousIt == this->Measures.begin())
        ++this->PreviousIt;
      this->Measures.pop_front();
      this->RawMeasures.pop_front();
    }
    return;
    #endif
    static_cast<void>(m);
  }

private:
  // ---------------Preintegration relative members---------------
  // List of old raw measurements received
  std::list<ImuMeasurement> RawMeasures;
  // Covariance of raw measurement
  // NOTE : As covariance is fixed for all raw measurements,
  // it is attached to the manager and not to the measurements
  Eigen::Matrix3d AccCovariance = std::pow(3.9939570888238808e-03, 2) * Eigen::Matrix3d::Identity();
  Eigen::Matrix3d GyrCovariance = std::pow(1.5636343949698187e-03, 2) * Eigen::Matrix3d::Identity();
  // Estimated gravity (m/s^2)
  Eigen::Vector3d Gravity = {0., 0., -9.80511}; // default : z upward
  // Frequency of the IMU
  float Frequency = 100.f; // Used when dt is not available
  #ifdef USE_GTSAM
  // IMU bias on acceleration and angular velocity
  gtsam::imuBias::ConstantBias Bias = gtsam::imuBias::ConstantBias(Eigen::Vector6d::Zero());
  // Preintegrator for data since last lidar time
  // Used to get poses at IMU frequency
  // NOTE : Those poses can then be used in SLAM optimization
  std::shared_ptr<gtsam::PreintegratedImuMeasurements> PreintegratorNewData;
  #endif
};

} // end of ExternalSensors namespace
} // end of LidarSlam namespace