/******************************************************************************
 * 
 * Copyright (c) 2014, Simon Brodeur
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * 
 *  - Redistributions of source code must retain the above copyright notice, 
 *    this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright notice, 
 *    this list of conditions and the following disclaimer in the documentation 
 *    and/or other materials provided with the distribution.
 *  - Neither the name of the NECOTIS research group nor the names of its contributors 
 *    may be used to endorse or promote products derived from this software 
 *    without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND 
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED 
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. 
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, 
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT 
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, 
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, 
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) 
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE 
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <unistd.h>
#include <iostream>
#include <ros/ros.h>
#include <ros/console.h>
#include <sensor_msgs/Imu.h>
#include <geometry_msgs/Quaternion.h>
#include <geometry_msgs/Vector3.h>
#include <sensor_msgs/MagneticField.h>
#include <sensor_msgs/Temperature.h>

#include <imu/L3GD20Gyro.h>
#include <imu/LMS303.h>

using namespace std;

#define I2CBus	2
#define G_ACC   9.81
#define PI  3.14159

class CaptureNode {

    public:
        ros::NodeHandle node_;
        ros::Publisher pubPos_;
        ros::Publisher pubMag_;
        ros::Publisher pubTemp_;
        std::string outputPos_;
        std::string outputMag_;
        std::string outputTemp_;

        L3GD20Gyro gyro_;
        LMS303 lms303_;
        double rate_;

        CaptureNode() :
            node_("~"), gyro_(I2CBus, 0x6B),  lms303_(I2CBus, 0x1D){

                gyro_.setGyroDataRate(DR_GYRO_100HZ);
                gyro_.setGyroScale(SCALE_GYRO_245dps);

                lms303_.setMagDataRate(DR_MAG_100HZ);
                lms303_.setMagScale(SCALE_MAG_2gauss);
                lms303_.setAccelDataRate(DR_ACCEL_100HZ);
                lms303_.setAccelScale(SCALE_ACCEL_4g);

                node_.param("outputPos", outputPos_, std::string("imu/pos/raw"));
                node_.param("outputMag", outputMag_, std::string("imu/mag/raw"));
                node_.param("outputTemp", outputTemp_, std::string("imu/temp/raw"));
                node_.param("rate", rate_, 15.0);

                pubPos_ = node_.advertise<sensor_msgs::Imu>(outputPos_, 1);
                pubMag_ = node_.advertise<sensor_msgs::MagneticField>(outputMag_, 1);
                pubTemp_ = node_.advertise<sensor_msgs::Temperature>(outputTemp_, 1);
            }

        virtual ~CaptureNode() {
            // Empty
        }

        bool spin() {
            ros::Rate rate(rate_);
            while (node_.ok()) {
                lms303_.readFullSensorState();
                gyro_.readFullSensorState();
                
                // Positioning message
                sensor_msgs::Imu msgPos;

                msgPos.header.stamp = ros::Time::now();
                msgPos.header.frame_id = "imu_link";

                msgPos.orientation.x = 0;
                msgPos.orientation.y = 0;
                msgPos.orientation.z = 0;
                msgPos.orientation.w = 0;
                
                //Convert to rad/sec
                //NOTE inverted x and y axis intentional. lms303 and Gyro 
                // were not using same axis reference
                
                msgPos.angular_velocity.y = -(gyro_.getGyroX()/180)*PI;
                msgPos.angular_velocity.x =  (gyro_.getGyroY()/180)*PI;
                msgPos.angular_velocity.z = -(gyro_.getGyroZ()/180)*PI;
                //msg.angular_velocity_covariance    = ;
                
                //Convert from g's to m/s/s
                msgPos.linear_acceleration.x = -G_ACC*lms303_.getAccelX();
                msgPos.linear_acceleration.y = -G_ACC*lms303_.getAccelY();
                msgPos.linear_acceleration.z = -G_ACC*lms303_.getAccelZ();
                //msg.linear_acceleration_covariance = ;

                pubPos_.publish(msgPos);

                // Magnetic orientation message
                // Convert Gauss to Tesla
                sensor_msgs::MagneticField msgMag;

                msgMag.header.stamp = ros::Time::now();
                msgMag.header.frame_id = "imu_link";

                // Y -> North X -> East, inverted values for IMU consolidator
                msgMag.magnetic_field.x = lms303_.getMagX()/10000.0;
                msgMag.magnetic_field.y = lms303_.getMagY()/10000.0;
                msgMag.magnetic_field.z = lms303_.getMagZ()/10000.0;

                msgMag  = applyMagneticCorrection(msgMag);
                pubMag_.publish(msgMag);


                // Temperature message
                sensor_msgs::Temperature msgTemp;

                msgTemp.header.stamp = ros::Time::now();
                msgTemp.header.frame_id = "imu_link";

                msgTemp.temperature = lms303_.getTemperature();
                
                pubTemp_.publish(msgTemp);
                
                rate.sleep();
            }
            return true;
        }


        
        sensor_msgs::MagneticField applyMagneticCorrection(const sensor_msgs::MagneticField & iCaptured)
        {
            sensor_msgs::MagneticField wNewMagField = iCaptured;
           
            //Correction constants
            const float magRotz[3][3] = { {        1.0,        0.0, 0.00703476  } ,
                                          {        0.0,        1.0,-0.07186357  } ,
                                          {-0.00698555, 0.07744466,        1.0  } };

            const float magRotxy[3][3] = { {-0.74874188, 0.66286168,        0.0  } ,
                                           {-0.66286168,-0.74874188,        0.0  } ,
                                           {        0.0,        0.0,        1.0  } };
            const float magFacxy[2] = { 0.93773639, 1.07111999};
            
            const float xOffset =  0.00002768;
            const float yOffset = -0.00000528;

            //Apply bank correction
            float flatMagFieldx = magRotz[0][0]*iCaptured.magnetic_field.x + 
                                  magRotz[0][1]*iCaptured.magnetic_field.y +
                                  magRotz[0][2]*iCaptured.magnetic_field.z  ;
            float flatMagFieldy = magRotz[1][0]*iCaptured.magnetic_field.x + 
                                  magRotz[1][1]*iCaptured.magnetic_field.y +
                                  magRotz[1][2]*iCaptured.magnetic_field.z  ;
            float flatMagFieldz = magRotz[2][0]*iCaptured.magnetic_field.x + 
                                  magRotz[2][1]*iCaptured.magnetic_field.y +
                                  magRotz[2][2]*iCaptured.magnetic_field.z  ;
            //Apply soft-iron correction

            float siCorrx = magRotxy[0][0]*flatMagFieldx + 
                            magRotxy[0][1]*flatMagFieldy +
                            magRotxy[0][2]*flatMagFieldz  ;
            float siCorry = magRotxy[1][0]*flatMagFieldx + 
                            magRotxy[1][1]*flatMagFieldy +
                            magRotxy[1][2]*flatMagFieldz  ;
            float siCorrz = magRotxy[2][0]*flatMagFieldx + 
                            magRotxy[2][1]*flatMagFieldy +
                            magRotxy[2][2]*flatMagFieldz  ;
            
            siCorrx *= magFacxy[0];
            siCorry *= magFacxy[1];
            
            wNewMagField.magnetic_field.x = magRotxy[0][0]*siCorrx + 
                                            magRotxy[1][0]*siCorry +
                                            magRotxy[2][0]*siCorrz  ;
            wNewMagField.magnetic_field.y = magRotxy[0][1]*siCorrx + 
                                            magRotxy[1][1]*siCorry +
                                            magRotxy[2][1]*siCorrz  ;
            wNewMagField.magnetic_field.z = magRotxy[0][2]*siCorrx + 
                                            magRotxy[1][2]*siCorry +
                                            magRotxy[2][2]*siCorrz  ;

            //Apply centering offsets (hard-iron correction)
            wNewMagField.magnetic_field.x += xOffset;
            wNewMagField.magnetic_field.y += yOffset;


            return wNewMagField;
        }

};

int main(int argc, char **argv) {
    ros::init(argc, argv, "capture");

    CaptureNode a;
    a.spin();
    return 0;
}
