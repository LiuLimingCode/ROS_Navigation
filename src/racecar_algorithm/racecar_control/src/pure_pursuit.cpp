/*
# Copyright 2018 HyphaROS Workshop.
# Latest Modifier: HaoChih, LIN (hypha.ros@gmail.com)
# Original Author: ChanYuan KUO & YoRu LU
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
*/

#include <iostream>
#include "ros/ros.h"
#include <std_srvs/SetBool.h>
#include <geometry_msgs/PoseWithCovarianceStamped.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/Twist.h>
#include <tf/tf.h>
#include <tf/transform_listener.h>
#include <tf/transform_datatypes.h>
#include <nav_msgs/Path.h>
#include <nav_msgs/Odometry.h>
#include <ackermann_msgs/AckermannDriveStamped.h>
#include <visualization_msgs/Marker.h>

#define DEBUG
#ifdef DEBUG
#define PURE_PURSUIT_INFO ROS_ERROR
#else
#define PURE_PURSUIT_INFO
#endif
#define PI 3.1415926535898

/********************/
/* CLASS DEFINITION */
/********************/
class PurePursuit
{
private:
    ros::NodeHandle n_;
    ros::Subscriber odom_sub, path_sub, goal_sub, amcl_sub;
    ros::Publisher ackermann_pub, cmdvel_pub, marker_pub;
    ros::ServiceServer stop_robot_srv;
    ros::Timer timer1, timer2;
    tf::TransformListener tf_listener;

    visualization_msgs::Marker points, goal_circle;
    geometry_msgs::Point odom_goal_pos, goal_pos;
    geometry_msgs::Twist cmd_vel;
    ackermann_msgs::AckermannDriveStamped ackermann_cmd;
    nav_msgs::Odometry odom;
    nav_msgs::Path map_path, odom_path;

    std::vector<geometry_msgs::Point> forwardPtVector;

    double L, Lfw, Vcmd_max, Vcmd_min, lfw, steering, velocity, cost_max, predicted_dist;
    double steering_gain, base_angle, goal_radius, speed_incremental, speed_expected;
    int controller_freq;
    bool cmd_vel_mode, debug_mode, smooth_accel, stop_robot;
    bool foundForwardPt, goal_received, goal_reached, transform_path_data;


public:

    PurePursuit(void)
    {
        //Private parameters handler
        ros::NodeHandle pn("~");

        //Car parameter
        pn.param("L", L, 0.26); // length of car
        pn.param("Vcmd_max", Vcmd_max, 1.0);// reference speed (m/s)
        pn.param("Vcmd_min", Vcmd_min, 1.0);// reference speed (m/s)
        pn.param("Lfw", Lfw, 3.0); // forward look ahead distance (m)
        pn.param("predicted_dist", predicted_dist, 0.1);
        pn.param("lfw", lfw, 0.13); // distance between front the center of car

        //Controller parameter
        pn.param("controller_freq", controller_freq, 20);
        pn.param("steering_gain", steering_gain, 1.0); // 在算法计算每一次舵机转角时都会乘上这个值
        pn.param("goal_radius", goal_radius, 0.5); // goal radius (m) 指机器人最后与终点相距多少米,可以判断为机器人到达终点
        pn.param("base_angle", base_angle, 0.0); // neutral point of servo (rad) 在算法计算每一次舵机转角时都会加上这个值
        pn.param("cmd_vel_mode", cmd_vel_mode, false); // whether or not publishing cmd_vel
        pn.param("debug_mode", debug_mode, false); // debug mode
        pn.param("smooth_accel", smooth_accel, true); // smooth the acceleration of car 限制加速度
        pn.param("speed_incremental", speed_incremental, 0.5); // speed incremental value (discrete acceleraton), unit: m/s 机器人加速度,该值乘上 controller_freq 才代表每秒的最大加速度
        pn.param("stop_robot", stop_robot, false);
        pn.param("cost_max", cost_max, 0.25); // distance between front the center of car

        //Publishers and Subscribers
        odom_sub = n_.subscribe("/pure_pursuit/odom", 1, &PurePursuit::odomCB, this);
        path_sub = n_.subscribe("/pure_pursuit/global_planner", 1, &PurePursuit::pathCB, this);
        goal_sub = n_.subscribe("/pure_pursuit/goal", 1, &PurePursuit::goalCB, this);
        amcl_sub = n_.subscribe("/amcl_pose", 5, &PurePursuit::amclCB, this);
        marker_pub = n_.advertise<visualization_msgs::Marker>("/pure_pursuit/path_marker", 10);
        ackermann_pub = n_.advertise<ackermann_msgs::AckermannDriveStamped>("/pure_pursuit/ackermann_cmd", 1);
        if(cmd_vel_mode) cmdvel_pub = n_.advertise<geometry_msgs::Twist>("/pure_pursuit/cmd_vel", 1);
        stop_robot_srv = n_.advertiseService("/pure_pursuit/stop_robot", &PurePursuit::stopRobotCB, this);

        //Timer
        timer1 = n_.createTimer(ros::Duration((1.0)/controller_freq), &PurePursuit::controlLoopCB, this); // Duration(0.05) -> 20Hz


        //Init variables
        foundForwardPt = false;
        goal_received = false;
        goal_reached = false;
        velocity = 0.0;
        steering = base_angle;

        //Show info
        ROS_INFO("[param] base_angle: %f", base_angle);
        ROS_INFO("[param] Vcmd_max: %f", Vcmd_max);
        ROS_INFO("[param] Vcmd_min: %f", Vcmd_min);
        ROS_INFO("[param] Lfw: %f", Lfw);

        //Visualization Marker Settings
        initMarker();

        cmd_vel = geometry_msgs::Twist();
        ackermann_cmd = ackermann_msgs::AckermannDriveStamped();
    }


    // 初始化 visualization_msgs::Marker points, goal_circle;
    void initMarker()
    {
        points.header.frame_id = goal_circle.header.frame_id = "odom";
        points.ns = goal_circle.ns = "Markers";
        points.action = goal_circle.action = visualization_msgs::Marker::ADD;
        points.pose.orientation.w = goal_circle.pose.orientation.w = 1.0;
        points.id = 0;
        goal_circle.id = 2;

        points.type = visualization_msgs::Marker::POINTS;
        goal_circle.type = visualization_msgs::Marker::CYLINDER;
        // POINTS markers use x and y scale for width/height respectively
        points.scale.x = 0.2;
        points.scale.y = 0.2;

        goal_circle.scale.x = goal_radius;
        goal_circle.scale.y = goal_radius;
        goal_circle.scale.z = 0.1;

        // Points are green
        points.color.g = 1.0f;
        points.color.a = 1.0;

        //goal_circle is yellow
        goal_circle.color.r = 1.0;
        goal_circle.color.g = 1.0;
        goal_circle.color.b = 0.0;
        goal_circle.color.a = 0.5;
    }

    // 得到 odom 数据
    void odomCB(const nav_msgs::Odometry::ConstPtr& odomMsg)
    {
        this->odom = *odomMsg;
    }

    // 得到路径数据
    void pathCB(const nav_msgs::Path::ConstPtr& pathMsg)
    {
        this->map_path = *pathMsg;
        transform_path_data = true;
    }

    // 得到 goal 数据,并且转换到 map 坐标系下
    void goalCB(const geometry_msgs::PoseStamped::ConstPtr& goalMsg)
    {
        this->goal_pos = goalMsg->pose.position;    
        try
        {
            geometry_msgs::PoseStamped odom_goal;
            tf_listener.transformPose("odom", ros::Time(0) , *goalMsg, "map" ,odom_goal); // 将goal从map坐标系转换到odom坐标系下
            odom_goal_pos = odom_goal.pose.position;
            goal_received = true;
            goal_reached = false;

            /*Draw Goal on RVIZ*/
            goal_circle.pose = odom_goal.pose;
            marker_pub.publish(goal_circle);
        }
        catch(tf::TransformException &ex)
        {
            ROS_ERROR("%s",ex.what());
            ros::Duration(1.0).sleep();
        }
    }

    double getYawFromPose(const geometry_msgs::Pose& carPose)
    {
        tf::Pose pose;
        tf::poseMsgToTF(carPose, pose);
        const double psi = tf::getYaw(pose.getRotation());

        return psi;
    }

    // 判断 wayPt 是否在 carPose 的前面
    bool isForwardWayPt(const geometry_msgs::Point& wayPt, const geometry_msgs::Pose& carPose)
    {
        float car2wayPt_x = wayPt.x - carPose.position.x;
        float car2wayPt_y = wayPt.y - carPose.position.y;
        double car_theta = getYawFromPose(carPose);

        float car_car2wayPt_x = cos(car_theta)*car2wayPt_x + sin(car_theta)*car2wayPt_y; // 将目标点与当前机器人的坐标值转换到机器人坐标系的X轴下,来判断目标点是否在机器人前方
        float car_car2wayPt_y = -sin(car_theta)*car2wayPt_x + cos(car_theta)*car2wayPt_y;

        if(car_car2wayPt_x > 0) /*is Forward WayPt*/
            return true;
        else
            return false;
    }

    double getDistanceBetweenPoints(const geometry_msgs::Point& toPoint, const geometry_msgs::Point& fromPoint)
    {
        double dx = toPoint.x - fromPoint.x;
        double dy = toPoint.y - fromPoint.y;
        return sqrt(dx * dx + dy * dy);
    }

    // 将path上的所有数据都转移到odom坐标系下
    void transformPathData(void)
    {
        if(map_path.poses.size() == 0) return;
        nav_msgs::Path path;
        if(!goal_reached && transform_path_data)
        {
            transform_path_data = false;
            path.poses.clear();
            for(int i = 0; i< map_path.poses.size(); i++)
            {
                geometry_msgs::PoseStamped map_path_pose = map_path.poses[i];
                geometry_msgs::PoseStamped odom_path_pose;

                try
                {
                    tf_listener.transformPose("odom", ros::Time(0) , map_path_pose, "map" ,odom_path_pose); // 将点从 map 坐标系转换到 odom 坐标系下
                    path.poses.push_back(odom_path_pose);
                }
                catch(tf::TransformException &ex)
                {
                    ROS_ERROR("%s",ex.what());
                    return;
                }
            }
            odom_path = path;
        }

        if(debug_mode) // 输出路径信息
        {
            PURE_PURSUIT_INFO("path info:");
            for(int index = 0; index < odom_path.poses.size(); ++index)
            {
                PURE_PURSUIT_INFO("index: %d, x: %lf, y: %lf", index, odom_path.poses[index].pose.position.x, odom_path.poses[index].pose.position.x);
            }
        }
    }

    bool findPredictedCarPose(const geometry_msgs::Pose& carPose, geometry_msgs::Pose & carPosePredicted)
    {
        bool foundPredictedCarPt = false;
        geometry_msgs::Point carPose_pos = carPose.position;
        double carPose_yaw = getYawFromPose(carPose);

        if(!goal_reached)
        {
            for(int i = 0; i< odom_path.poses.size(); i++) // 对 path 上的每一个点进行处理
            {
                geometry_msgs::PoseStamped odom_path_pose = odom_path.poses[i];
                geometry_msgs::Point odom_path_wayPt = odom_path_pose.pose.position;

                if(isForwardWayPt(odom_path_wayPt, carPose))
                {
                    double dist = getDistanceBetweenPoints(odom_path_wayPt, carPose_pos);
                    if(dist > predicted_dist)
                    {
                        foundPredictedCarPt = true;
                        carPosePredicted = odom_path.poses[i].pose;
                        break;
                    }
                }
            }
        }
        return foundPredictedCarPt;
    }

    geometry_msgs::Point get_odom_car2WayPtVec(const geometry_msgs::Pose& carPose)
    {
        geometry_msgs::Point carPose_pos = carPose.position;
        double carPose_yaw = getYawFromPose(carPose);
        geometry_msgs::Point forwardPt;
        geometry_msgs::Point odom_car2WayPtVec;
        foundForwardPt = false;
        forwardPtVector.clear();

        double cost = 0, x_diff, y_diff, yaw, yaw_last;

        if(!goal_reached)
        {

            for(int i = 0; i< odom_path.poses.size(); i++) // 对 path 上的每一个点进行处理
            {
                geometry_msgs::PoseStamped odom_path_pose = odom_path.poses[i];
                geometry_msgs::Point odom_path_wayPt = odom_path_pose.pose.position;

                bool _isForwardWayPt = isForwardWayPt(odom_path_wayPt, carPose);
                
                if(_isForwardWayPt)
                {
                    forwardPtVector.push_back(odom_path_wayPt);
                    int index = forwardPtVector.size() - 1;

                    double dist = getDistanceBetweenPoints(forwardPtVector[index], carPose_pos);

                    if(!foundForwardPt) // 如果 odom_path_wayPt 在 carPosePredicted 的前面
                    {
                        if(dist > Lfw)
                        {
                            forwardPt = odom_path_wayPt; // 如果 odom_path_wayPt 满足在 carPosePredicted 的前面,并且与 carPosePredicted 的距离大于参数 Lfw,记录满足条件的的一个点
                            foundForwardPt = true;
                            PURE_PURSUIT_INFO("found the forwardPt, index: %d, x: %lf, y: %lf", index, forwardPt.x, forwardPt.y);
                            break;
                        }                            
                    }
                }
            }
        }
        else if(goal_reached)
        {
            forwardPt = odom_goal_pos; // 如果已经接近终点,那么设定 forwardPt 为 odom_goal_pos
            foundForwardPt = false;
            //ROS_INFO("goal REACHED!");
        }

        /*Visualized Target Point on RVIZ*/
        /*Clear former target point Marker*/
        points.points.clear();
        
        if(foundForwardPt && !goal_reached) // 使用 Marker 将 forwardPt 和 carPose_pos 标记出来
        {
            if(forwardPtVector.size() == 0) ROS_ERROR("forwardPtVector.size() == 0");
            points.points = (forwardPtVector);
        }
        else ROS_ERROR("foundForwardPt%d", (int)foundForwardPt);

        marker_pub.publish(points);
        
        odom_car2WayPtVec.x = cos(carPose_yaw)*(forwardPt.x - carPose_pos.x) + sin(carPose_yaw)*(forwardPt.y - carPose_pos.y);
        odom_car2WayPtVec.y = -sin(carPose_yaw)*(forwardPt.x - carPose_pos.x) + cos(carPose_yaw)*(forwardPt.y - carPose_pos.y);
        return odom_car2WayPtVec; // 返回 forwardPt 与 carPose_pos 的距离并转换到机器人的坐标系下
    }

    double getSteering(double eta)
    {
        return atan2((this->L*sin(eta)),(this->Lfw/2 + this->lfw*cos(eta)));
    }

    // 接受到机器人在 map 坐标系下的坐标,判断机器人是否已经到达目标点
    void amclCB(const geometry_msgs::PoseWithCovarianceStamped::ConstPtr& amclMsg)
    {

        if(this->goal_received)
        {
            double car2goal_x = this->goal_pos.x - amclMsg->pose.pose.position.x;
            double car2goal_y = this->goal_pos.y - amclMsg->pose.pose.position.y;
            double dist2goal = sqrt(car2goal_x*car2goal_x + car2goal_y*car2goal_y);
            if(dist2goal < this->goal_radius)
            {
                this->goal_reached = true;
                this->goal_received = false;
                ROS_INFO("Goal Reached !");
            }
        }
    }

    // 控制周期
    void controlLoopCB(const ros::TimerEvent&)
    {

        geometry_msgs::Pose carPose = this->odom.pose.pose;
        geometry_msgs::Twist carVel = this->odom.twist.twist;

        if(this->goal_received)
        {
            PURE_PURSUIT_INFO("============= controlLoopCB =============");
            PURE_PURSUIT_INFO("current car pose: x: %lf, y: %lf, vx: %lf, vyaw: %lf", carPose.position.x, carPose.position.y, carVel.linear.x, carVel.angular.z);
            transformPathData();

            geometry_msgs::Pose carPosePredicted;
            if(findPredictedCarPose(carPose, carPosePredicted))
            {
                PURE_PURSUIT_INFO("found the predicted car pose, x: %lf, y: %lf", carPosePredicted.position.x, carPosePredicted.position.y);
                carPosePredicted.orientation = carPose.orientation;
            }
            else 
            {
                PURE_PURSUIT_INFO("didn't find the predicted car pose!!!!!!!!!!!");
                carPosePredicted = carPose;
            }

            /*Estimate Steering Angle*/
            geometry_msgs::Point odom_car2WayPtVec = get_odom_car2WayPtVec(carPosePredicted);
            double eta = atan2(odom_car2WayPtVec.y,odom_car2WayPtVec.x); // 计算 forwardPt 与 carPose_pos 的角度

            speed_expected = Vcmd_min;
            
            if(foundForwardPt)
            {
                this->steering = this->base_angle + getSteering(eta)*this->steering_gain; // 计算舵机转角

                /*Estimate Gas Input*/
                if(!this->goal_reached)
                {
                    if(this->smooth_accel) // 计算速度
                    {
                        if(speed_expected >= this->velocity) this->velocity = std::min(this->velocity + this->speed_incremental, speed_expected);
                        else this->velocity = speed_expected;
                    }
                    else
                    {
                        this->velocity = speed_expected;
                    }
                    if(debug_mode) ROS_INFO("Velocity = %.2f, Steering = %.2f", this->velocity, this->steering);
                }
            }
        }

        if(this->goal_reached)
        {
            this->velocity = 0.0;
            this->steering = this->base_angle;
        }
        
        if(!stop_robot)
        {
            this->ackermann_cmd.header.stamp = ros::Time::now();
            this->ackermann_cmd.drive.steering_angle = this->steering;
            this->ackermann_cmd.drive.speed = this->velocity;
            this->ackermann_pub.publish(this->ackermann_cmd);

            if(this->cmd_vel_mode)
            {
                this->cmd_vel.linear.x = this->velocity;
                this->cmd_vel.angular.z = this->steering;
                this->cmdvel_pub.publish(this->cmd_vel);
            }
        }
    }

    bool stopRobotCB(std_srvs::SetBool::Request &req, std_srvs::SetBool::Response &res)
    {
        stop_robot = req.data;
        res.success = stop_robot;
        res.message = "success"; 
        return true;
    }
}; // end of class


/*****************/
/* MAIN FUNCTION */
/*****************/
int main(int argc, char **argv)
{
    //Initiate ROS
    ros::init(argc, argv, "PurePursuit");
    PurePursuit controller;
    ros::AsyncSpinner spinner(2); // Use multi threads
    spinner.start();
    ros::waitForShutdown();
    return 0;
}
