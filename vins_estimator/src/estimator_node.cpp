/* Referenced papers:
 * - Qin et al; 2017; "Technical Report: VINS-Mono: A Robust and Versatile Monocular Visual-Inertial State Estimator"
 */

#include <stdio.h>
#include <queue>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <ros/ros.h>
#include <cv_bridge/cv_bridge.h>
#include <opencv2/opencv.hpp>

#include <soci.h>

#include "estimator.h"
#include "parameters.h"
#include "utility/visualization.h"
#include "loop-closure/loop_closure.h"
#include "loop-closure/keyframe.h"
#include "loop-closure/keyframe_database.h"
#include "camodocal/camera_models/CameraFactory.h"
#include "camodocal/camera_models/CataCamera.h"
#include "camodocal/camera_models/PinholeCamera.h"

Estimator estimator;

std::condition_variable con;
double current_time = -1;
queue<sensor_msgs::ImuConstPtr> imu_buf;
queue<sensor_msgs::PointCloudConstPtr> feature_buf;
std::mutex m_posegraph_buf;
queue<int> optimize_posegraph_buf;
queue<KeyFrame*> keyframe_buf;
queue<RetriveData> retrive_data_buf;

int sum_of_wait = 0;

std::mutex m_buf;
std::mutex m_state;
std::mutex i_buf;
std::mutex m_loop_drift;
std::mutex m_keyframedatabase_resample;
std::mutex m_update_visualization;
std::mutex m_keyframe_buf;
std::mutex m_retrive_data_buf;

/*
Most recent timestamp.
- Modified in:
  - `void predict(...)`.
  - `void update(...)`.
*/
double latest_time;

/*
Most recent estimate of body position.
- In world coordinates.
- Modified in:
  - `void predict(...)`.
  - `void update(...)`.
- Published to ROS in `void imu_callback()`.
  - ... Published only if certain conditions are met: state estimation system
    has good confidence.
- Note: Not to be confused with a temporary variable of the same name in
  *vins_estimator/src/estimator.cpp*.
*/
Eigen::Vector3d tmp_P;

/*
Most recent estimate of body orientation.
- In world coordinates.
- Modified in:
  - `void predict(...)`.
  - `void update(...)`.
- Published to ROS in `void imu_callback()`.
  - ... Published only if certain conditions are met: state estimation system
    has good confidence.
*/
Eigen::Quaterniond tmp_Q;

/*
Most recent estimate of body velocity.
- In world coordinates.
- Modified in:
  - `void predict(...)`.
  - `void update(...)`.
- Published to ROS in `void imu_callback()`.
  - ... Published only if certain conditions are met: state estimation system
    has good confidence.
- Note: Not to be confused with a temporary variable of the same name in
  *benchmark_publisher/src/benchmark_publisher_node.cpp*.
*/
Eigen::Vector3d tmp_V;

/*
Most recent estimate of linear acceleration bias.
- Relative to last body orientation.
  - Actually, I think the assumption is that this is always relative to the
    current body orientation.
- Modified in:
  - `void update(...)`.
*/
Eigen::Vector3d tmp_Ba;

/*
Most recent estimate of gyroscope bias. (I.e. angular velocity bias.)
- Relative to last body orientation.
  - Actually, I think the assumption is that this is always relative to the
    current body orientation.
- Modified in:
  - `void update(...)`.
*/
Eigen::Vector3d tmp_Bg;

/*
Most recent IMU linear acceleration.
- Raw data.
- Relative to last body orientation.
- Modified in:
  - `void predict(...)`.
  - `void update(...)`.
*/

Eigen::Vector3d acc_0;
/*
Most recent IMU angular velocity.
- Raw data.
- Relative to last body orientation.
- Modified in:
  - `void predict(...)`.
  - `void update(...)`.
- Not to be confused with member variables of Estimator and IntegrationBase.
*/
Eigen::Vector3d gyr_0;

double previous_time;
Eigen::Vector3d previous_angular_velocity;
Eigen::Vector3d previous_linear_acceleration;
Eigen::Vector3d previous_p;
Eigen::Vector3d previous_v;
Eigen::Quaterniond previous_q;
int stamp_sec;
int stamp_nsec;
double dt;
sensor_msgs::Imu raw_imu;

queue<pair<cv::Mat, double>> image_buf;
LoopClosure *loop_closure;
KeyFrameDatabase keyframe_database;

int global_frame_cnt = 0;
//camera param
camodocal::CameraPtr m_camera;
vector<int> erase_index;
std_msgs::Header cur_header;
Eigen::Vector3d relocalize_t{Eigen::Vector3d(0, 0, 0)};
Eigen::Matrix3d relocalize_r{Eigen::Matrix3d::Identity()};


soci::session main_sql;
soci::session predict_sql;
soci::statement *predict_sql_statement = nullptr;
int predict_sql_transaction_size = 100;
int predict_sql_run_count = 0;

void predict(const sensor_msgs::ImuConstPtr &imu_msg)
{
    if(nullptr == predict_sql_statement){
      predict_sql_statement = new soci::statement((
        predict_sql.prepare << R"SQL(
          INSERT INTO imu (
            imu_timesstamp_sec
          , imu_timesstamp_nsec

          , imu_angular_velocity_x
          , imu_angular_velocity_y
          , imu_angular_velocity_z

          , imu_linear_acceleration_x
          , imu_linear_acceleration_y
          , imu_linear_acceleration_z

          , previous_time

          , previous_angular_velocity_x
          , previous_angular_velocity_y
          , previous_angular_velocity_z

          , previous_linear_acceleration_x
          , previous_linear_acceleration_y
          , previous_linear_acceleration_z

          , previous_px
          , previous_py
          , previous_pz

          , previous_vx
          , previous_vy
          , previous_vz

          , previous_qw
          , previous_qx
          , previous_qy
          , previous_qz

          , bias_drx
          , bias_dry
          , bias_drz

          , bias_dvx
          , bias_dvy
          , bias_dvz

          , estimator_gx
          , estimator_gy
          , estimator_gz

          , dt

          , px
          , py
          , pz

          , vx
          , vy
          , vz

          , qw
          , qx
          , qy
          , qz

          ) VALUES (
            :imu_timesstamp_sec
          , :imu_timesstamp_nsec

          , :imu_angular_velocity_x
          , :imu_angular_velocity_y
          , :imu_angular_velocity_z

          , :imu_linear_acceleration_x
          , :imu_linear_acceleration_y
          , :imu_linear_acceleration_z

          , :previous_time

          , :previous_angular_velocity_x
          , :previous_angular_velocity_y
          , :previous_angular_velocity_z

          , :previous_linear_acceleration_x
          , :previous_linear_acceleration_y
          , :previous_linear_acceleration_z

          , :previous_px
          , :previous_py
          , :previous_pz

          , :previous_vx
          , :previous_vy
          , :previous_vz

          , :previous_qw
          , :previous_qx
          , :previous_qy
          , :previous_qz

          , :bias_drx
          , :bias_dry
          , :bias_drz

          , :bias_dvx
          , :bias_dvy
          , :bias_dvz

          , :estimator_gx
          , :estimator_gy
          , :estimator_gz

          , :dt

          , :px
          , :py
          , :pz

          , :vx
          , :vy
          , :vz

          , :qw
          , :qx
          , :qy
          , :qz

          )
        )SQL"
        , soci::use(raw_imu.header.stamp.sec)
        , soci::use(raw_imu.header.stamp.nsec)

        , soci::use(raw_imu.angular_velocity.x)
        , soci::use(raw_imu.angular_velocity.y)
        , soci::use(raw_imu.angular_velocity.z)

        , soci::use(raw_imu.linear_acceleration.x)
        , soci::use(raw_imu.linear_acceleration.y)
        , soci::use(raw_imu.linear_acceleration.z)

        , soci::use(previous_time)

        , soci::use(previous_angular_velocity[0])
        , soci::use(previous_angular_velocity[1])
        , soci::use(previous_angular_velocity[2])

        , soci::use(previous_linear_acceleration[0])
        , soci::use(previous_linear_acceleration[1])
        , soci::use(previous_linear_acceleration[2])

        , soci::use(previous_p[0])
        , soci::use(previous_p[1])
        , soci::use(previous_p[2])

        , soci::use(previous_v[0])
        , soci::use(previous_v[1])
        , soci::use(previous_v[2])

        , soci::use(previous_q.w())
        , soci::use(previous_q.x())
        , soci::use(previous_q.y())
        , soci::use(previous_q.z())

        , soci::use(tmp_Bg[0])
        , soci::use(tmp_Bg[1])
        , soci::use(tmp_Bg[2])

        , soci::use(tmp_Ba[0])
        , soci::use(tmp_Ba[1])
        , soci::use(tmp_Ba[2])

        , soci::use(estimator.g[0])
        , soci::use(estimator.g[1])
        , soci::use(estimator.g[2])

        , soci::use(dt)

        , soci::use(tmp_P[0])
        , soci::use(tmp_P[1])
        , soci::use(tmp_P[2])

        , soci::use(tmp_V[0])
        , soci::use(tmp_V[1])
        , soci::use(tmp_V[2])

        , soci::use(tmp_Q.w())
        , soci::use(tmp_Q.x())
        , soci::use(tmp_Q.y())
        , soci::use(tmp_Q.z())

      ));
    }

    //ROS_DEBUG("predict count: %d", predict_sql_run_count++);
    if(0 == ((predict_sql_run_count++) % predict_sql_transaction_size)){
      if(predict_sql_transaction_size < predict_sql_run_count){
        ROS_DEBUG("predict_sql.commit()");
        predict_sql.commit();
      }
      ROS_DEBUG("predict_sql.begin()");
      predict_sql.begin();
    //} else {
    //  ROS_DEBUG("not committing...");
    }

    raw_imu = *imu_msg;

    previous_time = latest_time;
    previous_angular_velocity = gyr_0;
    previous_linear_acceleration = acc_0;
    previous_p = tmp_P;
    previous_v = tmp_V;
    previous_q = tmp_Q;
    stamp_sec = imu_msg->header.stamp.sec;
    stamp_nsec = imu_msg->header.stamp.nsec;
    

    /* Extract timetamp and compute $dt$ since last data. */
    double t = imu_msg->header.stamp.toSec();
    dt = t - latest_time;
    /*
    For clarity, this could be moved to the end of this function, where
    `acc_0` and `gyr_0` are also updated.
    */
    latest_time = t;

    /* Extract reported raw linear acceleration. */
    double dx = imu_msg->linear_acceleration.x;
    double dy = imu_msg->linear_acceleration.y;
    double dz = imu_msg->linear_acceleration.z;
    Eigen::Vector3d linear_acceleration{dx, dy, dz};

    /* Extract reported raw angular velocity. */
    double rx = imu_msg->angular_velocity.x;
    double ry = imu_msg->angular_velocity.y;
    double rz = imu_msg->angular_velocity.z;
    Eigen::Vector3d angular_velocity{rx, ry, rz};

    /*
    Order of operations:
    - 1: Estimate local `un_acc_0`, the previous linear acceleration,
      corrected for bias and gravity, in world-relative coordinates.
      - This operation uses the previously-estimated body orientation `tmp_Q`,
        so must occur before step 2, which updates `tmp_Q`.
    - 2: Update global `tmp_Q`, the current estimated body orientation,
      corrected for bias, in world-relative coordinates.
    - 3: Estimate local `un_acc_1`, the current linear acceleration, corrected
      for bias, in world-relative coordinates.
      - This operation uses the updated orientation `tmp_Q`, estimated in step
        2, so must occur after step 2.
    - 4: Estimate local `un_acc`, the average linear acceleration since last
      measurement, corrected for bias and gravity, in world-relative
      coordinates.
      - This uses `un_acc_0` and `un_acc_1`, estimated in steps 1 and 3
        respectively, so must come after both.
    - 5: Update global `tmp_P`, the current estimated position.
      - This uses `un_acc`, estimated in step 4, so must come after step 4.
      - This uses the previously-estimated velocity `tmp_V`, so must occur
        before step 5, which updates `tmp_V`.
    - 6: Update global `tmp_V`, the current estimated velocity.
      - This uses `un_acc`, estimated in step 4, so must come after step 4.
    */

    /*
    Estimate previous linear acceleration, corrected for bias and gravity, in
    world-relative coordinates.

    TODO@kaben: double-check this math.

    global tmp_Q:
    - Previous estimated body orientation, world-relative (check this?)
    - In any case, serves to transform from body-relative to world-relative.

    global acc_0:
    - Previous IMU linear acceleration, raw.
    - Relative to last body orientation.

    global tmp_Ba:
    - Previous estimated linear acceleration bias.
    - Relative to last body orientation.
    - NOTE@kaben: this is updated elsewhere, in `update()`. Depending upon
      when this happens, it might be better to call this "latest estimate of
      linear acceleration bias".

    global estimator.g:
    - Previous estimated gravity vector.
    - World-relative.
    - NOTE@kaben: It looks like estimator is updated in `send_imu(...)` via
      `estimator.processIMU(...)`. Depending upon when this happens, it might
      be better to call this "latest estimate of gravity vector".

    tmp_Q.inverse():
    - Transforms from world-relative to body-relative.

    tmp_Q.inverse()*estimator.g:
    - Previous estimated gravity vector.
    - Relative to last body orientation.

    acc_0 - tmp_Ba - tmp_Q.inverse()*estimator.g:
    - Previous estimated linear acceleration, corrected for bias and gravity.
    - Relative to last body orientation.

    local un_acc_0 = tmp_Q * (acc_0 - tmp_Ba - tmp_Q.inverse()*estimator.g):
    - Previous estimated linear acceleration, corrected for bias and gravity.
    - World-relative.
    */
    Eigen::Vector3d un_acc_0 = tmp_Q * (acc_0 - tmp_Ba - tmp_Q.inverse() * estimator.g);

    /*
    Estimate current body orientation, corrected for bias, in world-relative
    coordinates.

    TODO@kaben: double-check this math.

    global gyr_0:
    - Previous IMU angular velocity, raw.
    - Relative to last body orientation.

    local angular_velocity:
    - Current IMU angular velocity, raw.
    - Relative to last body orientation.

    0.5 * (gyr_0 + angular_velocity):
    - Average IMU angular velocity since last measurement.
    - Relative to last body orientation.

    global tmp_Bg:
    - Previous estimated angular velocity bias.
    - Relative to last body orientation.

    local un_gyr = 0.5 * (gyr_0 + angular_velocity) - tmp_Bg:
    - Estimated average IMU angular velocity since last measurement, corrected
      for bias.
    - Relative to last body orientation.

    Utility::deltaQ(un_gyr * dt):
    - Estimated change in body orientation since last measurement.
    - Relative to last body orientation.

    global tmp_Q:
    - Previous estimated body orientation, world-relative (check this?)

    global tmp_Q = tmp_Q * Utility::deltaQ(un_gyr * dt):
    - Current estimated body orientation, world-relative.
    */
    Eigen::Vector3d un_gyr = 0.5 * (gyr_0 + angular_velocity) - tmp_Bg;
    tmp_Q = tmp_Q * Utility::deltaQ(un_gyr * dt);

    /*
    Estimate current linear acceleration, corrected for bias, in
    world-relative coordinates.

    TODO@kaben: double-check this math.

    local linear_acceleration:
    - Current IMU angular velocity, raw.
    - Relative to current body orientation.

    global tmp_Ba:
    - Previous estimated linear acceleration bias.
    - Technically, relative to last body orientation.
    - I assume this is also a valid estimate relative to current body
      orientation.
    - NOTE@kaben: this is updated elsewhere, in `update()`. Depend upon when
      this happens, it might be better to call this "latest estimate of linear
      acceleration bias".

    global estimator.g:
    - Previous estimated gravity vector.
    - World-relative.
    - NOTE@kaben: It looks like estimator may be updated at end of this
      function, via `estimator.processIMU(...)`.

    global tmp_Q:
    - Current estimated body orientation, world-relative.

    tmp_Q.inverse():
    - Transforms from world-relative to body-relative.

    tmp_Q.inverse()*estimator.g:
    - Previous estimated gravity vector.
    - Relative to current body orientation.

    linear_acceleration - tmp_Ba - tmp_Q.inverse() * estimator.g:
    - Current estimated linear acceleration, corrected for bias and gravity.
    - Relative to current body orientation.

    local un_acc_1 = tmp_Q * (linear_acceleration - tmp_Ba - tmp_Q.inverse() * estimator.g):
    - Current estimated linear acceleration, corrected for bias and gravity.
    - World-relative.
    */
    Eigen::Vector3d un_acc_1 = tmp_Q * (linear_acceleration - tmp_Ba - tmp_Q.inverse() * estimator.g);

    /*
    Estimate average linear acceleration since last measurement, corrected for
    bias and gravity, in world-relative coordinates.

    TODO@kaben: double-check this math.

    local un_acc = 0.5 * (un_acc_0 + un_acc_1):
    - Average estimated linear acceleration since last measurement, corrected
      for bias and gravity.
    - World-relative.
    */
    Eigen::Vector3d un_acc = 0.5 * (un_acc_0 + un_acc_1);

    /*
    Estimate current position and velocity.

    0.5 * dt * dt * un_acc:
    - Estimated change in position due to average acceleration since last
      measurement.
    - World-relative.

    global tmp_P:
    - Previous estimated position.
    - World-relative. (Deduced: must be in same coordinate system as change in
      position due to average acceleration, which is in world-relative
      coordinates.)

    global tmp_V:
    - Previous estimated velocity.
    - World-relative. (Deduced: must be in same coordinate system as change in
      position due to average acceleration, which is in world-relative
      coordinates.)

    dt * tmp_V:
    - Estimated change in position due to velocity since last measurement.
    - World-relative. (Deduced: must be in same coordinate system as change in
      position due to average acceleration, which is in world-relative
      coordinates.)

    global tmp_P = tmp_P + dt * tmp_V + 0.5 * dt * dt * un_acc:
    - Current estimated position.
    - World-relative.

    dt * un_acc:
    - Estimated change in velocity due to acceleration since last measurement.
    - World-relative.

    global tmp_V = tmp_V + dt * un_acc:
    - Current estimated velocity.
    - World-relative.
    */
    tmp_P = tmp_P + dt * tmp_V + 0.5 * dt * dt * un_acc;
    tmp_V = tmp_V + dt * un_acc;

    acc_0 = linear_acceleration;
    gyr_0 = angular_velocity;

    predict_sql_statement->execute(true);
}

void update()
{
    TicToc t_predict;
    latest_time = current_time;
    tmp_P = relocalize_r * estimator.Ps[WINDOW_SIZE] + relocalize_t;
    tmp_Q = relocalize_r * estimator.Rs[WINDOW_SIZE];
    tmp_V = estimator.Vs[WINDOW_SIZE];
    tmp_Ba = estimator.Bas[WINDOW_SIZE];
    tmp_Bg = estimator.Bgs[WINDOW_SIZE];
    acc_0 = estimator.acc_0;
    gyr_0 = estimator.gyr_0;

    queue<sensor_msgs::ImuConstPtr> tmp_imu_buf = imu_buf;
    for (sensor_msgs::ImuConstPtr tmp_imu_msg; !tmp_imu_buf.empty(); tmp_imu_buf.pop())
        predict(tmp_imu_buf.front());

}

std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>>
getMeasurements()
{
    std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>> measurements;

    while (true)
    {
        if (imu_buf.empty() || feature_buf.empty())
            return measurements;

        if (!(imu_buf.back()->header.stamp > feature_buf.front()->header.stamp))
        {
            ROS_WARN("wait for imu, only should happen at the beginning");
            sum_of_wait++;
            return measurements;
        }

        if (!(imu_buf.front()->header.stamp < feature_buf.front()->header.stamp))
        {
            ROS_WARN("throw img, only should happen at the beginning");
            feature_buf.pop();
            continue;
        }
        sensor_msgs::PointCloudConstPtr img_msg = feature_buf.front();
        feature_buf.pop();

        std::vector<sensor_msgs::ImuConstPtr> IMUs;
        while (imu_buf.front()->header.stamp <= img_msg->header.stamp)
        {
            IMUs.emplace_back(imu_buf.front());
            imu_buf.pop();
        }

        measurements.emplace_back(IMUs, img_msg);
    }
    return measurements;
}

void imu_callback(const sensor_msgs::ImuConstPtr &imu_msg)
{
    m_buf.lock();
    imu_buf.push(imu_msg);
    m_buf.unlock();
    con.notify_one();

    {
        std::lock_guard<std::mutex> lg(m_state);
        predict(imu_msg);
        std_msgs::Header header = imu_msg->header;
        header.frame_id = "world";
        if (estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR)
            pubLatestOdometry(tmp_P, tmp_Q, tmp_V, header);
    }
}

void raw_image_callback(const sensor_msgs::ImageConstPtr &img_msg)
{
    cv_bridge::CvImagePtr img_ptr = cv_bridge::toCvCopy(img_msg, sensor_msgs::image_encodings::MONO8);
    //image_pool[img_msg->header.stamp.toNSec()] = img_ptr->image;
    if(LOOP_CLOSURE)
    {
        i_buf.lock();
        image_buf.push(make_pair(img_ptr->image, img_msg->header.stamp.toSec()));
        i_buf.unlock();
    }
}

void feature_callback(const sensor_msgs::PointCloudConstPtr &feature_msg)
{
    m_buf.lock();
    feature_buf.push(feature_msg);
    m_buf.unlock();
    con.notify_one();
}

void send_imu(const sensor_msgs::ImuConstPtr &imu_msg)
{
    double t = imu_msg->header.stamp.toSec();
    if (current_time < 0)
        current_time = t;
    double dt = t - current_time;
    current_time = t;

    double ba[]{0.0, 0.0, 0.0};
    double bg[]{0.0, 0.0, 0.0};

    double dx = imu_msg->linear_acceleration.x - ba[0];
    double dy = imu_msg->linear_acceleration.y - ba[1];
    double dz = imu_msg->linear_acceleration.z - ba[2];

    double rx = imu_msg->angular_velocity.x - bg[0];
    double ry = imu_msg->angular_velocity.y - bg[1];
    double rz = imu_msg->angular_velocity.z - bg[2];
    //ROS_DEBUG("IMU %f, dt: %f, acc: %f %f %f, gyr: %f %f %f", t, dt, dx, dy, dz, rx, ry, rz);

    estimator.processIMU(dt, Vector3d(dx, dy, dz), Vector3d(rx, ry, rz));
}

//thread:loop detection
void process_loop_detection()
{
    if(loop_closure == NULL)
    {
        const char *voc_file = VOC_FILE.c_str();
        TicToc t_load_voc;
        ROS_DEBUG("loop start loop");
        cout << "voc file: " << voc_file << endl;
        loop_closure = new LoopClosure(voc_file, IMAGE_COL, IMAGE_ROW);
        ROS_DEBUG("loop load vocbulary %lf", t_load_voc.toc());
        loop_closure->initCameraModel(CAM_NAMES);
    }

    while(LOOP_CLOSURE)
    {
        KeyFrame* cur_kf = NULL; 
        m_keyframe_buf.lock();
        while(!keyframe_buf.empty())
        {
            if(cur_kf!=NULL)
                delete cur_kf;
            cur_kf = keyframe_buf.front();
            keyframe_buf.pop();
        }
        m_keyframe_buf.unlock();
        if (cur_kf != NULL)
        {
            cur_kf->global_index = global_frame_cnt;
            m_keyframedatabase_resample.lock();
            keyframe_database.add(cur_kf);
            m_keyframedatabase_resample.unlock();

            cv::Mat current_image;
            current_image = cur_kf->image;   

            bool loop_succ = false;
            int old_index = -1;
            vector<cv::Point2f> cur_pts;
            vector<cv::Point2f> old_pts;
            TicToc t_brief;
            cur_kf->extractBrief(current_image);
            //printf("loop extract %d feature using %lf\n", cur_kf->keypoints.size(), t_brief.toc());
            TicToc t_loopdetect;
            loop_succ = loop_closure->startLoopClosure(cur_kf->keypoints, cur_kf->descriptors, cur_pts, old_pts, old_index);
            double t_loop = t_loopdetect.toc();
            ROS_DEBUG("t_loopdetect %f ms", t_loop);
            if(loop_succ)
            {
                KeyFrame* old_kf = keyframe_database.getKeyframe(old_index);
                if (old_kf == NULL)
                {
                    ROS_WARN("NO such frame in keyframe_database");
                    ROS_BREAK();
                }
                ROS_DEBUG("loop succ %d with %drd image", global_frame_cnt, old_index);
                assert(old_index!=-1);
                
                Vector3d T_w_i_old, PnP_T_old;
                Matrix3d R_w_i_old, PnP_R_old;

                old_kf->getPose(T_w_i_old, R_w_i_old);
                std::vector<cv::Point2f> measurements_old;
                std::vector<cv::Point2f> measurements_old_norm;
                std::vector<cv::Point2f> measurements_cur;
                std::vector<int> features_id_matched;  
                cur_kf->findConnectionWithOldFrame(old_kf, measurements_old, measurements_old_norm, PnP_T_old, PnP_R_old, m_camera);
                measurements_cur = cur_kf->measurements_matched;
                features_id_matched = cur_kf->features_id_matched;
                // send loop info to VINS relocalization
                int loop_fusion = 0;
                if( (int)measurements_old_norm.size() > MIN_LOOP_NUM && global_frame_cnt - old_index > 35 && old_index > 30)
                {

                    Quaterniond PnP_Q_old(PnP_R_old);
                    RetriveData retrive_data;
                    retrive_data.cur_index = cur_kf->global_index;
                    retrive_data.header = cur_kf->header;
                    retrive_data.P_old = T_w_i_old;
                    retrive_data.R_old = R_w_i_old;
                    retrive_data.relative_pose = false;
                    retrive_data.relocalized = false;
                    retrive_data.measurements = measurements_old_norm;
                    retrive_data.features_ids = features_id_matched;
                    retrive_data.loop_pose[0] = PnP_T_old.x();
                    retrive_data.loop_pose[1] = PnP_T_old.y();
                    retrive_data.loop_pose[2] = PnP_T_old.z();
                    retrive_data.loop_pose[3] = PnP_Q_old.x();
                    retrive_data.loop_pose[4] = PnP_Q_old.y();
                    retrive_data.loop_pose[5] = PnP_Q_old.z();
                    retrive_data.loop_pose[6] = PnP_Q_old.w();
                    m_retrive_data_buf.lock();
                    retrive_data_buf.push(retrive_data);
                    m_retrive_data_buf.unlock();
                    cur_kf->detectLoop(old_index);
                    old_kf->is_looped = 1;
                    loop_fusion = 1;

                    m_update_visualization.lock();
                    keyframe_database.addLoop(old_index);
                    CameraPoseVisualization* posegraph_visualization = keyframe_database.getPosegraphVisualization();
                    pubPoseGraph(posegraph_visualization, cur_header);  
                    m_update_visualization.unlock();
                }


                // visualization loop info
                if(0 && loop_fusion)
                {
                    int COL = current_image.cols;
                    //int ROW = current_image.rows;
                    cv::Mat gray_img, loop_match_img;
                    cv::Mat old_img = old_kf->image;
                    cv::hconcat(old_img, current_image, gray_img);
                    cvtColor(gray_img, loop_match_img, CV_GRAY2RGB);
                    cv::Mat loop_match_img2;
                    loop_match_img2 = loop_match_img.clone();
                    /*
                    for(int i = 0; i< (int)cur_pts.size(); i++)
                    {
                        cv::Point2f cur_pt = cur_pts[i];
                        cur_pt.x += COL;
                        cv::circle(loop_match_img, cur_pt, 5, cv::Scalar(0, 255, 0));
                    }
                    for(int i = 0; i< (int)old_pts.size(); i++)
                    {
                        cv::circle(loop_match_img, old_pts[i], 5, cv::Scalar(0, 255, 0));
                    }
                    for (int i = 0; i< (int)old_pts.size(); i++)
                    {
                        cv::Point2f cur_pt = cur_pts[i];
                        cur_pt.x += COL ;
                        cv::line(loop_match_img, old_pts[i], cur_pt, cv::Scalar(0, 255, 0), 1, 8, 0);
                    }
                    ostringstream convert;
                    convert << "/home/tony-ws/raw_data/loop_image/"
                            << cur_kf->global_index << "-" 
                            << old_index << "-" << loop_fusion <<".jpg";
                    cv::imwrite( convert.str().c_str(), loop_match_img);
                    */
                    for(int i = 0; i< (int)measurements_cur.size(); i++)
                    {
                        cv::Point2f cur_pt = measurements_cur[i];
                        cur_pt.x += COL;
                        cv::circle(loop_match_img2, cur_pt, 5, cv::Scalar(0, 255, 0));
                    }
                    for(int i = 0; i< (int)measurements_old.size(); i++)
                    {
                        cv::circle(loop_match_img2, measurements_old[i], 5, cv::Scalar(0, 255, 0));
                    }
                    for (int i = 0; i< (int)measurements_old.size(); i++)
                    {
                        cv::Point2f cur_pt = measurements_cur[i];
                        cur_pt.x += COL ;
                        cv::line(loop_match_img2, measurements_old[i], cur_pt, cv::Scalar(0, 255, 0), 1, 8, 0);
                    }

                    ostringstream convert2;
                    convert2 << "/home/tony-ws/raw_data/loop_image/"
                            << cur_kf->global_index << "-" 
                            << old_index << "-" << loop_fusion <<"-2.jpg";
                    cv::imwrite( convert2.str().c_str(), loop_match_img2);
                }
                  
            }
            //release memory
            cur_kf->image.release();
            global_frame_cnt++;

            if (t_loop > 1000 || keyframe_database.size() > MAX_KEYFRAME_NUM)
            {
                m_keyframedatabase_resample.lock();
                erase_index.clear();
                keyframe_database.downsample(erase_index);
                m_keyframedatabase_resample.unlock();
                if(!erase_index.empty())
                    loop_closure->eraseIndex(erase_index);
            }
        }
        std::chrono::milliseconds dura(10);
        std::this_thread::sleep_for(dura);
    }
}

//thread: pose_graph optimization
void process_pose_graph()
{
    while(true)
    {
        m_posegraph_buf.lock();
        int index = -1;
        while (!optimize_posegraph_buf.empty())
        {
            index = optimize_posegraph_buf.front();
            optimize_posegraph_buf.pop();
        }
        m_posegraph_buf.unlock();
        if(index != -1)
        {
            Vector3d correct_t = Vector3d::Zero();
            Matrix3d correct_r = Matrix3d::Identity();
            TicToc t_posegraph;
            keyframe_database.optimize4DoFLoopPoseGraph(index,
                                                    correct_t,
                                                    correct_r);
            ROS_DEBUG("t_posegraph %f ms", t_posegraph.toc());
            m_loop_drift.lock();
            relocalize_r = correct_r;
            relocalize_t = correct_t;
            m_loop_drift.unlock();
            m_update_visualization.lock();
            keyframe_database.updateVisualization();
            CameraPoseVisualization* posegraph_visualization = keyframe_database.getPosegraphVisualization();
            m_update_visualization.unlock();
            pubOdometry(estimator, cur_header, relocalize_t, relocalize_r);
            pubPoseGraph(posegraph_visualization, cur_header); 
            nav_msgs::Path refine_path = keyframe_database.getPath();
            updateLoopPath(refine_path);
        }

        std::chrono::milliseconds dura(5000);
        std::this_thread::sleep_for(dura);
    }
}

// thread: visual-inertial odometry
void process()
{
    while (true)
    {
        std::vector<std::pair<std::vector<sensor_msgs::ImuConstPtr>, sensor_msgs::PointCloudConstPtr>> measurements;
        std::unique_lock<std::mutex> lk(m_buf);
        con.wait(lk, [&]
                 {
            return (measurements = getMeasurements()).size() != 0;
                 });
        lk.unlock();

        for (auto &measurement : measurements)
        {
            for (auto &imu_msg : measurement.first)
                send_imu(imu_msg);

            auto img_msg = measurement.second;
            ROS_DEBUG("processing vision data with stamp %f \n", img_msg->header.stamp.toSec());

            TicToc t_s;
            map<int, vector<pair<int, Vector3d>>> image;
            for (unsigned int i = 0; i < img_msg->points.size(); i++)
            {
                int v = img_msg->channels[0].values[i] + 0.5;
                int feature_id = v / NUM_OF_CAM;
                int camera_id = v % NUM_OF_CAM;
                double x = img_msg->points[i].x;
                double y = img_msg->points[i].y;
                double z = img_msg->points[i].z;
                ROS_ASSERT(z == 1);
                image[feature_id].emplace_back(camera_id, Vector3d(x, y, z));
            }
            estimator.processImage(image, img_msg->header);
            /**
            *** start build keyframe database for loop closure
            **/
            if(LOOP_CLOSURE)
            {
                // remove previous loop
                vector<RetriveData>::iterator it = estimator.retrive_data_vector.begin();
                for(; it != estimator.retrive_data_vector.end(); )
                {
                    if ((*it).header < estimator.Headers[0].stamp.toSec())
                    {
                        it = estimator.retrive_data_vector.erase(it);
                    }
                    else
                        it++;
                }
                m_retrive_data_buf.lock();
                while(!retrive_data_buf.empty())
                {
                    RetriveData tmp_retrive_data = retrive_data_buf.front();
                    retrive_data_buf.pop();
                    estimator.retrive_data_vector.push_back(tmp_retrive_data);
                }
                m_retrive_data_buf.unlock();
                //WINDOW_SIZE - 2 is key frame
                if(estimator.marginalization_flag == 0 && estimator.solver_flag == estimator.NON_LINEAR)
                {   
                    Vector3d vio_T_w_i = estimator.Ps[WINDOW_SIZE - 2];
                    Matrix3d vio_R_w_i = estimator.Rs[WINDOW_SIZE - 2];
                    i_buf.lock();
                    while(!image_buf.empty() && image_buf.front().second < estimator.Headers[WINDOW_SIZE - 2].stamp.toSec())
                    {
                        image_buf.pop();
                    }
                    i_buf.unlock();
                    //assert(estimator.Headers[WINDOW_SIZE - 1].stamp.toSec() == image_buf.front().second);
                    // relative_T   i-1_T_i relative_R  i-1_R_i
                    cv::Mat KeyFrame_image;
                    KeyFrame_image = image_buf.front().first;
                    
                    const char *pattern_file = PATTERN_FILE.c_str();
                    Vector3d cur_T;
                    Matrix3d cur_R;
                    cur_T = relocalize_r * vio_T_w_i + relocalize_t;
                    cur_R = relocalize_r * vio_R_w_i;
                    KeyFrame* keyframe = new KeyFrame(estimator.Headers[WINDOW_SIZE - 2].stamp.toSec(), vio_T_w_i, vio_R_w_i, cur_T, cur_R, image_buf.front().first, pattern_file);
                    keyframe->setExtrinsic(estimator.tic[0], estimator.ric[0]);
                    keyframe->buildKeyFrameFeatures(estimator, m_camera);
                    m_keyframe_buf.lock();
                    keyframe_buf.push(keyframe);
                    m_keyframe_buf.unlock();
                    // update loop info
                    if (!estimator.retrive_data_vector.empty() && estimator.retrive_data_vector[0].relative_pose)
                    {
                        if(estimator.Headers[0].stamp.toSec() == estimator.retrive_data_vector[0].header)
                        {
                            KeyFrame* cur_kf = keyframe_database.getKeyframe(estimator.retrive_data_vector[0].cur_index);                            
                            if (abs(estimator.retrive_data_vector[0].relative_yaw) > 30.0 || estimator.retrive_data_vector[0].relative_t.norm() > 20.0)
                            {
                                ROS_DEBUG("Wrong loop");
                                cur_kf->removeLoop();
                            }
                            else 
                            {
                                cur_kf->updateLoopConnection( estimator.retrive_data_vector[0].relative_t, 
                                                              estimator.retrive_data_vector[0].relative_q, 
                                                              estimator.retrive_data_vector[0].relative_yaw);
                                m_posegraph_buf.lock();
                                optimize_posegraph_buf.push(estimator.retrive_data_vector[0].cur_index);
                                m_posegraph_buf.unlock();
                            }
                        }
                    }
                }
            }
            double whole_t = t_s.toc();
            printStatistics(estimator, whole_t);
            std_msgs::Header header = img_msg->header;
            header.frame_id = "world";
            cur_header = header;
            m_loop_drift.lock();
            if (estimator.relocalize)
            {
                relocalize_t = estimator.relocalize_t;
                relocalize_r = estimator.relocalize_r;
            }
            pubOdometry(estimator, header, relocalize_t, relocalize_r);
            pubKeyPoses(estimator, header, relocalize_t, relocalize_r);
            pubCameraPose(estimator, header, relocalize_t, relocalize_r);
            pubPointCloud(estimator, header, relocalize_t, relocalize_r);
            pubTF(estimator, header, relocalize_t, relocalize_r);
            m_loop_drift.unlock();
            //ROS_ERROR("end: %f, at %f", img_msg->header.stamp.toSec(), ros::Time::now().toSec());
        }
        m_buf.lock();
        m_state.lock();
        if (estimator.solver_flag == Estimator::SolverFlag::NON_LINEAR)
            update();
        m_state.unlock();
        m_buf.unlock();
    }
}

int main(int argc, char **argv)
{
    main_sql.open("sqlite3://vins_capture.sqlite3");
    main_sql << "DROP TABLE IF EXISTS imu;";
    main_sql << R"SQL(
      CREATE TABLE IF NOT EXISTS imu (
        id INTEGER PRIMARY KEY

      , imu_timesstamp_sec INTEGER
      , imu_timesstamp_nsec INTEGER

      , imu_angular_velocity_x DOUBLE
      , imu_angular_velocity_y DOUBLE
      , imu_angular_velocity_z DOUBLE

      , imu_linear_acceleration_x DOUBLE
      , imu_linear_acceleration_y DOUBLE
      , imu_linear_acceleration_z DOUBLE

      , previous_time DOUBLE

      , previous_angular_velocity_x DOUBLE
      , previous_angular_velocity_y DOUBLE
      , previous_angular_velocity_z DOUBLE

      , previous_linear_acceleration_x DOUBLE
      , previous_linear_acceleration_y DOUBLE
      , previous_linear_acceleration_z DOUBLE

      , previous_px DOUBLE
      , previous_py DOUBLE
      , previous_pz DOUBLE

      , previous_vx DOUBLE
      , previous_vy DOUBLE
      , previous_vz DOUBLE

      , previous_qw DOUBLE
      , previous_qx DOUBLE
      , previous_qy DOUBLE
      , previous_qz DOUBLE

      , bias_drx DOUBLE
      , bias_dry DOUBLE
      , bias_drz DOUBLE

      , bias_dvx DOUBLE
      , bias_dvy DOUBLE
      , bias_dvz DOUBLE

      , estimator_gx DOUBLE
      , estimator_gy DOUBLE
      , estimator_gz DOUBLE

      , dt DOUBLE

      , px DOUBLE
      , py DOUBLE
      , pz DOUBLE

      , vx DOUBLE
      , vy DOUBLE
      , vz DOUBLE

      , qw DOUBLE
      , qx DOUBLE
      , qy DOUBLE
      , qz DOUBLE
      )
    )SQL";
    main_sql.close();

    predict_sql.open("sqlite3://vins_capture.sqlite3");

    ros::init(argc, argv, "vins_estimator");
    ros::NodeHandle n("~");
    ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME, ros::console::levels::Info);
    readParameters(n);
    estimator.setParameter();
#ifdef EIGEN_DONT_PARALLELIZE
    ROS_DEBUG("EIGEN_DONT_PARALLELIZE");
#endif
    ROS_WARN("waiting for image and imu...");

    registerPub(n);

    ros::Subscriber sub_imu = n.subscribe(IMU_TOPIC, 2000, imu_callback, ros::TransportHints().tcpNoDelay());
    ros::Subscriber sub_image = n.subscribe("/feature_tracker/feature", 2000, feature_callback);
    ros::Subscriber sub_raw_image = n.subscribe(IMAGE_TOPIC, 2000, raw_image_callback);

    std::thread measurement_process{process};
    std::thread loop_detection, pose_graph;
    if (LOOP_CLOSURE)
    {
        ROS_WARN("LOOP_CLOSURE true");
        loop_detection = std::thread(process_loop_detection);   
        pose_graph = std::thread(process_pose_graph);
        m_camera = CameraFactory::instance()->generateCameraFromYamlFile(CAM_NAMES);
    }
    ros::spin();

    if(predict_sql_transaction_size < predict_sql_run_count){
      ROS_INFO("predict_sql.commit()");
      predict_sql.commit();
    }
    if(nullptr != predict_sql_statement){
      delete predict_sql_statement;
      predict_sql_statement = nullptr;
    }
    predict_sql.close();

    return 0;
}
