#include "Registrator3P.h"

#include <opencv2/core/eigen.hpp>
#include <cs_geometry/Math.h>
#include <cs_geometry/Conversions.h>
#include <ptam/MapPoint.h>

using namespace backend;
using namespace cs_geom;
using namespace std;


#define N_SAMPLES 4

Sophus::SE3d Registrator3P::solve(const ptam::KeyFrame& kfa, const ptam::KeyFrame& kfb,
                                  const std::vector<cv::DMatch>& matches)
{
    if (matches.size() < 5)
        return Sophus::SE3d();

    // Generate nHyp_ hypotheses:
    std::vector<Hypothesis3P> hyp;
    nHyp_ = matches.size() * N_SAMPLES;
    while (hyp.size() < nHyp_) {
        std::vector<int> sampleInd;

        // choose x random matches
        randSampleNoReplacement(matches.size(), N_SAMPLES, sampleInd);

        std::vector<cv::Point3f> mapPointsA(N_SAMPLES);
        std::vector<cv::Point2f> imagePointsB(N_SAMPLES);

        for (int i = 0; i < N_SAMPLES; i++) {
            int ind = sampleInd[i];
            const Eigen::Vector3d& mpa = cs_geom::toEigenVec(kfa.mapPoints[matches[ind].queryIdx]->v3RelativePos);
            mapPointsA[i]   = cv::Point3f(mpa[0], mpa[1], mpa[2]);
            int scale = 1;// << kfb.keypoints[matches[ind].trainIdx].octave;
            imagePointsB[i].x = kfb.keypoints[matches[ind].trainIdx].pt.x;// * scale;
            imagePointsB[i].y = kfb.keypoints[matches[ind].trainIdx].pt.y;// * scale;

        }

        cv::Mat rvec(3,1,cv::DataType<double>::type);
        cv::Mat tvec(3,1,cv::DataType<double>::type);
        /// here we need the model of camera B!
        int camNum = kfb.nSourceCamera;
//        cout << "cv::solvePnP... cam " << camNum << endl;
        if (cv::solvePnP(mapPointsA, imagePointsB, cam_[camNum].K(), cam_[camNum].D(), rvec, tvec,  false, cv::EPNP)) {
            Sophus::SE3d se3;
            Eigen::Vector3d r;
            cv::cv2eigen(rvec, r);
            cv::cv2eigen(tvec, se3.translation());
            se3.so3() = Sophus::SO3d::exp(r);
            hyp.push_back(Hypothesis3P(se3));
        }
//        cout << "cv::solvePnP finish..." << endl;
    }
    if (!hyp.size())
        return Sophus::SE3d();

    // Preemptive scoring
    // determine the order in which observations will be evaluated
    std::vector<int> oInd;
    randPerm(matches.size(),oInd);

    // start preemptive scoring
    int i = 0;
    int pr = preemption(i, nHyp_, blockSize_);
    while (i < nMaxObs_ && i < (int) matches.size() && pr > 1) {
        // observation oInd(i) consists of one pair of points:

        const cv::DMatch& match = matches[oInd[i]];
        const Eigen::Vector3d& mpa   = cs_geom::toEigenVec(kfa.mapPoints[match.queryIdx]->v3RelativePos);
        const cv::Point2f&     imbcv = kfb.keypoints[match.trainIdx].pt;
        int scale = 1;// << kfb.keypoints[match.trainIdx].octave;
        Eigen::Vector2d imb(imbcv.x * scale, imbcv.y * scale);

        // update score for all hypotheses w.r.t. observation oInd(i)
        for (int h = 0; h < (int) hyp.size(); h++) {
            Eigen::Vector2d err = cam_[kfb.nSourceCamera].project3DtoPixel(hyp[h].relPose*mpa) - imb;
            hyp[h].score -= log(1.0 + err.dot(err));
//            hyp[h].score += err.dot(err) < 3.0;
        }

        i++;
        int prnext = preemption(i, nHyp_, blockSize_);
        if (prnext != pr) {
            // select best hypotheses
            std::nth_element(hyp.begin(), hyp.begin() + prnext, hyp.end(), Hypothesis3P::compare);
            // now the first prnext elements of h contain the best hypotheses, erase the rest
            hyp.erase(hyp.begin() + prnext, hyp.end());
        }
        pr = prnext;
    }
    // preemptive scoring is done

    // select the single best hypothesis of possibly more than one remaining
    std::nth_element(hyp.begin(),hyp.begin() + 1, hyp.end(), Hypothesis3P::compare);

//    std::cout << "preemptive scoring using " << i << " observations done." <<  std::endl;
//    std::cout << hyp[0].relPose.inverse().matrix();
    return hyp[0].relPose.inverse();
}

bool Registrator3P::solvePnP(const ptam::KeyFrame& kfa, const ptam::KeyFrame& kfb,
                                  const std::vector<cv::DMatch>& matches, Sophus::SE3d& result)
{
    if (matches.size() < 5)
        return false;

    std::vector<cv::Point3f> mapPointsA(matches.size());
    std::vector<cv::Point2f> imagePointsB(matches.size());

    for (int i = 0; i < matches.size(); i++) {
        int ind = i;
        const Eigen::Vector3d& mpa = cs_geom::toEigenVec(kfa.mapPoints[matches[ind].queryIdx]->v3RelativePos);
        mapPointsA[i]   = cv::Point3f(mpa[0], mpa[1], mpa[2]);
        int scale = 1;// << kfb.keypoints[matches[ind].trainIdx].octave;
        imagePointsB[i].x = kfb.keypoints[matches[ind].trainIdx].pt.x;// * scale;
        imagePointsB[i].y = kfb.keypoints[matches[ind].trainIdx].pt.y;// * scale;
    }

    cv::Mat rvec(3,1,cv::DataType<double>::type);
    cv::Mat tvec(3,1,cv::DataType<double>::type);
    /// here we need the model of camera B!
    int camNum = kfb.nSourceCamera;
//    cout << "cv::solvePnP...." << endl;
    if (cv::solvePnP(mapPointsA, imagePointsB, cam_[camNum].K(), cam_[camNum].D(), rvec, tvec,  false, cv::EPNP)) {
        Sophus::SE3d se3;
        Eigen::Vector3d r;
        cv::cv2eigen(rvec, r);
        cv::cv2eigen(tvec, se3.translation());
        se3.so3() = Sophus::SO3d::exp(r);
        result = se3;
        return true;
    }
    else
        return false;
}

std::vector<cv::DMatch> Registrator3P::getInliers(const ptam::KeyFrame& kfa, const ptam::KeyFrame& kfb,
                                                  const std::vector<cv::DMatch>& matches,
                                                  const Sophus::SE3d& relPoseAB,
                                                  double threshold,
                                                  std::vector<Observation>& obs)
{
    Sophus::SE3d relPoseBA = relPoseAB.inverse();
    std::vector<cv::DMatch> inliers;
    double thresh2 = threshold*threshold;
    for (uint i = 0; i < matches.size(); i++) {
        const cv::DMatch& m = matches[i];

        const Eigen::Vector3d& mpa   = cs_geom::toEigenVec(kfa.mapPoints[m.queryIdx]->v3RelativePos);
        cv::Point2f     imbcv;
        int scale = 1;// << kfb.keypoints[m.trainIdx].octave;
        imbcv.x = kfb.keypoints[m.trainIdx].pt.x * scale;
        imbcv.y = kfb.keypoints[m.trainIdx].pt.y * scale;
        Eigen::Vector2d ipb(imbcv.x, imbcv.y);

        Eigen::Vector2d err = cam_[kfb.nSourceCamera].project3DtoPixel(relPoseBA*mpa) - ipb;;

        if (err.dot(err) < thresh2) {
            inliers.push_back(m);
            obs.push_back(Observation(mpa, cam_[kfb.nSourceCamera].unprojectPixel(ipb)));
        }
    }

    return inliers;
}

bool Registrator3P::solvePnP_RANSAC(const ptam::KeyFrame& kfa, const ptam::KeyFrame& kfb,
                                  const std::vector<cv::DMatch>& matches, Sophus::SE3d &result,
                                    std::vector<int>& inliers, double minInliers, double threshPterr)
{
    if (matches.size() < 5)
        return false;

    std::vector<cv::Point3f> mapPointsA(matches.size());
    std::vector<cv::Point2f> imagePointsB(matches.size());

    for (int i = 0; i < matches.size(); i++) {
        int ind = i;
        const Eigen::Vector3d& mpa = cs_geom::toEigenVec(kfa.mapPointsFirstLevel[matches[ind].queryIdx]->v3RelativePos);
        mapPointsA[i]   = cv::Point3f(mpa[0], mpa[1], mpa[2]);
        int scale = 1;// << kfb.keypoints[matches[ind].trainIdx].octave;
        imagePointsB[i].x = kfb.keypoints[matches[ind].trainIdx].pt.x * scale;
        imagePointsB[i].y = kfb.keypoints[matches[ind].trainIdx].pt.y * scale;
    }

    cv::Mat rvec(3,1,cv::DataType<double>::type);
    cv::Mat tvec(3,1,cv::DataType<double>::type);
    /// here we need the model of camera B!
    int camNum = kfb.nSourceCamera;
    std::cout << "Now do cv::solvePnPRansac " << std::endl;
    cv::solvePnPRansac(mapPointsA, imagePointsB, cam_[camNum].K(), cam_[camNum].D(),
                       rvec, tvec,  false, mapPointsA.size()*4, threshPterr,
                       int(mapPointsA.size()*0.8), inliers, cv::EPNP);
    std::cout << "Inliers in RANSAC relative pose estimation: " << inliers.size() << std::endl;
    if (inliers.size() > matches.size() * minInliers) {
        Eigen::Vector3d r;
        Sophus::SE3 se3; // TBA
        cv::cv2eigen(rvec, r);
        cv::cv2eigen(tvec, se3.translation());
        se3.so3() = Sophus::SO3d::exp(r);
        result = se3.inverse(); // TAB
        return true;
    }
    return false;
}

void Registrator3P::getObserv(const ptam::KeyFrame& kfa, const ptam::KeyFrame& kfb,
                                                  const std::vector<cv::DMatch>& matches,
                                                  const std::vector<int> inliers,
                                                  std::vector<Observation>& obs)
{
    for (uint i = 0; i < inliers.size(); i++) {
        const cv::DMatch& m = matches[inliers[i]];

        const Eigen::Vector3d& mpa   = cs_geom::toEigenVec(kfa.mapPoints[m.queryIdx]->v3RelativePos);
        cv::Point2f     imbcv;
        int scale = 1;// << kfb.keypoints[m.trainIdx].octave;
        imbcv.x = kfb.keypoints[m.trainIdx].pt.x * scale;
        imbcv.y = kfb.keypoints[m.trainIdx].pt.y * scale;
        Eigen::Vector2d ipb(imbcv.x, imbcv.y);

        obs.push_back(Observation(mpa, cam_[kfb.nSourceCamera].unprojectPixel(ipb)));
    }
}
