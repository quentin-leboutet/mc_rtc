#include <mc_rbdyn/JointSensor.h>

namespace mc_rbdyn
{

JointSensor::JointSensor(const std::string & jointName) : JointSensor("js-" + jointName, jointName) {}

JointSensorPtr JointSensor::clone() const
{
  return JointSensorPtr(new JointSensor(*this));
}

} // namespace mc_rbdyn
