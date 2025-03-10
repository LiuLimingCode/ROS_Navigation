#!/usr/bin/env python
#coding:utf-8

import rospy
from std_msgs.msg import Int16
from geometry_msgs.msg import Twist
from sensor_msgs.msg import Joy
from ackermann_msgs.msg import AckermannDriveStamped

import sys, select, termios, tty
import time

state_pub = None
Stop_Flag_pub = None

speed_max = 0
speed_min = 0
speed_set = 0
max_steering_angle = 0

speed_t = 0
angle_t = 0

def joy_callback(data):
    global state_pub
    global Stop_Flag_pub

    global speed_set
    global speed_t
    global angle_t

    ack = AckermannDriveStamped()
    ack.header.stamp = rospy.Time.now()

    Stop = Int16()

    if (data.buttons[10] == 1):         #速度切换键（SELECT）
        if (speed_set == speed_max):
            speed_set = speed_min
        else:
            speed_set = speed_max

    if (data.axes[7] == 1):     #加速键（上键）
        speed_t = speed_set
    elif (data.axes[7] == -1):  #倒退键（下键）
        speed_t = -speed_set
    else:
        speed_t = 0

    if (data.buttons[1] == 1):  #右转键（B）
        angle_t = -max_steering_angle
    elif (data.buttons[3] == 1):   #左转键（X）
        angle_t = max_steering_angle
    else:
        angle_t = 0

    if(data.buttons[6] == 1 or data.buttons[8] == 1):
        Stop.data = 1
    if(data.buttons[7] == 1 or data.buttons[9] == 1):
        Stop.data = 2

    ack.drive.speed = speed_t
    ack.drive.steering_angle = angle_t
    print("speed=%f angle=%f"%(speed_t,angle_t))
    state_pub.publish(ack)
    Stop_Flag_pub.publish(Stop)

def main():
    global state_pub
    global Stop_Flag_pub

    global speed_max
    global speed_min
    global speed_set
    global max_steering_angle
    speed_max = rospy.get_param("~speed_max", 2.0)
    max_steering_angle = rospy.get_param("~max_steering_angle", 0.34)
    ackermann_cmd_topic = rospy.get_param("~ackermann_cmd_topic", "/cmd_ackermann")

    speed_min = 1.0
    speed_set = speed_min
    state_pub = rospy.Publisher(ackermann_cmd_topic, AckermannDriveStamped, queue_size=1)
    Stop_Flag_pub = rospy.Publisher("Car_Stop",Int16,queue_size=1)

if __name__=="__main__":
    global state_pub

    rospy.init_node('racecar_joy')
    main()
    rospy.Subscriber("joy", Joy, joy_callback)

    try:
        rospy.spin()
    except:
        print "error"
    finally:
    	print "finally"
        ack = AckermannDriveStamped()
        ack.header.stamp = rospy.Time.now()
        ack.drive.speed = 0
        ack.drive.steering_angle = 0
        state_pub.publish(ack)