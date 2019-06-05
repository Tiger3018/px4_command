/***************************************************************************************************************************
* px4_pos_controller.cpp
*
* Author: Qyp
*
* Update Time: 2019.5.9
*
* Introduction:  PX4 Position Controller using cascade PID method or PD+UDE or passivity
*         1. Subscribe command.msg from upper nodes (e.g. Target_tracking.cpp)
*         2. Calculate the accel_sp using pos_controller_PID.h(pos_controller_UDE.h pos_controller_passivity.h)
*         3. Send command to mavros package using command_to_mavros.h (mavros package will send the message to PX4 as Mavlink msg)
*         4. PX4 firmware will recieve the Mavlink msg by mavlink_receiver.cpp in mavlink module.
***************************************************************************************************************************/
#include <ros/ros.h>

#include <command_to_mavros.h>
#include <px4_command/command.h>
#include <pos_controller_PID.h>
#include <pos_controller_UDE.h>
#include <pos_controller_passivity.h>
#include <pos_controller_ne.h>


#include <Eigen/Eigen>

using namespace std;

using namespace namespace_command_to_mavros;
using namespace namespace_PID;
using namespace namespace_UDE;
using namespace namespace_passivity;
using namespace namespace_NE;


//自定义的Command变量
//相应的命令分别为 移动(惯性系ENU)，移动(机体系)，悬停，降落，上锁，紧急降落，待机
enum Command
{
    Move_ENU,
    Move_Body,
    Hold,
    Land,
    Disarm,
    Failsafe_land,
    Idle,
    Takeoff
};

Eigen::Vector3d pos_sp(0,0,0);
Eigen::Vector3d vel_sp(0,0,0);
double yaw_sp = 0;
Eigen::Vector3d accel_sp(0,0,0);

//Command Now [from upper node]
px4_command::command Command_Now;                      //无人机当前执行命令

//Command Last [from upper node]
px4_command::command Command_Last;                     //无人机上一条执行命令
Eigen::Vector3d pos_drone_mocap;                          //无人机当前位置 (vicon)
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>函数声明<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
float get_ros_time(ros::Time begin);
void prinft_command_state();
void rotation_yaw(float yaw_angle, float input[2], float output[2]);
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>回调函数<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
void Command_cb(const px4_command::command::ConstPtr& msg)
{
    Command_Now = *msg;
}
void optitrack_cb(const geometry_msgs::PoseStamped::ConstPtr& msg)
{
    //位置 -- optitrack系 到 ENU系
    int optitrack_frame = 1; //Frame convention 0: Z-up -- 1: Y-up
    // Read the Drone Position from the Vrpn Package [Frame: Vicon]  (Vicon to ENU frame)
    Eigen::Vector3d pos_drone_mocap_enu(-msg->pose.position.x,msg->pose.position.z,msg->pose.position.y);

    pos_drone_mocap = pos_drone_mocap_enu;
}
//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>主 函 数<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
int main(int argc, char **argv)
{
    ros::init(argc, argv, "px4_pos_controller");
    ros::NodeHandle nh("~");

    ros::Subscriber Command_sub = nh.subscribe<px4_command::command>("/px4/command", 10, Command_cb);

    ros::Subscriber optitrack_sub = nh.subscribe<geometry_msgs::PoseStamped>("/vrpn_client_node/UAV/pose", 1000, optitrack_cb);

    int switch_ude;

    nh.param<int>("switch_ude", switch_ude, 0);

    ros::Rate rate(50.0);

    command_to_mavros pos_controller;

    pos_controller.printf_param();

    pos_controller.show_geo_fence();

    pos_controller_PID pos_controller_pid;
    pos_controller_UDE pos_controller_ude;
    pos_controller_passivity pos_controller_ps;
    pos_controller_NE pos_controller_ne;

    if(switch_ude == 0)
    {
        pos_controller_pid.printf_param();
    }else if(switch_ude == 1)
    {
        pos_controller_ude.printf_param();
    }else if(switch_ude == 2)
    {
        pos_controller_ps.printf_param();
    }else if(switch_ude == 3)
    {
        pos_controller_ne.printf_param();
    }


    int check_flag;
    // 这一步是为了程序运行前检查一下参数是否正确
    // 输入1,继续，其他，退出程序
    cout << "Please check the parameter and setting，1 for go on， else for quit: "<<endl;
    cin >> check_flag;

    if(check_flag != 1)
    {
        return -1;
    }

    // 等待和飞控的连接
    while(ros::ok() && pos_controller.current_state.connected)
    {
        ros::spinOnce();
        rate.sleep();
        ROS_INFO("Not Connected");
    }

    // 连接成功
    ROS_INFO("Connected!!");

    // 先读取一些飞控的数据
    int i =0;
    for(i=0;i<50;i++)
    {
        ros::spinOnce();
        rate.sleep();
    }

    //Set the takeoff position
    pos_controller.set_takeoff_position();

    if(switch_ude == 3)
    {
        pos_controller_ne.set_initial_pos(pos_controller.pos_drone_fcu);
    }

    //初始化命令-
    // 默认设置：Idle模式 电机怠速旋转 等待来自上层的控制指令

    Command_Now.comid = 0;
    Command_Now.command = Idle;
    Command_Now.sub_mode = 0;
    Command_Now.pos_sp[0] = 0;
    Command_Now.pos_sp[1] = 0;
    Command_Now.pos_sp[2] = 0;
    Command_Now.vel_sp[0] = 0;
    Command_Now.vel_sp[1] = 0;
    Command_Now.vel_sp[2] = 0;
    Command_Now.yaw_sp = 0;
    // 记录启控时间
    ros::Time begin_time = ros::Time::now();
    float last_time = get_ros_time(begin_time);
    float dt = 0;

//>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>主  循  环<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<
    while(ros::ok())
    {
        //执行回调函数
        ros::spinOnce();

        // 当前时间
        float cur_time = get_ros_time(begin_time);
        dt = cur_time  - last_time;

        dt = constrain_function2(dt, 0.01, 0.03);

        last_time = cur_time;

        //Check for geo fence: If drone is out of the geo fence, it will land now.
        pos_controller.check_failsafe();

        //Printf the drone state
        pos_controller.prinft_drone_state2(cur_time);

        //Printf the command state
        prinft_command_state();

        if(switch_ude == 0)
        {
            pos_controller_pid.printf_result();
        }else if(switch_ude == 1)
        {
            pos_controller_ude.printf_result();
        }else if(switch_ude == 2)
        {
            pos_controller_ps.printf_result();
        }else if(switch_ude == 3)
        {
            pos_controller_ne.printf_result();
        }


        //无人机一旦接受到Land指令，则会屏蔽其他指令
        if(Command_Last.command == Land)
        {
            Command_Now.command = Land;
        }

        switch (Command_Now.command)
        {
        case Move_ENU:
            pos_sp = Eigen::Vector3d(Command_Now.pos_sp[0],Command_Now.pos_sp[1],Command_Now.pos_sp[2]);
            vel_sp = Eigen::Vector3d(Command_Now.vel_sp[0],Command_Now.vel_sp[1],Command_Now.vel_sp[2]);

            if(switch_ude == 0)
            {
                accel_sp = pos_controller_pid.pos_controller(pos_controller.pos_drone_fcu, pos_controller.vel_drone_fcu, pos_sp, vel_sp, Command_Now.sub_mode, dt);
            }else if(switch_ude == 1)
            {
                accel_sp = pos_controller_ude.pos_controller(pos_controller.pos_drone_fcu, pos_controller.vel_drone_fcu, pos_sp, dt);
            }else if(switch_ude == 2)
            {
                accel_sp = pos_controller_ps.pos_controller(pos_drone_mocap, pos_controller.vel_drone_fcu, pos_sp, dt);
            }else if(switch_ude == 3)
            {
                accel_sp = pos_controller_ne.pos_controller(pos_controller.pos_drone_fcu, pos_controller.vel_drone_fcu, pos_sp, dt);
            }

            pos_controller.send_accel_setpoint(accel_sp, Command_Now.yaw_sp);

            break;

        case Move_Body:
            //只有在comid增加时才会进入解算
            if( Command_Now.comid  >  Command_Last.comid )
            {
                //xy velocity mode
                if( Command_Now.sub_mode & 0b10 )
                {
                    float d_vel_body[2] = {Command_Now.vel_sp[0], Command_Now.vel_sp[1]};         //the desired xy velocity in Body Frame
                    float d_vel_enu[2];                                                           //the desired xy velocity in NED Frame

                    rotation_yaw(pos_controller.Euler_fcu[2], d_vel_body, d_vel_enu);
                    vel_sp[0] = d_vel_enu[0];
                    vel_sp[1] = d_vel_enu[1];
                }
                //xy position mode
                else
                {
                    float d_pos_body[2] = {Command_Now.pos_sp[0], Command_Now.pos_sp[1]};         //the desired xy position in Body Frame
                    float d_pos_enu[2];                                                           //the desired xy position in enu Frame (The origin point is the drone)
                    rotation_yaw(pos_controller.Euler_fcu[2], d_pos_body, d_pos_enu);

                    pos_sp[0] = pos_controller.pos_drone_fcu[0] + d_pos_enu[0];
                    pos_sp[1] = pos_controller.pos_drone_fcu[1] + d_pos_enu[1];
                }

                //z velocity mode
                if( Command_Now.sub_mode & 0b01 )
                {
                    vel_sp[2] = Command_Now.vel_sp[2];
                }
                //z posiiton mode
                {
                    pos_sp[2] = pos_controller.pos_drone_fcu[2] + Command_Now.pos_sp[2];
                }

                yaw_sp = pos_controller.Euler_fcu[2]* 180/M_PI + Command_Now.yaw_sp;

            }

            if(switch_ude == 0)
            {
                accel_sp = pos_controller_pid.pos_controller(pos_controller.pos_drone_fcu, pos_controller.vel_drone_fcu, pos_sp, vel_sp, Command_Now.sub_mode, dt);
            }else if(switch_ude == 1)
            {
                accel_sp = pos_controller_ude.pos_controller(pos_controller.pos_drone_fcu, pos_controller.vel_drone_fcu, pos_sp, dt);
            }else if(switch_ude == 2)
            {
                accel_sp = pos_controller_ps.pos_controller(pos_drone_mocap, pos_controller.vel_drone_fcu, pos_sp, dt);
            }else if(switch_ude == 3)
            {
                accel_sp = pos_controller_ne.pos_controller(pos_controller.pos_drone_fcu, pos_controller.vel_drone_fcu, pos_sp, dt);
            }

            pos_controller.send_accel_setpoint(accel_sp, yaw_sp);

            break;

        case Hold:
            if (Command_Last.command != Hold)
            {
                pos_controller.Hold_position = Eigen::Vector3d(pos_controller.pos_drone_fcu[0],pos_controller.pos_drone_fcu[1],pos_controller.pos_drone_fcu[2]);
                pos_controller.Hold_yaw = pos_controller.Euler_fcu[2]* 180/M_PI;
                pos_sp = pos_controller.Hold_position;
                yaw_sp = pos_controller.Hold_yaw;
            }

            if(switch_ude == 0)
            {
                accel_sp = pos_controller_pid.pos_controller(pos_controller.pos_drone_fcu, pos_controller.vel_drone_fcu, pos_controller.Hold_position, vel_sp, Command_Now.sub_mode, dt);
            }else if(switch_ude == 1)
            {
                accel_sp = pos_controller_ude.pos_controller(pos_controller.pos_drone_fcu, pos_controller.vel_drone_fcu, pos_controller.Hold_position, dt);
            }else if(switch_ude == 2)
            {
                accel_sp = pos_controller_ps.pos_controller(pos_drone_mocap, pos_controller.vel_drone_fcu, pos_sp, dt);
            }else if(switch_ude == 3)
            {
                accel_sp = pos_controller_ne.pos_controller(pos_controller.pos_drone_fcu, pos_controller.vel_drone_fcu, pos_sp, dt);
            }

            pos_controller.send_accel_setpoint(accel_sp, pos_controller.Hold_yaw);

            break;


        case Land:
            if (Command_Last.command != Land)
            {
                pos_sp = Eigen::Vector3d(pos_controller.pos_drone_fcu[0],pos_controller.pos_drone_fcu[1],pos_controller.Takeoff_position[2]);
                yaw_sp = pos_controller.Euler_fcu[2]* 180/M_PI;
            }

            //如果距离起飞高度小于10厘米，则直接上锁并切换为手动模式；
            if(abs(pos_controller.pos_drone_fcu[2] - pos_controller.Takeoff_position[2]) < (0.1))
            {
                if(pos_controller.current_state.mode == "OFFBOARD")
                {
                    pos_controller.mode_cmd.request.custom_mode = "MANUAL";
                    pos_controller.set_mode_client.call(pos_controller.mode_cmd);
                }

                if(pos_controller.current_state.armed)
                {
                    pos_controller.arm_cmd.request.value = false;
                    pos_controller.arming_client.call(pos_controller.arm_cmd);

                }

                if (pos_controller.arm_cmd.response.success)
                {
                    cout<<"Disarm successfully!"<<endl;
                }
            }else
            {

                if(switch_ude == 0)
                {
                    accel_sp = pos_controller_pid.pos_controller(pos_controller.pos_drone_fcu, pos_controller.vel_drone_fcu, pos_sp, vel_sp, 0b00, dt);
                }else if(switch_ude == 1)
                {
                    accel_sp = pos_controller_ude.pos_controller(pos_controller.pos_drone_fcu, pos_controller.vel_drone_fcu, pos_sp, dt);
                }else if(switch_ude == 2)
                {
                    accel_sp = pos_controller_ps.pos_controller(pos_drone_mocap, pos_controller.vel_drone_fcu, pos_sp, dt);
                }else if(switch_ude == 3)
                {
                    accel_sp = pos_controller_ne.pos_controller(pos_controller.pos_drone_fcu, pos_controller.vel_drone_fcu, pos_sp, dt);
                }

                pos_controller.send_accel_setpoint(accel_sp, yaw_sp);
            }

            break;

        case Disarm:
            if(pos_controller.current_state.mode == "OFFBOARD")
            {
                pos_controller.mode_cmd.request.custom_mode = "MANUAL";
                pos_controller.set_mode_client.call(pos_controller.mode_cmd);
            }

            if(pos_controller.current_state.armed)
            {
                pos_controller.arm_cmd.request.value = false;
                pos_controller.arming_client.call(pos_controller.arm_cmd);

            }

            if (pos_controller.arm_cmd.response.success)
            {
                cout<<"Disarm successfully!"<<endl;
            }

            break;

        // 【】
        case Failsafe_land:
            break;

        // 【】
        case Idle:
            pos_controller.idle();
            break;

        case Takeoff:
            pos_sp = Eigen::Vector3d(pos_controller.Takeoff_position[0],pos_controller.Takeoff_position[1],pos_controller.Takeoff_position[2]+pos_controller.Takeoff_height);
            vel_sp = Eigen::Vector3d(0.0,0.0,0.0);

            if(switch_ude == 0)
            {
                accel_sp = pos_controller_pid.pos_controller(pos_controller.pos_drone_fcu, pos_controller.vel_drone_fcu, pos_sp, vel_sp, Command_Now.sub_mode, dt);
            }else if(switch_ude == 1)
            {
                accel_sp = pos_controller_ude.pos_controller(pos_controller.pos_drone_fcu, pos_controller.vel_drone_fcu, pos_sp, dt);
            }else if(switch_ude == 2)
            {
                accel_sp = pos_controller_ps.pos_controller(pos_drone_mocap, pos_controller.vel_drone_fcu, pos_sp, dt);
            }else if(switch_ude == 3)
            {
                accel_sp = pos_controller_ne.pos_controller(pos_controller.pos_drone_fcu, pos_controller.vel_drone_fcu, pos_sp, dt);
            }

            pos_controller.send_accel_setpoint(accel_sp, Command_Now.yaw_sp);

            break;
        }

        Command_Last = Command_Now;

        rate.sleep();
    }

    return 0;

}

// 【获取当前时间函数】 单位：秒
float get_ros_time(ros::Time begin)
{
    ros::Time time_now = ros::Time::now();
    float currTimeSec = time_now.sec-begin.sec;
    float currTimenSec = time_now.nsec / 1e9 - begin.nsec / 1e9;
    return (currTimeSec + currTimenSec);
}
// 【打印控制指令函数】
void prinft_command_state()
{
    cout <<">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>Command State<<<<<<<<<<<<<<<<<<<<<<<<<<<<" <<endl;

    int sub_mode;
    sub_mode = Command_Now.sub_mode;

    switch(Command_Now.command)
    {
    case Move_ENU:
        cout << "Command: [ Move_ENU ] " <<endl;

        if((sub_mode & 0b10) == 0) //xy channel
        {
            cout << "Submode: xy position control "<<endl;
            cout << "X_setpoint   : " << Command_Now.pos_sp[0] << " [ m ]"  << "  Y_setpoint : "<< Command_Now.pos_sp[1] << " [ m ]"<<endl;
        }
        else{
            cout << "Submode: xy velocity control "<<endl;
            cout << "X_setpoint   : " << Command_Now.vel_sp[0] << " [m/s]" << "  Y_setpoint : "<< Command_Now.vel_sp[1] << " [m/s]" <<endl;
        }

        if((sub_mode & 0b01) == 0) //z channel
        {
            cout << "Submode:  z position control "<<endl;
            cout << "Z_setpoint   : "<< Command_Now.pos_sp[2] << " [ m ]" << endl;
        }
        else
        {
            cout << "Submode:  z velocity control "<<endl;
            cout << "Z_setpoint   : "<< Command_Now.vel_sp[2] << " [m/s]" <<endl;
        }

        cout << "Yaw_setpoint : "  << Command_Now.yaw_sp << " [deg] " <<endl;

        break;
    case Move_Body:
        cout << "Command: [ Move_Body ] " <<endl;

        if((sub_mode & 0b10) == 0) //xy channel
        {
            cout << "Submode: xy position control "<<endl;
            cout << "X_setpoint   : " << Command_Now.pos_sp[0] << " [ m ]"  << "  Y_setpoint : "<< Command_Now.pos_sp[1] << " [ m ]"<<endl;
        }
        else{
            cout << "Submode: xy velocity control "<<endl;
            cout << "X_setpoint   : " << Command_Now.vel_sp[0] << " [m/s]" << "  Y_setpoint : "<< Command_Now.vel_sp[1] << " [m/s]" <<endl;
        }

        if((sub_mode & 0b01) == 0) //z channel
        {
            cout << "Submode:  z position control "<<endl;
            cout << "Z_setpoint   : "<< Command_Now.pos_sp[2] << " [ m ]" << endl;
        }
        else
        {
            cout << "Submode:  z velocity control "<<endl;
            cout << "Z_setpoint   : "<< Command_Now.vel_sp[2] << " [m/s]" <<endl;
        }

        cout << "Yaw_setpoint : "  << Command_Now.yaw_sp << " [deg] " <<endl;

        break;

    case Hold:
        cout << "Command: [ Hold ] " <<endl;
        cout << "Hold Position [X Y Z] : " << pos_sp[0] << " [ m ] "<< pos_sp[1]<<" [ m ] "<< pos_sp[2]<<" [ m ] "<<endl;
        cout << "Yaw_setpoint : "  << yaw_sp << " [deg] " <<endl;
        break;

    case Land:
        cout << "Command: [ Land ] " <<endl;
        cout << "Land Position [X Y Z] : " << pos_sp[0] << " [ m ] "<< pos_sp[1]<<" [ m ] "<< pos_sp[2]<<" [ m ] "<<endl;
        cout << "Yaw_setpoint : "  << Command_Now.yaw_sp << " [deg] " <<endl;
        break;

    case Disarm:
        cout << "Command: [ Disarm ] " <<endl;
        break;

    case Failsafe_land:
        cout << "Command: [ Failsafe_land ] " <<endl;
        break;

    case Idle:
        cout << "Command: [ Idle ] " <<endl;
        break;

    case Takeoff:
        cout << "Command: [ Takeoff ] " <<endl;
        cout << "Takeoff Position [X Y Z] : " << pos_sp[0] << " [ m ] "<< pos_sp[1]<<" [ m ] "<< pos_sp[2]<<" [ m ] "<<endl;
        cout << "Yaw_setpoint : "  << Command_Now.yaw_sp << " [deg] " <<endl;
        break;
    }



}
// 【坐标系旋转函数】- 机体系到enu系
// input是机体系,output是惯性系，yaw_angle是当前偏航角
void rotation_yaw(float yaw_angle, float input[2], float output[2])
{
    output[0] = input[0] * cos(yaw_angle) - input[1] * sin(yaw_angle);
    output[1] = input[0] * sin(yaw_angle) + input[1] * cos(yaw_angle);
}
