#pragma once
#include "Surface.h"
#include "ChiliMath.h"
#include <immintrin.h>
#include "FrameTimer.h"
#include <fstream>
#include <functional>
#include "Cpuid.h"
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cassert>
#include <algorithm>

#define BLOOM_PROCESSOR_USE_SSE false
#define BLOOM_PROCESSOR_USE_MT false

class BloomProcessor
{
public:
	BloomProcessor( Surface& input )
		:
		input( input ),
		hBuffer( input.GetWidth() / 4,input.GetHeight() / 4 ),
		vBuffer( input.GetWidth() / 4,input.GetHeight() / 4 )
	{
		float kernelFloat[diameter];
		for( int x = 0; x < diameter; x++ )
		{
			kernelFloat[x] = gaussian( std::fabs( float( x - int( GetKernelCenter() ) ) ),
				float( diameter / 6.0f ) );
		}
		for( int x = 0; x < diameter; x++ )
		{
			kernel[x] = unsigned char( 255 * ( kernelFloat[x]
				/ kernelFloat[GetKernelCenter()] ) );
		}
		hBuffer.Fill( Colors::Black );
		vBuffer.Fill( Colors::Black );

		// setup function pointers
		if( BLOOM_PROCESSOR_USE_SSE )
		{
			if( InstructionSet::SSSE3() )
			{
				SetSSSE3Mode();
			}
			else
			{
				SetSSE2Mode();
			}
		}
		else
		{
			SetX86Mode();
		}

		// initialize upsize workers with their job parameters
		InitUpsizeWorkers();
	}
	void DownsizePass()
	{
		DownsizePassFunc( this );
	}
	void HorizontalPass()
	{
		HorizontalPassFunc( this );
	}
	void VerticalPass()
	{
		VerticalPassFunc( this );
	}
	void UpsizeBlendPass()
	{
		UpsizeBlendPassFunc( this );
	}
	void SetSSE2Mode()
	{
		DownsizePassFunc = std::mem_fn( &BloomProcessor::_DownsizePassSSE2 );
		HorizontalPassFunc = std::mem_fn( &BloomProcessor::_HorizontalPassSSE2 );
		VerticalPassFunc = std::mem_fn( &BloomProcessor::_VerticalPassSSE2 );
		UpsizeBlendPassFunc = std::mem_fn( &BloomProcessor::_UpsizeBlendPassSSE2 );
	}
	void SetSSSE3Mode()
	{
		DownsizePassFunc = std::mem_fn( &BloomProcessor::_DownsizePassSSSE3 );
		HorizontalPassFunc = std::mem_fn( &BloomProcessor::_HorizontalPassSSSE3 );
		VerticalPassFunc = std::mem_fn( &BloomProcessor::_VerticalPassSSSE3 );
		if( BLOOM_PROCESSOR_USE_MT )
		{
			UpsizeBlendPassFunc = std::mem_fn( &BloomProcessor::_UpsizeBlendPassSSSE3MT );
		}
		else
		{
			UpsizeBlendPassFunc = std::mem_fn( &BloomProcessor::_UpsizeBlendPassSSSE3 );
		}
	}
	void SetX86Mode()
	{
		DownsizePassFunc = std::mem_fn( &BloomProcessor::_DownsizePassX86 );
		HorizontalPassFunc = std::mem_fn( &BloomProcessor::_HorizontalPassX86 );
		VerticalPassFunc = std::mem_fn( &BloomProcessor::_VerticalPassX86 );
		UpsizeBlendPassFunc = std::mem_fn( &BloomProcessor::_UpsizeBlendPassX86 );
	}
	void Go()
	{
		DownsizePass();
		HorizontalPass();
		VerticalPass();
		UpsizeBlendPass();
	}
	static unsigned int GetFringeSize()
	{
		return ( diameter / 2u ) * 4u;
	}
private:
	class UpsizeWorker
	{
	public:
		enum Type
		{
			Top,
			Bottom,
			Middle
		};
	public:
		UpsizeWorker( const __m128i* pInJ,size_t inPitchJ,size_t inWidthJ,size_t nMiddleLines,
			__m128i* pOutJ,size_t outPitchJ,Type type,BloomProcessor& boss )
		{
			std::unique_lock<std::mutex> ctorLock( mutexWorker );

			// @@NOTE@@ one does not *simply* capture all with = or &
			threadWorker = std::thread(
				[this,pInJ,inPitchJ,inWidthJ,nMiddleLines,pOutJ,outPitchJ,type,&boss]()
			{
				std::unique_lock<std::mutex> lockWorker( mutexWorker );

				const __m128i zero = _mm_setzero_si128();
				__m128i grad_coef = _mm_set_epi16( 160u,160u,160u,160u,224u,224u,224u,224u );

				// interpolate horizontally between low 2 pixels of input
				const auto GenerateGradient = [&]( __m128i in )
				{
					// unpack inputs (low 2 pixels) to 16-bit channel size
					const __m128i in16 = _mm_unpacklo_epi8( in,zero );

					// copy low pixel to high and low 64 bits
					const __m128i in_a = _mm_shuffle_epi32( in16,_MM_SHUFFLE( 1,0,1,0 ) );
					// multiply input by decreasing coeffients (lower pixels)
					const __m128i prod_a_lo = _mm_mullo_epi16( in_a,grad_coef );
					// transform decreasing coef to lower range (for high pixels)
					grad_coef = _mm_sub_epi16( grad_coef,_mm_set128_epi16( grad_coef ) );
					// multiply input by decreasing coeffients (higher pixels)
					const __m128i prod_a_hi = _mm_mullo_epi16( in_a,grad_coef );

					// copy high pixel to high and low 64 bits
					const __m128i in_b = _mm_shuffle_epi32( in16,_MM_SHUFFLE( 3,2,3,2 ) );
					// transform decreasing coef to increasing coefficients (for low pixels)
					grad_coef = _mm_shuffle_epi32( grad_coef,_MM_SHUFFLE( 0,1,3,2 ) );
					// multiply input by increasing coeffients (lower pixels)
					const __m128i prod_b_lo = _mm_mullo_epi16( in_b,grad_coef );
					// transform increasing coef to higher range (for high pixels)
					grad_coef = _mm_add_epi16( grad_coef,_mm_set128_epi16( grad_coef ) );
					// multiply input by increasing coeffients (higher pixels)
					const __m128i prod_b_hi = _mm_mullo_epi16( in_b,grad_coef );

					// return coefficients to original state
					grad_coef = _mm_shuffle_epi32( grad_coef,_MM_SHUFFLE( 0,1,3,2 ) );

					// add low products and divide
					const __m128i ab_lo = _mm_srli_epi16( _mm_adds_epu16( prod_a_lo,prod_b_lo ),8 );
					// add high products and divide
					const __m128i ab_hi = _mm_srli_epi16( _mm_adds_epu16( prod_a_hi,prod_b_hi ),8 );

					// pack and return result
					return _mm_packus_epi16( ab_lo,ab_hi );
				};

				// upsize for top and bottom edge cases
				const auto UpsizeEdge = [&]( const __m128i* pIn,const __m128i* pInEnd,__m128i* pOutTop,
					__m128i* pOutBottom )
				{
					__m128i in = _mm_load_si128( pIn++ );
					// left corner setup (prime the alignment pump)
					__m128i oldPix = _mm_shuffle_epi32( in,_MM_SHUFFLE( 0,0,0,0 ) );

					// main loop
					while( true )
					{
						// gradient 0-1
						__m128i newPix = GenerateGradient( in );
						__m128i out = _mm_alignr_epi8( newPix,oldPix,8 );
						*pOutTop = _mm_adds_epu8( *pOutTop,out );
						*pOutBottom = _mm_adds_epu8( *pOutBottom,out );
						pOutTop++;
						pOutBottom++;
						oldPix = newPix;

						// gradient 1-2
						newPix = GenerateGradient( _mm_srli_si128( in,4 ) );
						out = _mm_alignr_epi8( newPix,oldPix,8 );
						*pOutTop = _mm_adds_epu8( *pOutTop,out );
						*pOutBottom = _mm_adds_epu8( *pOutBottom,out );
						pOutTop++;
						pOutBottom++;
						oldPix = newPix;

						// gradient 2-3
						newPix = GenerateGradient( _mm_srli_si128( in,8 ) );
						out = _mm_alignr_epi8( newPix,oldPix,8 );
						*pOutTop = _mm_adds_epu8( *pOutTop,out );
						*pOutBottom = _mm_adds_epu8( *pOutBottom,out );
						pOutTop++;
						pOutBottom++;
						oldPix = newPix;

						// end condition
						if( pIn >= pInEnd )
						{
							break;
						}

						// gradient 3-0'
						const __m128i newIn = _mm_load_si128( pIn++ );
						newPix = GenerateGradient( _mm_alignr_epi8( newIn,in,12 ) );
						out = _mm_alignr_epi8( newPix,oldPix,8 );
						*pOutTop = _mm_adds_epu8( *pOutTop,out );
						*pOutBottom = _mm_adds_epu8( *pOutBottom,out );
						pOutTop++;
						pOutBottom++;
						oldPix = newPix;
						in = newIn;
					}

					// right corner
					const __m128i out = _mm_alignr_epi8( _mm_shuffle_epi32( in,_MM_SHUFFLE( 3,3,3,3 ) ),oldPix,8 );
					*pOutTop = _mm_adds_epu8( *pOutTop,out );
					*pOutBottom = _mm_adds_epu8( *pOutBottom,out );
				};

				// hold values from last iteration
				__m128i old0;
				__m128i old1;
				__m128i old2;
				__m128i old3;

				// interpolate horizontally between first 2 pixels of inputs and then vertically
				const auto VerticalGradientOutput = [&]( __m128i in0,__m128i in1,
					__m128i* pOut0,__m128i* pOut1,__m128i* pOut2,__m128i* pOut3 )
				{
					const __m128i topGrad = GenerateGradient( in0 );
					const __m128i bottomGrad = GenerateGradient( in1 );

					// generate points between top and bottom pixel arrays
					const __m128i half = _mm_avg_epu8( topGrad,bottomGrad );

					{
						// first quarter needed for top half
						const __m128i firstQuarter = _mm_avg_epu8( topGrad,half );

						// generate 1/8 pt from top to bottom
						const __m128i firstEighth = _mm_avg_epu8( topGrad,firstQuarter );
						// combine old 1/8 pt and new and add to original image with saturation
						*pOut0 = _mm_adds_epu8( *pOut0,_mm_alignr_epi8( firstEighth,old0,8 ) );
						old0 = firstEighth;

						// generate 3/8 pt from top to bottom
						const __m128i thirdEighth = _mm_avg_epu8( firstQuarter,half );
						// combine old 3/8 pt and new and add to original image with saturation
						*pOut1 = _mm_adds_epu8( *pOut1,_mm_alignr_epi8( thirdEighth,old1,8 ) );
						old1 = thirdEighth;
					}

						{
							// third quarter needed for bottom half
							const __m128i thirdQuarter = _mm_avg_epu8( half,bottomGrad );

							// generate 5/8 pt from top to bottom
							const __m128i fifthEighth = _mm_avg_epu8( half,thirdQuarter );
							// combine old 5/8 pt and new and add to original image with saturation
							*pOut2 = _mm_adds_epu8( *pOut2,_mm_alignr_epi8( fifthEighth,old2,8 ) );
							old2 = fifthEighth;

							// generate 7/8 pt from top to bottom
							const __m128i seventhEighth = _mm_avg_epu8( thirdQuarter,bottomGrad );
							// combine old 7/8 pt and new and add to original image with saturation
							*pOut3 = _mm_adds_epu8( *pOut3,_mm_alignr_epi8( seventhEighth,old3,8 ) );
							old3 = seventhEighth;
						}
				};

				// upsize for middle cases
				const auto DoLine = [&]( const __m128i* pIn0,const __m128i* pIn1,const __m128i* const pEnd,
					__m128i* pOut0,__m128i* pOut1,__m128i* pOut2,__m128i* pOut3 )
				{
					__m128i in0 = _mm_load_si128( pIn0++ );
					__m128i in1 = _mm_load_si128( pIn1++ );

					// left side prime pump
					{
						// left edge clamps to left most pixel
						const __m128i top = _mm_shuffle_epi32( in0,_MM_SHUFFLE( 0,0,0,0 ) );
						const __m128i bottom = _mm_shuffle_epi32( in1,_MM_SHUFFLE( 0,0,0,0 ) );

						// generate points between top and bottom pixel arrays
						const __m128i half = _mm_avg_epu8( top,bottom );

						{
							// first quarter needed for top half
							const __m128i firstQuarter = _mm_avg_epu8( top,half );

							// generate 1/8 pt from top to bottom
							old0 = _mm_avg_epu8( top,firstQuarter );

							// generate 3/8 pt from top to bottom
							old1 = _mm_avg_epu8( firstQuarter,half );
						}

							{
								// third quarter needed for bottom half
								const __m128i thirdQuarter = _mm_avg_epu8( half,bottom );

								// generate 5/8 pt from top to bottom
								old2 = _mm_avg_epu8( half,thirdQuarter );

								// generate 7/8 pt from top to bottom
								old3 = _mm_avg_epu8( thirdQuarter,bottom );
							}
					}

					// main loop
					while( true )
					{
						// gradient 0-1
						VerticalGradientOutput( in0,in1,pOut0++,pOut1++,pOut2++,pOut3++ );

						// gradient 1-2
						VerticalGradientOutput(
							_mm_srli_si128( in0,4 ),
							_mm_srli_si128( in1,4 ),
							pOut0++,pOut1++,pOut2++,pOut3++ );

						// gradient 2-3
						VerticalGradientOutput(
							_mm_srli_si128( in0,8 ),
							_mm_srli_si128( in1,8 ),
							pOut0++,pOut1++,pOut2++,pOut3++ );

						// end condition
						if( pIn0 >= pEnd )
						{
							break;
						}

						// gradient 3-0'
						const __m128i newIn0 = _mm_load_si128( pIn0++ );
						const __m128i newIn1 = _mm_load_si128( pIn1++ );
						VerticalGradientOutput(
							_mm_alignr_epi8( newIn0,in0,12 ),
							_mm_alignr_epi8( newIn1,in1,12 ),
							pOut0++,pOut1++,pOut2++,pOut3++ );
						in0 = newIn0;
						in1 = newIn1;
					}

					// right side finish pump
						{
							// right edge clamps to right most pixel
							const __m128i top = _mm_shuffle_epi32( in0,_MM_SHUFFLE( 3,3,3,3 ) );
							const __m128i bottom = _mm_shuffle_epi32( in1,_MM_SHUFFLE( 3,3,3,3 ) );

							// generate points between top and bottom pixel arrays
							const __m128i half = _mm_avg_epu8( top,bottom );

							{
								// first quarter needed for top half
								const __m128i firstQuarter = _mm_avg_epu8( top,half );

								// generate 1/8 pt from top to bottom
								*pOut0 = _mm_adds_epu8( *pOut0,_mm_alignr_epi8(
									_mm_avg_epu8( top,firstQuarter ),old0,8 ) );

								// generate 3/8 pt from top to bottom
								*pOut1 = _mm_adds_epu8( *pOut1,_mm_alignr_epi8(
									_mm_avg_epu8( firstQuarter,half ),old1,8 ) );
							}
							{
								// third quarter needed for bottom half
								const __m128i thirdQuarter = _mm_avg_epu8( half,bottom );

								// generate 5/8 pt from top to bottom
								*pOut2 = _mm_adds_epu8( *pOut2,_mm_alignr_epi8(
									_mm_avg_epu8( half,thirdQuarter ),old2,8 ) );

								// generate 7/8 pt from top to bottom
								*pOut3 = _mm_adds_epu8( *pOut3,_mm_alignr_epi8(
									_mm_avg_epu8( thirdQuarter,bottom ),old3,8 ) );
							}
						}
				};

				// parameters for processing lambdas
				// edge params
				const __m128i* pInEdge;
				const __m128i* pInEndEdge;
				__m128i* pOutTopEdge;
				__m128i* pOutBottomEdge;
				// middle line params
				const __m128i* pIn0Start;
				const __m128i* pIn1Start;
				const __m128i* pEndStart;
				const __m128i* pMiddleEndLine;
				__m128i* pOut0Start;
				__m128i* pOut1Start;
				__m128i* pOut2Start;
				__m128i* pOut3Start;
				size_t outLineIterate = outPitchJ * 4u;

				// initialize parameters for processing lambdas
				switch( type )
				{
				case Top:
					// edge params
					pInEdge = pInJ;
					pInEndEdge = pInJ + inWidthJ;
					pOutTopEdge = pOutJ;
					pOutBottomEdge = pOutJ + outPitchJ;
					// middle params
					pIn0Start = pInJ;
					pIn1Start = pInJ + inPitchJ;
					pEndStart = pInJ + inWidthJ;
					pMiddleEndLine = pInJ + inPitchJ * nMiddleLines;
					pOut0Start = pOutJ + outPitchJ * 2u;
					pOut1Start = pOutJ + outPitchJ * 3u;
					pOut2Start = pOutJ + outPitchJ * 4u;
					pOut3Start = pOutJ + outPitchJ * 5u;
					break;
				case Bottom:
					// middle params
					pIn0Start = pInJ;
					pIn1Start = pInJ + inPitchJ;
					pEndStart = pInJ + inWidthJ;
					pMiddleEndLine = pInJ + inPitchJ * nMiddleLines;
					pOut0Start = pOutJ;
					pOut1Start = pOutJ + outPitchJ;
					pOut2Start = pOutJ + outPitchJ * 2u;
					pOut3Start = pOutJ + outPitchJ * 3u;
					// edge params
					pInEdge = pInJ + inPitchJ * nMiddleLines;
					pInEndEdge = pInJ + inPitchJ * nMiddleLines + inWidthJ;
					pOutTopEdge = pOutJ + outPitchJ * nMiddleLines * 4u;
					pOutBottomEdge = pOutJ + outPitchJ * ( nMiddleLines * 4u + 1u );
					break;
				case Middle:
					// middle params only
					pIn0Start = pInJ;
					pIn1Start = pInJ + inPitchJ;
					pEndStart = pInJ + inWidthJ;
					pMiddleEndLine = pInJ + inPitchJ * nMiddleLines;
					pOut0Start = pOutJ;
					pOut1Start = pOutJ + outPitchJ;
					pOut2Start = pOutJ + outPitchJ * 2u;
					pOut3Start = pOutJ + outPitchJ * 3u;
					break;
				}

				// notify ctor-calling thread so that it gets ready to wake
				// (can't wake right away because worker thread still holds mutex
				cvWorker.notify_all();

				// thread work loop
				while( true )
				{
					// initialize the line pointers from precalculated starting positions
					const __m128i* pIn0 = pIn0Start;
					const __m128i* pIn1 = pIn1Start;
					const __m128i* pEnd = pEndStart;
					__m128i* pOut0 = pOut0Start;
					__m128i* pOut1 = pOut1Start;
					__m128i* pOut2 = pOut2Start;
					__m128i* pOut3 = pOut3Start;

					// reset started flag (also signals init finished during initialization)
					started = false;

					// wait for command (also frees main thread during init)
					cvWorker.wait( lockWorker,[this](){ return started || dying; } );

					// end thread if dying flag set
					if( dying )
					{
						break;
					}

					// we must be started then...
					// process the bloom filter stage
					if( type == Top )
					{
						UpsizeEdge( pInEdge,pInEndEdge,pOutTopEdge,pOutBottomEdge );
					}
					while( pIn0 < pMiddleEndLine )
					{
						// process line
						DoLine( pIn0,pIn1,pEnd,pOut0,pOut1,pOut2,pOut3 );
						// increment pointers to next line
						pIn0 += inPitchJ;
						pIn1 += inPitchJ;
						pEnd += inPitchJ;
						pOut0 += outLineIterate;
						pOut1 += outLineIterate;
						pOut2 += outLineIterate;
						pOut3 += outLineIterate;
					}
					if( type == Bottom )
					{
						UpsizeEdge( pInEdge,pInEndEdge,pOutTopEdge,pOutBottomEdge );
					}

					// notify boss of being finished
					{
						std::lock_guard<std::mutex> lock( boss.mutexBoss );
						boss.nActiveThreads--;
					}
					boss.cvBoss.notify_all();
				}
			} );

			// during worker initialization, started==true means not done initializing
			cvWorker.wait( ctorLock,[this](){ return !started; } );
		}
		UpsizeWorker( const UpsizeWorker& ) = delete;
		UpsizeWorker( UpsizeWorker&& ) = delete;
		UpsizeWorker& operator=( const UpsizeWorker& ) = delete;
		void Start()
		{
			{
				std::lock_guard<std::mutex> lock( mutexWorker );
				started = true;
			}
			cvWorker.notify_all();
		}
		~UpsizeWorker()
		{
			{
				std::lock_guard<std::mutex> lock( mutexWorker );
				dying = true;
			}
			cvWorker.notify_all();
			threadWorker.join();
		}
	private:
		std::thread threadWorker;
		std::condition_variable cvWorker;
		std::mutex mutexWorker;
		// doubles as signal that initialization has finished (finished when false)
		bool started = true;
		bool dying = false;
	};
private:
	void InitUpsizeWorkers()
	{	
		// constants for line loop pointer arithmetic
		const size_t inWidthScalar = hBuffer.GetWidth();
		const size_t outWidthScalar = input.GetWidth();
		const size_t inFringe = diameter / 2u;
		const size_t outFringe = GetFringeSize();

		// calculate parameters and create workers
		// 48 4-row lines per worker except for the last worker (it get 47)
		// first and last workers handle the top and bottom two rows
		workerPtrs.push_back( std::make_unique<UpsizeWorker>(
			reinterpret_cast<const __m128i*>(
				&hBuffer.Data()[inWidthScalar * inFringe + inFringe] ),
			inWidthScalar / 4u,
			inWidthScalar / 4u - inFringe / 2u,
			48u,
			reinterpret_cast<__m128i*>(
				&input.Data()[outWidthScalar * outFringe + outFringe] ),
			outWidthScalar / 4u,
			UpsizeWorker::Top,
			*this ) );

		workerPtrs.push_back( std::make_unique<UpsizeWorker>(
			reinterpret_cast<const __m128i*>(
				&hBuffer.Data()[inWidthScalar * ( inFringe + 48u ) + inFringe] ),
			inWidthScalar / 4u,
			inWidthScalar / 4u - inFringe / 2u,
			48u,
			reinterpret_cast<__m128i*>(
				&input.Data()[outWidthScalar * ( outFringe + 48u * 4u + 2u ) + outFringe] ),
			outWidthScalar / 4u,
			UpsizeWorker::Middle,
			*this ) );

		workerPtrs.push_back( std::make_unique<UpsizeWorker>(
			reinterpret_cast<const __m128i*>(
				&hBuffer.Data()[inWidthScalar * ( inFringe + 96u ) + inFringe] ),
			inWidthScalar / 4u,
			inWidthScalar / 4u - inFringe / 2u,
			48u,
			reinterpret_cast<__m128i*>(
				&input.Data()[outWidthScalar * ( outFringe + 96u * 4u + 2u ) + outFringe] ),
			outWidthScalar / 4u,
			UpsizeWorker::Middle,
			*this ) );

		workerPtrs.push_back( std::make_unique<UpsizeWorker>(
			reinterpret_cast<const __m128i*>(
				&hBuffer.Data()[inWidthScalar * ( inFringe + 144u ) + inFringe] ),
			inWidthScalar / 4u,
			inWidthScalar / 4u - inFringe / 2u,
			47u,
			reinterpret_cast<__m128i*>(
				&input.Data()[outWidthScalar * ( outFringe + 144u * 4u + 2u ) + outFringe] ),
			outWidthScalar / 4u,
			UpsizeWorker::Bottom,
			*this ) );
	}
	template<int shift>
	static __m128i AlignRightSSE2( __m128i hi,__m128i lo )
	{
		const __m128i top = _mm_slli_si128( hi,16 - shift );
		const __m128i bot = _mm_srli_si128( lo,shift );
		return _mm_or_si128( top,bot );
	}
	static __m128i _mm_set128_epi16( const __m128i dummy )
	{
		__m128i x = _mm_cmpeq_epi16( dummy,dummy );
		x = _mm_srli_epi16( x,15 );
		return _mm_slli_epi16( x,7 );
	}
	static unsigned int GetKernelCenter()
	{
		return ( diameter - 1 ) / 2;
	}
	void _DownsizePassSSSE3()
	{
		// surface height needs to be a multiple of 4
		assert( input.GetHeight() % 4u == 0u );

		// useful constants
		const __m128i zero = _mm_setzero_si128();
		const __m128i bloomShufLo = _mm_set_epi8(
			128u,128u,128u,7u,128u,7u,128u,7u,
			128u,128u,128u,3u,128u,3u,128u,3u );
		const __m128i bloomShufHi = _mm_set_epi8(
			128u,128u,128u,15u,128u,15u,128u,15u,
			128u,128u,128u,11u,128u,11u,128u,11u );

		// subroutine
		const auto ProcessRow = [=]( __m128i row )
		{
			// unpack byte channels of 2 upper and 2 lower pixels to words
			const __m128i chanLo = _mm_unpacklo_epi8( row,zero );
			const __m128i chanHi = _mm_unpackhi_epi8( row,zero );

			// broadcast bloom value to all channels in the same pixel
			const __m128i bloomLo = _mm_shuffle_epi8( row,bloomShufLo );
			const __m128i bloomHi = _mm_shuffle_epi8( row,bloomShufHi );

			// multiply bloom with color channels
			const __m128i prodLo = _mm_mullo_epi16( chanLo,bloomLo );
			const __m128i prodHi = _mm_mullo_epi16( chanHi,bloomHi );

			// predivide channels by 16
			const __m128i predivLo = _mm_srli_epi16( prodLo,4u );
			const __m128i predivHi = _mm_srli_epi16( prodHi,4u );

			// add upper and lower 2-pixel groups and return result
			return _mm_add_epi16( predivLo,predivHi );
		};

		for( size_t yIn = 0u,yOut = 0u; yIn < size_t( input.GetHeight() ); yIn += 4u,yOut++ )
		{
			// initialize input pointers
			const __m128i* pRow0 = reinterpret_cast<const __m128i*>(
				&input.Data()[input.GetPitch() * yIn] );
			const __m128i* pRow1 = reinterpret_cast<const __m128i*>(
				&input.Data()[input.GetPitch() * ( yIn + 1u )] );
			const __m128i* pRow2 = reinterpret_cast<const __m128i*>(
				&input.Data()[input.GetPitch() * ( yIn + 2u )] );
			const __m128i* pRow3 = reinterpret_cast<const __m128i*>(
				&input.Data()[input.GetPitch() * ( yIn + 3u )] );
			// initialize output pointer
			Color* pOut = &hBuffer.Data()[hBuffer.GetPitch() * yOut];
			// row end pointer
			const __m128i* const pRowEnd = pRow1;

			for( ; pRow0 < pRowEnd; pRow0++,pRow1++,pRow2++,pRow3++,pOut++ )
			{
				// load pixels
				const __m128i row0 = _mm_load_si128( pRow0 );
				const __m128i row1 = _mm_load_si128( pRow1 );
				const __m128i row2 = _mm_load_si128( pRow2 );
				const __m128i row3 = _mm_load_si128( pRow3 );

				// process rows and sum results
				__m128i sum = ProcessRow( row0 );
				sum = _mm_add_epi16( sum,ProcessRow( row1 ) );
				sum = _mm_add_epi16( sum,ProcessRow( row2 ) );
				sum = _mm_add_epi16( sum,ProcessRow( row3 ) );

				// add high and low pixel channel sums
				sum = _mm_add_epi16( sum,_mm_srli_si128( sum,8u ) );

				// divide channel sums by 256
				sum = _mm_srli_epi16( sum,8u );

				// pack word channels to bytes and store in output buffer
				*pOut = _mm_cvtsi128_si32( _mm_packus_epi16( sum,sum ) );
			}
		}
	}
	void _DownsizePassSSE2()
	{		// surface height needs to be a multiple of 4
		assert( input.GetHeight() % 4u == 0u );

		// useful constants
		const __m128i zero = _mm_setzero_si128();

		// subroutine
		const auto ProcessRow = [&zero]( __m128i row )
		{
			// unpack byte channels of 2 upper and 2 lower pixels to words
			const __m128i chanLo = _mm_unpacklo_epi8( row,zero );
			const __m128i chanHi = _mm_unpackhi_epi8( row,zero );

			// broadcast bloom value to all channels in the same pixel
			const __m128i bloomLo = _mm_shufflehi_epi16( _mm_shufflelo_epi16(
				chanLo,_MM_SHUFFLE( 3u,3u,3u,3u ) ),_MM_SHUFFLE( 3u,3u,3u,3u ) );
			const __m128i bloomHi = _mm_shufflehi_epi16( _mm_shufflelo_epi16(
				chanHi,_MM_SHUFFLE( 3u,3u,3u,3u ) ),_MM_SHUFFLE( 3u,3u,3u,3u ) );

			// multiply bloom with color channels
			const __m128i prodLo = _mm_mullo_epi16( chanLo,bloomLo );
			const __m128i prodHi = _mm_mullo_epi16( chanHi,bloomHi );

			// predivide channels by 16
			const __m128i predivLo = _mm_srli_epi16( prodLo,4u );
			const __m128i predivHi = _mm_srli_epi16( prodHi,4u );

			// add upper and lower 2-pixel groups and return result
			return _mm_add_epi16( predivLo,predivHi );
		};

		for( size_t yIn = 0u,yOut = 0u; yIn < size_t( input.GetHeight() ); yIn += 4u,yOut++ )
		{
			// initialize input pointers
			const __m128i* pRow0 = reinterpret_cast<const __m128i*>(
				&input.Data()[input.GetPitch() * yIn] );
			const __m128i* pRow1 = reinterpret_cast<const __m128i*>(
				&input.Data()[input.GetPitch() * ( yIn + 1u )] );
			const __m128i* pRow2 = reinterpret_cast<const __m128i*>(
				&input.Data()[input.GetPitch() * ( yIn + 2u )] );
			const __m128i* pRow3 = reinterpret_cast<const __m128i*>(
				&input.Data()[input.GetPitch() * ( yIn + 3u )] );
			// initialize output pointer
			Color* pOut = &hBuffer.Data()[hBuffer.GetPitch() * yOut];
			// row end pointer
			const __m128i* const pRowEnd = pRow1;

			for( ; pRow0 < pRowEnd; pRow0++,pRow1++,pRow2++,pRow3++,pOut++ )
			{
				// load pixels
				const __m128i row0 = _mm_load_si128( pRow0 );
				const __m128i row1 = _mm_load_si128( pRow1 );
				const __m128i row2 = _mm_load_si128( pRow2 );
				const __m128i row3 = _mm_load_si128( pRow3 );

				// process rows and sum results
				__m128i sum = ProcessRow( row0 );
				sum = _mm_add_epi16( sum,ProcessRow( row1 ) );
				sum = _mm_add_epi16( sum,ProcessRow( row2 ) );
				sum = _mm_add_epi16( sum,ProcessRow( row3 ) );

				// add high and low pixel channel sums
				sum = _mm_add_epi16( sum,_mm_srli_si128( sum,8u ) );

				// divide channel sums by 256
				sum = _mm_srli_epi16( sum,8u );

				// pack word channels to bytes and store in output buffer
				*pOut = _mm_cvtsi128_si32( _mm_packus_epi16( sum,sum ) );
			}
		}
	}
	void _DownsizePassX86()
	{
		const Color* const pInputBuffer = input.Data();
		Color* const pOutputBuffer = hBuffer.Data();
		const size_t inWidth = input.GetWidth();
		const size_t inHeight = input.GetHeight();
		const size_t outWidth = hBuffer.GetWidth();
		const size_t outHeight = hBuffer.GetHeight();

		for( size_t y = 0; y < outHeight; y++ )
		{
			for( size_t x = 0; x < outWidth; x++ )
			{
				const Color p0 = pInputBuffer[( y * 4 )     * inWidth + x * 4];
				const Color p1 = pInputBuffer[( y * 4 )     * inWidth + x * 4 + 1];
				const Color p2 = pInputBuffer[( y * 4 )     * inWidth + x * 4 + 2];
				const Color p3 = pInputBuffer[( y * 4 )     * inWidth + x * 4 + 3];
				const Color p4 = pInputBuffer[( y * 4 + 1 ) * inWidth + x * 4];
				const Color p5 = pInputBuffer[( y * 4 + 1 ) * inWidth + x * 4 + 1];
				const Color p6 = pInputBuffer[( y * 4 + 1 ) * inWidth + x * 4 + 2];
				const Color p7 = pInputBuffer[( y * 4 + 1 ) * inWidth + x * 4 + 3];
				const Color p8 = pInputBuffer[( y * 4 + 2 ) * inWidth + x * 4];
				const Color p9 = pInputBuffer[( y * 4 + 2 ) * inWidth + x * 4 + 1];
				const Color p10 = pInputBuffer[( y * 4 + 2 ) * inWidth + x * 4 + 2];
				const Color p11 = pInputBuffer[( y * 4 + 2 ) * inWidth + x * 4 + 3];
				const Color p12 = pInputBuffer[( y * 4 + 3 ) * inWidth + x * 4];
				const Color p13 = pInputBuffer[( y * 4 + 3 ) * inWidth + x * 4 + 1];
				const Color p14 = pInputBuffer[( y * 4 + 3 ) * inWidth + x * 4 + 2];
				const Color p15 = pInputBuffer[( y * 4 + 3 ) * inWidth + x * 4 + 3];
				const unsigned int x0 = p0.GetX();
				const unsigned int x1 = p1.GetX();
				const unsigned int x2 = p2.GetX();
				const unsigned int x3 = p3.GetX();
				const unsigned int x4 = p0.GetX();
				const unsigned int x5 = p5.GetX();
				const unsigned int x6 = p6.GetX();
				const unsigned int x7 = p7.GetX();
				const unsigned int x8 = p8.GetX();
				const unsigned int x9 = p9.GetX();
				const unsigned int x10 = p10.GetX();
				const unsigned int x11 = p11.GetX();
				const unsigned int x12 = p12.GetX();
				const unsigned int x13 = p13.GetX();
				const unsigned int x14 = p14.GetX();
				const unsigned int x15 = p15.GetX();
				pOutputBuffer[y * outWidth + x] =
				{ unsigned char( ( p0.GetR() * x0 + p1.GetR() * x1 + p2.GetR() * x2 + p3.GetR() * x3 + p4.GetR() * x4 + p5.GetR() * x5 + p6.GetR() * x6 + p7.GetR() * x7 + p8.GetR() * x8 + p9.GetR() * x9 + p10.GetR() * x10 + p11.GetR() * x11 + p12.GetR() * x12 + p13.GetR() * x13 + p14.GetR() * x14 + p15.GetR() * x15 ) / ( 16 * 255 ) ),
				unsigned char( ( p0.GetG() * x0 + p1.GetG() * x1 + p2.GetG() * x2 + p3.GetG() * x3 + p4.GetG() * x4 + p5.GetG() * x5 + p6.GetG() * x6 + p7.GetG() * x7 + p8.GetG() * x8 + p9.GetG() * x9 + p10.GetG() * x10 + p11.GetG() * x11 + p12.GetG() * x12 + p13.GetG() * x13 + p14.GetG() * x14 + p15.GetG() * x15 ) / ( 16 * 255 ) ),
				unsigned char( ( p0.GetB() * x0 + p1.GetB() * x1 + p2.GetB() * x2 + p3.GetB() * x3 + p4.GetB() * x4 + p5.GetB() * x5 + p6.GetB() * x6 + p7.GetB() * x7 + p8.GetB() * x8 + p9.GetB() * x9 + p10.GetB() * x10 + p11.GetB() * x11 + p12.GetB() * x12 + p13.GetB() * x13 + p14.GetB() * x14 + p15.GetB() * x15 ) / ( 16 * 255 ) ) };
			}
		}
	}
	void _HorizontalPassSSSE3()
	{
		// useful constants
		const __m128i zero = _mm_setzero_si128();
		

		// routines
		auto Process8Pixels = [zero]( const __m128i srclo,const __m128i srchi,const __m128i coef )
		{
			// for accumulating sum of high and low pixels in srclo and srchi
			__m128i sum;

			// process low pixels of srclo
			{
				// unpack two pixel byte->word components into src from lo end of srclo
				const __m128i src = _mm_unpacklo_epi8( srclo,zero );

				// broadcast coefficients 1,0 to top and bottom 4 words
				// (first duplicate WORD coeffients in low DWORDS, then shuffle by DWORDS)
				const __m128i co = _mm_shuffle_epi32( _mm_shufflelo_epi16(
					coef,_MM_SHUFFLE( 1,1,0,0 ) ),_MM_SHUFFLE( 1,1,0,0 ) );

				// multiply pixel components by coefficients
				const __m128i prod = _mm_mullo_epi16( co,src );

				// predivide by 16 and accumulate
				sum = _mm_srli_epi16( prod,4 );
			}
			// process high pixels of srclo
			{
				// unpack two pixel byte->word components into src from lo end of srclo
				const __m128i src = _mm_unpackhi_epi8( srclo,zero );

				// broadcast coefficients 1,0 to top and bottom 4 words
				// (first duplicate WORD coeffients in low DWORDS, then shuffle by DWORDS)
				const __m128i co = _mm_shuffle_epi32( _mm_shufflelo_epi16(
					coef,_MM_SHUFFLE( 3,3,2,2 ) ),_MM_SHUFFLE( 1,1,0,0 ) );

				// multiply pixel components by coefficients
				const __m128i prod = _mm_mullo_epi16( co,src );

				// predivide by 16 and accumulate
				const __m128i prediv = _mm_srli_epi16( prod,4 );
				sum = _mm_add_epi16( sum,prediv );
			}
			// process low pixels of srchi
			{
				// unpack two pixel byte->word components into src from lo end of srchi
				const __m128i src = _mm_unpacklo_epi8( srchi,zero );

				// broadcast coefficients 1,0 to top and bottom 4 words
				// (first duplicate WORD coeffients in low DWORDS, then shuffle by DWORDS)
				const __m128i co = _mm_shuffle_epi32( _mm_shufflehi_epi16(
					coef,_MM_SHUFFLE( 1,1,0,0 ) ),_MM_SHUFFLE( 3,3,2,2 ) );

				// multiply pixel components by coefficients
				const __m128i prod = _mm_mullo_epi16( co,src );

				// predivide by 16 and accumulate
				const __m128i prediv = _mm_srli_epi16( prod,4 );
				sum = _mm_add_epi16( sum,prediv );
			}
			// process high pixels of srchi
			{
				// unpack two pixel byte->word components into src from lo end of srchi
				const __m128i src = _mm_unpackhi_epi8( srchi,zero );

				// broadcast coefficients 1,0 to top and bottom 4 words
				// (first duplicate WORD coeffients in low DWORDS, then shuffle by DWORDS)
				const __m128i co = _mm_shuffle_epi32( _mm_shufflehi_epi16(
					coef,_MM_SHUFFLE( 3,3,2,2 ) ),_MM_SHUFFLE( 3,3,2,2 ) );

				// multiply pixel components by coefficients
				const __m128i prod = _mm_mullo_epi16( co,src );

				// predivide by 16 and accumulate
				const __m128i prediv = _mm_srli_epi16( prod,4 );
				sum = _mm_add_epi16( sum,prediv );
			}
			return sum;
		};

		// indexing constants
		const size_t centerKernel = GetKernelCenter();
		const size_t width = hBuffer.GetWidth();
		const size_t height = hBuffer.GetHeight();

		// load coefficent bytes and unpack to words
		const __m128i coef = _mm_load_si128( ( __m128i* )kernel );
		const __m128i coefLo = _mm_unpacklo_epi8( coef,zero );
		const __m128i coefHi = _mm_unpackhi_epi8( coef,zero );

		for( size_t y = 0u; y < height; y++ )
		{
			// setup pointers
			const __m128i* pIn = reinterpret_cast<const __m128i*>(
				&hBuffer.Data()[y * hBuffer.GetPitch()] );
			const __m128i* const pEnd = reinterpret_cast<const __m128i*>(
				&hBuffer.Data()[( y + 1 ) * hBuffer.GetPitch()] );
			Color* pOut = &vBuffer.Data()[y * vBuffer.GetPitch() + GetKernelCenter()];

			// preload input pixels for convolution window
			__m128i src0 = _mm_load_si128( pIn );
			pIn++;
			__m128i src1 = _mm_load_si128( pIn );
			pIn++;
			__m128i src2 = _mm_load_si128( pIn );
			pIn++;
			__m128i src3 = _mm_load_si128( pIn );
			pIn++;

			for( ; pIn < pEnd; pIn++ )
			{
				// on-deck pixels for shifting into convolution window
				__m128i deck = _mm_load_si128( pIn );

				for( size_t i = 0u; i < 4u; i++,pOut++ )
				{
					// process convolution window and accumulate
					__m128i sum16 = Process8Pixels( src0,src1,coefLo );
					sum16 = _mm_add_epi16( sum16,Process8Pixels( src2,src3,coefHi ) );

					// add low and high accumulators
					sum16 = _mm_add_epi16( sum16,_mm_srli_si128( sum16,8 ) );

					// divide by 32 (16 x 32 = 512 in total / 4x overdrive factor)
					sum16 = _mm_srli_epi16( sum16,5 );

					// pack result and output to buffer
					*pOut = _mm_cvtsi128_si32( _mm_packus_epi16( sum16,sum16 ) );

					// 640-bit shift--from deck down to src0
					src0 = _mm_alignr_epi8( src1,src0,4 );
					src1 = _mm_alignr_epi8( src2,src1,4 );
					src2 = _mm_alignr_epi8( src3,src2,4 );
					src3 = _mm_alignr_epi8( deck,src3,4 );
					deck = _mm_srli_si128( deck,4 );
				}
			}
			// final pixel end of row
			{
				// process convolution window and accumulate
				__m128i sum16 = Process8Pixels( src0,src1,coefLo );
				sum16 = _mm_add_epi16( sum16,Process8Pixels( src2,src3,coefHi ) );

				// add low and high accumulators
				sum16 = _mm_add_epi16( sum16,_mm_srli_si128( sum16,8 ) );

				// divide by 32 (16 x 32 = 512 in total / 4x overdrive factor)
				sum16 = _mm_srli_epi16( sum16,5 );

				// pack result and output to buffer
				*pOut = _mm_cvtsi128_si32( _mm_packus_epi16( sum16,sum16 ) );
			}
		}

	}
	void _HorizontalPassSSE2()
	{
		// useful constants
		const __m128i zero = _mm_setzero_si128();

		// routines
		auto Process8Pixels = [=]( const __m128i srclo,const __m128i srchi,const __m128i coef )
		{
			// for accumulating sum of high and low pixels in srclo and srchi
			__m128i sum;

			// process low pixels of srclo
			{
				// unpack two pixel byte->word components into src from lo end of srclo
				const __m128i src = _mm_unpacklo_epi8( srclo,zero );

				// broadcast coefficients 1,0 to top and bottom 4 words
				// (first duplicate WORD coeffients in low DWORDS, then shuffle by DWORDS)
				const __m128i co = _mm_shuffle_epi32( _mm_shufflelo_epi16(
					coef,_MM_SHUFFLE( 1,1,0,0 ) ),_MM_SHUFFLE( 1,1,0,0 ) );

				// multiply pixel components by coefficients
				const __m128i prod = _mm_mullo_epi16( co,src );

				// predivide by 16 and accumulate
				sum = _mm_srli_epi16( prod,4 );
			}
			// process high pixels of srclo
			{
				// unpack two pixel byte->word components into src from lo end of srclo
				const __m128i src = _mm_unpackhi_epi8( srclo,zero );

				// broadcast coefficients 3,2 to top and bottom 4 words
				// (first duplicate WORD coeffients in low DWORDS, then shuffle by DWORDS)
				const __m128i co = _mm_shuffle_epi32( _mm_shufflelo_epi16(
					coef,_MM_SHUFFLE( 3,3,2,2 ) ),_MM_SHUFFLE( 1,1,0,0 ) );

				// multiply pixel components by coefficients
				const __m128i prod = _mm_mullo_epi16( co,src );

				// predivide by 16 and accumulate
				const __m128i prediv = _mm_srli_epi16( prod,4 );
				sum = _mm_add_epi16( sum,prediv );
			}
			// process low pixels of srchi
			{
				// unpack two pixel byte->word components into src from lo end of srchi
				const __m128i src = _mm_unpacklo_epi8( srchi,zero );

				// broadcast coefficients 5,4 to top and bottom 4 words
				// (first duplicate WORD coeffients in low DWORDS, then shuffle by DWORDS)
				const __m128i co = _mm_shuffle_epi32( _mm_shufflehi_epi16(
					coef,_MM_SHUFFLE( 1,1,0,0 ) ),_MM_SHUFFLE( 3,3,2,2 ) );

				// multiply pixel components by coefficients
				const __m128i prod = _mm_mullo_epi16( co,src );

				// predivide by 16 and accumulate
				const __m128i prediv = _mm_srli_epi16( prod,4 );
				sum = _mm_add_epi16( sum,prediv );
			}
			// process high pixels of srchi
			{
				// unpack two pixel byte->word components into src from lo end of srchi
				const __m128i src = _mm_unpackhi_epi8( srchi,zero );

				// broadcast coefficients 7,6 to top and bottom 4 words
				// (first duplicate WORD coeffients in low DWORDS, then shuffle by DWORDS)
				const __m128i co = _mm_shuffle_epi32( _mm_shufflehi_epi16(
					coef,_MM_SHUFFLE( 3,3,2,2 ) ),_MM_SHUFFLE( 3,3,2,2 ) );

				// multiply pixel components by coefficients
				const __m128i prod = _mm_mullo_epi16( co,src );

				// predivide by 16 and accumulate
				const __m128i prediv = _mm_srli_epi16( prod,4 );
				sum = _mm_add_epi16( sum,prediv );
			}
			return sum;
		};
		// the lo must be pre-shifted 32-bit (to allow chaining)
		auto Shift256 = [=]( __m128i& lo,__m128i& hi )
		{
			// move carry out dword to high position for masking into lo
			const __m128i carry = _mm_slli_si128( hi,12 );
			// shift hi down by a dword
			hi = _mm_srli_si128( hi,4 );
			// copy high pixel from hi to high pixel location in lo
			lo = _mm_or_si128( lo,carry );
		};

		// indexing constants
		const size_t centerKernel = GetKernelCenter();
		const size_t width = hBuffer.GetWidth();
		const size_t height = hBuffer.GetHeight();

		// load coefficent bytes and unpack to words
		const __m128i coef = _mm_load_si128( (__m128i*)kernel );
		const __m128i coefLo = _mm_unpacklo_epi8( coef,zero );
		const __m128i coefHi = _mm_unpackhi_epi8( coef,zero );

		for( size_t y = 0u; y < height; y++ )
		{
			// setup pointers
			const __m128i* pIn = reinterpret_cast<const __m128i*>(
				&hBuffer.Data()[y * hBuffer.GetPitch()] );
			const __m128i* const pEnd = reinterpret_cast<const __m128i*>(
				&hBuffer.Data()[( y + 1 ) * hBuffer.GetPitch()] );
			Color* pOut = &vBuffer.Data()[y * vBuffer.GetPitch() + GetKernelCenter()];

			// preload input pixels for convolution window
			__m128i src0 = _mm_load_si128( pIn );
			pIn++;
			__m128i src1 = _mm_load_si128( pIn );
			pIn++;
			__m128i src2 = _mm_load_si128( pIn );
			pIn++;
			__m128i src3 = _mm_load_si128( pIn );
			pIn++;

			for( ; pIn < pEnd; pIn++ )
			{
				// on-deck pixels for shifting into convolution window
				__m128i deck = _mm_load_si128( pIn );

				for( size_t i = 0u; i < 4u; i++,pOut++ )
				{
					// process convolution window and accumulate
					__m128i sum16 = Process8Pixels( src0,src1,coefLo );
					sum16 = _mm_add_epi16( sum16,Process8Pixels( src2,src3,coefHi ) );

					// add low and high accumulators
					sum16 = _mm_add_epi16( sum16,_mm_srli_si128( sum16,8 ) );

					// divide by 32 (16 x 32 = 512 in total / 4x overdrive factor)
					sum16 = _mm_srli_epi16( sum16,5 );

					// pack result and output to buffer
					*pOut = _mm_cvtsi128_si32( _mm_packus_epi16( sum16,sum16 ) );

					// shift pixels from deck through convolution window
					// pre-shift src0 to begin chaining
					src0 = _mm_srli_si128( src0,4 );
					// 640-bit chained shift
					Shift256( src0,src1 );
					Shift256( src1,src2 );
					Shift256( src2,src3 );
					Shift256( src3,deck );
				}
			}
			// final pixel end of row
			{
				// process convolution window and accumulate
				__m128i sum16 = Process8Pixels( src0,src1,coefLo );
				sum16 = _mm_add_epi16( sum16,Process8Pixels( src2,src3,coefHi ) );

				// add low and high accumulators
				sum16 = _mm_add_epi16( sum16,_mm_srli_si128( sum16,8 ) );

				// divide by 32 (16 x 32 = 512 in total / 4x overdrive factor)
				sum16 = _mm_srli_epi16( sum16,5 );

				// pack result and output to buffer
				*pOut = _mm_cvtsi128_si32( _mm_packus_epi16( sum16,sum16 ) );
			}
		}
	}
	void _HorizontalPassX86()
	{
		const size_t centerKernel = GetKernelCenter();
		const size_t width = hBuffer.GetWidth();
		const size_t height = hBuffer.GetHeight();

		for( size_t y = 0u; y < height; y++ )
		{
			for( size_t x = 0u; x < width - diameter + 1; x++ )
			{
				unsigned int r = 0;
				unsigned int g = 0;
				unsigned int b = 0;
				const Color* const pBuffer = &hBuffer.Data()[y * width + x];
				for( size_t i = 0; i < diameter; i++ )
				{
					const Color c = pBuffer[i];
					const unsigned int coef = kernel[i];
					r += c.GetR() * coef;
					g += c.GetG() * coef;
					b += c.GetB() * coef;
				}
				vBuffer.Data()[y * width + x + centerKernel] =
				{
					unsigned char( std::min( r / divisorKernel,255u ) ),
					unsigned char( std::min( g / divisorKernel,255u ) ),
					unsigned char( std::min( b / divisorKernel,255u ) )
				};
			}
		}
	}
	void _VerticalPassSSSE3()
	{
		const size_t centerKernel = GetKernelCenter();
		const size_t height = vBuffer.GetHeight();
		const size_t fringe = diameter / 2u;
		const __m128i zero = _mm_setzero_si128();
		const __m128i coef = _mm_load_si128(
			reinterpret_cast<const __m128i*>( &kernel ) );
		const __m128i coefMaskDelta = _mm_set_epi8(
			0x00u,0x01u,0x00u,0x01u,0x00u,0x01u,0x00u,0x01u,
			0x00u,0x01u,0x00u,0x01u,0x00u,0x01u,0x00u,0x01u );
		const __m128i coefMaskStart = _mm_set_epi8(
			0x80u,0x00u,0x80u,0x00u,0x80u,0x00u,0x80u,0x00u,
			0x80u,0x00u,0x80u,0x00u,0x80u,0x00u,0x80u,0x00u );

		auto Process = [=]( const __m128i* const pInput,__m128i& sumLo,__m128i& sumHi,__m128i& coefMask )
		{
			const __m128i input = _mm_load_si128( pInput );
			const __m128i coefBroadcast = _mm_shuffle_epi8( coef,coefMask );
			coefMask = _mm_add_epi8( coefMask,coefMaskDelta );
			{
				const __m128i inputLo = _mm_unpacklo_epi8( input,zero );
				const __m128i productLo = _mm_mullo_epi16( inputLo,coefBroadcast );
				const __m128i predivLo = _mm_srli_epi16( productLo,4 );
				sumLo = _mm_add_epi16( sumLo,predivLo );
			}
			{
				const __m128i inputHi = _mm_unpackhi_epi8( input,zero );
				const __m128i productHi = _mm_mullo_epi16( inputHi,coefBroadcast );
				const __m128i predivHi = _mm_srli_epi16( productHi,4 );
				sumHi = _mm_add_epi16( sumHi,predivHi );
			}
		};

		const __m128i* columnPtrIn = reinterpret_cast<const __m128i*>(
			&vBuffer.Data()[fringe] );
		__m128i* columnPtrOut = reinterpret_cast<__m128i*>(
			&hBuffer.Data()[fringe + centerKernel * hBuffer.GetPitch()] );
		const size_t rowDeltaXmm = reinterpret_cast<const __m128i*>(
			&vBuffer.Data()[fringe + vBuffer.GetPitch()] ) - columnPtrIn;
		const __m128i* const columnPtrInEnd = reinterpret_cast<const __m128i*>(
			&vBuffer.Data()[vBuffer.GetPitch() - fringe] );

		for( ; columnPtrIn < columnPtrInEnd; columnPtrIn++,columnPtrOut++ )
		{
			const __m128i* rowPtrIn = columnPtrIn;
			__m128i* rowPtrOut = columnPtrOut;
			const __m128i* const rowPtrInEnd = &rowPtrIn[(height - 15u) * rowDeltaXmm];

			for( ; rowPtrIn < rowPtrInEnd; rowPtrIn += rowDeltaXmm,rowPtrOut += rowDeltaXmm )
			{
				__m128i coefMask = coefMaskStart;
				const __m128i* windowPtrIn = rowPtrIn;
				const __m128i* windowPtrInEnd = &rowPtrIn[16u * rowDeltaXmm];

				__m128i sumHi = zero;
				__m128i sumLo = zero;

				for( ; windowPtrIn < windowPtrInEnd; windowPtrIn += rowDeltaXmm )
				{
					Process( windowPtrIn,sumLo,sumHi,coefMask );
				}

				sumHi = _mm_srli_epi16( sumHi,5 );
				sumLo = _mm_srli_epi16( sumLo,5 );

				_mm_store_si128( rowPtrOut,_mm_packus_epi16( sumLo,sumHi ) );
			}
		}
	}
	void _VerticalPassSSE2()
	{
#pragma warning (push)
#pragma warning (disable: 4556)
#pragma region Convolution Macro
#define CONVOLUTE_STEP_VERTICAL_BLUR( sumLo,sumHi,windowPtrIn,index )\
	{\
		const __m128i input = _mm_load_si128( windowPtrIn );\
		__m128i coefBroadcast;\
		if( index < 8 )\
		{\
			coefBroadcast = _mm_unpacklo_epi8( coef,zero );\
			if( index < 4 )\
			{\
				coefBroadcast = _mm_shuffle_epi32( _mm_shufflelo_epi16(\
					coefBroadcast,_MM_SHUFFLE( index,index,index,index ) ),\
					_MM_SHUFFLE( 0,0,0,0 ) );\
			}\
			else\
			{\
				coefBroadcast = _mm_shuffle_epi32( _mm_shufflehi_epi16(\
					coefBroadcast,_MM_SHUFFLE( index - 4,index - 4,index - 4,index - 4 ) ),\
					_MM_SHUFFLE( 2,2,2,2 ) );\
			}\
		}\
		else\
		{\
			coefBroadcast = _mm_unpackhi_epi8( coef,zero );\
			if( index < 12 )\
			{\
				coefBroadcast = _mm_shuffle_epi32( _mm_shufflelo_epi16(\
					coefBroadcast,_MM_SHUFFLE( index - 8,index - 8,index - 8,index - 8 ) ),\
					_MM_SHUFFLE( 0,0,0,0 ) );\
			}\
			else\
			{\
				coefBroadcast = _mm_shuffle_epi32( _mm_shufflehi_epi16(\
					coefBroadcast,_MM_SHUFFLE( index - 12,index - 12,index - 12,index - 12 ) ),\
					_MM_SHUFFLE( 2,2,2,2 ) );\
			}\
		}\
		\
		{\
			const __m128i inputLo = _mm_unpacklo_epi8( input,zero ); \
			const __m128i productLo = _mm_mullo_epi16( inputLo,coefBroadcast ); \
			const __m128i predivLo = _mm_srli_epi16( productLo,4 ); \
			sumLo = _mm_add_epi16( sumLo,predivLo ); \
		}\
		{\
			const __m128i inputHi = _mm_unpackhi_epi8( input,zero ); \
			const __m128i productHi = _mm_mullo_epi16( inputHi,coefBroadcast ); \
			const __m128i predivHi = _mm_srli_epi16( productHi,4 ); \
			sumHi = _mm_add_epi16( sumHi,predivHi ); \
		}\
	}
#pragma endregion

		const size_t centerKernel = GetKernelCenter();
		const size_t height = vBuffer.GetHeight();
		const size_t fringe = diameter / 2u;
		const __m128i zero = _mm_setzero_si128();
		const __m128i coef = _mm_load_si128( 
			reinterpret_cast<const __m128i*>( &kernel ) );

		const __m128i* columnPtrIn = reinterpret_cast<const __m128i*>(
			&vBuffer.Data()[fringe] );
		__m128i* columnPtrOut = reinterpret_cast<__m128i*>(
			&hBuffer.Data()[fringe + centerKernel * hBuffer.GetPitch()] );
		const size_t rowDeltaXmm = reinterpret_cast<const __m128i*>(
			&vBuffer.Data()[fringe + vBuffer.GetPitch()] ) - columnPtrIn;
		const __m128i* const columnPtrInEnd = reinterpret_cast<const __m128i*>(
			&vBuffer.Data()[vBuffer.GetPitch() - fringe] );

		for( ; columnPtrIn < columnPtrInEnd; columnPtrIn++,columnPtrOut++ )
		{
			const __m128i* rowPtrIn = columnPtrIn;
			__m128i* rowPtrOut = columnPtrOut;
			const __m128i* const rowPtrInEnd = &rowPtrIn[ (height - 15u) * rowDeltaXmm];

			for( ; rowPtrIn < rowPtrInEnd; rowPtrIn += rowDeltaXmm,rowPtrOut += rowDeltaXmm )
			{
				const __m128i* windowPtrIn = rowPtrIn;

				__m128i sumHi = zero;
				__m128i sumLo = zero;
				CONVOLUTE_STEP_VERTICAL_BLUR( sumLo,sumHi,windowPtrIn,0 );
				windowPtrIn += rowDeltaXmm;
				CONVOLUTE_STEP_VERTICAL_BLUR( sumLo,sumHi,windowPtrIn,1 );
				windowPtrIn += rowDeltaXmm;
				CONVOLUTE_STEP_VERTICAL_BLUR( sumLo,sumHi,windowPtrIn,2 );
				windowPtrIn += rowDeltaXmm;
				CONVOLUTE_STEP_VERTICAL_BLUR( sumLo,sumHi,windowPtrIn,3 );
				windowPtrIn += rowDeltaXmm;
				CONVOLUTE_STEP_VERTICAL_BLUR( sumLo,sumHi,windowPtrIn,4 );
				windowPtrIn += rowDeltaXmm;
				CONVOLUTE_STEP_VERTICAL_BLUR( sumLo,sumHi,windowPtrIn,5 );
				windowPtrIn += rowDeltaXmm;
				CONVOLUTE_STEP_VERTICAL_BLUR( sumLo,sumHi,windowPtrIn,6 );
				windowPtrIn += rowDeltaXmm;
				CONVOLUTE_STEP_VERTICAL_BLUR( sumLo,sumHi,windowPtrIn,7 );
				windowPtrIn += rowDeltaXmm;
				CONVOLUTE_STEP_VERTICAL_BLUR( sumLo,sumHi,windowPtrIn,8 );
				windowPtrIn += rowDeltaXmm;
				CONVOLUTE_STEP_VERTICAL_BLUR( sumLo,sumHi,windowPtrIn,9 );
				windowPtrIn += rowDeltaXmm;
				CONVOLUTE_STEP_VERTICAL_BLUR( sumLo,sumHi,windowPtrIn,10 );
				windowPtrIn += rowDeltaXmm;
				CONVOLUTE_STEP_VERTICAL_BLUR( sumLo,sumHi,windowPtrIn,11 );
				windowPtrIn += rowDeltaXmm;
				CONVOLUTE_STEP_VERTICAL_BLUR( sumLo,sumHi,windowPtrIn,12 );
				windowPtrIn += rowDeltaXmm;
				CONVOLUTE_STEP_VERTICAL_BLUR( sumLo,sumHi,windowPtrIn,13 );
				windowPtrIn += rowDeltaXmm;
				CONVOLUTE_STEP_VERTICAL_BLUR( sumLo,sumHi,windowPtrIn,14 );
				windowPtrIn += rowDeltaXmm;
				CONVOLUTE_STEP_VERTICAL_BLUR( sumLo,sumHi,windowPtrIn,15 );

				sumHi = _mm_srli_epi16( sumHi,5 );
				sumLo = _mm_srli_epi16( sumLo,5 );

				_mm_store_si128( rowPtrOut,_mm_packus_epi16( sumLo,sumHi ) );
			}
		}
#undef CONVOLUTE_STEP_VERTICAL_BLUR
#pragma warning (pop)
	}
	void _VerticalPassX86()
	{
		const size_t centerKernel = GetKernelCenter();
		const size_t width = vBuffer.GetWidth();
		const size_t height = vBuffer.GetHeight();
		const size_t fringe = diameter / 2u;

		for( size_t x = fringe; x < width - fringe; x++ )
		{
			for( size_t y = 0u; y < height - diameter + 1; y++ )
			{
				unsigned int r = 0;
				unsigned int g = 0;
				unsigned int b = 0;
				const Color* pBuffer = &vBuffer.Data()[y * width + x];
				for( size_t i = 0; i < diameter; i++,
					pBuffer += width )
				{
					const Color c = *pBuffer;
					const unsigned int coef = kernel[i];
					r += c.GetR() * coef;
					g += c.GetG() * coef;
					b += c.GetB() * coef;
				}
				hBuffer.Data()[( y + centerKernel ) * width + x] =
				{
					unsigned char( std::min( r / divisorKernel,255u ) ),
					unsigned char( std::min( g / divisorKernel,255u ) ),
					unsigned char( std::min( b / divisorKernel,255u ) )
				};
			}
		}
	}
	void _UpsizeBlendPassSSSE3MT()
	{
		// prevent workers from reporting done while in the middle of setup
		std::unique_lock<std::mutex> lock( mutexBoss );

		// reset active thread count
		nActiveThreads = workerPtrs.size();

		// activate worker threads
		for( auto& pw : workerPtrs )
		{
			pw->Start();
		}

		// wait for workers to notify of finish, releasing boss mutex
		cvBoss.wait( lock,[this](){ return nActiveThreads == 0u; } );
	}
	void _UpsizeBlendPassSSSE3()
	{
		const __m128i zero = _mm_setzero_si128();
		__m128i grad_coef = _mm_set_epi16( 160u,160u,160u,160u,224u,224u,224u,224u );

		// interpolate horizontally between low 2 pixels of input
		const auto GenerateGradient = [&]( __m128i in )
		{
			// unpack inputs (low 2 pixels) to 16-bit channel size
			const __m128i in16 = _mm_unpacklo_epi8( in,zero );

			// copy low pixel to high and low 64 bits
			const __m128i in_a = _mm_shuffle_epi32( in16,_MM_SHUFFLE( 1,0,1,0 ) );
			// multiply input by decreasing coeffients (lower pixels)
			const __m128i prod_a_lo = _mm_mullo_epi16( in_a,grad_coef );
			// transform decreasing coef to lower range (for high pixels)
			grad_coef = _mm_sub_epi16( grad_coef,_mm_set128_epi16( grad_coef ) );
			// multiply input by decreasing coeffients (higher pixels)
			const __m128i prod_a_hi = _mm_mullo_epi16( in_a,grad_coef );

			// copy high pixel to high and low 64 bits
			const __m128i in_b = _mm_shuffle_epi32( in16,_MM_SHUFFLE( 3,2,3,2 ) );
			// transform decreasing coef to increasing coefficients (for low pixels)
			grad_coef = _mm_shuffle_epi32( grad_coef,_MM_SHUFFLE( 0,1,3,2 ) );
			// multiply input by increasing coeffients (lower pixels)
			const __m128i prod_b_lo = _mm_mullo_epi16( in_b,grad_coef );
			// transform increasing coef to higher range (for high pixels)
			grad_coef = _mm_add_epi16( grad_coef,_mm_set128_epi16( grad_coef ) );
			// multiply input by increasing coeffients (higher pixels)
			const __m128i prod_b_hi = _mm_mullo_epi16( in_b,grad_coef );

			// return coefficients to original state
			grad_coef = _mm_shuffle_epi32( grad_coef,_MM_SHUFFLE( 0,1,3,2 ) );

			// add low products and divide
			const __m128i ab_lo = _mm_srli_epi16( _mm_adds_epu16( prod_a_lo,prod_b_lo ),8 );
			// add high products and divide
			const __m128i ab_hi = _mm_srli_epi16( _mm_adds_epu16( prod_a_hi,prod_b_hi ),8 );

			// pack and return result
			return _mm_packus_epi16( ab_lo,ab_hi );
		};

		// upsize for top and bottom edge cases
		const auto UpsizeEdge = [&]( const __m128i* pIn,const __m128i* pInEnd,__m128i* pOutTop,
			__m128i* pOutBottom )
		{
			__m128i in = _mm_load_si128( pIn++ );
			// left corner setup (prime the alignment pump)
			__m128i oldPix = _mm_shuffle_epi32( in,_MM_SHUFFLE( 0,0,0,0 ) );

			// main loop
			while( true )
			{
				// gradient 0-1
				__m128i newPix = GenerateGradient( in );
				__m128i out = _mm_alignr_epi8( newPix,oldPix,8 );
				*pOutTop = _mm_adds_epu8( *pOutTop,out );
				*pOutBottom = _mm_adds_epu8( *pOutBottom,out );
				pOutTop++;
				pOutBottom++;
				oldPix = newPix;

				// gradient 1-2
				newPix = GenerateGradient( _mm_srli_si128( in,4 ) );
				out = _mm_alignr_epi8( newPix,oldPix,8 );
				*pOutTop = _mm_adds_epu8( *pOutTop,out );
				*pOutBottom = _mm_adds_epu8( *pOutBottom,out );
				pOutTop++;
				pOutBottom++;
				oldPix = newPix;

				// gradient 2-3
				newPix = GenerateGradient( _mm_srli_si128( in,8 ) );
				out = _mm_alignr_epi8( newPix,oldPix,8 );
				*pOutTop = _mm_adds_epu8( *pOutTop,out );
				*pOutBottom = _mm_adds_epu8( *pOutBottom,out );
				pOutTop++;
				pOutBottom++;
				oldPix = newPix;

				// end condition
				if( pIn >= pInEnd )
				{
					break;
				}

				// gradient 3-0'
				const __m128i newIn = _mm_load_si128( pIn++ );
				newPix = GenerateGradient( _mm_alignr_epi8( newIn,in,12 ) );
				out = _mm_alignr_epi8( newPix,oldPix,8 );
				*pOutTop = _mm_adds_epu8( *pOutTop,out );
				*pOutBottom = _mm_adds_epu8( *pOutBottom,out );
				pOutTop++;
				pOutBottom++;
				oldPix = newPix;
				in = newIn;
			}

			// right corner
			const __m128i out = _mm_alignr_epi8( _mm_shuffle_epi32( in,_MM_SHUFFLE( 3,3,3,3 ) ),oldPix,8 );
			*pOutTop = _mm_adds_epu8( *pOutTop,out );
			*pOutBottom = _mm_adds_epu8( *pOutBottom,out );
		};

		// hold values from last iteration
		__m128i old0;
		__m128i old1;
		__m128i old2;
		__m128i old3;

		// interpolate horizontally between first 2 pixels of inputs and then vertically
		const auto VerticalGradientOutput = [&]( __m128i in0,__m128i in1,
			__m128i* pOut0,__m128i* pOut1,__m128i* pOut2,__m128i* pOut3 )
		{
			const __m128i topGrad = GenerateGradient( in0 );
			const __m128i bottomGrad = GenerateGradient( in1 );

			// generate points between top and bottom pixel arrays
			const __m128i half = _mm_avg_epu8( topGrad,bottomGrad );

			{
				// first quarter needed for top half
				const __m128i firstQuarter = _mm_avg_epu8( topGrad,half );

				// generate 1/8 pt from top to bottom
				const __m128i firstEighth = _mm_avg_epu8( topGrad,firstQuarter );
				// combine old 1/8 pt and new and add to original image with saturation
				*pOut0 = _mm_adds_epu8( *pOut0,_mm_alignr_epi8( firstEighth,old0,8 ) );
				old0 = firstEighth;

				// generate 3/8 pt from top to bottom
				const __m128i thirdEighth = _mm_avg_epu8( firstQuarter,half );
				// combine old 3/8 pt and new and add to original image with saturation
				*pOut1 = _mm_adds_epu8( *pOut1,_mm_alignr_epi8( thirdEighth,old1,8 ) );
				old1 = thirdEighth;
			}

			{
				// third quarter needed for bottom half
				const __m128i thirdQuarter = _mm_avg_epu8( half,bottomGrad );

				// generate 5/8 pt from top to bottom
				const __m128i fifthEighth = _mm_avg_epu8( half,thirdQuarter );
				// combine old 5/8 pt and new and add to original image with saturation
				*pOut2 = _mm_adds_epu8( *pOut2,_mm_alignr_epi8( fifthEighth,old2,8 ) );
				old2 = fifthEighth;

				// generate 7/8 pt from top to bottom
				const __m128i seventhEighth = _mm_avg_epu8( thirdQuarter,bottomGrad );
				// combine old 7/8 pt and new and add to original image with saturation
				*pOut3 = _mm_adds_epu8( *pOut3,_mm_alignr_epi8( seventhEighth,old3,8 ) );
				old3 = seventhEighth;
			}
		};

		// upsize for middle cases
		const auto DoLine = [&]( const __m128i* pIn0,const __m128i* pIn1,const __m128i* const pEnd,
			__m128i* pOut0,__m128i* pOut1,__m128i* pOut2,__m128i* pOut3 )
		{
			__m128i in0 = _mm_load_si128( pIn0++ );
			__m128i in1 = _mm_load_si128( pIn1++ );

			// left side prime pump
			{
				// left edge clamps to left most pixel
				const __m128i top = _mm_shuffle_epi32( in0,_MM_SHUFFLE( 0,0,0,0 ) );
				const __m128i bottom = _mm_shuffle_epi32( in1,_MM_SHUFFLE( 0,0,0,0 ) );

				// generate points between top and bottom pixel arrays
				const __m128i half = _mm_avg_epu8( top,bottom );

				{
					// first quarter needed for top half
					const __m128i firstQuarter = _mm_avg_epu8( top,half );

					// generate 1/8 pt from top to bottom
					old0 = _mm_avg_epu8( top,firstQuarter );

					// generate 3/8 pt from top to bottom
					old1 = _mm_avg_epu8( firstQuarter,half );
				}

				{
					// third quarter needed for bottom half
					const __m128i thirdQuarter = _mm_avg_epu8( half,bottom );

					// generate 5/8 pt from top to bottom
					old2 = _mm_avg_epu8( half,thirdQuarter );

					// generate 7/8 pt from top to bottom
					old3 = _mm_avg_epu8( thirdQuarter,bottom );
				}
			}

			// main loop
			while( true )
			{
				// gradient 0-1
				VerticalGradientOutput( in0,in1,pOut0++,pOut1++,pOut2++,pOut3++ );

				// gradient 1-2
				VerticalGradientOutput(
					_mm_srli_si128( in0,4 ),
					_mm_srli_si128( in1,4 ),
					pOut0++,pOut1++,pOut2++,pOut3++ );

				// gradient 2-3
				VerticalGradientOutput(
					_mm_srli_si128( in0,8 ),
					_mm_srli_si128( in1,8 ),
					pOut0++,pOut1++,pOut2++,pOut3++ );

				// end condition
				if( pIn0 >= pEnd )
				{
					break;
				}

				// gradient 3-0'
				const __m128i newIn0 = _mm_load_si128( pIn0++ );
				const __m128i newIn1 = _mm_load_si128( pIn1++ );
				VerticalGradientOutput(
					_mm_alignr_epi8( newIn0,in0,12 ),
					_mm_alignr_epi8( newIn1,in1,12 ),
					pOut0++,pOut1++,pOut2++,pOut3++ );
				in0 = newIn0;
				in1 = newIn1;
			}

			// right side finish pump
			{
				// right edge clamps to right most pixel
				const __m128i top = _mm_shuffle_epi32( in0,_MM_SHUFFLE( 3,3,3,3 ) );
				const __m128i bottom = _mm_shuffle_epi32( in1,_MM_SHUFFLE( 3,3,3,3 ) );

				// generate points between top and bottom pixel arrays
				const __m128i half = _mm_avg_epu8( top,bottom );

				{
					// first quarter needed for top half
					const __m128i firstQuarter = _mm_avg_epu8( top,half );

					// generate 1/8 pt from top to bottom
					*pOut0 = _mm_adds_epu8( *pOut0,_mm_alignr_epi8( 
						_mm_avg_epu8( top,firstQuarter ),old0,8 ) );

					// generate 3/8 pt from top to bottom
					*pOut1 = _mm_adds_epu8( *pOut1,_mm_alignr_epi8(
						_mm_avg_epu8( firstQuarter,half ),old1,8 ) );
				}
				{
					// third quarter needed for bottom half
					const __m128i thirdQuarter = _mm_avg_epu8( half,bottom );

					// generate 5/8 pt from top to bottom
					*pOut2 = _mm_adds_epu8( *pOut2,_mm_alignr_epi8(
						_mm_avg_epu8( half,thirdQuarter ),old2,8 ) );

					// generate 7/8 pt from top to bottom
					*pOut3 = _mm_adds_epu8( *pOut3,_mm_alignr_epi8(
						_mm_avg_epu8( thirdQuarter,bottom ),old3,8 ) );
				}
			}
		};

		// constants for line loop pointer arithmetic
		const size_t inWidthScalar = hBuffer.GetWidth();
		const size_t outWidthScalar = input.GetWidth();
		const size_t inFringe = diameter / 2u;
		const size_t outFringe = GetFringeSize();

		// do top line
		UpsizeEdge(
			reinterpret_cast<const __m128i*>( 
				&hBuffer.Data()[inWidthScalar * inFringe + inFringe] ),
			reinterpret_cast<const __m128i*>( 
				&hBuffer.Data()[inWidthScalar * ( inFringe + 1 ) - inFringe] ),
			reinterpret_cast<__m128i*>( 
				&input.Data()[outWidthScalar * outFringe + outFringe] ),
			reinterpret_cast<__m128i*>(
				&input.Data()[outWidthScalar * ( outFringe + 1u ) + outFringe] ) );

		// setup pointers for resizing line loop
		const __m128i* pIn0 = reinterpret_cast<const __m128i*>( 
			&hBuffer.Data()[inWidthScalar * inFringe + inFringe] );
		const __m128i* pIn1 = reinterpret_cast<const __m128i*>(
			&hBuffer.Data()[(inWidthScalar * (inFringe + 1)) + inFringe] );
		const __m128i* pLineEnd = reinterpret_cast<const __m128i*>(
			&hBuffer.Data()[inWidthScalar * (inFringe + 1) - inFringe] );
		const __m128i* const pEnd = reinterpret_cast<const __m128i*>(
			&hBuffer.Data()[inWidthScalar * ( hBuffer.GetHeight() - ( inFringe + 1u ) ) + inFringe] );
		__m128i* pOut0 = reinterpret_cast<__m128i*>( 
			&input.Data()[outWidthScalar * ( outFringe + 2u) + outFringe] );
		__m128i* pOut1 = reinterpret_cast<__m128i*>( 
			&input.Data()[outWidthScalar * ( outFringe + 3u ) + outFringe] );
		__m128i* pOut2 = reinterpret_cast<__m128i*>( 
			&input.Data()[outWidthScalar * ( outFringe + 4u ) + outFringe] );
		__m128i* pOut3 = reinterpret_cast<__m128i*>( 
			&input.Data()[outWidthScalar * ( outFringe + 5u ) + outFringe] );

		const size_t inStep = pIn1 - pIn0;
		// no overlap in output
		const size_t outStep = (pOut1 - pOut0) * 4u;

		// do middle lines
		for( ; pIn0 < pEnd; pIn0 += inStep,pIn1 += inStep,pLineEnd += inStep,
			pOut0 += outStep,pOut1 += outStep,pOut2 += outStep,pOut3 += outStep )
		{
			DoLine( pIn0,pIn1,pLineEnd,pOut0,pOut1,pOut2,pOut3 );
		}

		// do bottom line
		UpsizeEdge(
			reinterpret_cast<const __m128i*>( 
				&hBuffer.Data()[inWidthScalar * ( hBuffer.GetHeight() - ( inFringe + 1u ) ) + inFringe] ),
			reinterpret_cast<const __m128i*>( 
				&hBuffer.Data()[inWidthScalar * ( hBuffer.GetHeight() - inFringe ) - inFringe] ),
			reinterpret_cast<__m128i*>( 
				&input.Data()[outWidthScalar * ( input.GetHeight() - ( outFringe + 2u ) ) + outFringe] ),
			reinterpret_cast<__m128i*>(
				&input.Data()[outWidthScalar * ( input.GetHeight() - ( outFringe + 1u ) ) + outFringe] ) );
	}
	void _UpsizeBlendPassSSE2()
	{
		const __m128i zero = _mm_setzero_si128();
		__m128i grad_coef = _mm_set_epi16( 160u,160u,160u,160u,224u,224u,224u,224u );

		// interpolate horizontally between low 2 pixels of input
		const auto GenerateGradient = [&]( __m128i in )
		{
			// unpack inputs (low 2 pixels) to 16-bit channel size
			const __m128i in16 = _mm_unpacklo_epi8( in,zero );

			// copy low pixel to high and low 64 bits
			const __m128i in_a = _mm_shuffle_epi32( in16,_MM_SHUFFLE( 1,0,1,0 ) );
			// multiply input by decreasing coeffients (lower pixels)
			const __m128i prod_a_lo = _mm_mullo_epi16( in_a,grad_coef );
			// transform decreasing coef to lower range (for high pixels)
			grad_coef = _mm_sub_epi16( grad_coef,_mm_set128_epi16( grad_coef ) );
			// multiply input by decreasing coeffients (higher pixels)
			const __m128i prod_a_hi = _mm_mullo_epi16( in_a,grad_coef );

			// copy high pixel to high and low 64 bits
			const __m128i in_b = _mm_shuffle_epi32( in16,_MM_SHUFFLE( 3,2,3,2 ) );
			// transform decreasing coef to increasing coefficients (for low pixels)
			grad_coef = _mm_shuffle_epi32( grad_coef,_MM_SHUFFLE( 0,1,3,2 ) );
			// multiply input by increasing coeffients (lower pixels)
			const __m128i prod_b_lo = _mm_mullo_epi16( in_b,grad_coef );
			// transform increasing coef to higher range (for high pixels)
			grad_coef = _mm_add_epi16( grad_coef,_mm_set128_epi16( grad_coef ) );
			// multiply input by increasing coeffients (higher pixels)
			const __m128i prod_b_hi = _mm_mullo_epi16( in_b,grad_coef );

			// return coefficients to original state
			grad_coef = _mm_shuffle_epi32( grad_coef,_MM_SHUFFLE( 0,1,3,2 ) );

			// add low products and divide
			const __m128i ab_lo = _mm_srli_epi16( _mm_adds_epu16( prod_a_lo,prod_b_lo ),8 );
			// add high products and divide
			const __m128i ab_hi = _mm_srli_epi16( _mm_adds_epu16( prod_a_hi,prod_b_hi ),8 );

			// pack and return result
			return _mm_packus_epi16( ab_lo,ab_hi );
		};

		// upsize for top and bottom edge cases
		const auto UpsizeEdge = [&]( const __m128i* pIn,const __m128i* pInEnd,__m128i* pOutTop,
			__m128i* pOutBottom )
		{
			__m128i in = _mm_load_si128( pIn++ );
			// left corner setup (prime the alignment pump)
			__m128i oldPix = _mm_shuffle_epi32( in,_MM_SHUFFLE( 0,0,0,0 ) );

			// main loop
			while( true )
			{
				// gradient 0-1
				__m128i newPix = GenerateGradient( in );
				__m128i out = AlignRightSSE2<8>( newPix,oldPix );
				*pOutTop = _mm_adds_epu8( *pOutTop,out );
				*pOutBottom = _mm_adds_epu8( *pOutBottom,out );
				pOutTop++;
				pOutBottom++;
				oldPix = newPix;

				// gradient 1-2
				newPix = GenerateGradient( _mm_srli_si128( in,4 ) );
				out = AlignRightSSE2<8>( newPix,oldPix );
				*pOutTop = _mm_adds_epu8( *pOutTop,out );
				*pOutBottom = _mm_adds_epu8( *pOutBottom,out );
				pOutTop++;
				pOutBottom++;
				oldPix = newPix;

				// gradient 2-3
				newPix = GenerateGradient( _mm_srli_si128( in,8 ) );
				out = AlignRightSSE2<8>( newPix,oldPix );
				*pOutTop = _mm_adds_epu8( *pOutTop,out );
				*pOutBottom = _mm_adds_epu8( *pOutBottom,out );
				pOutTop++;
				pOutBottom++;
				oldPix = newPix;

				// end condition
				if( pIn >= pInEnd )
				{
					break;
				}

				// gradient 3-0'
				const __m128i newIn = _mm_load_si128( pIn++ );
				newPix = GenerateGradient( AlignRightSSE2<12>( newIn,in ) );
				out = AlignRightSSE2<8>( newPix,oldPix );
				*pOutTop = _mm_adds_epu8( *pOutTop,out );
				*pOutBottom = _mm_adds_epu8( *pOutBottom,out );
				pOutTop++;
				pOutBottom++;
				oldPix = newPix;
				in = newIn;
			}

			// right corner
			const __m128i out = AlignRightSSE2<8>( _mm_shuffle_epi32( in,_MM_SHUFFLE( 3,3,3,3 ) ),oldPix );
			*pOutTop = _mm_adds_epu8( *pOutTop,out );
			*pOutBottom = _mm_adds_epu8( *pOutBottom,out );
		};

		// hold values from last iteration
		__m128i old0;
		__m128i old1;
		__m128i old2;
		__m128i old3;

		// interpolate horizontally between first 2 pixels of inputs and then vertically
		const auto VerticalGradientOutput = [&]( __m128i in0,__m128i in1,
			__m128i* pOut0,__m128i* pOut1,__m128i* pOut2,__m128i* pOut3 )
		{
			const __m128i topGrad = GenerateGradient( in0 );
			const __m128i bottomGrad = GenerateGradient( in1 );

			// generate points between top and bottom pixel arrays
			const __m128i half = _mm_avg_epu8( topGrad,bottomGrad );

			{
				// first quarter needed for top half
				const __m128i firstQuarter = _mm_avg_epu8( topGrad,half );

				// generate 1/8 pt from top to bottom
				const __m128i firstEighth = _mm_avg_epu8( topGrad,firstQuarter );
				// combine old 1/8 pt and new and add to original image with saturation
				*pOut0 = _mm_adds_epu8( *pOut0,AlignRightSSE2<8>( firstEighth,old0 ) );
				old0 = firstEighth;

				// generate 3/8 pt from top to bottom
				const __m128i thirdEighth = _mm_avg_epu8( firstQuarter,half );
				// combine old 3/8 pt and new and add to original image with saturation
				*pOut1 = _mm_adds_epu8( *pOut1,AlignRightSSE2<8>( thirdEighth,old1 ) );
				old1 = thirdEighth;
			}

			{
				// third quarter needed for bottom half
				const __m128i thirdQuarter = _mm_avg_epu8( half,bottomGrad );

				// generate 5/8 pt from top to bottom
				const __m128i fifthEighth = _mm_avg_epu8( half,thirdQuarter );
				// combine old 5/8 pt and new and add to original image with saturation
				*pOut2 = _mm_adds_epu8( *pOut2,AlignRightSSE2<8>( fifthEighth,old2 ) );
				old2 = fifthEighth;

				// generate 7/8 pt from top to bottom
				const __m128i seventhEighth = _mm_avg_epu8( thirdQuarter,bottomGrad );
				// combine old 7/8 pt and new and add to original image with saturation
				*pOut3 = _mm_adds_epu8( *pOut3,AlignRightSSE2<8>( seventhEighth,old3 ) );
				old3 = seventhEighth;
			}
		};

		// upsize for middle cases
		const auto DoLine = [&]( const __m128i* pIn0,const __m128i* pIn1,const __m128i* const pEnd,
			__m128i* pOut0,__m128i* pOut1,__m128i* pOut2,__m128i* pOut3 )
		{
			__m128i in0 = _mm_load_si128( pIn0++ );
			__m128i in1 = _mm_load_si128( pIn1++ );

			// left side prime pump
			{
				// left edge clamps to left most pixel
				const __m128i top = _mm_shuffle_epi32( in0,_MM_SHUFFLE( 0,0,0,0 ) );
				const __m128i bottom = _mm_shuffle_epi32( in1,_MM_SHUFFLE( 0,0,0,0 ) );

				// generate points between top and bottom pixel arrays
				const __m128i half = _mm_avg_epu8( top,bottom );

				{
					// first quarter needed for top half
					const __m128i firstQuarter = _mm_avg_epu8( top,half );

					// generate 1/8 pt from top to bottom
					old0 = _mm_avg_epu8( top,firstQuarter );

					// generate 3/8 pt from top to bottom
					old1 = _mm_avg_epu8( firstQuarter,half );
				}

				{
					// third quarter needed for bottom half
					const __m128i thirdQuarter = _mm_avg_epu8( half,bottom );

					// generate 5/8 pt from top to bottom
					old2 = _mm_avg_epu8( half,thirdQuarter );

					// generate 7/8 pt from top to bottom
					old3 = _mm_avg_epu8( thirdQuarter,bottom );
				}
			}

			// main loop
			while( true )
			{
				// gradient 0-1
				VerticalGradientOutput( in0,in1,pOut0++,pOut1++,pOut2++,pOut3++ );

				// gradient 1-2
				VerticalGradientOutput(
					_mm_srli_si128( in0,4 ),
					_mm_srli_si128( in1,4 ),
					pOut0++,pOut1++,pOut2++,pOut3++ );

				// gradient 2-3
				VerticalGradientOutput(
					_mm_srli_si128( in0,8 ),
					_mm_srli_si128( in1,8 ),
					pOut0++,pOut1++,pOut2++,pOut3++ );

				// end condition
				if( pIn0 >= pEnd )
				{
					break;
				}

				// gradient 3-0'
				const __m128i newIn0 = _mm_load_si128( pIn0++ );
				const __m128i newIn1 = _mm_load_si128( pIn1++ );
				VerticalGradientOutput(
					AlignRightSSE2<12>( newIn0,in0 ),
					AlignRightSSE2<12>( newIn1,in1 ),
					pOut0++,pOut1++,pOut2++,pOut3++ );
				in0 = newIn0;
				in1 = newIn1;
			}

			// right side finish pump
			{
				// right edge clamps to right most pixel
				const __m128i top = _mm_shuffle_epi32( in0,_MM_SHUFFLE( 3,3,3,3 ) );
				const __m128i bottom = _mm_shuffle_epi32( in1,_MM_SHUFFLE( 3,3,3,3 ) );

				// generate points between top and bottom pixel arrays
				const __m128i half = _mm_avg_epu8( top,bottom );

				{
					// first quarter needed for top half
					const __m128i firstQuarter = _mm_avg_epu8( top,half );

					// generate 1/8 pt from top to bottom
					*pOut0 = _mm_adds_epu8( *pOut0,AlignRightSSE2<8>(
						_mm_avg_epu8( top,firstQuarter ),old0 ) );

					// generate 3/8 pt from top to bottom
					*pOut1 = _mm_adds_epu8( *pOut1,AlignRightSSE2<8>(
						_mm_avg_epu8( firstQuarter,half ),old1 ) );
				}
				{
					// third quarter needed for bottom half
					const __m128i thirdQuarter = _mm_avg_epu8( half,bottom );

					// generate 5/8 pt from top to bottom
					*pOut2 = _mm_adds_epu8( *pOut2,AlignRightSSE2<8>(
						_mm_avg_epu8( half,thirdQuarter ),old2 ) );

					// generate 7/8 pt from top to bottom
					*pOut3 = _mm_adds_epu8( *pOut3,AlignRightSSE2<8>(
						_mm_avg_epu8( thirdQuarter,bottom ),old3 ) );
				}
			}
		};

		// constants for line loop pointer arithmetic
		const size_t inWidthScalar = hBuffer.GetWidth();
		const size_t outWidthScalar = input.GetWidth();
		const size_t inFringe = diameter / 2u;
		const size_t outFringe = GetFringeSize();

		// do top line
		UpsizeEdge(
			reinterpret_cast<const __m128i*>(
			&hBuffer.Data()[inWidthScalar * inFringe + inFringe] ),
			reinterpret_cast<const __m128i*>(
			&hBuffer.Data()[inWidthScalar * ( inFringe + 1 ) - inFringe] ),
			reinterpret_cast<__m128i*>(
			&input.Data()[outWidthScalar * outFringe + outFringe] ),
			reinterpret_cast<__m128i*>(
			&input.Data()[outWidthScalar * ( outFringe + 1u ) + outFringe] ) );

		// setup pointers for resizing line loop
		const __m128i* pIn0 = reinterpret_cast<const __m128i*>(
			&hBuffer.Data()[inWidthScalar * inFringe + inFringe] );
		const __m128i* pIn1 = reinterpret_cast<const __m128i*>(
			&hBuffer.Data()[( inWidthScalar * ( inFringe + 1 ) ) + inFringe] );
		const __m128i* pLineEnd = reinterpret_cast<const __m128i*>(
			&hBuffer.Data()[inWidthScalar * ( inFringe + 1 ) - inFringe] );
		const __m128i* const pEnd = reinterpret_cast<const __m128i*>(
			&hBuffer.Data()[inWidthScalar * ( hBuffer.GetHeight() - ( inFringe + 1u ) ) + inFringe] );
		__m128i* pOut0 = reinterpret_cast<__m128i*>(
			&input.Data()[outWidthScalar * ( outFringe + 2u ) + outFringe] );
		__m128i* pOut1 = reinterpret_cast<__m128i*>(
			&input.Data()[outWidthScalar * ( outFringe + 3u ) + outFringe] );
		__m128i* pOut2 = reinterpret_cast<__m128i*>(
			&input.Data()[outWidthScalar * ( outFringe + 4u ) + outFringe] );
		__m128i* pOut3 = reinterpret_cast<__m128i*>(
			&input.Data()[outWidthScalar * ( outFringe + 5u ) + outFringe] );

		const size_t inStep = pIn1 - pIn0;
		// no overlap in output
		const size_t outStep = ( pOut1 - pOut0 ) * 4u;

		// do middle lines
		for( ; pIn0 < pEnd; pIn0 += inStep,pIn1 += inStep,pLineEnd += inStep,
			pOut0 += outStep,pOut1 += outStep,pOut2 += outStep,pOut3 += outStep )
		{
			DoLine( pIn0,pIn1,pLineEnd,pOut0,pOut1,pOut2,pOut3 );
		}

		// do bottom line
		UpsizeEdge(
			reinterpret_cast<const __m128i*>(
			&hBuffer.Data()[inWidthScalar * ( hBuffer.GetHeight() - ( inFringe + 1u ) ) + inFringe] ),
			reinterpret_cast<const __m128i*>(
			&hBuffer.Data()[inWidthScalar * ( hBuffer.GetHeight() - inFringe ) - inFringe] ),
			reinterpret_cast<__m128i*>(
			&input.Data()[outWidthScalar * ( input.GetHeight() - ( outFringe + 2u ) ) + outFringe] ),
			reinterpret_cast<__m128i*>(
			&input.Data()[outWidthScalar * ( input.GetHeight() - ( outFringe + 1u ) ) + outFringe] ) );
	}
	void _UpsizeBlendPassX86()
	{
		Color* const pOutputBuffer = input.Data();
		const Color* const pInputBuffer = hBuffer.Data();
		const size_t inFringe = diameter / 2u;
		const size_t inWidth = hBuffer.GetWidth();
		const size_t inHeight = hBuffer.GetHeight();
		const size_t inBottom = inHeight - inFringe;
		const size_t inTopLeft = ( inWidth + 1u ) * inFringe;
		const size_t inTopRight = inWidth * ( inFringe + 1u ) - inFringe - 1u;
		const size_t inBottomLeft = inWidth * ( inBottom - 1u ) + inFringe;
		const size_t inBottomRight = inWidth * inBottom - inFringe - 1u;
		const size_t outFringe = GetFringeSize();
		const size_t outWidth = input.GetWidth();
		const size_t outRight = outWidth - outFringe;
		const size_t outBottom = input.GetHeight() - outFringe;
		const size_t outTopLeft = ( outWidth + 1u ) * outFringe;
		const size_t outTopRight = outWidth * ( outFringe + 1u ) - outFringe - 1u;
		const size_t outBottomLeft = outWidth * ( outBottom - 1u ) + outFringe;
		const size_t outBottomRight = outWidth * outBottom - outFringe - 1u;

		auto AddSaturate = []( Color* pOut,unsigned int inr,unsigned int ing,unsigned int inb )
		{
			*pOut = {
				unsigned char( std::min( inr + pOut->GetR(),255u ) ),
				unsigned char( std::min( ing + pOut->GetG(),255u ) ),
				unsigned char( std::min( inb + pOut->GetB(),255u ) )
			};
		};

		// top two rows
		{
			// top left block
			{
				const unsigned int r = pInputBuffer[inTopLeft].GetR();
				const unsigned int g = pInputBuffer[inTopLeft].GetG();
				const unsigned int b = pInputBuffer[inTopLeft].GetB();
				AddSaturate( &pOutputBuffer[outTopLeft],r,g,b );
				AddSaturate( &pOutputBuffer[outTopLeft + 1u],r,g,b );
				AddSaturate( &pOutputBuffer[outTopLeft + outWidth],r,g,b );
				AddSaturate( &pOutputBuffer[outTopLeft + outWidth + 1u],r,g,b );
			}

			// center
			{
				Color* const pOutUpper = &pOutputBuffer[outFringe * outWidth];
				Color* const pOutLower = &pOutputBuffer[( outFringe + 1u ) * outWidth];
				const Color* const pIn = &pInputBuffer[inFringe * inWidth];
				for( size_t x = outFringe + 2u; x < outRight - 2u; x += 4u )
				{
					const size_t baseX = ( x - 2u ) / 4u;
					const unsigned int r0 = pIn[baseX].GetR();
					const unsigned int g0 = pIn[baseX].GetG();
					const unsigned int b0 = pIn[baseX].GetB();
					const unsigned int r1 = pIn[baseX + 1u].GetR();
					const unsigned int g1 = pIn[baseX + 1u].GetG();
					const unsigned int b1 = pIn[baseX + 1u].GetB();
					{
						const unsigned int r = ( r0 * 224u + r1 * 32u ) / 256u;
						const unsigned int g = ( g0 * 224u + g1 * 32u ) / 256u;
						const unsigned int b = ( b0 * 224u + b1 * 32u ) / 256u;
						AddSaturate( &pOutUpper[x],r,g,b );
						AddSaturate( &pOutLower[x],r,g,b );
					}
					{
						const unsigned int r = ( r0 * 160u + r1 * 96u ) / 256u;
						const unsigned int g = ( g0 * 160u + g1 * 96u ) / 256u;
						const unsigned int b = ( b0 * 160u + b1 * 96u ) / 256u;
						AddSaturate( &pOutUpper[x + 1u],r,g,b );
						AddSaturate( &pOutLower[x + 1u],r,g,b );
					}
					{
						const unsigned int r = ( r0 * 96u + r1 * 160u ) / 256u;
						const unsigned int g = ( g0 * 96u + g1 * 160u ) / 256u;
						const unsigned int b = ( b0 * 96u + b1 * 160u ) / 256u;
						AddSaturate( &pOutUpper[x + 2u],r,g,b );
						AddSaturate( &pOutLower[x + 2u],r,g,b );
					}
					{
						const unsigned int r = ( r0 * 32u + r1 * 224u ) / 256u;
						const unsigned int g = ( g0 * 32u + g1 * 224u ) / 256u;
						const unsigned int b = ( b0 * 32u + b1 * 224u ) / 256u;
						AddSaturate( &pOutUpper[x + 3u],r,g,b );
						AddSaturate( &pOutLower[x + 3u],r,g,b );
					}
				}
			}

			// top right block
			{
				const unsigned int r = pInputBuffer[inTopRight].GetR();
				const unsigned int g = pInputBuffer[inTopRight].GetG();
				const unsigned int b = pInputBuffer[inTopRight].GetB();
				AddSaturate( &pOutputBuffer[outTopRight - 1u],r,g,b );
				AddSaturate( &pOutputBuffer[outTopRight],r,g,b );
				AddSaturate( &pOutputBuffer[outTopRight + outWidth - 1u],r,g,b );
				AddSaturate( &pOutputBuffer[outTopRight + outWidth],r,g,b );
			}
		}

		// center rows
		for( size_t y = outFringe + 2u; y < outBottom - 2u; y += 4u )
		{
			const size_t baseY = ( y - 2u ) / 4u;

			// first two pixels
			{
				const unsigned int r0 = pInputBuffer[baseY * inWidth + inFringe].GetR();
				const unsigned int g0 = pInputBuffer[baseY * inWidth + inFringe].GetG();
				const unsigned int b0 = pInputBuffer[baseY * inWidth + inFringe].GetB();
				const unsigned int r1 = pInputBuffer[( baseY + 1u ) * inWidth + inFringe].GetR();
				const unsigned int g1 = pInputBuffer[( baseY + 1u ) * inWidth + inFringe].GetG();
				const unsigned int b1 = pInputBuffer[( baseY + 1u ) * inWidth + inFringe].GetB();
				{
					const unsigned int r = ( r0 * 224u + r1 * 32u ) / 256u;
					const unsigned int g = ( g0 * 224u + g1 * 32u ) / 256u;
					const unsigned int b = ( b0 * 224u + b1 * 32u ) / 256u;
					AddSaturate( &pOutputBuffer[y * outWidth + outFringe],r,g,b );
					AddSaturate( &pOutputBuffer[y * outWidth + outFringe + 1u],r,g,b );
				}
				{
					const unsigned int r = ( r0 * 160u + r1 * 96u ) / 256u;
					const unsigned int g = ( g0 * 160u + g1 * 96u ) / 256u;
					const unsigned int b = ( b0 * 160u + b1 * 96u ) / 256u;
					AddSaturate( &pOutputBuffer[( y + 1u ) * outWidth + outFringe],r,g,b );
					AddSaturate( &pOutputBuffer[( y + 1u ) * outWidth + outFringe + 1u],r,g,b );
				}
				{
					const unsigned int r = ( r0 * 96u + r1 * 160u ) / 256u;
					const unsigned int g = ( g0 * 96u + g1 * 160u ) / 256u;
					const unsigned int b = ( b0 * 96u + b1 * 160u ) / 256u;
					AddSaturate( &pOutputBuffer[( y + 2u ) * outWidth + outFringe],r,g,b );
					AddSaturate( &pOutputBuffer[( y + 2u ) * outWidth + outFringe + 1u],r,g,b );
				}
				{
					const unsigned int r = ( r0 * 32u + r1 * 224u ) / 256u;
					const unsigned int g = ( g0 * 32u + g1 * 224u ) / 256u;
					const unsigned int b = ( b0 * 32u + b1 * 224u ) / 256u;
					AddSaturate( &pOutputBuffer[( y + 3u ) * outWidth + outFringe],r,g,b );
					AddSaturate( &pOutputBuffer[( y + 3u ) * outWidth + outFringe + 1u],r,g,b );
				}
			}

			// center pixels
			for( size_t x = outFringe + 2u; x < outRight - 2u; x += 4u )
			{
				const size_t baseX = ( x - 2u ) / 4u;
				const Color p0 = pInputBuffer[baseY * inWidth + baseX];
				const Color p1 = pInputBuffer[baseY * inWidth + baseX + 1u];
				const Color p2 = pInputBuffer[( baseY + 1u ) * inWidth + baseX];
				const Color p3 = pInputBuffer[( baseY + 1u ) * inWidth + baseX + 1u];
				const Color d0 = pOutputBuffer[y * outWidth + x];
				const Color d1 = pOutputBuffer[y * outWidth + x + 1u];
				const Color d2 = pOutputBuffer[y * outWidth + x + 2u];
				const Color d3 = pOutputBuffer[y * outWidth + x + 3u];
				const Color d4 = pOutputBuffer[( y + 1u ) * outWidth + x];
				const Color d5 = pOutputBuffer[( y + 1u ) * outWidth + x + 1u];
				const Color d6 = pOutputBuffer[( y + 1u ) * outWidth + x + 2u];
				const Color d7 = pOutputBuffer[( y + 1u ) * outWidth + x + 3u];
				const Color d8 = pOutputBuffer[( y + 2u ) * outWidth + x];
				const Color d9 = pOutputBuffer[( y + 2u ) * outWidth + x + 1u];
				const Color d10 = pOutputBuffer[( y + 2u ) * outWidth + x + 2u];
				const Color d11 = pOutputBuffer[( y + 2u ) * outWidth + x + 3u];
				const Color d12 = pOutputBuffer[( y + 3u ) * outWidth + x];
				const Color d13 = pOutputBuffer[( y + 3u ) * outWidth + x + 1u];
				const Color d14 = pOutputBuffer[( y + 3u ) * outWidth + x + 2u];
				const Color d15 = pOutputBuffer[( y + 3u ) * outWidth + x + 3u];


				unsigned int lr1 = p0.GetR() * 224u + p2.GetR() * 32u;
				unsigned int lg1 = p0.GetG() * 224u + p2.GetG() * 32u;
				unsigned int lb1 = p0.GetB() * 224u + p2.GetB() * 32u;
				unsigned int rr1 = p1.GetR() * 224u + p3.GetR() * 32u;
				unsigned int rg1 = p1.GetG() * 224u + p3.GetG() * 32u;
				unsigned int rb1 = p1.GetB() * 224u + p3.GetB() * 32u;

				pOutputBuffer[y * outWidth + x] =
				{ unsigned char( std::min( ( lr1 * 224u + rr1 * 32u ) / 65536u + d0.GetR(),255u ) ),
				unsigned char( std::min( ( lg1 * 224u + rg1 * 32u ) / 65536u + d0.GetG(),255u ) ),
				unsigned char( std::min( ( lb1 * 224u + rb1 * 32u ) / 65536u + d0.GetB(),255u ) ) };

				pOutputBuffer[y * outWidth + x + 1u] =
				{ unsigned char( std::min( ( lr1 * 160u + rr1 * 96u ) / 65536u + d1.GetR(),255u ) ),
				unsigned char( std::min( ( lg1 * 160u + rg1 * 96u ) / 65536u + d1.GetG(),255u ) ),
				unsigned char( std::min( ( lb1 * 160u + rb1 * 96u ) / 65536u + d1.GetB(),255u ) ) };

				pOutputBuffer[y * outWidth + x + 2u] =
				{ unsigned char( std::min( ( lr1 * 96u + rr1 * 160u ) / 65536u + d2.GetR(),255u ) ),
				unsigned char( std::min( ( lg1 * 96u + rg1 * 160u ) / 65536u + d2.GetG(),255u ) ),
				unsigned char( std::min( ( lb1 * 96u + rb1 * 160u ) / 65536u + d2.GetB(),255u ) ) };

				pOutputBuffer[y * outWidth + x + 3u] =
				{ unsigned char( std::min( ( lr1 * 32u + rr1 * 224u ) / 65536u + d3.GetR(),255u ) ),
				unsigned char( std::min( ( lg1 * 32u + rg1 * 224u ) / 65536u + d3.GetG(),255u ) ),
				unsigned char( std::min( ( lb1 * 32u + rb1 * 224u ) / 65536u + d3.GetB(),255u ) ) };

				lr1 = p0.GetR() * 160u + p2.GetR() * 96u;
				lg1 = p0.GetG() * 160u + p2.GetG() * 96u;
				lb1 = p0.GetB() * 160u + p2.GetB() * 96u;
				rr1 = p1.GetR() * 160u + p3.GetR() * 96u;
				rg1 = p1.GetG() * 160u + p3.GetG() * 96u;
				rb1 = p1.GetB() * 160u + p3.GetB() * 96u;

				pOutputBuffer[( y + 1u ) * outWidth + x] =
				{ unsigned char( std::min( ( lr1 * 224u + rr1 * 32u ) / 65536u + d4.GetR(),255u ) ),
				unsigned char( std::min( ( lg1 * 224u + rg1 * 32u ) / 65536u + d4.GetG(),255u ) ),
				unsigned char( std::min( ( lb1 * 224u + rb1 * 32u ) / 65536u + d4.GetB(),255u ) ) };

				pOutputBuffer[( y + 1u ) * outWidth + x + 1u] =
				{ unsigned char( std::min( ( lr1 * 160u + rr1 * 96u ) / 65536u + d5.GetR(),255u ) ),
				unsigned char( std::min( ( lg1 * 160u + rg1 * 96u ) / 65536u + d5.GetG(),255u ) ),
				unsigned char( std::min( ( lb1 * 160u + rb1 * 96u ) / 65536u + d5.GetB(),255u ) ) };

				pOutputBuffer[( y + 1u ) * outWidth + x + 2u] =
				{ unsigned char( std::min( ( lr1 * 96u + rr1 * 160u ) / 65536u + d6.GetR(),255u ) ),
				unsigned char( std::min( ( lg1 * 96u + rg1 * 160u ) / 65536u + d6.GetG(),255u ) ),
				unsigned char( std::min( ( lb1 * 96u + rb1 * 160u ) / 65536u + d6.GetB(),255u ) ) };

				pOutputBuffer[( y + 1u ) * outWidth + x + 3u] =
				{ unsigned char( std::min( ( lr1 * 32u + rr1 * 224u ) / 65536u + d7.GetR(),255u ) ),
				unsigned char( std::min( ( lg1 * 32u + rg1 * 224u ) / 65536u + d7.GetG(),255u ) ),
				unsigned char( std::min( ( lb1 * 32u + rb1 * 224u ) / 65536u + d7.GetB(),255u ) ) };

				lr1 = p0.GetR() * 96u + p2.GetR() * 160u;
				lg1 = p0.GetG() * 96u + p2.GetG() * 160u;
				lb1 = p0.GetB() * 96u + p2.GetB() * 160u;
				rr1 = p1.GetR() * 96u + p3.GetR() * 160u;
				rg1 = p1.GetG() * 96u + p3.GetG() * 160u;
				rb1 = p1.GetB() * 96u + p3.GetB() * 160u;

				pOutputBuffer[( y + 2u ) * outWidth + x] =
				{ unsigned char( std::min( ( lr1 * 224u + rr1 * 32u ) / 65536u + d8.GetR(),255u ) ),
				unsigned char( std::min( ( lg1 * 224u + rg1 * 32u ) / 65536u + d8.GetG(),255u ) ),
				unsigned char( std::min( ( lb1 * 224u + rb1 * 32u ) / 65536u + d8.GetB(),255u ) ) };

				pOutputBuffer[( y + 2u ) * outWidth + x + 1u] =
				{ unsigned char( std::min( ( lr1 * 160u + rr1 * 96u ) / 65536u + d9.GetR(),255u ) ),
				unsigned char( std::min( ( lg1 * 160u + rg1 * 96u ) / 65536u + d9.GetG(),255u ) ),
				unsigned char( std::min( ( lb1 * 160u + rb1 * 96u ) / 65536u + d9.GetB(),255u ) ) };

				pOutputBuffer[( y + 2u ) * outWidth + x + 2u] =
				{ unsigned char( std::min( ( lr1 * 96u + rr1 * 160u ) / 65536u + d10.GetR(),255u ) ),
				unsigned char( std::min( ( lg1 * 96u + rg1 * 160u ) / 65536u + d10.GetG(),255u ) ),
				unsigned char( std::min( ( lb1 * 96u + rb1 * 160u ) / 65536u + d10.GetB(),255u ) ) };

				pOutputBuffer[( y + 2u ) * outWidth + x + 3u] =
				{ unsigned char( std::min( ( lr1 * 32u + rr1 * 224u ) / 65536u + d11.GetR(),255u ) ),
				unsigned char( std::min( ( lg1 * 32u + rg1 * 224u ) / 65536u + d11.GetG(),255u ) ),
				unsigned char( std::min( ( lb1 * 32u + rb1 * 224u ) / 65536u + d11.GetB(),255u ) ) };

				lr1 = p0.GetR() * 32u + p2.GetR() * 224u;
				lg1 = p0.GetG() * 32u + p2.GetG() * 224u;
				lb1 = p0.GetB() * 32u + p2.GetB() * 224u;
				rr1 = p1.GetR() * 32u + p3.GetR() * 224u;
				rg1 = p1.GetG() * 32u + p3.GetG() * 224u;
				rb1 = p1.GetB() * 32u + p3.GetB() * 224u;

				pOutputBuffer[( y + 3u ) * outWidth + x] =
				{ unsigned char( std::min( ( lr1 * 224u + rr1 * 32u ) / 65536u + d12.GetR(),255u ) ),
				unsigned char( std::min( ( lg1 * 224u + rg1 * 32u ) / 65536u + d12.GetG(),255u ) ),
				unsigned char( std::min( ( lb1 * 224u + rb1 * 32u ) / 65536u + d12.GetB(),255u ) ) };

				pOutputBuffer[( y + 3u ) * outWidth + x + 1u] =
				{ unsigned char( std::min( ( lr1 * 160u + rr1 * 96u ) / 65536u + d13.GetR(),255u ) ),
				unsigned char( std::min( ( lg1 * 160u + rg1 * 96u ) / 65536u + d13.GetG(),255u ) ),
				unsigned char( std::min( ( lb1 * 160u + rb1 * 96u ) / 65536u + d13.GetB(),255u ) ) };

				pOutputBuffer[( y + 3u ) * outWidth + x + 2u] =
				{ unsigned char( std::min( ( lr1 * 96u + rr1 * 160u ) / 65536u + d14.GetR(),255u ) ),
				unsigned char( std::min( ( lg1 * 96u + rg1 * 160u ) / 65536u + d14.GetG(),255u ) ),
				unsigned char( std::min( ( lb1 * 96u + rb1 * 160u ) / 65536u + d14.GetB(),255u ) ) };

				pOutputBuffer[( y + 3u ) * outWidth + x + 3u] =
				{ unsigned char( std::min( ( lr1 * 32u + rr1 * 224u ) / 65536u + d15.GetR(),255u ) ),
				unsigned char( std::min( ( lg1 * 32u + rg1 * 224u ) / 65536u + d15.GetG(),255u ) ),
				unsigned char( std::min( ( lb1 * 32u + rb1 * 224u ) / 65536u + d15.GetB(),255u ) ) };
			}

			// last two pixels
			{
				const unsigned int r0 = pInputBuffer[( baseY + 1u ) * inWidth - inFringe - 2u].GetR();
				const unsigned int g0 = pInputBuffer[( baseY + 1u ) * inWidth - inFringe - 2u].GetG();
				const unsigned int b0 = pInputBuffer[( baseY + 1u ) * inWidth - inFringe - 2u].GetB();
				const unsigned int r1 = pInputBuffer[( baseY + 2u ) * inWidth - inFringe - 1u].GetR();
				const unsigned int g1 = pInputBuffer[( baseY + 2u ) * inWidth - inFringe - 1u].GetG();
				const unsigned int b1 = pInputBuffer[( baseY + 2u ) * inWidth - inFringe - 1u].GetB();
				{
					const unsigned int r = ( r0 * 224u + r1 * 32u ) / 256u;
					const unsigned int g = ( g0 * 224u + g1 * 32u ) / 256u;
					const unsigned int b = ( b0 * 224u + b1 * 32u ) / 256u;
					AddSaturate( &pOutputBuffer[( y + 1 ) * outWidth - outFringe - 2u],r,g,b );
					AddSaturate( &pOutputBuffer[( y + 1 ) * outWidth - outFringe - 1u],r,g,b );
				}
				{
					const unsigned int r = ( r0 * 160u + r1 * 96u ) / 256u;
					const unsigned int g = ( g0 * 160u + g1 * 96u ) / 256u;
					const unsigned int b = ( b0 * 160u + b1 * 96u ) / 256u;
					AddSaturate( &pOutputBuffer[( y + 2 ) * outWidth - outFringe - 2u],r,g,b );
					AddSaturate( &pOutputBuffer[( y + 2 ) * outWidth - outFringe - 1u],r,g,b );
				}
				{
					const unsigned int r = ( r0 * 96u + r1 * 160u ) / 256u;
					const unsigned int g = ( g0 * 96u + g1 * 160u ) / 256u;
					const unsigned int b = ( b0 * 96u + b1 * 160u ) / 256u;
					AddSaturate( &pOutputBuffer[( y + 3 ) * outWidth - outFringe - 2u],r,g,b );
					AddSaturate( &pOutputBuffer[( y + 3 ) * outWidth - outFringe - 1u],r,g,b );
				}
				{
					const unsigned int r = ( r0 * 32u + r1 * 224u ) / 256u;
					const unsigned int g = ( g0 * 32u + g1 * 224u ) / 256u;
					const unsigned int b = ( b0 * 32u + b1 * 224u ) / 256u;
					AddSaturate( &pOutputBuffer[( y + 4 ) * outWidth - outFringe - 2u],r,g,b );
					AddSaturate( &pOutputBuffer[( y + 4 ) * outWidth - outFringe - 1u],r,g,b );
				}
			}
		}

		// bottom two rows
		{
			// bottom left block
			{
				const unsigned int r = pInputBuffer[inBottomLeft].GetR();
				const unsigned int g = pInputBuffer[inBottomLeft].GetG();
				const unsigned int b = pInputBuffer[inBottomLeft].GetB();
				AddSaturate( &pOutputBuffer[outBottomLeft - outWidth],r,g,b );
				AddSaturate( &pOutputBuffer[outBottomLeft - outWidth + 1u],r,g,b );
				AddSaturate( &pOutputBuffer[outBottomLeft],r,g,b );
				AddSaturate( &pOutputBuffer[outBottomLeft + 1u],r,g,b );
			}

			// center
			{
				Color* const pOutUpper = &pOutputBuffer[( outBottom - 2u ) * outWidth];
				Color* const pOutLower = &pOutputBuffer[( outBottom - 1u ) * outWidth];
				const Color* const pIn = &pInputBuffer[( inBottom - 1u ) * inWidth];
				for( size_t x = outFringe + 2u; x < outRight - 2u; x += 4u )
				{
					const size_t baseX = ( x - 2u ) / 4u;
					const unsigned int r0 = pIn[baseX].GetR();
					const unsigned int g0 = pIn[baseX].GetG();
					const unsigned int b0 = pIn[baseX].GetB();
					const unsigned int r1 = pIn[baseX + 1u].GetR();
					const unsigned int g1 = pIn[baseX + 1u].GetG();
					const unsigned int b1 = pIn[baseX + 1u].GetB();
					{
						const unsigned int r = ( r0 * 224u + r1 * 32u ) / 256u;
						const unsigned int g = ( g0 * 224u + g1 * 32u ) / 256u;
						const unsigned int b = ( b0 * 224u + b1 * 32u ) / 256u;
						AddSaturate( &pOutUpper[x],r,g,b );
						AddSaturate( &pOutLower[x],r,g,b );
					}
					{
						const unsigned int r = ( r0 * 160u + r1 * 96u ) / 256u;
						const unsigned int g = ( g0 * 160u + g1 * 96u ) / 256u;
						const unsigned int b = ( b0 * 160u + b1 * 96u ) / 256u;
						AddSaturate( &pOutUpper[x + 1u],r,g,b );
						AddSaturate( &pOutLower[x + 1u],r,g,b );
					}
					{
						const unsigned int r = ( r0 * 96u + r1 * 160u ) / 256u;
						const unsigned int g = ( g0 * 96u + g1 * 160u ) / 256u;
						const unsigned int b = ( b0 * 96u + b1 * 160u ) / 256u;
						AddSaturate( &pOutUpper[x + 2u],r,g,b );
						AddSaturate( &pOutLower[x + 2u],r,g,b );
					}
					{
						const unsigned int r = ( r0 * 32u + r1 * 224u ) / 256u;
						const unsigned int g = ( g0 * 32u + g1 * 224u ) / 256u;
						const unsigned int b = ( b0 * 32u + b1 * 224u ) / 256u;
						AddSaturate( &pOutUpper[x + 3u],r,g,b );
						AddSaturate( &pOutLower[x + 3u],r,g,b );
					}
				}
			}

			// bottom right block
			{
				const unsigned int r = pInputBuffer[inBottomRight].GetR();
				const unsigned int g = pInputBuffer[inBottomRight].GetG();
				const unsigned int b = pInputBuffer[inBottomRight].GetB();
				AddSaturate( &pOutputBuffer[outBottomRight - outWidth - 1u],r,g,b );
				AddSaturate( &pOutputBuffer[outBottomRight - outWidth],r,g,b );
				AddSaturate( &pOutputBuffer[outBottomRight - 1u],r,g,b );
				AddSaturate( &pOutputBuffer[outBottomRight],r,g,b );
			}
		}
	}
private:
	static const unsigned int diameter = 16u;
	__declspec( align( 16 ) ) unsigned char kernel[diameter];
	unsigned int divisorKernel = 512u; // 4x overdrive
	Surface& input;
	Surface hBuffer;
	Surface vBuffer;
	// function pointers
	std::function<void( BloomProcessor* )> DownsizePassFunc;
	std::function<void( BloomProcessor* )> HorizontalPassFunc;
	std::function<void( BloomProcessor* )> VerticalPassFunc;
	std::function<void( BloomProcessor* )> UpsizeBlendPassFunc;

	// MT members
	// we use unique_ptr because UpsizeWorker is neither movable nor copyable
	std::vector<std::unique_ptr<UpsizeWorker>> workerPtrs;
	std::condition_variable cvBoss;
	std::mutex mutexBoss;
	size_t nActiveThreads = 0u;
};