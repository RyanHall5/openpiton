// Stand-in for AIBench's loadImage.h, used only to build the ORIGINAL AIBench
// kernels on x86 for checking the bare-metal ports (see ../check_ports.sh).
// Same typedefs and the same fourDim2oneDim() as the original, minus the
// OpenCV image loading: the input comes from aib_host.cpp instead.
#ifndef _LOADIMAGE_H
#define _LOADIMAGE_H
#include <cstdio>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <cmath>
#include <cstdlib>
#include <cfloat>
#include <dirent.h>
using namespace std;

typedef vector<vector<vector<float>>> singleImage;
typedef vector<vector<vector<vector<float>>>> batchImage;
typedef vector<vector<vector<float>>> singleMatrix;
typedef vector<vector<vector<vector<float>>>> batchMatrix;

// copied from AIBench loadImage.h
void fourDim2oneDim(batchImage& imgBatch, vector<float>& data)
{
	if(imgBatch.size() == 0 || imgBatch[0].size() == 0 || imgBatch[0][0].size() == 0 || imgBatch[0][0][0].size() == 0)
		return ;
	int batchSize = imgBatch.size();
	int heightSize = imgBatch[0].size();
	int wightSize = imgBatch[0][0].size();
	int channelSize = imgBatch[0][0][0].size();
	data.clear();
	for(int n = 0; n < batchSize; ++n)
	for(int h = 0; h < heightSize; ++h)
	for(int w = 0; w < wightSize; ++w)
	for(int c = 0; c < channelSize; ++c)
		data.push_back(imgBatch[n][h][w][c]);
	return ;
}
#endif
