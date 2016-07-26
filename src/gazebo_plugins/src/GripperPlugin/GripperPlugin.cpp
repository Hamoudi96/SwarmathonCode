#include "GripperPlugin.h"

using namespace gazebo;
using namespace std;

/**
 * This function is inherited from the ModelPlugin class and implemented here.
 * It loads all necessary data for the plugin from the provided model and SDF
 * parameters. In addition, this function sets up the two required subscribers
 * needed for ROS to communicate with the gripper for a rover.
 *
 * <p> In the event that a required XML tag is not found in the SDF file the
 * plugin will initiate an exit(1) call with extreme prejudice resulting,
 * typically, in a segmentation fault.
 *
 * @param _model The Swarmie Rover model this gripper is attached to.
 * @param _sdf   The SDF configuration file for this plugin the model.
 */
void GripperPlugin::Load(physics::ModelPtr _model, sdf::ElementPtr _sdf) {
  model = _model;
  sdf = _sdf;
  previousUpdateTime = model->GetWorld()->GetSimTime();
  previousDebugUpdateTime = model->GetWorld()->GetSimTime();

  // print debug statements if toggled to "true" in the model SDF file
  loadDebugMode();

  ROS_DEBUG_STREAM_COND(isDebuggingModeActive, "[Gripper Plugin : "
    << model->GetName() << "]\n    ===== BEGIN LOADING =====");

  // set the update period (number of updates per second) for this plugin
  loadUpdatePeriod();
  ROS_DEBUG_STREAM_COND(isDebuggingModeActive, "[Gripper Plugin : "
    << model->GetName() << "]\n    set the plugin update period:\n"
    << "        " << updatePeriod << " Hz or " << (1.0/updatePeriod)
    << " updates per second");

  // LOAD GRIPPER JOINTS - begin
  wristJoint = loadJoint("wristJoint");
  leftFingerJoint = loadJoint("leftFingerJoint");
  rightFingerJoint = loadJoint("rightFingerJoint");
  ROS_DEBUG_STREAM_COND(isDebuggingModeActive, "[Gripper Plugin : "
    << model->GetName() << "]\n    loaded the gripper's joints:\n"
    << "        " << wristJoint->GetName() << endl << "        "
    << leftFingerJoint->GetName() << endl << "        "
    << rightFingerJoint->GetName());
  // LOAD GRIPPER JOINTS - end

  // INITIALIZE GRIPPER MANAGER - begin
  GripperManager::GripperJointNames jointNames;
  jointNames.wristJointName = model->GetName() + "_" + wristJoint->GetName();
  jointNames.leftFingerJointName =
    model->GetName() + "_" + leftFingerJoint->GetName();
  jointNames.rightFingerJointName =
    model->GetName() + "_" + rightFingerJoint->GetName();
  PIDController::PIDSettings wristPID = loadPIDSettings("wrist");
  PIDController::PIDSettings fingerPID = loadPIDSettings("finger");
  gripperManager = GripperManager(jointNames, wristPID, fingerPID);
  ROS_DEBUG_STREAM_COND(isDebuggingModeActive, "[Gripper Plugin : "
    << model->GetName() << "]\n    initialized the GripperManager:\n"
    << "        wristPID:  Kp=" << wristPID.Kp << ", Ki=" << wristPID.Ki
    << ", Kd=" << wristPID.Kd << ", force min=" << wristPID.min
    << ", force max=" << wristPID.max << ", dt=" << wristPID.dt << endl
    << "        fingerPID: Kp=" << fingerPID.Kp << ", Ki=" << fingerPID.Ki
    << ", Kd=" << fingerPID.Kd << ", force min=" << fingerPID.min
    << ", force max=" << fingerPID.max << ", dt=" << fingerPID.dt);
  // INITIALIZE GRIPPER MANAGER - end

  // Connect the updateWorldEventHandler function to Gazebo;
  // ConnectWorldUpdateBegin sets our handler to be called at the beginning of
  // each physics update iteration
  updateConnection = event::Events::ConnectWorldUpdateBegin(
    boost::bind(&GripperPlugin::updateWorldEventHandler, this)
  );
  ROS_DEBUG_STREAM_COND(isDebuggingModeActive, "[Gripper Plugin : "
    << model->GetName() << "]\n    bind world update function to gazebo:\n"
    << "        void GripperPlugin::updateWorldEventHandler()");

  // ROS must be initialized in order to set up this plugin's subscribers
  if (!ros::isInitialized()) {
    ROS_ERROR_STREAM("[Gripper Plugin : " << model->GetName()
      << "] In GripperPlugin.cpp: Load(): ROS must be initialized before "
      << "this plugin can be used!");
    exit(1);
  }

  rosNode.reset(new ros::NodeHandle(string(model->GetName()) + "_gripper"));
  ROS_DEBUG_STREAM_COND(isDebuggingModeActive, "[Gripper Plugin : "
    << model->GetName() << "]\n    initialize a NodeHandle for this plugin:\n"
    << "        " << model->GetName() + "_gripper");

  // SUBSCRIBE TO ROS TOPICS - begin
  string wristTopic = loadSubscriptionTopic("wristTopic");
  ros::SubscribeOptions wristSubscriptionOptions =
    ros::SubscribeOptions::create<std_msgs::Float32>(
      wristTopic, 1,
      boost::bind(&GripperPlugin::setWristAngleHandler, this, _1),
      ros::VoidPtr(), &rosQueue
    );

  string fingerTopic = loadSubscriptionTopic("fingerTopic");
  ros::SubscribeOptions fingerSubscriptionOptions =
    ros::SubscribeOptions::create<std_msgs::Float32>(
      fingerTopic, 1,
      boost::bind(&GripperPlugin::setFingerAngleHandler, this, _1),
      ros::VoidPtr(), &rosQueue
    );

  wristAngleSubscriber = rosNode->subscribe(wristSubscriptionOptions);
  fingerAngleSubscriber = rosNode->subscribe(fingerSubscriptionOptions);

  ROS_DEBUG_STREAM_COND(isDebuggingModeActive, "[Gripper Plugin : "
    << model->GetName() << "]\n    subscribe to all gripper topics:\n"
    << "        " << wristTopic << endl << "        " << fingerTopic);
  // SUBSCRIBE TO ROS TOPICS - end

  // spin up the queue helper thread
  rosQueueThread =
    std::thread(std::bind(&GripperPlugin::processRosQueue, this));
  ROS_DEBUG_STREAM_COND(isDebuggingModeActive, "[Gripper Plugin : "
    << model->GetName() << "]\n    bind queue helper function to private "
    << "thread:\n        void GripperPlugin::processRosQueue()");

  ROS_DEBUG_STREAM_COND(isDebuggingModeActive, "[Gripper Plugin : "
    << model->GetName() << "]\n    ===== FINISHED LOADING =====");
}

/**
 * This function handles updates to the gripper plugin. It is called by the
 * Gazebo physics engine at the start of each update iteration. The subscribers
 * will handle updating the desiredWristAngle and desiredFingerAngle variables.
 * This function will take those updated values and apply them to the joints
 * of the gripper as needed to instigate the desired movements requested by the
 * gripper publishers.
 */
void GripperPlugin::updateWorldEventHandler() {
  common::Time currentTime = model->GetWorld()->GetSimTime();

  // only update the gripper plugin once every updatePeriod
  if((currentTime - previousUpdateTime).Float() < updatePeriod) {
    return;
  }

  previousUpdateTime = currentTime;

  GripperManager::GripperState currentState;
  GripperManager::GripperState desiredState;

  // get the current gripper state
  currentState.wristAngle = wristJoint->GetAngle(0).Radian();
  currentState.leftFingerAngle = leftFingerJoint->GetAngle(0).Radian();
  currentState.rightFingerAngle = rightFingerJoint->GetAngle(0).Radian();

  // get the total angle of both fingers:
  // => right finger joint angle is always negative
  // => left finger joint angle is always positive
  // total finger angle = left finger joint angle - (-right finger joint angle)
  // total finger angle is ALWAYS POSITIVE (or zero)
  float leftFingerAngle = leftFingerJoint->GetAngle(0).Radian();
  float rightFingerAngle = rightFingerJoint->GetAngle(0).Radian();

  // Set the desired gripper state
  desiredState.leftFingerAngle = desiredFingerAngle.Radian() / 2.0;
  desiredState.rightFingerAngle = -desiredFingerAngle.Radian() / 2.0;
  desiredState.wristAngle = desiredWristAngle.Radian();
  
  // Get the forces to apply to the joints from the PID controllers
  GripperManager::GripperForces commandForces =
    gripperManager.getForces(desiredState, currentState);

  // Apply the command forces to the joints
  wristJoint->SetForce(0, commandForces.wristForce);
  leftFingerJoint->SetForce(0, commandForces.leftFingerForce);
  rightFingerJoint->SetForce(0, commandForces.rightFingerForce);

  // If debugging mode is active, print debugging statements
  if((currentTime - previousDebugUpdateTime).Float() >= debugUpdatePeriod) {
    previousDebugUpdateTime = currentTime;

    ROS_DEBUG_STREAM_COND(
      isDebuggingModeActive, "[Gripper Plugin : "
      << model->GetName() << "]\n"
      << "           Wrist Angle: Current Angle: " << setw(12)
      << currentState.wristAngle        << " rad\n"
      << "                        Desired Angle: " << setw(12)
      << desiredState.wristAngle        << " rad\n"
      << "                        Applied Force: " << setw(12)
      << commandForces.wristForce       << " N\n"
      << "     Left Finger Angle: Current Angle: " << setw(12)
      << currentState.leftFingerAngle   << " rad\n"
      << "                        Desired Angle: " << setw(12)
      << desiredState.leftFingerAngle   << " rad\n"
      << "                        Applied Force: " << setw(12)
      << commandForces.leftFingerForce  << " N\n"
      << "    Right Finger Angle: Current Angle: " << setw(12)
      << currentState.rightFingerAngle  << " rad\n"
      << "                        Desired Angle: " << setw(12)
      << desiredState.rightFingerAngle  << " rad\n"
      << "                        Applied Force: " << setw(12)
      << commandForces.rightFingerForce << " N\n"
    );
  }
}

/**
 * This is the subscriber function for the desiredWristAngle variable. Updates
 * to desiredWristAngle will cause the gripper to be moved vertically around
 * its axis of rotation.
 *
 * @param msg A publisher message consisting of a postive floating point value
 *            which represents an angle in radians.
 */
void GripperPlugin::setWristAngleHandler(const std_msgs::Float32ConstPtr& msg) {
  float wristAngle = msg->data;
  desiredWristAngle = wristAngle;
}

/**
 * This is the subscriber function for the desiredFingerAngle variable. Updates
 * to desiredFingerAngle will cause the gripper to open or close its fingers
 * based on the change in angle.
 *
 * @param msg A publisher message consisting of a postive floating point value
 *            which represents an angle in radians.
 */
void GripperPlugin::setFingerAngleHandler(const std_msgs::Float32ConstPtr& msg) {
  float fingerAngle = msg->data;
  desiredFingerAngle = fingerAngle;
}

/**
 * This function is used inside of a custom thread to process the messages
 * being passed from the publishers to the subscribers. It is the interface
 * between ROS and the setWristAngleHandler() and setFingerAngleHandler()
 * functions.
 */
void GripperPlugin::processRosQueue() {
  static const double timeout = 0.01;
  while (rosNode->ok()) {
    rosQueue.callAvailable(ros::WallDuration(timeout));
  }
}

/**
 * This function sets the "isDebuggingModeActive" flag to true or false
 * depending on the <debug> tag for this plugin in the configuration SDF file.
 * By default, the value of "isDebuggingModeActive" is false.
 *
 * <p>When debugging mode is active, extra print statements with the current
 * status of the gripper's joints will be printed to the console. These
 * print statements will occur at a rate of once per 3 seconds (simulated time)
 * or as defined by the user.
 *
 * <p>For example:
 * <p><debug>
 * <p>    <printToConsole>true</printToConsole>
 * <p>    <printDelayInSeconds>5.0</printDelayInSeconds>
 * <p></debug>
 */
void GripperPlugin::loadDebugMode() {
  isDebuggingModeActive = false;

  if(sdf->HasElement("debug")) {
    sdf::ElementPtr debug = sdf->GetElement("debug");

    if(debug->HasElement("printToConsole")) {
      string debugString = debug->GetElement("printToConsole")->Get<string>();

      if(debugString.compare("true") == 0) {
        isDebuggingModeActive = true;

        if(debug->HasElement("printDelayInSeconds")) {
          debugUpdatePeriod =
            debug->GetElement("printDelayInSeconds")->Get<float>();

          // fatal error: the debugUpdatePeriod cannot be <= 0
          if(debugUpdatePeriod <= 0.0) {
            ROS_ERROR_STREAM("[Gripper Plugin : " << model->GetName()
              << "]: In GripperPlugin.cpp: loadDebugMode(): "
              << "printDelayInSeconds = " << debugUpdatePeriod
              << ", printDelayInSeconds cannot be <= 0.0");
            exit(1);
          }
        } else {
          ROS_INFO_STREAM("[Gripper Plugin : " << model->GetName()
            << "]: In GripperPlugin.cpp: loadDebugMode(): "
            << "missing nested <printDelayInSeconds> tag in <debug> tag, "
            << "defaulting to 3.0 seconds");
          debugUpdatePeriod = 3.0;
        }

        ros::console::levels::Level dLevel = ros::console::levels::Debug;

        if(ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, dLevel)) {
          ros::console::notifyLoggerLevelsChanged();
        }
      } else if(debugString.compare("false") != 0) {
        ROS_INFO_STREAM("[Gripper Plugin : " << model->GetName()
          << "]: In GripperPlugin.cpp: loadDebugMode(): "
          << "invalid value in <printToConsole> tag in <debug> tag, "
          << "printToConsole = " << debugString << ", defaulting to false");
      }
    } else {
      ROS_INFO_STREAM("[Gripper Plugin : " << model->GetName()
        << "]: In GripperPlugin.cpp: loadDebugMode(): "
        << "missing nested <printToConsole> tag in <debug> tag, "
        << "defaulting to false");
    }
  } else {
    ROS_INFO_STREAM("[Gripper Plugin : " << model->GetName()
      << "]: In GripperPlugin.cpp: loadDebugMode(): "
      << "missing <debug> tag, defaulting to false");
  }
}

/**
 * This function loads the update rate from the SDF configuration file and uses
 * that value to set the update period. Effectively, the updatePeriod variable
 * defines how many times per second the plugin will apply changes from the ROS
 * subscribers. This value also determines the rate that debug statements will
 * be printed to the console if debugging mode is active.
 */
void GripperPlugin::loadUpdatePeriod() {
  float updateRate;

  if(!sdf->HasElement("updateRate")) {
    ROS_INFO_STREAM("[Gripper Plugin : " << model->GetName()
      << "]: In GripperPlugin.cpp: loadUpdatePeriod(): "
      << "missing <updateRate> tag, defaulting to 1000.0");
    updateRate = 1000.0;
  } else {
    updateRate = sdf->GetElement("updateRate")->Get<float>();

    // fatal error: the update cannot be <= 0 and especially cannot = 0
    if(updateRate <= 0) {
      ROS_ERROR_STREAM("[Gripper Plugin : " << model->GetName()
        << "]: In GripperPlugin.cpp: loadUpdatePeriod(): "
        << "updateRate = " << updateRate << ", updateRate cannot be <= 0.0");
      exit(1);
    }
  }

  // set the update period for this plugin: the plugin will refresh at a rate
  // of "updateRate" times per second, i.e., at "updatePeriod" hertz
  updatePeriod = 1.0 / updateRate;
}

/**
 * This function loads a string used for a subscription topic for this plugin.
 * Currently, this plugin will use two subscribers: one for the wrist, and one
 * for both finger joints.
 *
 * @param topicTag A std::string representing a tag in the SDF configuration
 *                 file for this plugin where a topic is defined.
 * @return A std::string containing the topic defined within the supplied tag.
 */
std::string GripperPlugin::loadSubscriptionTopic(std::string topicTag) {
  string topic;

  if(sdf->HasElement(topicTag)) {
    topic = sdf->GetElement(topicTag)->Get<std::string>();
  } else {
    ROS_ERROR_STREAM("[Gripper Plugin : " << model->GetName()
      << "]: In GripperPlugin.cpp: loadSubscriptionTopic(): No <" << topicTag
      << "> tag is defined in the model SDF file");
    exit(1);
  }

  return topic;
}

/**
 * This function loads a joint specified in an SDF configuration file. All
 * joints that are loaded are required for the plugin to work properly.
 * Therefore, in the event that a joint isn't properly loaded, exit(1) is
 * called and ROS is shutdown and/or crashed as a result.
 *
 * @param jointTag A string representing the name of the tag in the SDF file
 *                 where a joint is defined.
 * @return One of the gripper's three joints: wrist, left finger, or right
 *         finger.
 */
physics::JointPtr GripperPlugin::loadJoint(std::string jointTag) {
  string jointName;
  physics::JointPtr joint;
	    
  if(sdf->HasElement(jointTag)) {
    jointName = sdf->GetElement(jointTag)->Get<std::string>();
    joint = this->model->GetJoint(jointName);
		
    if(!joint) {
      ROS_ERROR_STREAM("[Gripper Plugin : " << model->GetName()
        << "]: In GripperPlugin.cpp: loadJoint(): No " << jointName
        << " joint is defined in the model SDF file");
      exit(1);
    }
  } else {
      ROS_ERROR_STREAM("[Gripper Plugin : " << model->GetName()
        << "]: In GripperPlugin.cpp: loadJoint(): No <" << jointTag
        << "> tag is defined in the model SDF file");
      exit(1);
  }

  return joint;
}

/**
 * This function loads (optional) user definable settings for the PID
 * controllers for the gripper wrist and finger joints. If the tags are not
 * defined or defined improperly, a set of default values will be used.
 *
 * @param PIDTag Defines which joint(s) we will load settings for. The only two
 *               valid values for PIDTag = "wrist" or "finger".
 * @return A PIDSettings struct with all values initialized based on user input
 *         from an SDF file or the predefined defaults within this code.
 */
PIDController::PIDSettings GripperPlugin::loadPIDSettings(string PIDTag) {
  if(PIDTag.compare("wrist") != 0 && PIDTag.compare("finger") != 0) {
    ROS_ERROR_STREAM("[Gripper Plugin : " << model->GetName()
      << "]: In GripperPlugin.cpp: loadPIDSettings(): PIDTag " << PIDTag
      << " is invalid: use either \"wrist\" or \"finger\"");
    exit(1);
  }

  PIDController::PIDSettings settings;
  math::Vector3 pid;
  math::Vector2d forceLimits;

  if(!sdf->HasElement(PIDTag + "PID")) {
    ROS_INFO_STREAM("[Gripper Plugin : " << model->GetName()
      << "]: In GripperPlugin.cpp: loadPIDSettings(): missing <" << PIDTag
      << "PID> tag, defaulting to P=2.5, I=0.0, D=0.0");
    pid = math::Vector3(2.5, 0.0, 0.0);
  } else {
    pid = sdf->GetElement(PIDTag + "PID")->Get<math::Vector3>();
  }

  if(!sdf->HasElement(PIDTag + "ForceLimits")) {
    ROS_INFO_STREAM("[Gripper Plugin : " << model->GetName()
      << "]: In GripperPlugin.cpp: loadPIDSettings(): missing <" << PIDTag
      << "ForceLimits> tag, defaulting to MIN = -10.0 N, MAX = 10.0 N");
    forceLimits = math::Vector2d(-10.0, 10.0);
  } else {
    forceLimits =
      sdf->GetElement(PIDTag + "ForceLimits")->Get<math::Vector2d>();
  }

  settings.Kp  = (float)pid.x;
  settings.Ki  = (float)pid.y;
  settings.Kd  = (float)pid.z;
  settings.dt  = updatePeriod;
  settings.min = (float)forceLimits.x;
  settings.max = (float)forceLimits.y;

  return settings;
}
