#ifndef DIFF2_KERNELS_H_
#define DIFF2_KERNELS_H_

#include <vector>
#include <iostream>
#include <fstream>
#include <string.h>

#include "src/acc/cpu/cpu_settings.h"
#include "src/acc/acc_projector.h"
#include "src/acc/cpu/cpu_kernels/cpu_utils.h"
#include "src/acc/cpu/cpu_kernels/helper.h"

namespace CpuKernels
{

/*
 *     DIFFERENCE-BASED KERNELS
 */

// We are specializing 2D and 3D cases, since they benefit from different
// optimizations.

// Among the optimizations:
// sincos lookup table optimization. Function translatePixel calls
// sincos(x*tx + y*ty). We precompute 2D lookup tables for x and y directions.
// The first dimension is x or y pixel index, and the second dimension is x or y
// translation index. Since sin(a+B) = sin(A) * cos(B) + cos(A) * sin(B), and
// cos(A+B) = cos(A) * cos(B) - sin(A) * sin(B), we can use lookup table to
// compute sin(x*tx + y*ty) and cos(x*tx + y*ty).

template<bool REF3D, bool DATA3D, int block_sz, int eulers_per_block, int prefetch_fraction>
inline void diff2_coarse(
		unsigned long     grid_size,
		XFLOAT *g_eulers,
		XFLOAT *trans_x,
		XFLOAT *trans_y,
		XFLOAT *trans_z,
		XFLOAT *g_real,	
		XFLOAT *g_imag,
		AccProjectorKernel &projector,
		XFLOAT *g_corr,
		XFLOAT *g_diff2s,
		unsigned long translation_num,
		unsigned long image_size
		)
{
	const int xSize = projector.imgX;
	const int ySize = projector.imgY;
	const int zSize = projector.imgZ;
	const int xySize = xSize*ySize;
	const int maxR = projector.maxR;
	const int pass_num_floor = image_size / block_sz;
	const int pass_num = (image_size%block_sz == 0) ? pass_num_floor : pass_num_floor + 1;

#ifdef USE_SINCOS_TABLE
	// pre-compute sin and cos for x and y component
	XFLOAT sin_x[translation_num][xSize], cos_x[translation_num][xSize];
	XFLOAT sin_y[translation_num][ySize], cos_y[translation_num][ySize];
	XFLOAT sin_z[translation_num][zSize], cos_z[translation_num][zSize];

	if (DATA3D) {
		computeSincosLookupTable3D(translation_num, trans_x, trans_y, trans_z,
								xSize, ySize, zSize,                               
								&sin_x[0][0], &cos_x[0][0], 
								&sin_y[0][0], &cos_y[0][0],
								&sin_z[0][0], &cos_z[0][0]);
	}
	else {
		computeSincosLookupTable2D(translation_num, trans_x, trans_y, 
								xSize, ySize,
								&sin_x[0][0], &cos_x[0][0], 
								&sin_y[0][0], &cos_y[0][0]);	
	}
	
	alignas(MEM_ALIGN) XFLOAT trans_cos_x[block_sz], trans_sin_x[block_sz];
	alignas(MEM_ALIGN) XFLOAT trans_cos_y[block_sz], trans_sin_y[block_sz];
	alignas(MEM_ALIGN) XFLOAT trans_cos_z[block_sz], trans_sin_z[block_sz];
#endif
	
	int x[pass_num][block_sz], y[pass_num][block_sz], z[pass_num][block_sz];
	XFLOAT s_real[pass_num][block_sz];
	XFLOAT s_imag[pass_num][block_sz];
	XFLOAT s_corr[pass_num][block_sz];
	
	// Pre-calculate x/y/z
	for (int pass = 0; pass < pass_num; pass++) { // finish an entire ref image each block
		const int start = pass * block_sz;
		// Rotate the reference image per block_sz, saved in cache
#if _OPENMP >= 201307	// For OpenMP 4.0 and later
		#pragma omp simd simdlen(SIMD_LEN) safelen(block_sz)
#endif
		for (int tid=0; tid<block_sz; tid++) {
			const int pixel = start + tid;
			if (pixel < image_size) {
				if (DATA3D) {
					z[pass][tid] = pixel / xySize;
					const int xy = pixel % xySize;
					x[pass][tid] =    xy % xSize;
					y[pass][tid] =    xy / xSize;
					if (z[pass][tid] > maxR)
						z[pass][tid] -= zSize;
				}
				else {
					x[pass][tid] = pixel % xSize;
					y[pass][tid] = pixel / xSize;
					z[pass][tid] = (XFLOAT)0.0;
				}
				if (y[pass][tid] > maxR)
					y[pass][tid] -= ySize;

				s_real[pass][tid] = g_real[pixel];
				s_imag[pass][tid] = g_imag[pixel];
				s_corr[pass][tid] = g_corr[pixel] * (XFLOAT)0.5;
			}
		}
	}

	XFLOAT diff2s[translation_num][eulers_per_block];
	alignas(MEM_ALIGN) XFLOAT diffi[eulers_per_block];
	XFLOAT s_ref_real[eulers_per_block][block_sz];
	XFLOAT s_ref_imag[eulers_per_block][block_sz];
	alignas(MEM_ALIGN) XFLOAT s_eulers[eulers_per_block * 16];

	for (unsigned long block = 0; block < grid_size; block++) {
		//Prefetch euler matrices with cacheline friendly index
		for (int e = 0; e < eulers_per_block; e++)
			for (int i = 0; i < 9; i++)
				s_eulers[e*16+i] = g_eulers[(size_t)block * (size_t)eulers_per_block * (size_t)9 + e*9+i];

		memset(&diff2s[0][0], 0, sizeof(XFLOAT) * translation_num * eulers_per_block);

		//Step through data
		for (int pass = 0; pass < pass_num; pass++) { // finish an entire ref image each block
			const int start = pass * block_sz;
			for (int i = 0; i < eulers_per_block; i ++) {
#if _OPENMP >= 201307	// For OpenMP 4.0 and later
				#pragma omp simd simdlen(SIMD_LEN) safelen(block_sz)
#endif
#ifdef USE_INTEL_COMPILER
				#pragma forceinline
#endif
				for (int tid=0; tid<block_sz; tid++) {
					if (start + tid < image_size) {
						if (DATA3D) // if DATA3D, then REF3D as well.
							projector.project3Dmodel(
									x[pass][tid], y[pass][tid], z[pass][tid],
									s_eulers[i*16  ],
									s_eulers[i*16+1],
									s_eulers[i*16+2],
									s_eulers[i*16+3],
									s_eulers[i*16+4],
									s_eulers[i*16+5],
									s_eulers[i*16+6],
									s_eulers[i*16+7],
									s_eulers[i*16+8],
									s_ref_real[i][tid],
									s_ref_imag[i][tid]);
						else if (REF3D)
							projector.project3Dmodel(
									x[pass][tid], y[pass][tid],
									s_eulers[i*16  ],
									s_eulers[i*16+1],
									s_eulers[i*16+3],
									s_eulers[i*16+4],
									s_eulers[i*16+6],
									s_eulers[i*16+7],
									s_ref_real[i][tid],
									s_ref_imag[i][tid]);
						else
							projector.project2Dmodel(
									x[pass][tid], y[pass][tid],
									s_eulers[i*16  ],
									s_eulers[i*16+1],
									s_eulers[i*16+3],
									s_eulers[i*16+4],
									s_ref_real[i][tid],
									s_ref_imag[i][tid]);
					}
				}
			}

			for (int i=0; i<translation_num; i++) {
				XFLOAT tx = trans_x[i];
				XFLOAT ty = trans_y[i];
				XFLOAT tz = trans_z[i];                 

#ifdef USE_SINCOS_TABLE
#if _OPENMP >= 201307	// For OpenMP 4.0 and later
				#pragma omp simd simdlen(SIMD_LEN) safelen(block_sz)
#endif
				for (int tid=0; tid<block_sz; tid++) {
					if (start + tid < image_size) {
						int xidx = x[pass][tid];
						int yidx = y[pass][tid];
						int zidx;

						if (DATA3D) {
							zidx = z[pass][tid];
							if ( zidx < 0) {
								trans_cos_z[tid] =  cos_z[i][-zidx];
								trans_sin_z[tid] = -sin_z[i][-zidx];
							}
							else {
								trans_cos_z[tid] = cos_z[i][zidx];
								trans_sin_z[tid] = sin_z[i][zidx];
							}
						}

						if ( yidx < 0) {
							trans_cos_y[tid] =  cos_y[i][-yidx];
							trans_sin_y[tid] = -sin_y[i][-yidx];
						}
						else {
							trans_cos_y[tid] = cos_y[i][yidx];
							trans_sin_y[tid] = sin_y[i][yidx];
						}

						if ( xidx < 0) {
							trans_cos_x[tid] =  cos_x[i][-xidx];
							trans_sin_x[tid] = -sin_x[i][-xidx];
						}
						else {
							trans_cos_x[tid] = cos_x[i][xidx];
							trans_sin_x[tid] = sin_x[i][xidx];
						}
					}
				}  // tid  						
#endif

				memset(diffi, 0, sizeof(XFLOAT) * eulers_per_block);
#if _OPENMP > 201307	// For OpenMP 4.5 and later
				#pragma omp simd reduction(+:diffi) simdlen(SIMD_LEN) safelen(block_sz)
#endif
				for (int tid=0; tid<block_sz; tid++) {
					if (start + tid < image_size) {
						XFLOAT real, imag;
#ifdef USE_SINCOS_TABLE
						if (DATA3D) {
							XFLOAT s  = trans_sin_x[tid] * trans_cos_y[tid] + trans_cos_x[tid] * trans_sin_y[tid];
							XFLOAT c  = trans_cos_x[tid] * trans_cos_y[tid] - trans_sin_x[tid] * trans_sin_y[tid];

							XFLOAT ss = s * trans_cos_z[tid] + c * trans_sin_z[tid];
							XFLOAT cc = c * trans_cos_z[tid] - s * trans_sin_z[tid];

							real = cc * s_real[pass][tid] - ss * s_imag[pass][tid];
							imag = cc * s_imag[pass][tid] + ss * s_real[pass][tid];
						}
						else  { // 2D data
							XFLOAT ss = trans_sin_x[tid] * trans_cos_y[tid] + trans_cos_x[tid] * trans_sin_y[tid];
							XFLOAT cc = trans_cos_x[tid] * trans_cos_y[tid] - trans_sin_x[tid] * trans_sin_y[tid];

							real = cc * s_real[pass][tid] - ss * s_imag[pass][tid];
							imag = cc * s_imag[pass][tid] + ss * s_real[pass][tid];
						}
#else
						if (DATA3D)
							TRANSLATE_PIXEL_3D(x[pass][tid], y[pass][tid], z[pass][tid], tx, ty, tz, s_real[pass][tid], s_imag[pass][tid], real, imag)
						else
							TRANSLATE_PIXEL_2D(x[pass][tid], y[pass][tid], tx, ty, s_real[pass][tid], s_imag[pass][tid], real, imag)
#endif

						#pragma unroll(eulers_per_block)
						for (int j = 0; j < eulers_per_block; j ++) {
							XFLOAT diff_real =  s_ref_real[j][tid] - real;
							XFLOAT diff_imag =  s_ref_imag[j][tid] - imag;

							diffi[j] += (diff_real * diff_real + diff_imag * diff_imag) * s_corr[pass][tid];
						}
					}
				} // for tid
				for (int j = 0; j < eulers_per_block; j++)
					diff2s[i][j] += diffi[j];
			}  // for each translation
		}  // for each pass

		XFLOAT *pData = g_diff2s + (size_t)block * (size_t)eulers_per_block * (size_t)translation_num;
		for(int j=0; j<eulers_per_block; j++)
			for(int i=0; i<translation_num; i++)
				pData[j*translation_num + i] += diff2s[i][j];
	} // block
}

template<bool REF3D>
inline void diff2_fine_2D(
		unsigned long     grid_size,
		XFLOAT *g_eulers,
		XFLOAT *g_imgs_real,		
		XFLOAT *g_imgs_imag,
		XFLOAT *g_trans_x,
		XFLOAT *g_trans_y,
		XFLOAT *g_trans_z,		
		AccProjectorKernel &projector,
		XFLOAT *g_corr_img,
		XFLOAT *g_diff2s,
		unsigned long image_size,
		XFLOAT sum_init,
		unsigned long orientation_num,
		unsigned long translation_num,
		unsigned long num_jobs,
		unsigned long *d_rot_idx,
		unsigned long *d_trans_idx,
		unsigned long *d_job_idx,
		unsigned long *d_job_num
		)
{
    // Set up arrays to hold largest possible values
	int xSize = projector.imgX;
	int ySize = projector.imgY;
	XFLOAT sin_x[translation_num][xSize], cos_x[translation_num][xSize];
	XFLOAT sin_y[translation_num][ySize], cos_y[translation_num][ySize];

	alignas(MEM_ALIGN) XFLOAT trans_x[translation_num], trans_y[translation_num];
	
	alignas(MEM_ALIGN) XFLOAT ref_real[xSize],  ref_imag[xSize];
	alignas(MEM_ALIGN) XFLOAT imgs_real[xSize], imgs_imag[xSize];
	
	alignas(MEM_ALIGN) XFLOAT s[translation_num];   
	
	// Now do calculations
	for (unsigned long bid = 0; bid < grid_size; bid++) {
		unsigned long trans_num        = (unsigned long)d_job_num[bid];     
		unsigned long int iy_part = d_trans_idx[d_job_idx[bid]];  

		size_t offset = d_rot_idx[d_job_idx[bid]] * 9;
		XFLOAT e1 = g_eulers[offset  ], e2 = g_eulers[offset+1];
		XFLOAT e3 = g_eulers[offset+3], e4 = g_eulers[offset+4];
		XFLOAT e5 = g_eulers[offset+6], e6 = g_eulers[offset+7];        

		// build lookup table for sin and cos
		for(unsigned long i=0; i<trans_num; i++) {
			int itrans = d_trans_idx[d_job_idx[bid]] + i;
			trans_x[i] = g_trans_x[itrans];
			trans_y[i] = g_trans_y[itrans];	       
		}	
		computeSincosLookupTable2D(trans_num, trans_x, trans_y,
				xSize, ySize,
				&sin_x[0][0], &cos_x[0][0], 
				&sin_y[0][0], &cos_y[0][0]);		
 
		memset(s, 0, sizeof(XFLOAT) * trans_num);

		unsigned long pixel = 0;
		for(int iy = 0; iy < ySize; iy++) {
			int xstart = 0, xend = xSize;
			int y = iy;
			if (iy > projector.maxR) {
				if (iy >= ySize - projector.maxR)
					y = iy - ySize;
				else {
					// handle special case for one pixel
					xstart = projector.maxR;
					xend   = xstart + 1;
				}
			}

#if _OPENMP >= 201307	// For OpenMP 4.0 and later
			#pragma omp simd simdlen(SIMD_LEN)
#endif
#ifdef USE_INTEL_COMPILER
			#pragma forceinline
#endif
			for(int x = xstart; x < xend; x++) {
				if(REF3D)
					projector.project3Dmodel(x, y, e1, e2, e3, e4, e5, e6, 
										 ref_real[x], ref_imag[x]);
				else			                         
					projector.project2Dmodel(x, y, e1, e2, e3, e4, 
										 ref_real[x], ref_imag[x]);			                      
			}

#if _OPENMP >= 201307	// For OpenMP 4.0 and later
			#pragma omp simd simdlen(SIMD_LEN)
#endif
			for(int x = xstart; x < xend; x++) {
	#ifdef ACC_DOUBLE_PRECISION        
				XFLOAT half_corr = sqrt (g_corr_img[pixel + x] * (XFLOAT)0.5);
	#else
				XFLOAT half_corr = sqrtf(g_corr_img[pixel + x] * (XFLOAT)0.5);
	#endif            
				ref_real[x]  *= half_corr;
				ref_imag[x]  *= half_corr;            
				imgs_real[x]  = g_imgs_real[pixel + x] * half_corr;
				imgs_imag[x]  = g_imgs_imag[pixel + x] * half_corr;            
			}


			for (unsigned long itrans=0; itrans<trans_num; itrans++) {
				XFLOAT trans_cos_y, trans_sin_y;
				if ( y < 0) {
					trans_cos_y =  cos_y[itrans][-y];
					trans_sin_y = -sin_y[itrans][-y];            
				}
				else {
					trans_cos_y = cos_y[itrans][y];
					trans_sin_y = sin_y[itrans][y];
				}

				XFLOAT *trans_cos_x = &cos_x[itrans][0];
				XFLOAT *trans_sin_x = &sin_x[itrans][0];     

				XFLOAT sum = (XFLOAT) 0.0;                   
#if _OPENMP >= 201307	// For OpenMP 4.0 and later
				#pragma omp simd reduction(+:sum) simdlen(SIMD_LEN)
#endif
				for(int x = xstart; x < xend; x++) {
					XFLOAT ss = trans_sin_x[x] * trans_cos_y + trans_cos_x[x] * trans_sin_y;
					XFLOAT cc = trans_cos_x[x] * trans_cos_y - trans_sin_x[x] * trans_sin_y;

					XFLOAT shifted_real = cc * imgs_real[x] - ss * imgs_imag[x];
					XFLOAT shifted_imag = cc * imgs_imag[x] + ss * imgs_real[x];

					XFLOAT diff_real =  ref_real[x] - shifted_real;
					XFLOAT diff_imag =  ref_imag[x] - shifted_imag;

					sum += (diff_real * diff_real + diff_imag * diff_imag);
				}
				s[itrans] += sum;
			}

			pixel += (unsigned long)xSize;
		}  // for pass

		for (unsigned long itrans=0; itrans<trans_num; itrans++)
		{
			unsigned long int iy = d_job_idx[bid]+itrans;
			g_diff2s[iy] += s[itrans] + sum_init;
		}
	}  // for bid
}

inline void diff2_fine_3D(
		unsigned long  grid_size,
		XFLOAT *g_eulers,
		XFLOAT *g_imgs_real,
		XFLOAT *g_imgs_imag,
		XFLOAT *g_trans_x,
		XFLOAT *g_trans_y,
		XFLOAT *g_trans_z,		
		AccProjectorKernel &projector,
		XFLOAT *g_corr_img,
		XFLOAT *g_diff2s,
		unsigned long image_size,
		XFLOAT sum_init,
		unsigned long orientation_num,
		unsigned long translation_num,
		unsigned long num_jobs,
		unsigned long *d_rot_idx,
		unsigned long *d_trans_idx,
		unsigned long *d_job_idx,
		unsigned long *d_job_num
		)
{
    // Set up arrays to hold largest possible values
	int xSize = projector.imgX;
	int ySize = projector.imgY;
	int zSize = projector.imgZ;
	XFLOAT sin_x[translation_num][xSize], cos_x[translation_num][xSize];
	XFLOAT sin_y[translation_num][ySize], cos_y[translation_num][ySize];
	XFLOAT sin_z[translation_num][zSize], cos_z[translation_num][zSize];	

	alignas(MEM_ALIGN) XFLOAT trans_x[translation_num], trans_y[translation_num], trans_z[translation_num];
	
	alignas(MEM_ALIGN) XFLOAT ref_real[xSize],  ref_imag[xSize];
	alignas(MEM_ALIGN) XFLOAT imgs_real[xSize], imgs_imag[xSize];
	
	alignas(MEM_ALIGN) XFLOAT s[translation_num];   
		
	// Now do calculations
	for (unsigned long bid = 0; bid < grid_size; bid++) {
		unsigned long trans_num        = (unsigned long)d_job_num[bid];     
		unsigned long int iy_part = d_trans_idx[d_job_idx[bid]];  

		size_t offset = d_rot_idx[d_job_idx[bid]] * 9;
		XFLOAT e1 = g_eulers[offset  ], e2 = g_eulers[offset+1];
		XFLOAT e3 = g_eulers[offset+2], e4 = g_eulers[offset+3];
		XFLOAT e5 = g_eulers[offset+4], e6 = g_eulers[offset+5];
		XFLOAT e7 = g_eulers[offset+6], e8 = g_eulers[offset+7];                    
		XFLOAT e9 = g_eulers[offset+8];

		// pre-compute sin and cos for x and y component
		for(unsigned long i=0; i<trans_num; i++) {
			int itrans = d_trans_idx[d_job_idx[bid]] + i;
			trans_x[i] = g_trans_x[itrans];
			trans_y[i] = g_trans_y[itrans];	    
			trans_z[i] = g_trans_z[itrans];	    	    
		}		
		computeSincosLookupTable3D(trans_num, trans_x, trans_y, trans_z,
								   xSize, ySize, zSize,
								  &sin_x[0][0], &cos_x[0][0], 
								  &sin_y[0][0], &cos_y[0][0],
								  &sin_z[0][0], &cos_z[0][0]);

		memset(s, 0, sizeof(XFLOAT) * trans_num);

		// index of comparison
		unsigned long pixel = 0;
		for(int iz = 0; iz < zSize; iz ++) {
			int xstart_z = 0, xend_z = xSize;
			int z = iz;
			if (z > projector.maxR)
			{
				if (z >= zSize - projector.maxR)
					z = z - projector.imgZ;
				else {
					xstart_z = projector.maxR;
					xend_z   = xstart_z + 1;
				}
			}

			for(int iy = 0; iy < ySize; iy++) {
				int xstart_y = xstart_z, xend_y = xend_z;
				int y = iy;
				if (iy > projector.maxR) {
					if (iy >= ySize - projector.maxR)
						y = iy - ySize;
					else {
						xstart_y = projector.maxR;
						xend_y   = xstart_y + 1;
					}
				}

#if _OPENMP >= 201307	// For OpenMP 4.0 and later
				#pragma omp simd simdlen(SIMD_LEN)
#endif
#ifdef USE_INTEL_COMPILER
				#pragma forceinline
#endif
				for(int x = xstart_y; x < xend_y; x++) {
					projector.project3Dmodel(x, y, z, e1, e2, e3, e4, e5, e6, e7, e8, e9, 
											 ref_real[x], ref_imag[x]);
				}

#if _OPENMP >= 201307	// For OpenMP 4.0 and later
				#pragma omp simd simdlen(SIMD_LEN)
#endif
				for(int x = xstart_y; x < xend_y; x++) {
	#ifdef ACC_DOUBLE_PRECISION        
					XFLOAT half_corr = sqrt (g_corr_img[pixel + x] * (XFLOAT)0.5);
	#else
					XFLOAT half_corr = sqrtf(g_corr_img[pixel + x] * (XFLOAT)0.5);
	#endif            
					ref_real[x]  *= half_corr;
					ref_imag[x]  *= half_corr;            
					imgs_real[x]  = g_imgs_real[pixel + x] * half_corr;
					imgs_imag[x]  = g_imgs_imag[pixel + x] * half_corr;            
				}

				for (unsigned long itrans=0; itrans<trans_num; itrans++) {
					XFLOAT trans_cos_z, trans_sin_z;
					if ( z < 0) {
						trans_cos_z =  cos_z[itrans][-z];
						trans_sin_z = -sin_z[itrans][-z];            
					}
					else {
						trans_cos_z = cos_z[itrans][z];
						trans_sin_z = sin_z[itrans][z];
					}

					XFLOAT trans_cos_y, trans_sin_y;
					if ( y < 0) {
						trans_cos_y =  cos_y[itrans][-y];
						trans_sin_y = -sin_y[itrans][-y];            
					}
					else {
						trans_cos_y = cos_y[itrans][y];
						trans_sin_y = sin_y[itrans][y];
					}

					XFLOAT *trans_cos_x = &cos_x[itrans][0];
					XFLOAT *trans_sin_x = &sin_x[itrans][0];     

					XFLOAT sum = (XFLOAT) 0.0;                   
#if _OPENMP >= 201307	// For OpenMP 4.0 and later
					#pragma omp simd reduction(+:sum)
#endif
					for(int x = xstart_y; x < xend_y; x++) {
						XFLOAT s1  = trans_sin_x[x] * trans_cos_y + trans_cos_x[x] * trans_sin_y;
						XFLOAT c1  = trans_cos_x[x] * trans_cos_y - trans_sin_x[x] * trans_sin_y;

						XFLOAT ss = s1 * trans_cos_z + c1 * trans_sin_z;
						XFLOAT cc = c1 * trans_cos_z - s1 * trans_sin_z;				

						XFLOAT shifted_real = cc * imgs_real[x] - ss * imgs_imag[x];
						XFLOAT shifted_imag = cc * imgs_imag[x] + ss * imgs_real[x];

						XFLOAT diff_real =  ref_real[x] - shifted_real;
						XFLOAT diff_imag =  ref_imag[x] - shifted_imag;

						sum += (diff_real * diff_real + diff_imag * diff_imag);
					}
					s[itrans] += sum;
				}

				pixel += (unsigned long)xSize;
			} // for y direction
		}  // for z direction

		for (unsigned long itrans=0; itrans<trans_num; itrans++)
		{
			unsigned long int iy = d_job_idx[bid]+itrans;
			g_diff2s[iy] += s[itrans] + sum_init;
		}
	} // for bid
}


/*
 *   	CROSS-CORRELATION-BASED KERNELS
 */
template<bool REF3D>
inline void diff2_CC_coarse_2D(
		unsigned long     grid_size,
		XFLOAT *g_eulers,
		XFLOAT *g_imgs_real,
		XFLOAT *g_imgs_imag,
		XFLOAT *g_trans_x,
		XFLOAT *g_trans_y,
		AccProjectorKernel &projector,
		XFLOAT *g_corr_img,
		XFLOAT  *g_diff2,
		unsigned long trans_num,
		unsigned long image_size,
		XFLOAT   exp_local_sqrtXi2
		)
{
	// pre-compute sin and cos for x and y direction
	int xSize = projector.imgX;
	int ySize = projector.imgY;
	XFLOAT sin_x[trans_num][xSize], cos_x[trans_num][xSize];
	XFLOAT sin_y[trans_num][ySize], cos_y[trans_num][ySize];

	computeSincosLookupTable2D(trans_num, g_trans_x, g_trans_y, xSize, ySize,
			&sin_x[0][0], &cos_x[0][0], 
			&sin_y[0][0], &cos_y[0][0]);
	
	// Set up other arrays
	XFLOAT s_weight[trans_num][xSize];	
	XFLOAT s_norm[trans_num][xSize];

	alignas(MEM_ALIGN) XFLOAT ref_real[xSize], ref_imag[xSize];
	alignas(MEM_ALIGN) XFLOAT img_real[xSize], img_imag[xSize], corr_imag[xSize];
			
	for (unsigned long iorient = 0; iorient < grid_size; iorient++) {
	
		XFLOAT e0,e1,e3,e4,e6,e7;
		e0 = g_eulers[iorient*9  ];
		e1 = g_eulers[iorient*9+1];
		e3 = g_eulers[iorient*9+3];
		e4 = g_eulers[iorient*9+4];
		e6 = g_eulers[iorient*9+6];
		e7 = g_eulers[iorient*9+7];

		memset(s_weight, 0, sizeof(XFLOAT) * xSize * trans_num);
		memset(s_norm, 0, sizeof(XFLOAT) *   xSize * trans_num);

		unsigned long pixel = 0;
		for(int iy = 0; iy < ySize; iy++) {
			int xstart = 0, xend = xSize;
			int y = iy;
			if (iy > projector.maxR) {
				if (iy >= ySize - projector.maxR)
					y = iy - ySize;
				else {
					// handle special case for one pixel
					xstart = projector.maxR;
					xend   = xstart + 1;
				}
			}

#if _OPENMP >= 201307	// For OpenMP 4.0 and later
			#pragma omp simd simdlen(SIMD_LEN)
#endif
#ifdef USE_INTEL_COMPILER
			#pragma forceinline
#endif
			for(int x = xstart; x < xend; x++) {
				if(REF3D)
					projector.project3Dmodel(
						x, y,
						e0, e1, e3, e4, e6, e7,
						ref_real[x], ref_imag[x]);
				else
					projector.project2Dmodel(
						x, y,
						e0, e1, e3, e4,
						ref_real[x], ref_imag[x]);

				img_real[x]  = g_imgs_real[pixel + x];
				img_imag[x]  = g_imgs_imag[pixel + x];
				corr_imag[x] = g_corr_img[pixel + x];            
			}

			for(unsigned long itrans=0; itrans<trans_num; itrans++) {
				XFLOAT trans_cos_y, trans_sin_y;
				if ( y < 0) {
					trans_cos_y =  cos_y[itrans][-y];
					trans_sin_y = -sin_y[itrans][-y];            
				}
				else {
					trans_cos_y = cos_y[itrans][y];
					trans_sin_y = sin_y[itrans][y];
				}

				XFLOAT *trans_cos_x = &cos_x[itrans][0];
				XFLOAT *trans_sin_x = &sin_x[itrans][0];     

				for(int x = xstart; x < xend; x++) {

					XFLOAT ss = trans_sin_x[x] * trans_cos_y + trans_cos_x[x] * trans_sin_y;
					XFLOAT cc = trans_cos_x[x] * trans_cos_y - trans_sin_x[x] * trans_sin_y;

					XFLOAT real = cc * img_real[x] - ss * img_imag[x];
					XFLOAT imag = cc * img_imag[x] + ss * img_real[x];

					s_weight[itrans][x] += (ref_real[x] * real        + ref_imag[x] * imag       ) * corr_imag[x];
					s_norm  [itrans][x] += (ref_real[x] * ref_real[x] + ref_imag[x] * ref_imag[x]) * corr_imag[x];
				}
			}

			pixel += (unsigned long)xSize;
		}

		for(unsigned long itrans=0; itrans<trans_num; itrans++) {
			XFLOAT sum_weight = (XFLOAT)0.0;
			XFLOAT sum_norm   = (XFLOAT)0.0;		

			for(int i=0; i<xSize; i++){
				sum_weight += s_weight[itrans][i];
				sum_norm   += s_norm  [itrans][i];
			}

	#ifdef RELION_SINGLE_PRECISION                  
			g_diff2[(unsigned long)iorient*(unsigned long)trans_num + itrans] += 
					- ( sum_weight / sqrtf(sum_norm));
	#else                   
			g_diff2[(unsigned long)iorient*(unsigned long)trans_num + itrans] += 
					- ( sum_weight / sqrt(sum_norm));
	#endif
		}
	} // for iorient
}

inline void diff2_CC_coarse_3D(
		unsigned long     grid_size,
		XFLOAT *g_eulers,
		XFLOAT *g_imgs_real,
		XFLOAT *g_imgs_imag,
		XFLOAT *g_trans_x,
		XFLOAT *g_trans_y,
		XFLOAT *g_trans_z,	
		AccProjectorKernel &projector,
		XFLOAT *g_corr_img,
		XFLOAT  *g_diff2,
		unsigned long trans_num,
		unsigned long image_size,
		XFLOAT   exp_local_sqrtXi2
		)
{
	// pre-compute sin and cos for x, y, and z direction
	int xSize = projector.imgX;
	int ySize = projector.imgY;
	int zSize = projector.imgZ;
	XFLOAT sin_x[trans_num][xSize], cos_x[trans_num][xSize];
	XFLOAT sin_y[trans_num][ySize], cos_y[trans_num][ySize];
	XFLOAT sin_z[trans_num][zSize], cos_z[trans_num][zSize];	

	computeSincosLookupTable3D(trans_num, g_trans_x, g_trans_y, g_trans_z,
							   xSize, ySize, zSize,                               
							  &sin_x[0][0], &cos_x[0][0], 
							  &sin_y[0][0], &cos_y[0][0],
							  &sin_z[0][0], &cos_z[0][0]);
		
	// Set up some arrays
	XFLOAT s_weight[trans_num][xSize];
	XFLOAT s_norm[trans_num][xSize];

	alignas(MEM_ALIGN) XFLOAT ref_real[xSize], ref_imag[xSize];
	alignas(MEM_ALIGN) XFLOAT img_real[xSize], img_imag[xSize], corr_imag[xSize];
	
	for (unsigned long iorient = 0; iorient < grid_size; iorient++) {
		XFLOAT e0, e1, e2, e3, e4, e5, e6, e7, e8;
		e0 = g_eulers[iorient*9  ];
		e1 = g_eulers[iorient*9+1];
		e2 = g_eulers[iorient*9+2];
		e3 = g_eulers[iorient*9+3];
		e4 = g_eulers[iorient*9+4];
		e5 = g_eulers[iorient*9+5];
		e6 = g_eulers[iorient*9+6];
		e7 = g_eulers[iorient*9+7];
		e8 = g_eulers[iorient*9+8];

		memset(s_weight, 0, sizeof(XFLOAT) * xSize * trans_num);
		memset(s_norm,   0, sizeof(XFLOAT) *   xSize * trans_num);

		unsigned long pixel = 0;
		for(int iz = 0; iz < zSize; iz ++) {
			int xstart_z = 0, xend_z = xSize;
			int z = iz;
			if (z > projector.maxR)
			{
				if (z >= zSize - projector.maxR)
					z = z - projector.imgZ;
				else {
					xstart_z = projector.maxR;
					xend_z   = xstart_z + 1;
				}
			}	

			for(int iy = 0; iy < ySize; iy++) {
				int xstart_y = xstart_z, xend_y = xend_z;
				int y = iy;
				if (iy > projector.maxR) {
					if (iy >= ySize - projector.maxR)
						y = iy - ySize;
					else {
						xstart_y = projector.maxR;
						xend_y   = xstart_y + 1;
					}
				}

#if _OPENMP >= 201307	// For OpenMP 4.0 and later
				#pragma omp simd simdlen(SIMD_LEN)
#endif
#ifdef USE_INTEL_COMPILER
				#pragma forceinline
#endif
				for(int x = xstart_y; x < xend_y; x++) {
					projector.project3Dmodel(
						x, y, z,
						e0, e1, e2, e3, e4, e5, e6, e7, e8,
						ref_real[x], ref_imag[x]);

					img_real[x]  = g_imgs_real[pixel + x];
					img_imag[x]  = g_imgs_imag[pixel + x];
					corr_imag[x] = g_corr_img[pixel + x];    	        
				}

				for(int itrans=0; itrans<trans_num; itrans++) {
					XFLOAT trans_cos_z, trans_sin_z;
					if ( z < 0) {
						trans_cos_z =  cos_z[itrans][-z];
						trans_sin_z = -sin_z[itrans][-z];            
					}
					else {
						trans_cos_z = cos_z[itrans][z];
						trans_sin_z = sin_z[itrans][z];
					}			

					XFLOAT trans_cos_y, trans_sin_y;
					if ( y < 0) {
						trans_cos_y =  cos_y[itrans][-y];
						trans_sin_y = -sin_y[itrans][-y];            
					}
					else {
						trans_cos_y = cos_y[itrans][y];
						trans_sin_y = sin_y[itrans][y];
					}

					XFLOAT *trans_cos_x = &cos_x[itrans][0];
					XFLOAT *trans_sin_x = &sin_x[itrans][0];     

					for(int x = xstart_y; x < xend_y; x++) {

						XFLOAT s  = trans_sin_x[x] * trans_cos_y + trans_cos_x[x] * trans_sin_y;
						XFLOAT c  = trans_cos_x[x] * trans_cos_y - trans_sin_x[x] * trans_sin_y;

						XFLOAT ss = s * trans_cos_z + c * trans_sin_z;
						XFLOAT cc = c * trans_cos_z - s * trans_sin_z;				

						XFLOAT real = cc * img_real[x] - ss * img_imag[x];
						XFLOAT imag = cc * img_imag[x] + ss * img_real[x];

						s_weight[itrans][x] += (ref_real[x] * real        + ref_imag[x] * imag       ) * corr_imag[x];
						s_norm  [itrans][x] += (ref_real[x] * ref_real[x] + ref_imag[x] * ref_imag[x]) * corr_imag[x];
					}
				}
				
				pixel += (unsigned long)xSize;
			}
		}

		for(unsigned long itrans=0; itrans<trans_num; itrans++) {
			XFLOAT sum_weight = (XFLOAT)0.0;
			XFLOAT sum_norm   = (XFLOAT)0.0;		

			for(int i=0; i<xSize; i++){
				sum_weight += s_weight[itrans][i];
				sum_norm   += s_norm  [itrans][i];
			}

	#ifdef RELION_SINGLE_PRECISION                  
			g_diff2[(unsigned long)iorient*(unsigned long)trans_num + itrans] += 
					- ( sum_weight / sqrtf(sum_norm));
	#else                   
			g_diff2[(unsigned long)iorient*(unsigned long)trans_num + itrans] += 
					- ( sum_weight / sqrt(sum_norm));
	#endif
		}
	} // for iorient
}


template<bool REF3D>
inline void diff2_CC_fine_2D(
		unsigned long     grid_size,
		XFLOAT *g_eulers,
		XFLOAT *g_imgs_real,
		XFLOAT *g_imgs_imag,
		XFLOAT *g_trans_x,
		XFLOAT *g_trans_y,
		AccProjectorKernel &projector,
		XFLOAT *g_corr_img,
		XFLOAT *g_diff2s,
		unsigned long image_size,
		XFLOAT sum_init,
		XFLOAT exp_local_sqrtXi2,
		unsigned long orientation_num,
		unsigned long translation_num,
		unsigned long num_jobs,
		unsigned long *d_rot_idx,
		unsigned long *d_trans_idx,
		unsigned long *d_job_idx,
		unsigned long *d_job_num
		)
{
    // Set up arrays to hold largest possible values
	int xSize = projector.imgX;
	int ySize = projector.imgY;
	XFLOAT sin_x[translation_num][xSize], cos_x[translation_num][xSize];
	XFLOAT sin_y[translation_num][ySize], cos_y[translation_num][ySize];

	alignas(MEM_ALIGN) XFLOAT trans_x[translation_num], trans_y[translation_num];

	XFLOAT  s   [translation_num][xSize]; 
	XFLOAT  s_cc[translation_num][xSize];
	
	alignas(MEM_ALIGN) XFLOAT ref_real[xSize], ref_imag[xSize];
	alignas(MEM_ALIGN) XFLOAT img_real[xSize], img_imag[xSize], corr_imag[xSize];
	
	// Now do calculations
	for (unsigned long bid = 0; bid < grid_size; bid++) {

		unsigned long trans_num   = d_job_num[bid]; //how many transes we have for this rot

		// pre-compute sin and cos for x and y direction
		for(unsigned long i=0; i<trans_num; i++) {
			unsigned long itrans = d_trans_idx[d_job_idx[bid]] + i;
			trans_x[i] = g_trans_x[itrans];
			trans_y[i] = g_trans_y[itrans];	    
		}	
		computeSincosLookupTable2D(trans_num, trans_x, trans_y, xSize, ySize,
				&sin_x[0][0], &cos_x[0][0], 
				&sin_y[0][0], &cos_y[0][0]);

		unsigned long int iorient = d_rot_idx[d_job_idx[bid]];	
		XFLOAT e0,e1,e3,e4,e6,e7;
		e0 = g_eulers[iorient*9  ];
		e1 = g_eulers[iorient*9+1];
		e3 = g_eulers[iorient*9+3];
		e4 = g_eulers[iorient*9+4];
		e6 = g_eulers[iorient*9+6];
		e7 = g_eulers[iorient*9+7];

		memset(&s[0][0],    0, sizeof(XFLOAT) * xSize * trans_num);
		memset(&s_cc[0][0], 0, sizeof(XFLOAT) * xSize * trans_num);

		unsigned long pixel = 0;
		for(int iy = 0; iy < ySize; iy++) {
			int xstart = 0, xend = xSize;
			int y = iy;
			if (iy > projector.maxR) {
				if (iy >= ySize - projector.maxR)
					y = iy - ySize;
				else {
					// handle special case for one pixel
					xstart = projector.maxR;
					xend   = xstart + 1;
				}
			}

#if _OPENMP >= 201307	// For OpenMP 4.0 and later
			#pragma omp simd simdlen(SIMD_LEN)
#endif
#ifdef USE_INTEL_COMPILER
			#pragma forceinline
#endif
			for(int x = xstart; x < xend; x++) {
				if(REF3D)
					projector.project3Dmodel(
						x, y,
						e0, e1, e3, e4, e6, e7,
						ref_real[x], ref_imag[x]);
				else
					projector.project2Dmodel(
						x, y,
						e0, e1, e3, e4, 
						ref_real[x], ref_imag[x]);

				img_real[x]  = g_imgs_real[pixel + x];
				img_imag[x]  = g_imgs_imag[pixel + x];
				corr_imag[x] = g_corr_img [pixel + x];
			}

			for (unsigned long itrans=0; itrans<trans_num; itrans++) // finish all translations in each partial pass
			{			
				XFLOAT trans_cos_y, trans_sin_y;
				if ( y < 0) {
					trans_cos_y =  cos_y[itrans][-y];
					trans_sin_y = -sin_y[itrans][-y];            
				}
				else {
					trans_cos_y = cos_y[itrans][y];
					trans_sin_y = sin_y[itrans][y];
				}

				XFLOAT *trans_cos_x = &cos_x[itrans][0];
				XFLOAT *trans_sin_x = &sin_x[itrans][0];     

				for(int x = xstart; x < xend; x++) {			
					XFLOAT ss = trans_sin_x[x] * trans_cos_y + trans_cos_x[x] * trans_sin_y;
					XFLOAT cc = trans_cos_x[x] * trans_cos_y - trans_sin_x[x] * trans_sin_y;

					XFLOAT shifted_real = cc * img_real[x] - ss * img_imag[x];
					XFLOAT shifted_imag = cc * img_imag[x] + ss * img_real[x];					    

					s[itrans][x]    += (ref_real[x] * shifted_real + ref_imag[x] * shifted_imag) * corr_imag[x];
					s_cc[itrans][x] += (ref_real[x] * ref_real[x] + ref_imag[x] * ref_imag[x])   * corr_imag[x];
				}
			}

			pixel += (unsigned long)xSize;
		} // loop y direction

		for(unsigned long itrans=0; itrans<trans_num; itrans++) {
			XFLOAT sum1 = (XFLOAT)0.0;
			XFLOAT sum2 = (XFLOAT)0.0;
			for(int x=0; x<xSize; x++){
				sum1 += s   [itrans][x];
				sum2 += s_cc[itrans][x]; 
			} 

			unsigned long int iy = d_job_idx[bid] + itrans;
	#ifdef RELION_SINGLE_PRECISION         
			g_diff2s[iy] += - sum1 / sqrtf(sum2);
	#else
			g_diff2s[iy] += - sum1 / sqrt(sum2);
	#endif
		}
	} // for bid
}

inline void diff2_CC_fine_3D(
		unsigned long     grid_size,
		XFLOAT *g_eulers,
		XFLOAT *g_imgs_real,
		XFLOAT *g_imgs_imag,
		XFLOAT *g_trans_x,
		XFLOAT *g_trans_y,
		XFLOAT *g_trans_z,		
		AccProjectorKernel &projector,
		XFLOAT *g_corr_img,
		XFLOAT *g_diff2s,
		unsigned long image_size,
		XFLOAT sum_init,
		XFLOAT exp_local_sqrtXi2,
		unsigned long orientation_num,
		unsigned long translation_num,
		unsigned long num_jobs,
		unsigned long *d_rot_idx,
		unsigned long *d_trans_idx,
		unsigned long *d_job_idx,
		unsigned long *d_job_num
		)
{
    // Set up arrays to hold largest possible values
	int xSize = projector.imgX;
	int ySize = projector.imgY;
	int zSize = projector.imgZ;
	XFLOAT sin_x[translation_num][xSize], cos_x[translation_num][xSize];
	XFLOAT sin_y[translation_num][ySize], cos_y[translation_num][ySize];
	XFLOAT sin_z[translation_num][zSize], cos_z[translation_num][zSize];	

	alignas(MEM_ALIGN) XFLOAT trans_x[translation_num], trans_y[translation_num], trans_z[translation_num];

	XFLOAT  s   [translation_num][xSize]; 
	XFLOAT  s_cc[translation_num][xSize];

	alignas(MEM_ALIGN) XFLOAT ref_real[xSize], ref_imag[xSize];
	alignas(MEM_ALIGN) XFLOAT img_real[xSize], img_imag[xSize], corr_imag[xSize];
	
	// Now do calculations
	for (unsigned long bid = 0; bid < grid_size; bid++) {

		unsigned long trans_num   = d_job_num[bid]; //how many transes we have for this rot

		// pre-compute sin and cos for x and y direction
		for(unsigned long i=0; i<trans_num; i++) {
			unsigned long itrans = d_trans_idx[d_job_idx[bid]] + i;
			trans_x[i] = g_trans_x[itrans];
			trans_y[i] = g_trans_y[itrans];	    
			trans_z[i] = g_trans_z[itrans];	    	    
		}		
		computeSincosLookupTable3D(trans_num, trans_x, trans_y, trans_z,
								   xSize, ySize, zSize,                               
								  &sin_x[0][0], &cos_x[0][0], 
								  &sin_y[0][0], &cos_y[0][0],
								  &sin_z[0][0], &cos_z[0][0]);

		memset(&s[0][0],    0, sizeof(XFLOAT) * xSize * trans_num);
		memset(&s_cc[0][0], 0, sizeof(XFLOAT) * xSize * trans_num);

		// index of comparison
		unsigned long int iorient = d_rot_idx[d_job_idx[bid]];	
		XFLOAT e0,e1,e2,e3,e4,e5,e6,e7,e8;
		e0 = g_eulers[iorient*9  ];
		e1 = g_eulers[iorient*9+1];
		e2 = g_eulers[iorient*9+2];	
		e3 = g_eulers[iorient*9+3];
		e4 = g_eulers[iorient*9+4];
		e5 = g_eulers[iorient*9+5];	
		e6 = g_eulers[iorient*9+6];
		e7 = g_eulers[iorient*9+7];
		e8 = g_eulers[iorient*9+8];

		unsigned long pixel = 0;
		for(int iz = 0; iz < zSize; iz ++) {
			int xstart_z = 0, xend_z = xSize;
			int z = iz;
			if (z > projector.maxR)
			{
				if (z >= zSize - projector.maxR)
					z = z - projector.imgZ;
				else {
					xstart_z = projector.maxR;
					xend_z   = xstart_z + 1;
				}
			}

			for(int iy = 0; iy < ySize; iy++) {
				int xstart_y = xstart_z, xend_y = xend_z;
				int y = iy;
				if (iy > projector.maxR) {
					if (iy >= ySize - projector.maxR)
						y = iy - ySize;
					else {
						xstart_y = projector.maxR;
						xend_y   = xstart_y + 1;
					}
				}

#if _OPENMP >= 201307	// For OpenMP 4.0 and later
				#pragma omp simd simdlen(SIMD_LEN)
#endif
#ifdef USE_INTEL_COMPILER
				#pragma forceinline
#endif
				for(int x = xstart_y; x < xend_y; x++) {
					projector.project3Dmodel(
						x, y, z, e0, e1, e2, e3, e4, e5, e6, e7, e8,
						ref_real[x], ref_imag[x]);

					img_real[x]  = g_imgs_real[pixel + x];
					img_imag[x]  = g_imgs_imag[pixel + x];
					corr_imag[x] = g_corr_img [pixel + x];	            
				}

				for (unsigned long itrans=0; itrans<trans_num; itrans++) // finish all translations in each partial pass
				{			
					XFLOAT trans_cos_z, trans_sin_z;
					if ( z < 0) {
						trans_cos_z =  cos_z[itrans][-z];
						trans_sin_z = -sin_z[itrans][-z];            
					}
					else {
						trans_cos_z = cos_z[itrans][z];
						trans_sin_z = sin_z[itrans][z];
					}			

					XFLOAT trans_cos_y, trans_sin_y;
					if ( y < 0) {
						trans_cos_y =  cos_y[itrans][-y];
						trans_sin_y = -sin_y[itrans][-y];            
					}
					else {
						trans_cos_y = cos_y[itrans][y];
						trans_sin_y = sin_y[itrans][y];
					}

					XFLOAT *trans_cos_x = &cos_x[itrans][0];
					XFLOAT *trans_sin_x = &sin_x[itrans][0];     

					for(int x = xstart_y; x < xend_y; x++) {			
						XFLOAT s1  = trans_sin_x[x] * trans_cos_y + trans_cos_x[x] * trans_sin_y;
						XFLOAT c1  = trans_cos_x[x] * trans_cos_y - trans_sin_x[x] * trans_sin_y;

						XFLOAT ss = s1 * trans_cos_z + c1 * trans_sin_z;
						XFLOAT cc = c1 * trans_cos_z - s1 * trans_sin_z;		

						XFLOAT shifted_real = cc * img_real[x] - ss * img_imag[x];
						XFLOAT shifted_imag = cc * img_imag[x] + ss * img_real[x];			

						s[itrans][x]    += (ref_real[x] * shifted_real + ref_imag[x] * shifted_imag) * corr_imag[x];
						s_cc[itrans][x] += (ref_real[x]*ref_real[x] + ref_imag[x]*ref_imag[x])       * corr_imag[x];
					}
				}
			}

			pixel += (unsigned long)xSize;
		} // loop y direction

		for(unsigned long itrans=0; itrans<trans_num; itrans++) {
			XFLOAT sum1 = (XFLOAT)0.0;
			XFLOAT sum2 = (XFLOAT)0.0;
			for(int x=0; x<xSize; x++){
				sum1 += s   [itrans][x];
				sum2 += s_cc[itrans][x]; 
			} 

			unsigned long int iy = d_job_idx[bid] + itrans;
	#ifdef RELION_SINGLE_PRECISION         
			g_diff2s[iy] += - sum1 / sqrtf(sum2);
	#else
			g_diff2s[iy] += - sum1 / sqrt(sum2);
	#endif

		}		
	} // for bid
}

} // end of namespace CpuKernels

#endif /* DIFF2_KERNELS_H_ */
