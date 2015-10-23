/*
 * Copyright 2014 Instituto de Sistemas e Robotica, Instituto Superior Tecnico
 *
 * This file is part of RoAH RSBB.
 *
 * RoAH RSBB is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * RoAH RSBB is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with RoAH RSBB.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __CORE_ZONE_MANAGER_H__
#define __CORE_ZONE_MANAGER_H__

#include "core_includes.h"

#include "core_shared_state.h"
#include "core_zone_base.h"
#include "core_zone_exec.h"



class Zone
  : boost::noncopyable
{
    CoreSharedState& ss_;

    string name_;
    multimap<Time, const Event> events_;
    multimap<Time, const Event>::const_iterator current_event_;

    unique_ptr<ExecutingBenchmark> executing_benchmark_;

  public:
    typedef std::shared_ptr<Zone> Ptr;

    Zone (CoreSharedState& ss,
          YAML::Node const& zone_node)
      : ss_ (ss)
    {
      if (! zone_node["zone"]) {
        ROS_FATAL_STREAM ("Schedule file is missing a \"zone\" entry!");
        abort_rsbb();
      }
      name_ = zone_node["zone"].as<string>();

      if (! zone_node["schedule"]) {
        ROS_FATAL_STREAM ("Schedule file is missing a \"schedule\" entry!");
        abort_rsbb();
      }
      if (! zone_node["schedule"].IsSequence()) {
        ROS_FATAL_STREAM ("Schedule in schedule file is not a sequence!");
        abort_rsbb();
      }
      for (YAML::Node const& event_node : zone_node["schedule"]) {
        Event e = Event (event_node);
        e.benchmark = ss_.benchmarks.get (e.benchmark_code);
        if (e.team != "ALL") {
          e.password = ss_.passwords.get (e.team);
        }
        events_.insert (make_pair (e.scheduled_time, e));

        if (! ( (e.benchmark_code == "HGTKMH")
                || (e.benchmark_code == "HWV")
                || (e.benchmark_code == "HCFGAC")
                || (e.benchmark_code == "HOPF")
                || (e.benchmark_code == "HNF")
                || (e.benchmark_code == "HSUF"))) {
          ROS_FATAL_STREAM ("Zone " << name_ << ": unsupported benchmark code " << e.benchmark_code);
          abort_rsbb();
        }

        if (e.benchmark_code == "HSUF") {
          if (e.team != "ALL") {
            ROS_FATAL_STREAM ("Zone " << name_ << ": benchmark code HSUF only supported for team ALL");
            abort_rsbb();
          }
        }

        if ( (e.benchmark_code == "HGTKMH")
             || (e.benchmark_code == "HWV")
             || (e.benchmark_code == "HCFGAC")
             || (e.benchmark_code == "HOPF")
             || (e.benchmark_code == "HNF")) {
          if (e.team == "ALL") {
            ROS_FATAL_STREAM ("Zone " << name_ << ": benchmark code " << e.benchmark_code << " not supported for team ALL");
            abort_rsbb();
          }
        }
      }

      if (events_.empty()) {
        ROS_FATAL_STREAM ("Zone " << name_ << " has no schedule defined");
        abort_rsbb();
      }

      current_event_ = events_.cbegin();
    }

    string
    name() const
    {
      return name_;
    }

    void
    end()
    {
      getGlobalCallbackQueue()->addCallback (boost::make_shared<roah_rsbb::CallbackItem> (boost::bind (&unique_ptr<ExecutingBenchmark>::reset, &executing_benchmark_, nullptr)));
    }

    void
    connect()
    {
      if (executing_benchmark_) {
        ROS_WARN_STREAM ("Zone: " << name() << " CONNECT (already issued)");
        return;
      }

      ROS_DEBUG_STREAM ("Zone: " << name() << " CONNECT");

      if (current_event_->second.benchmark_code == "HSUF") {
        if (current_event_->second.team != "ALL") {
          ROS_FATAL_STREAM ("Zone " << name_ << ": benchmark code HSUF only supported for team ALL");
          abort_rsbb();
        }

        executing_benchmark_.reset (new ExecutingAllRobotsBenchmark (ss_, current_event_->second, boost::bind (&Zone::end, this)));

        return;
      }

      roah_rsbb::RobotInfo ri = ss_.active_robots.get (current_event_->second.team);
      if (ri.team.empty()) {
        ROS_WARN_STREAM ("Zone: " << name() << " CONNECT ignored because robot not present");
        return;
      }

      if (ss_.benchmarking_robots.count (current_event_->second.team)) {
        ROS_ERROR_STREAM ("Zone: " << name() << " CONNECT ignored because robot of team " << current_event_->second.team << " is already executing a benchmark");
        return;
      }

      bool ok = false;
      do {
        try {
          if ( (current_event_->second.benchmark_code == "HGTKMH")
               || (current_event_->second.benchmark_code == "HWV")
               || (current_event_->second.benchmark_code == "HCFGAC")) {
            executing_benchmark_.reset (new ExecutingSimpleBenchmark (ss_, current_event_->second, boost::bind (&Zone::end, this), ri.robot));
          }
          else if ( (current_event_->second.benchmark_code == "HOPF")
                    || (current_event_->second.benchmark_code == "HNF")) {
            executing_benchmark_.reset (new ExecutingExternallyControlledBenchmark (ss_, current_event_->second, boost::bind (&Zone::end, this), ri.robot));
          }
          else {
            ROS_FATAL_STREAM ("Zone " << name_ << " unsupported benchmark code: " << current_event_->second.benchmark_code);
            abort_rsbb();
          }
          ok = true;
        }
        catch (...) {
          ROS_ERROR_STREAM ("Failed to create a private channel. Retrying on next port.");
        }
      }
      while (! ok);
    }

    void
    disconnect()
    {
      if (! executing_benchmark_) {
        ROS_WARN_STREAM ("Zone: " << name() << " DISCONNECT (not executing, ignored)");
        return;
      }

      ROS_DEBUG_STREAM ("Zone: " << name() << " DISCONNECT");
      executing_benchmark_->terminate_benchmark();
    }

    void
    set_score (roah_rsbb::Score const& score)
    {
      if (! executing_benchmark_) {
        ROS_WARN_STREAM ("Zone: " << name() << " SET_SCORE (not executing, ignored)");
        return;
      }

      ROS_DEBUG_STREAM ("Zone: " << name() << " SET_SCORE");
      executing_benchmark_->set_score (score);
    }

    void
    manual_operation_complete()
    {
      if (! executing_benchmark_) {
        ROS_WARN_STREAM ("Zone: " << name() << " MANUAL_OPERATION_COMPLETE (not executing, ignored)");
        return;
      }

      ROS_DEBUG_STREAM ("Zone: " << name() << " MANUAL_OPERATION_COMPLETE");
      executing_benchmark_->manual_operation_complete();
    }

    void
    omf_complete()
    {
      if (! executing_benchmark_) {
        ROS_WARN_STREAM ("Zone: " << name() << " omf_complete (not executing, ignored)");
        return;
      }

      ROS_DEBUG_STREAM ("Zone: " << name() << " omf_complete");
      executing_benchmark_->omf_complete();
    }

    void
    omf_damaged (uint8_t damaged)
    {
      if (! executing_benchmark_) {
        ROS_WARN_STREAM ("Zone: " << name() << " omf_damaged (not executing, ignored)");
        return;
      }

      ROS_DEBUG_STREAM ("Zone: " << name() << " omf_damaged");
      executing_benchmark_->omf_damaged (damaged);
    }

    void
    omf_button (uint8_t button)
    {
      if (! executing_benchmark_) {
        ROS_WARN_STREAM ("Zone: " << name() << " omf_button (not executing, ignored)");
        return;
      }

      ROS_DEBUG_STREAM ("Zone: " << name() << " omf_button");
      executing_benchmark_->omf_button (button);
    }

    void
    start()
    {
      if (! executing_benchmark_) {
        ROS_WARN_STREAM ("Zone: " << name() << " START (not executing, ignored)");
        return;
      }

      ROS_DEBUG_STREAM ("Zone: " << name() << " START");
      executing_benchmark_->start();
    }

    void
    stop()
    {
      if (! executing_benchmark_) {
        ROS_WARN_STREAM ("Zone: " << name() << " STOP (not executing, ignored)");
        return;
      }

      ROS_DEBUG_STREAM ("Zone: " << name() << " STOP");
      executing_benchmark_->stop();
    }

    void
    previous()
    {
      if (executing_benchmark_) {
        ROS_WARN_STREAM ("Zone: " << name() << " PREVIOUS (executing, ignored)");
        return;
      }

      ROS_DEBUG_STREAM ("Zone: " << name() << " PREVIOUS");

      if (current_event_ != events_.cbegin()) {
        --current_event_;
      }
    }

    void
    next()
    {
      if (executing_benchmark_) {
        ROS_WARN_STREAM ("Zone: " << name() << " NEXT (executing, ignored)");
        return;
      }

      ROS_DEBUG_STREAM ("Zone: " << name() << " NEXT");

      if (current_event_ != prev (events_.cend())) {
        ++current_event_;
      }
    }

    roah_rsbb::ZoneState
    msg (Time const& now)
    {
      roah_rsbb::ZoneState zone;

      zone.zone = name();

      zone.name = current_event_->second.benchmark.name;
      zone.desc = current_event_->second.benchmark.desc;
      zone.code = current_event_->second.benchmark.code;
      zone.timeout = current_event_->second.benchmark.timeout;
      zone.team = current_event_->second.team;
      zone.round = current_event_->second.round;
      zone.run = current_event_->second.run;
      zone.schedule = current_event_->second.scheduled_time;

      if (executing_benchmark_) {
        executing_benchmark_->fill (now, zone);

        zone.connect_enabled = false;
        zone.disconnect_enabled = true;
        zone.prev_enabled = false;
        zone.next_enabled = false;
      }
      else {
        zone.timer = current_event_->second.benchmark.timeout;
        zone.state = "";
        zone.manual_operation = "";

        zone.start_enabled = false;
        zone.stop_enabled = false;

        Duration allowed_skew = Duration (param_direct<double> ("~allowed_skew", 0.1));
        if (current_event_->second.benchmark.code == "HSUF") {
          vector<string> teams_out_of_sync;
          for (roah_rsbb::RobotInfo const& ri : ss_.active_robots.get ()) {
            if ( ( (-allowed_skew) >= ri.skew) || (ri.skew >= allowed_skew)) {
              teams_out_of_sync.push_back (ri.team);
            }
          }
          if (teams_out_of_sync.empty()) {
            zone.connect_enabled = true;
            add_to_sting (zone.state) << "Benchmark will start with all active robots";
          }
          else {
            zone.connect_enabled = false;
            add_to_sting t (zone.state);
            t << "Robots with clock skew:";
            for (string const& i : teams_out_of_sync) {
              t << " " << i;
            }
          }
        }
        else if (ss_.benchmarking_robots.count (current_event_->second.team)) {
          zone.connect_enabled = false;
          add_to_sting (zone.state) << "Robot is already executing another benchmark";
        }
        else {
          roah_rsbb::RobotInfo ri = ss_.active_robots.get (current_event_->second.team);
          if (ri.team.empty()) {
            zone.connect_enabled = false;
            add_to_sting (zone.state) << "Robot not detected as active";
          }
          else {
            if ( ( (-allowed_skew) < ri.skew) && (ri.skew < allowed_skew)) {
              zone.connect_enabled = true;
              add_to_sting (zone.state) << "Robot ready to accept connection";
            }
            else {
              zone.connect_enabled = false;
              add_to_sting (zone.state) << "Clock skew too large: " + boost::lexical_cast<string> (ri.skew);
            }
          }
        }

        zone.disconnect_enabled = false;
        zone.prev_enabled = current_event_ != events_.cbegin();
        zone.next_enabled = current_event_ != prev (events_.cend());
      }

      return zone;
    }

    void
    msg (Time const& now,
         multimap<Time, roah_rsbb::ScheduleInfo>& map)
    {
      for (multimap<Time, const Event>::const_iterator i = events_.cbegin();
           i != events_.cend();
           ++i) {
        roah_rsbb::ScheduleInfo msg;
        msg.team = i->second.team;
        msg.benchmark = i->second.benchmark.desc;
        msg.round = i->second.round;
        msg.run = i->second.run;
        msg.time = to_string (i->second.scheduled_time);
        msg.running = ( (i == current_event_) && executing_benchmark_);
        map.insert (make_pair (i->second.scheduled_time, msg));
      }
    }
};



class CoreZoneManager
  : boost::noncopyable
{
    CoreSharedState& ss_;

    map<string, Zone::Ptr> zones_;

  public:
    CoreZoneManager (CoreSharedState& ss)
      : ss_ (ss)
    {
      using namespace YAML;

      Node file = LoadFile (param_direct<string> ("~schedule_file", "schedule.yaml"));
      if (! file.IsSequence()) {
        ROS_FATAL_STREAM ("Schedule file is not a sequence!");
        abort_rsbb();
      }
      for (Node const& zone_node : file) {
        Zone::Ptr zone = make_shared<Zone> (ss_, zone_node);
        zones_[zone->name()] = zone;
      }
    }

    Zone::Ptr
    get (string const& name)
    {
      auto zone = zones_.find (name);
      if (zone != zones_.end()) {
        return zone->second;
      }
      return Zone::Ptr();
    }

    void
    msg (Time const& now,
         vector<roah_rsbb::ZoneState>& msg)
    {
      for (auto const& i : zones_) {
        if (i.second) {
          msg.push_back (i.second->msg (now));
        }
      }
    }

    void
    msg (Time const& now,
         multimap<Time, roah_rsbb::ScheduleInfo>& map)
    {
      for (auto const& i : zones_) {
        if (i.second) {
          i.second->msg (now, map);
        }
      }
    }
};

#endif
