// Copyright (c) EIVA 2018
#include <bitset>
#include <chrono>
#include <vector>
#include <iostream>
#include <Eigen/LU>
#include <stdexcept>
#include <opencv2/core/eigen.hpp>
//#include <opencv2/core/eigen.hpp>
#include <opencv2/opencv.hpp>
//#include <opencv2/features2d.hpp>
//#include <opencv2/opencv.hpp>
//#include <opencv2/videoio.hpp>
#include "opencv2/opencv_modules.hpp"
#include <opencv2/features2d/features2d.hpp>
//#include <opencv2/xfeatures2d.hpp>
//#include <opencv2/imgcodecs.hpp>
//#include <opencv2/stitching/detail/matchers.hpp>
//#include <opencv2/videoio.hpp>
#include "ORBextractor.h"


#include <vector>

// #define USE_LATCH
#ifdef USE_LATCH
	#include "latch.h"
#endif

#include "feature_extractor.h"
#include <algorithm>
#define VC_EXTRALEAN
#define WIN32_LEAN_AND_MEAN

using namespace std::chrono;
using namespace ORB_EXTRACTOR;
const int FeatureExtractor::NUM_GRID_CELLS = 15;  // This number is arbitrary, because 10 is a good number  actual number of cells will be NUM_GRID_CELLS^2
const int FeatureExtractor::EDGE_THRESHOLD = 31;  // Num pixels ommited in orb extractor

template<typename A,typename B,typename C>
A clamp(const A& a, const B& lower, const C& upper)
{
	return a<lower ? lower : (a > upper ? upper : a);
}

FeatureExtractor::FeatureExtractor(int n_keypoints, 
								   float scale_factor, 
								   int n_levels, 
								   int threshold_high, 
								   int threshold_low, 
								   int interest_point_detector_type, 
								   int keypoint_search_multiplier){
	//int edge_treshold = 36; 
	
	_interest_point_detector_type = interest_point_detector_type;
	_n_keypoints = n_keypoints;
	switch (_interest_point_detector_type) {
	case 0:
		break;
	case 1:
		n_keypoints = n_keypoints * keypoint_search_multiplier;
		break;
	case 2:
		break;
	case 3:
		n_keypoints /= FeatureExtractor::NUM_GRID_CELLS;
		break;
	default:
		throw std::invalid_argument("Such feature detector is not defined change -> interest_point_detector_type");
	}

	_orbHigh = cv::ORB::create(n_keypoints, scale_factor, n_levels, FeatureExtractor::EDGE_THRESHOLD, 0, 2, cv::ORB::HARRIS_SCORE, 31, threshold_high);
	_orbLow =   cv::ORB::create(n_keypoints, scale_factor, n_levels, FeatureExtractor::EDGE_THRESHOLD, 0, 2, cv::ORB::HARRIS_SCORE, 31, threshold_low);
	_orb_slam = new ORBextractor(n_keypoints, scale_factor, n_levels, threshold_high, threshold_low);
}
void FeatureExtractor::set_mask(Eigen::Matrix<uint8_t, Eigen::Dynamic, Eigen::Dynamic> mask){
	cv::eigen2cv(mask, _cv_mask);
}
Eigen::MatrixXd FeatureExtractor::detect_interest_points(Eigen::Matrix<uint8_t, Eigen::Dynamic, Eigen::Dynamic> eigen_image)
{	
	//cv::Mat cv_image(image.rows(),image.cols(),CV_8UC1);
	cv::eigen2cv(eigen_image, _cv_image);	
	switch (_interest_point_detector_type) {
	case 0:
		_orbHigh->detect(_cv_image, _keypoints, _cv_mask);
		break;
	case 1:
		_orbLow->detect(_cv_image, _keypoints, _cv_mask);
		internal_adaptive_non_maximum_suppression();
		break;
	case 2:
		_orb_slam->operator()(_cv_image, _keypoints);
		break;
	case 3:
		inititialze_POI_on_grid();
		internal_adaptive_non_maximum_suppression();
		break;
	default:
		throw std::invalid_argument("Such feature detector is not defined change -> interest_point_detector_type");
	}
	// Remove points close to the border 
	//remove_outside(_keypoints,_cv_image.cols,_cv_image.rows,36);

	return get_keypoints();
}

Eigen::MatrixXd FeatureExtractor::get_keypoints()
{	
	// x,y,scale, angle, response
	Eigen::MatrixXd interest_points(5,_keypoints.size());

	for(unsigned int i_kp = 0; i_kp < _keypoints.size(); i_kp++){
		interest_points(0,i_kp) = _keypoints[i_kp].pt.x;
		interest_points(1,i_kp) = _keypoints[i_kp].pt.y;
		interest_points(2,i_kp) = _keypoints[i_kp].size;
		interest_points(3,i_kp) = _keypoints[i_kp].angle;
		interest_points(4,i_kp) = _keypoints[i_kp].response;
	}
	return interest_points;
}
void FeatureExtractor::remove_outside( std::vector<cv::KeyPoint>& keypoints,const int width, const int height, const int border_value) {
	keypoints.erase(std::remove_if(keypoints.begin(), keypoints.end(), [width, height,border_value](const cv::KeyPoint& kp) {return kp.pt.x <= border_value || kp.pt.y <= border_value || kp.pt.x >= width - border_value || kp.pt.y >= height - border_value; }), keypoints.end());
}
Eigen::Matrix<uint8_t, Eigen::Dynamic, Eigen::Dynamic> FeatureExtractor::compute_descriptor(bool use_latch,int print_kp_idx)
{	
	const int n_found_kp = (int)(_keypoints.size());
	constexpr bool multithread = true;
	Eigen::Matrix<uint8_t, Eigen::Dynamic, Eigen::Dynamic> eigen_desc;

	if(use_latch){
#ifdef USE_LATCH

		// Using cv::Mat. Smart when using opencv. ///////////////////////////////////////////
		/*if(false){
			cv::Mat cv_desc = cv::Mat::zeros(n_found_kp,64,CV_8UC1);

			// A pointer is created to point to the data of the cv::Mat. 
			uint64_t* p_desc = (uint64_t*)(cv_desc.data);

			// Calculate LATCH descriptors 
			LATCH<multithread>(_cv_image.data, _cv_image.cols, _cv_image.rows, static_cast<int>(_cv_image.step), _keypoints, p_desc);

			cv::cv2eigen(cv_desc,eigen_desc);
			/////////////////////////////////////////////////////////////////////////////////
		}*/
		
		// Using eigen. Smart when data is being return using pybind11, that uses eigen.  /////////////////
		eigen_desc = Eigen::Matrix<uint8_t, Eigen::Dynamic, Eigen::Dynamic>::Zero(64,n_found_kp);
		uint64_t* p_desc = (uint64_t*)(eigen_desc.data());

		// Calculate LATCH descriptors 
		LATCH<multithread>(_cv_image.data, _cv_image.cols, _cv_image.rows, static_cast<int>(_cv_image.step), _keypoints, p_desc);
		/////////////////////////////////////////////////////////////////////////////////

		if(print_kp_idx>-1){
			// LATCH ORIGINAL //////////////////////////////////////////////
			std::cout << "Descriptor (LATCH original): " << print_kp_idx << "/" << _keypoints.size()-1 << std::endl;
			int shift = print_kp_idx*8;
			for (int i = 0; i < 8; ++i) {
				std::cout << std::bitset<64>(p_desc[i+shift]) << std::endl;
			}
			std::cout << std::endl;

			// LATCH CLASS (FLIPPED) /////////////////////////////////////////////////	
			std::cout << "LATCH (FLIPPED)" << std::endl;
			for (int i = 0; i < 8; ++i) {
				for (int ii = 0; ii < 8; ++ii) {
					std::cout << std::bitset<8>(eigen_desc(i*8+7-ii,print_kp_idx));
				}
				std::cout << std::endl; 
			}
			std::cout << std::endl;
		}

		std::cout << "n_keypoints: " << n_found_kp << " == " << _keypoints.size() << std::endl;
		// ASSERTION: The LATCH function will remove keypoints close to the border. This must be avoided
		// as this will create a mismatch between the index of descriptors and keypoints. 
		assert(!(n_found_kp==_keypoints.size()) && "The LATCH function have remove keypoints close to the border. This must be avoided as this will create a mismatch between the index of descriptors and keypoints." );
#else
		
	        throw std::invalid_argument( "LATCH NOT WORKING FOR ARM" );
#endif

	}
	else {
		
	}
	
	
	return eigen_desc;
}

Eigen::Matrix<uint8_t, Eigen::Dynamic, Eigen::Dynamic> FeatureExtractor::detect_and_compute(Eigen::Matrix<uint8_t, Eigen::Dynamic, Eigen::Dynamic> eigen_image, bool use_latch)
{	
	cv::eigen2cv(eigen_image,_cv_image);
	Eigen::Matrix<uint8_t, Eigen::Dynamic, Eigen::Dynamic> eigen_desc;
	if(use_latch) throw std::invalid_argument("Latch-based (CURRENTLY NOT WORKING)");
	if (_cv_image.rows != _cv_mask.rows && _cv_image.cols != _cv_mask.cols)
	{
		_cv_mask = cv::Mat::ones(_cv_image.rows, _cv_image.cols, _cv_image.type());
	}
	cv::Mat cv_desc;
	switch (_interest_point_detector_type){
		case 0:
			_orbHigh->detectAndCompute(_cv_image, _cv_mask, _keypoints, cv_desc);
			break;
		case 1:
			_orbLow->detect(_cv_image, _keypoints, _cv_mask);
			internal_adaptive_non_maximum_suppression();
			_orbHigh->compute(_cv_image, _keypoints, cv_desc);
			break;
		case 2:
			_orb_slam->operator()(_cv_image, cv::noArray(),_keypoints,cv_desc);
			break;
		case 3:
			inititialze_POI_on_grid();
			internal_adaptive_non_maximum_suppression();
			_orbHigh->compute(_cv_image, _keypoints, cv_desc);
			break;
		}
	if (cv_desc.rows == 0) {
		_keypoints.clear();
		return eigen_desc;
	} else
	{
		cv::cv2eigen(cv_desc, eigen_desc);
		return eigen_desc;
	}
	
#if 0
}
	// Latch-based (CURRENTLY NOT WORKING)
	else {
		if((_interest_point_detector_type == 0) || (_interest_point_detector_type == 1)){
			FeatureExtractor::orb->detect(_cv_image, _keypoints, _cv_mask);

			if (_interest_point_detector_type == 1){
				internal_adaptive_non_maximum_suppression();
			}
		}
		else if (_interest_point_detector_type == 2) {
			(*FeatureExtractor::_orb_slam)(_cv_image, _keypoints);
		}
		// Bug fixing
		remove_outside(_keypoints,_cv_image.cols,_cv_image.rows,36);

		eigen_desc = compute_descriptor(true);
	}
#endif

}
// https://www.sciencedirect.com/science/article/pii/S016786551830062X
// paper: Efficient adaptive non-maximal suppression algorithms for homogeneous spatial keypoint distribution
// https://github.com/BAILOOL/ANMS-Codes
// TODO: Add the license probably.
void FeatureExtractor::internal_adaptive_non_maximum_suppression(){
//void FeatureExtractor::adaptive_non_maximum_suppression(Eigen::MatrixXd pointLocation, int numInPoints, int numRetPoints, float tolerance, int cols, int rows){
	//std::cout << "Function called successfully " << std::endl;
	//std::cout << "STEP0 " << std::endl;
    // several temp expression variables to simplify solution equation
	//std::cout << "pointLocation.size " << pointLocation.rows <<  std::endl;
	//std::vector<cv::KeyPoint> keypoints, int numRetPoints, float tolerance, int cols, int rows
	if (_keypoints.size() == 0)
		return;
	// Sort points according to response. 
	std::sort(_keypoints.begin(), _keypoints.end(), [](cv::KeyPoint a, cv::KeyPoint b) { return a.response > b.response; });

	float tolerance = 0.1f;
	int cols = _cv_image.cols;
	int rows = _cv_image.rows;
	int numInPoints = _keypoints.size();
	int numRetPoints = _n_keypoints;
    int exp1 = rows + cols + 2*numRetPoints;
    long long exp2 = ((long long) 4*cols + (long long)4*numRetPoints + (long long)4*rows*numRetPoints + (long long)rows*rows + (long long) cols*cols - (long long)2*rows*cols + (long long)4*rows*cols*numRetPoints);
    double exp3 = sqrt(double(exp2));
    double exp4 = (2*(numRetPoints - 1));

    double sol1 = -round((exp1+exp3)/exp4); // first solution
    double sol2 = -round((exp1-exp3)/exp4); // second solution

    double high = (sol1>sol2)? sol1 : sol2; //binary search range initialization with positive solution
    double low = floor(sqrt((double)numInPoints/numRetPoints));

    double width;
    int prevWidth = -1;
	//std::cout << "STEP0 " << std::endl;
    std::vector<int> ResultVec;
    bool complete = false;
    unsigned int K = numRetPoints; 
	unsigned int Kmin = round(K-(K*tolerance)); 
	unsigned int Kmax = round(K+(K*tolerance));
    
    std::vector<int> result; 
	result.reserve(numInPoints);
	
	//std::cout <<  exp1 << ", " <<  exp2 << ", " <<  exp3 << ", " << exp4 << ", " << sol1 << ", " << sol2 << ", " << high << ", " << low << ", " << K << ", " << Kmin << ", " << Kmax << std::endl;

    while(!complete){
        width = low+(high-low)/2;
        if (width == prevWidth || low>high) { //needed to reassure the same radius is not repeated again
            ResultVec = result; //return the keypoints from the previous iteration
            break;
        }
        result.clear();
        double c = width/2; //initializing Grid
        int numCellCols = floor(cols/c);
        int numCellRows = floor(rows/c);
		
        std::vector<std::vector<bool> > coveredVec(numCellRows+1,std::vector<bool>(numCellCols+1,false));

        for (unsigned int i=0;i<numInPoints;++i){
			int row = floor(_keypoints[i].pt.y/c); //get position of the cell current point is located at
            int col = floor(_keypoints[i].pt.x/c);

			
            if (coveredVec[row][col]==false){ // if the cell is not covered
                result.push_back(i);
                int rowMin = ((row-floor(width/c))>=0)? (row-floor(width/c)) : 0; //get range which current radius is covering
                int rowMax = ((row+floor(width/c))<=numCellRows)? (row+floor(width/c)) : numCellRows;
                int colMin = ((col-floor(width/c))>=0)? (col-floor(width/c)) : 0;
                int colMax = ((col+floor(width/c))<=numCellCols)? (col+floor(width/c)) : numCellCols;
                for (int rowToCov=rowMin; rowToCov<=rowMax; ++rowToCov){
                    for (int colToCov=colMin ; colToCov<=colMax; ++colToCov){
                        if (!coveredVec[rowToCov][colToCov]) coveredVec[rowToCov][colToCov] = true; //cover cells within the square bounding box with width w
                    }
                }
            }
        }
        if (result.size()>=Kmin && result.size()<=Kmax){ //solution found
            ResultVec = result;
            complete = true;
			//std::cout << "STEP END0 " << std::endl;
        }
        else if (result.size()<Kmin) high = width-1; //update binary search range
        else low = width+1;
        prevWidth = width;
    }

	// 
	std::vector<cv::KeyPoint> tmpKeypoints;
	//std::cout << "STEP END1 " << std::endl;
	for(std::vector<int>::iterator it = ResultVec.begin(); it != ResultVec.end(); ++it) {
     	tmpKeypoints.push_back(_keypoints[*it]);
	}
	_keypoints = tmpKeypoints;
}

void FeatureExtractor::inititialze_POI_on_grid()
{
	const int minBorderX = 0;
	const int minBorderY = 0;
	const int maxBorderX = _cv_image.cols;
	const int maxBorderY = _cv_image.rows;
	_keypoints.clear();
	_keypoints.reserve(_n_keypoints * 10); // intial speed up on resize
	const double width = (maxBorderX - minBorderX);
	const double height = (maxBorderY - minBorderY);
	const int nCols =  FeatureExtractor::NUM_GRID_CELLS;
	const int nRows =  FeatureExtractor::NUM_GRID_CELLS;
	const int wCell = ceil(width / nCols);
	const int hCell = ceil(height / nRows);

	for (int i = 0; i < nRows; i++)
	{
		// #FIXYury Add clamp function
		const float iniY = clamp(minBorderY + i * hCell - FeatureExtractor::EDGE_THRESHOLD*2, minBorderY, maxBorderY);
		const float maxY = clamp(iniY + hCell + FeatureExtractor::EDGE_THRESHOLD * 2, minBorderY, maxBorderY);

		for (int j = 0; j < nCols; j++)
		{
			const float iniX = clamp(minBorderX + j * wCell - FeatureExtractor::EDGE_THRESHOLD * 2, minBorderX, maxBorderX);
			const float maxX = clamp(iniX + wCell + FeatureExtractor::EDGE_THRESHOLD * 2, minBorderX, maxBorderX);
			std::vector<cv::KeyPoint> CellPOI;
			_orbHigh->detect(_cv_image.rowRange(iniY, maxY).colRange(iniX, maxX), CellPOI, _cv_mask.rowRange(iniY, maxY).colRange(iniX, maxX));
			if (CellPOI.size() < 10)
			{
				_orbLow->detect(_cv_image.rowRange(iniY, maxY).colRange(iniX, maxX), CellPOI, _cv_mask.rowRange(iniY, maxY).colRange(iniX, maxX));
			}

			for (std::vector<cv::KeyPoint>::iterator vit = CellPOI.begin(); vit != CellPOI.end(); vit++)
			{
				(*vit).pt.x += iniX;
				(*vit).pt.y += iniY;
				_keypoints.push_back(*vit);
			}
		}
	}
}

//
//
//int main(int argc, const char * argv[]) {
//	// ------------- Configuration ------------
//	//constexpr int warmups = 30;
//	constexpr int runs = 100;
//	constexpr int numkps = 2000;
//	constexpr bool multithread = true;
//	constexpr char name[] = "../pipe.jpg";
//
//	int n_runs = 10;
//	// --------------------------------
//
//
//	// ------------- Image Read ------------
//	cv::Mat image = cv::imread(name, CV_LOAD_IMAGE_GRAYSCALE);
//	if (!image.data) {
//		std::cerr << "ERROR: failed to open image. Aborting." << std::endl;
//		return EXIT_FAILURE;
//	}
//	// --------------------------------
//	
//	
//	int edge_treshold = 36; // THIS VALUE IS IMPORTANT !!!
//	cv::Ptr<cv::ORB> orb = cv::ORB::create(numkps, 1.2f, 8, edge_treshold, 0, 2, cv::ORB::HARRIS_SCORE, 31, 20); //
//	std::vector<cv::KeyPoint> keypoints;
//	std::vector<cv::KeyPoint> keypoints1;
//	cv::Mat descriptors;
//	orb->setMaxFeatures(numkps);
//
//	// ------------- Interst Point detector (opencv) ------------
//	high_resolution_clock::time_point t0 = high_resolution_clock::now();
//	for (int i = 0; i < n_runs; ++i){
//		orb->detect(image, keypoints);
//	}
//	high_resolution_clock::time_point t1 = high_resolution_clock::now();
//	for (int i = 0; i < n_runs; ++i){
//		orb->compute(image, keypoints,descriptors);
//	}
//	high_resolution_clock::time_point t2 = high_resolution_clock::now();
//	//std::cout << "Interst Point detector (opencv): " << static_cast<double>((t1 - t0).count()) / (double)(n_runs) * 1e-6 <<  "ms" << std::endl;
//	//std::cout << "Descriptor (opencv): " << static_cast<double>((t2 - t1).count()) / (double)(n_runs) * 1e-6 <<  "ms" << std::endl;
//	std::cout << "Interst Point detector(" << static_cast<double>((t1 - t0).count()) / (double)(n_runs) * 1e-6 
//			  << ") and descriptor (" << static_cast<double>((t2 - t1).count()) / (double)(n_runs) * 1e-6 
//			  << ") (seperated - opencv): " << static_cast<double>((t2 - t0).count()) / (double)(n_runs) * 1e-6 <<  "ms" << std::endl;
//	// -----------------------------------------------------------
//	
//	// ------------- Interst Point detector and descriptor (opencv) ------------
//	t0 = high_resolution_clock::now();
//	for (int i = 0; i < n_runs; ++i){
//		orb->detectAndCompute(image, cv::noArray(), keypoints1,descriptors);
//	}
//	t1 = high_resolution_clock::now();
//	std::cout << "Interst Point detector and descriptor (jointed - opencv): " << static_cast<double>((t1 - t0).count()) / (double)(n_runs) * 1e-6 <<  "ms" << std::endl;
//	// -----------------------------------------------------------
//
//#ifdef USE_LATCH
//	// ------------- LATCH ------------
//	uint64_t* desc = new uint64_t[8 * keypoints.size()];
//	// For testing... Force values to zero. 
//	for (int i = 0; i < 8 * keypoints.size(); ++i) {
//		desc[i] = 0;
//	}
//
//	high_resolution_clock::time_point start1 = high_resolution_clock::now();
//	for (int i = 0; i < runs; ++i) LATCH<multithread>(image.data, image.cols, image.rows, static_cast<int>(image.step), keypoints, desc);
//	high_resolution_clock::time_point end1 = high_resolution_clock::now();
//	// --------------------------------
//	
//	
//
//	//std::cout << std::endl << "LATCH (warmup) took " << static_cast<double>((end0 - start0).count()) * 1e-3 / (static_cast<double>(warmups) * static_cast<double>(kps.size())) << " us per desc over " << kps.size() << " desc" << (kps.size() == 1 ? "." : "s.") << std::endl << std::endl;
//	std::cout << std::endl << "LATCH (after) took " << static_cast<double>((end1 - start1).count()) * 1e-3 / (static_cast<double>(runs) * static_cast<double>(keypoints.size())) << " us per desc over " << keypoints.size() << " desc" << (keypoints.size() == 1 ? "." : "s.") << std::endl << std::endl;
//
//	// TESTING ///////////////////
//	int print_desc_idx = 0; // keypoints_class.size()
//	std::cout << "argc:" << argc << std::endl;
//	if (argc > 1) {
//		print_desc_idx = std::atoi(argv[1]);
//	}
//	std::cout << "Select keypoint: " << argv[1] << std::endl;
//
//	// ------------- FeatureExtractor Class ------------
//	FeatureExtractor feature_extractor = FeatureExtractor(numkps);
//	
//
//	Eigen::Matrix<uint8_t,Eigen::Dynamic,Eigen::Dynamic> eig_image;
//	cv::cv2eigen(image,eig_image);
//	t0 = high_resolution_clock::now();
//	for (int i = 0; i < n_runs; ++i){
//		feature_extractor.detect_interest_points(eig_image);
//	}
//	t1 = high_resolution_clock::now();
//
//	std::vector<cv::KeyPoint> keypoints_class = feature_extractor._keypoints;
//
//	Eigen::Matrix<uint8_t, Eigen::Dynamic, Eigen::Dynamic> eigen_desc;
//	eigen_desc = feature_extractor.compute_descriptor(true,print_desc_idx);
//	
//
//	std::cout << "Interst Point detector (Class) " << static_cast<double>((t1 - t0).count()) / (double)(n_runs) * 1e-6 <<  "ms" << std::endl;
//	// -----------------------------------------------------------
//
//	
//
//	// LATCH ORIGINAL //////////////////////////////////////////////
//	std::cout << "Descriptor (LATCH original): " << print_desc_idx << "/" << keypoints_class.size()-1 << std::endl;
//	int shift = print_desc_idx*8;
//	for (int i = 0; i < 8; ++i) {
//		std::cout << std::bitset<64>(desc[i+shift]) << std::endl;
//	}
//	std::cout << std::endl;
//
//	// LATCH CLASS (FLIPPED) /////////////////////////////////////////////////	
//	std::cout << "LATCH (FLIPPED)" << std::endl;
//	for (int i = 0; i < 8; ++i) {
//		for (int ii = 0; ii < 8; ++ii) {
//			std::cout << std::bitset<8>(eigen_desc(i*8+7-ii,print_desc_idx));
//		}
//		std::cout << std::endl; 
//	}
//	std::cout << std::endl;
//
//	long long total = 0;
//	for (size_t i = 0; i < 8 * keypoints_class.size(); ++i) total += desc[i];
//	std::cout << "Checksum: " << std::hex << total << std::endl << std::endl;
//#endif
//	
//}
