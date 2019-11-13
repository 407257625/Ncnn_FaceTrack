#ifndef ZEUSEESFACETRACKING_H
#define ZEUSEESFACETRACKING_H

#include <opencv2/opencv.hpp>
#include "mtcnn.h"
#include "time.h"

cv::Rect boundingRect(const std::vector<cv::Point>& pts) {
	if (pts.size() > 1)
	{
		int xmin = pts[0].x;
		int ymin = pts[0].y;
		int xmax = pts[0].x;
		int ymax = pts[0].y;
		for (int i = 1; i < pts.size(); i++)
		{
			if (pts[i].x < xmin)
				xmin = pts[i].x;
			if (pts[i].y < ymin)
				ymin = pts[i].y;
			if (pts[i].x > xmax)
				xmax = pts[i].x;
			if (pts[i].y > ymax)
				ymax = pts[i].y;
		}
		return cv::Rect(xmin, ymin, xmax - xmin, ymax - ymin);
	}
}


//typedef int T;
//T i = 1;


class Face {
public:

	Face(int instance_id, cv::Rect rect) {
		face_id = instance_id;

		loc = rect;
		isCanShow = false; //追踪一次后待框稳定后即可显示
		
	}

	Face() {

		isCanShow = false; //追踪一次后待框稳定后即可显示
	}

	Bbox faceBbox;
	cv::Rect loc;
	int face_id = -1;
	long frameId = 0;
	int ptr_num = 0;

	bool isCanShow;
	cv::Mat frame_face_prev;

	static cv::Rect SquarePadding(cv::Rect facebox, int margin_rows, int margin_cols, bool max_b)
	{
		int c_x = facebox.x + facebox.width / 2;
		int c_y = facebox.y + facebox.height / 2;
		int large = 0;
		if (max_b)
			large = max(facebox.height, facebox.width) / 2;
		else
			large = min(facebox.height, facebox.width) / 2;
		cv::Rect rectNot(c_x - large, c_y - large, c_x + large, c_y + large);
		rectNot.x = max(0, rectNot.x);
		rectNot.y = max(0, rectNot.y);
		rectNot.height = min(rectNot.height, margin_rows - 1);
		rectNot.width = min(rectNot.width, margin_cols - 1);
		if (rectNot.height - rectNot.y != rectNot.width - rectNot.x)
			return SquarePadding(cv::Rect(rectNot.x, rectNot.y, rectNot.width - rectNot.x, rectNot.height - rectNot.y), margin_rows, margin_cols, false);

		return cv::Rect(rectNot.x, rectNot.y, rectNot.width - rectNot.x, rectNot.height - rectNot.y);
	}

	static cv::Rect SquarePadding(cv::Rect facebox, int padding)
	{

		int c_x = facebox.x - padding;
		int c_y = facebox.y - padding;
		return cv::Rect(facebox.x - padding, facebox.y - padding, facebox.width + padding * 2, facebox.height + padding * 2);;
	}

	static double getDistance(cv::Point x, cv::Point y)
	{
		return sqrt((x.x - y.x) * (x.x - y.x) + (x.y - y.y) * (x.y - y.y));
	}


	std::vector<std::vector<cv::Point> > faceSequence;
	std::vector<std::vector<float>> attitudeSequence;


};



class FaceTracking {
public:
	FaceTracking(std::string modelPath)
	{
		this->detector = new MTCNN(modelPath);

		faceMinSize = 70;
		this->detector->SetMinFace(faceMinSize);
		detection_Time = -1;

	}

	~FaceTracking() {
		delete this->detector;

	}

	void detecting(cv::Mat* image) {
		ncnn::Mat ncnn_img = ncnn::Mat::from_pixels(image->data, ncnn::Mat::PIXEL_BGR2RGB, image->cols, image->rows);
		std::vector<Bbox> finalBbox;
		if(isMaxFace)
			detector->detectMaxFace(ncnn_img, finalBbox);
		else
			detector->detect(ncnn_img, finalBbox);
		const int num_box = finalBbox.size();
		std::vector<cv::Rect> bbox;
		bbox.resize(num_box);
		candidateFaces_lock = 1;
		for (int i = 0; i < num_box; i++) {
			bbox[i] = cv::Rect(finalBbox[i].x1, finalBbox[i].y1, finalBbox[i].x2 - finalBbox[i].x1 + 1,
				finalBbox[i].y2 - finalBbox[i].y1 + 1);
			bbox[i] = Face::SquarePadding(bbox[i], image->rows, image->cols, true);
			
			std::shared_ptr<Face> face(new Face(trackingID, bbox[i]));
			(*image)(bbox[i]).copyTo(face->frame_face_prev);

			trackingID = trackingID + 1;
			candidateFaces.push_back(*face);
		}
		candidateFaces_lock = 0;
	}

	void Init(cv::Mat& image) {
		ImageHighDP = image.clone();

		trackingID = 0;
		detection_Interval = 200; //detect faces every 200 ms
		detecting(&image);
		stabilization = false;
		
	}

	void doingLandmark_onet(cv::Mat& face, Bbox& faceBbox, int zeroadd_x, int zeroadd_y, int stable_state = 0) {
		ncnn::Mat in = ncnn::Mat::from_pixels_resize(face.data, ncnn::Mat::PIXEL_BGR, face.cols, face.rows, 48, 48);
		faceBbox = detector->onet(in, zeroadd_x, zeroadd_y, face.cols, face.rows);
		
	}



	void tracking_corrfilter(const cv::Mat& frame, const cv::Mat& model, cv::Rect& trackBox, float scale)
	{
		trackBox.x /= scale;
		trackBox.y /= scale;
		trackBox.height /= scale;
		trackBox.width /= scale;
		int zeroadd_x = 0;
		int zeroadd_y = 0;
		cv::Mat frame_;
		cv::Mat model_;
		cv::resize(frame, frame_, cv::Size(), 1 / scale, 1 / scale);
		cv::resize(model, model_, cv::Size(), 1 / scale, 1 / scale);
		cv::Mat gray;
		cvtColor(frame_, gray, cv::COLOR_RGB2GRAY);
		cv::Mat gray_model;
		cvtColor(model_, gray_model, cv::COLOR_RGB2GRAY);
		cv::Rect searchWindow;
		searchWindow.width = trackBox.width * 3;
		searchWindow.height = trackBox.height * 3;
		searchWindow.x = trackBox.x + trackBox.width * 0.5 - searchWindow.width * 0.5;
		searchWindow.y = trackBox.y + trackBox.height * 0.5 - searchWindow.height * 0.5;
		searchWindow &= cv::Rect(0, 0, frame_.cols, frame_.rows);
		cv::Mat similarity;
		matchTemplate(gray(searchWindow), gray_model, similarity, cv::TM_CCOEFF_NORMED);
		double mag_r;
		cv::Point point;
		minMaxLoc(similarity, 0, &mag_r, 0, &point);
		trackBox.x = point.x + searchWindow.x;
		trackBox.y = point.y + searchWindow.y;
		trackBox.x *= scale;
		trackBox.y *= scale;
		trackBox.height *= scale;
		trackBox.width *= scale;
	}

	bool tracking(cv::Mat& image, Face& face)
	{
		cv::Rect faceROI = face.loc;
		cv::Mat faceROI_Image;
		tracking_corrfilter(image, face.frame_face_prev, faceROI, tpm_scale);
		image(faceROI).copyTo(faceROI_Image);


		doingLandmark_onet(faceROI_Image, face.faceBbox, faceROI.x, faceROI.y, face.frameId > 1);

		
		
		//ncnn::Mat rnet_data = ncnn::Mat::from_pixels_resize(faceROI_Image.data, ncnn::Mat::PIXEL_BGR2RGB, faceROI_Image.cols, faceROI_Image.rows, 24, 24);
		
		//float sim = detector->rnet(rnet_data);
		
		float sim = face.faceBbox.score;
		
		if (sim > 0.1) {
			//stablize
			//float diff_x = 0;
			//float diff_y = 0;

			cv::Rect bdbox;
			bdbox.x = face.faceBbox.x1;
			bdbox.y = face.faceBbox.y1;
			bdbox.width = face.faceBbox.x2 - face.faceBbox.x1;
			bdbox.height = face.faceBbox.y2 - face.faceBbox.y1;

			bdbox = Face::SquarePadding(bdbox, static_cast<int>(bdbox.height * -0.05));
			bdbox = Face::SquarePadding(bdbox, image.rows, image.cols, 1);




			face.faceBbox.x1 = bdbox.x;
			face.faceBbox.y1 = bdbox.y;
			face.faceBbox.x2 = bdbox.x + bdbox.width;
			face.faceBbox.y2 = bdbox.y + bdbox.height;


			face.loc = bdbox;


			image(bdbox).copyTo(face.frame_face_prev);
			face.frameId += 1;
			face.isCanShow = true;

			return true;
		}
		return false;

	}
	void setMask(cv::Mat& image, cv::Rect& rect_mask)
	{

		int height = image.rows;
		int width = image.cols;
		cv::Mat subImage = image(rect_mask);
		subImage.setTo(0);
	}

	void update(cv::Mat& image)
	{
		ImageHighDP = image.clone();
		//std::cout << trackingFace.size() << std::endl;
		if (candidateFaces.size() > 0 && !candidateFaces_lock)
		{
			for (int i = 0; i < candidateFaces.size(); i++)
			{
				trackingFace.push_back(candidateFaces[i]);
			}
			candidateFaces.clear();
		}
		for (std::vector<Face>::iterator iter = trackingFace.begin(); iter != trackingFace.end();)
		{
			if (!tracking(image, *iter))
			{
				iter = trackingFace.erase(iter); //追踪失败 则删除此人脸
			}
			else {
				iter++;
			}
		}

		if (detection_Time < 0)
		{
			detection_Time = (double)cv::getTickCount();
		}
		else {
			double diff = (double)(cv::getTickCount() - detection_Time) * 1000 / cv::getTickFrequency();
			if (diff > detection_Interval)
			{
				
				//set Mask to protect the tracking face not to be detected.
				for (auto& face : trackingFace)
				{
					setMask(ImageHighDP, face.loc);
				}
				detection_Time = (double)cv::getTickCount();
				// do detection in thread
				detecting(&ImageHighDP);
			}

		}
	}



	std::vector<Face> trackingFace; //跟踪中的人脸


private:

	//int isLostDetection;
	//int isTracking;
	//int isDetection;
	cv::Mat ImageHighDP;
	//cv::Mat ImageLowDP;
	//int downSimpilingFactor;
	int faceMinSize;
	MTCNN* detector;
	std::vector<Face> candidateFaces; // 将检测到的人脸放入此列队 待跟踪的人脸
	bool candidateFaces_lock;
	double detection_Time;
	double detection_Interval;
	int trackingID;
	bool stabilization;
	int tpm_scale = 2;
	bool isMaxFace = true;
};
#endif //ZEUSEESFACETRACKING_H
