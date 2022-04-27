/*
 * Copyright 2015-2022 CNRS-UM LIRMM, CNRS-AIST JRL
 */

#include <mc_solver/CollisionsConstraint.h>

#include <mc_solver/ConstraintSetLoader.h>
#include <mc_solver/TasksQPSolver.h>

#include <mc_rbdyn/SCHAddon.h>
#include <mc_rbdyn/configuration_io.h>

#include <mc_rtc/gui/Arrow.h>
#include <mc_rtc/gui/Checkbox.h>
#include <mc_rtc/gui/Label.h>

#include <Tasks/QPConstr.h>

namespace mc_solver
{

/** Helper to cast the constraint */
static tasks::qp::CollisionConstr & tasks_constraint(mc_rtc::void_ptr & ptr)
{
  return *static_cast<tasks::qp::CollisionConstr *>(ptr.get());
}

static mc_rtc::void_ptr make_constraint(QPSolver::Backend backend, const mc_rbdyn::Robots & robots, double timeStep)
{
  switch(backend)
  {
    case QPSolver::Backend::Tasks:
      return mc_rtc::make_void_ptr<tasks::qp::CollisionConstr>(robots.mbs(), timeStep);
    default:
      mc_rtc::log::error_and_throw("[CollisionConstr] Not implemented for solver backend: {}", backend);
  }
}

CollisionsConstraint::CollisionsConstraint(const mc_rbdyn::Robots & robots,
                                           unsigned int r1Index,
                                           unsigned int r2Index,
                                           double timeStep)
: constraint_(make_constraint(backend_, robots, timeStep)), r1Index(r1Index), r2Index(r2Index), collId(0), collIdDict()
{
}

bool CollisionsConstraint::removeCollision(QPSolver & solver, const std::string & b1Name, const std::string & b2Name)
{
  auto p = __popCollId(b1Name, b2Name);
  if(!p.second.isNone())
  {
    if(monitored_.count(p.first))
    {
      toggleCollisionMonitor(p.first);
      category_.push_back("Monitors");
      std::string name = "Monitor " + p.second.body1 + "/" + p.second.body2;
      gui_->removeElement(category_, name);
      category_.pop_back();
    }
    cols.erase(std::find(cols.begin(), cols.end(), p.second));
    switch(backend_)
    {
      case QPSolver::Backend::Tasks:
      {
        auto & collConstr = tasks_constraint(constraint_);
        auto & qpsolver = tasks_solver(solver);
        bool ret = collConstr.rmCollision(p.first);
        if(ret)
        {
          collConstr.updateNrVars({}, qpsolver.data());
          qpsolver.updateConstrSize();
        }
        return ret;
      }
      default:
        break;
    }
  }
  return false;
}

void CollisionsConstraint::removeCollisions(QPSolver & solver, const std::vector<mc_rbdyn::Collision> & cols)
{
  for(const auto & c : cols)
  {
    removeCollision(solver, c.body1, c.body2);
  }
}

bool CollisionsConstraint::removeCollisionByBody(QPSolver & solver,
                                                 const std::string & b1Name,
                                                 const std::string & b2Name)
{
  const mc_rbdyn::Robot & r1 = solver.robots().robot(r1Index);
  const mc_rbdyn::Robot & r2 = solver.robots().robot(r2Index);
  std::vector<mc_rbdyn::Collision> toRm;
  for(const auto & col : cols)
  {
    if(r1.convex(col.body1).first == b1Name && r2.convex(col.body2).first == b2Name)
    {
      auto out = __popCollId(col.body1, col.body2);
      toRm.push_back(out.second);
      switch(backend_)
      {
        case QPSolver::Backend::Tasks:
        {
          auto & collConstr = tasks_constraint(constraint_);
          collConstr.rmCollision(out.first);
        }
        break;
        default:
          break;
      }
      if(monitored_.count(out.first))
      {
        toggleCollisionMonitor(out.first);
        category_.push_back("Monitors");
        std::string name = "Monitor " + out.second.body1 + "/" + out.second.body2;
        gui_->removeElement(category_, name);
        category_.pop_back();
      }
    }
  }
  for(const auto & it : toRm)
  {
    cols.erase(std::find(cols.begin(), cols.end(), it));
  }
  if(toRm.size())
  {
    switch(backend_)
    {
      case QPSolver::Backend::Tasks:
      {
        auto & collConstr = tasks_constraint(constraint_);
        auto & qpsolver = tasks_solver(solver);
        collConstr.updateNrVars({}, qpsolver.data());
        qpsolver.updateConstrSize();
      }
      break;
      default:
        break;
    }
  }
  return toRm.size() > 0;
}

void CollisionsConstraint::__addCollision(const mc_solver::QPSolver & solver, const mc_rbdyn::Collision & col)
{
  const auto & robots = solver.robots();
  const mc_rbdyn::Robot & r1 = robots.robot(r1Index);
  const mc_rbdyn::Robot & r2 = robots.robot(r2Index);
  if(col.body1.size() == 0 || col.body2.size() == 0)
  {
    mc_rtc::log::error("Attempted to add a collision without a specific body");
    return;
  }
  auto replace_b1 = [](const mc_rbdyn::Collision & col, const std::string & b) {
    auto out = col;
    out.body1 = b;
    return out;
  };
  auto replace_b2 = [](const mc_rbdyn::Collision & col, const std::string & b) {
    auto out = col;
    out.body2 = b;
    return out;
  };
  auto handle_wildcard = [&, this](const mc_rbdyn::Robot & robot, const std::string & body, bool is_b1) {
    if(body.back() != '*')
    {
      return false;
    }
    std::string search = body.substr(0, body.size() - 1);
    bool match = false;
    for(const auto & convex : robot.convexes())
    {
      const auto & cName = convex.first;
      if(cName.size() < search.size())
      {
        continue;
      }
      if(cName.substr(0, search.size()) == search)
      {
        match = true;
        auto nCol = is_b1 ? replace_b1(col, cName) : replace_b2(col, cName);
        __addCollision(solver, nCol);
      }
    }
    if(!match)
    {
      mc_rtc::log::error_and_throw("No match found for collision wildcard {} in {}", body, robot.name());
    }
    return true;
  };
  if(handle_wildcard(r1, col.body1, true) || handle_wildcard(r2, col.body2, false))
  {
    return;
  }
  const auto & body1 = r1.convex(col.body1);
  const auto & body2 = r2.convex(col.body2);
  const sva::PTransformd & X_b1_c = r1.collisionTransform(col.body1);
  const sva::PTransformd & X_b2_c = r2.collisionTransform(col.body2);
  int collId = __createCollId(col);
  if(collId < 0)
  {
    return;
  }
  cols.push_back(col);
  auto jointsToSelector = [](const mc_rbdyn::Robot & robot,
                             const std::vector<std::string> & joints) -> Eigen::VectorXd {
    if(joints.size() == 0)
    {
      return Eigen::VectorXd::Zero(0);
    }
    Eigen::VectorXd ret = Eigen::VectorXd::Zero(robot.mb().nrDof());
    for(const auto & j : joints)
    {
      auto mbcIndex = robot.jointIndexByName(j);
      auto dofIndex = robot.mb().jointPosInDof(static_cast<int>(mbcIndex));
      const auto & joint = robot.mb().joint(static_cast<int>(mbcIndex));
      ret.segment(dofIndex, joint.dof()).setOnes();
    }
    return ret;
  };
  auto r1Selector = jointsToSelector(robots.robot(r1Index), col.r1Joints);
  auto r2Selector =
      r1Index == r2Index ? Eigen::VectorXd::Zero(0).eval() : jointsToSelector(robots.robot(r2Index), col.r2Joints);
  switch(backend_)
  {
    case QPSolver::Backend::Tasks:
    {
      auto & collConstr = tasks_constraint(constraint_);
      if(r1.mb().nrDof() == 0)
      {
        collConstr.addCollision(robots.mbs(), collId, static_cast<int>(r2Index), body2.first, body2.second.get(),
                                X_b2_c, static_cast<int>(r1Index), body1.first, body1.second.get(), X_b1_c, col.iDist,
                                col.sDist, col.damping, defaultDampingOffset, r2Selector, r1Selector);
      }
      else
      {
        collConstr.addCollision(robots.mbs(), collId, static_cast<int>(r1Index), body1.first, body1.second.get(),
                                X_b1_c, static_cast<int>(r2Index), body2.first, body2.second.get(), X_b2_c, col.iDist,
                                col.sDist, col.damping, defaultDampingOffset, r1Selector, r2Selector);
      }
    }
    break;
    default:
      break;
  }
  if(solver.gui())
  {
    if(!gui_)
    {
      gui_ = solver.gui();
      category_ = {"Collisions", r1.name() + "/" + r2.name()};
    }
    addMonitorButton(collId, col);
  }
}

void CollisionsConstraint::addMonitorButton(int collId, const mc_rbdyn::Collision & col)
{
  if(gui_ && inSolver_)
  {
    auto & gui = *gui_;
    std::string name = col.body1 + "/" + col.body2;
    category_.push_back("Monitors");
    gui.addElement(category_, mc_rtc::gui::Checkbox(
                                  "Monitor " + name, [collId, this]() { return monitored_.count(collId) != 0; },
                                  [collId, this]() { toggleCollisionMonitor(collId); }));
    category_.pop_back();
  }
}

void CollisionsConstraint::toggleCollisionMonitor(int collId)
{
  auto findCollisionById = [this, collId]() -> const mc_rbdyn::Collision & {
    for(const auto & c : collIdDict)
    {
      if(c.second.first == collId)
      {
        return c.second.second;
      }
    }
    mc_rtc::log::error_and_throw("[CollisionConstraint] Attempted to toggleCollisionMonitor on non-existent collision");
  };
  const auto & col = findCollisionById();
  auto & gui = *gui_;
  std::string label = col.body1 + "::" + col.body2;
  if(monitored_.count(collId))
  {
    // Remove the monitor
    gui.removeElement(category_, label);
    category_.push_back("Arrows");
    gui.removeElement(category_, label);
    category_.pop_back();
    monitored_.erase(collId);
  }
  else
  {
    // Add the monitor
    switch(backend_)
    {
      case QPSolver::Backend::Tasks:
      {
        auto & collConstr = tasks_constraint(constraint_);
        gui.addElement(category_, mc_rtc::gui::Label(label, [&collConstr, collId]() {
                         return fmt::format("{:0.2f} cm", collConstr.getCollisionData(collId).distance * 100);
                       }));
        category_.push_back("Arrows");
        gui.addElement(
            category_,
            mc_rtc::gui::Arrow(
                label,
                [&collConstr, collId]() -> const Eigen::Vector3d & { return collConstr.getCollisionData(collId).p1; },
                [&collConstr, collId]() -> const Eigen::Vector3d & { return collConstr.getCollisionData(collId).p2; }));
        category_.pop_back();
      }
      break;
      default:
        break;
    }
    monitored_.insert(collId);
  }
}

void CollisionsConstraint::addCollision(QPSolver & solver, const mc_rbdyn::Collision & col)
{
  addCollisions(solver, {col});
}

void CollisionsConstraint::addCollisions(QPSolver & solver, const std::vector<mc_rbdyn::Collision> & cols)
{
  for(const auto & c : cols)
  {
    __addCollision(solver, c);
  }
  switch(backend_)
  {
    case QPSolver::Backend::Tasks:
    {
      auto & collConstr = tasks_constraint(constraint_);
      auto & qpsolver = tasks_solver(solver);
      collConstr.updateNrVars({}, qpsolver.data());
      qpsolver.updateConstrSize();
    }
    default:
      break;
  }
}

void CollisionsConstraint::addToSolverImpl(QPSolver & solver)
{
  switch(backend_)
  {
    case QPSolver::Backend::Tasks:
    {
      auto & collConstr = tasks_constraint(constraint_);
      auto & qpsolver = tasks_solver(solver);
      collConstr.addToSolver(solver.robots().mbs(), qpsolver.solver());
    }
    default:
      break;
  }
  for(const auto & cols : collIdDict)
  {
    addMonitorButton(cols.second.first, cols.second.second);
  }
}

void CollisionsConstraint::removeFromSolverImpl(QPSolver & solver)
{
  switch(backend_)
  {
    case QPSolver::Backend::Tasks:
    {
      auto & collConstr = tasks_constraint(constraint_);
      auto & qpsolver = tasks_solver(solver);
      collConstr.removeFromSolver(qpsolver.solver());
    }
    default:
      break;
  }
  gui_->removeCategory(category_);
}

void CollisionsConstraint::reset()
{
  cols.clear();
  collIdDict.clear();
  switch(backend_)
  {
    case QPSolver::Backend::Tasks:
    {
      auto & collConstr = tasks_constraint(constraint_);
      collConstr.reset();
    }
    default:
      break;
  }
  if(gui_)
  {
    gui_->removeCategory(category_);
  }
}

std::string CollisionsConstraint::__keyByNames(const std::string & name1, const std::string & name2)
{
  return name1 + name2;
}

int CollisionsConstraint::__createCollId(const mc_rbdyn::Collision & col)
{
  std::string key = __keyByNames(col.body1, col.body2);
  auto it = collIdDict.find(key);
  if(it != collIdDict.end())
  {
    return -1;
  }
  int collId = this->collId;
  collIdDict[key] = std::pair<int, mc_rbdyn::Collision>(collId, col);
  this->collId += 1;
  return collId;
}

std::pair<int, mc_rbdyn::Collision> CollisionsConstraint::__popCollId(const std::string & name1,
                                                                      const std::string & name2)
{
  std::string key = __keyByNames(name1, name2);
  if(collIdDict.count(key))
  {
    std::pair<int, mc_rbdyn::Collision> p = collIdDict[key];
    collIdDict.erase(key);
    return p;
  }
  return std::pair<unsigned int, mc_rbdyn::Collision>(0, mc_rbdyn::Collision());
}

} // namespace mc_solver

namespace
{

static auto registered = mc_solver::ConstraintSetLoader::register_load_function(
    "collision",
    [](mc_solver::QPSolver & solver, const mc_rtc::Configuration & config) {
      auto ret = std::make_shared<mc_solver::CollisionsConstraint>(
          solver.robots(), robotIndexFromConfig(config, solver.robots(), "collision", false, "r1Index", "r1", ""),
          robotIndexFromConfig(config, solver.robots(), "collision", false, "r2Index", "r2", ""), solver.dt());
      if(ret->r1Index == ret->r2Index)
      {
        if(config("useCommon", false))
        {
          ret->addCollisions(solver, solver.robots().robotModule(ret->r1Index).commonSelfCollisions());
        }
        else if(config("useMinimal", false))
        {
          ret->addCollisions(solver, solver.robots().robotModule(ret->r1Index).minimalSelfCollisions());
        }
      }
      std::vector<mc_rbdyn::Collision> collisions = config("collisions", std::vector<mc_rbdyn::Collision>{});
      ret->addCollisions(solver, collisions);
      return ret;
    });
}
