/*
 *  emuband.cpp
 *  EMUFormat
 *
 *  Created by Sam Gillingham on 26/03/2024.
 *  Copyright 2024 EMUFormat. All rights reserved.
 *
 *  This file is part of EMUFormat.
 *
 *  Permission is hereby granted, free of charge, to any person 
 *  obtaining a copy of this software and associated documentation 
 *  files (the "Software"), to deal in the Software without restriction, 
 *  including without limitation the rights to use, copy, modify, 
 *  merge, publish, distribute, sublicense, and/or sell copies of the 
 *  Software, and to permit persons to whom the Software is furnished 
 *  to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be 
 *  included in all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
 *  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES 
 *  OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. 
 *  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR 
 *  ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF 
 *  CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION 
 *  WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <limits> 
#include <cmath>

#include "emuband.h"
#include "emucompress.h"

EMURasterBand::EMURasterBand(EMUDataset *pDataset, int nBandIn, GDALDataType eType, 
        bool bThematic, const std::shared_ptr<std::mutex>& other)
{
    poDS = pDataset;
    nBlockXSize = 512;
    nBlockYSize = 512;
    nBand = nBandIn;
    eDataType = eType;
    m_bNoDataSet = false;
    m_nNoData = 0;
    m_bThematic = bThematic;

    nRasterXSize = pDataset->GetRasterXSize();          // ask the dataset for the total image size
    nRasterYSize = pDataset->GetRasterYSize();
    
    m_dMin = std::numeric_limits<double>::quiet_NaN();
    m_dMax = std::numeric_limits<double>::quiet_NaN();
    m_dMean = std::numeric_limits<double>::quiet_NaN();
    m_dStdDev = std::numeric_limits<double>::quiet_NaN();
    m_dMedian = std::numeric_limits<double>::quiet_NaN();
    m_dMode = std::numeric_limits<double>::quiet_NaN();
    m_papszMetadataList = nullptr;
    
    m_mutex = other;
}

EMURasterBand::~EMURasterBand()
{
    
}

CPLErr EMURasterBand::IReadBlock(int nBlockXOff, int nBlockYOff, void *pData)
{
    if(poDS->GetAccess() != GA_Update)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
             "The EMU driver only supports reading when open in readonly mode");
        return CE_Failure;
    }

    const std::lock_guard<std::mutex> lock(*m_mutex);

    EMUDataset *poEMUDS = cpl::down_cast<EMUDataset *>(poDS);

    EMUTileValue val;
    try
    {
        val = poEMUDS->getTileOffset(0, nBand, nBlockXOff, nBlockYOff);
    }
    catch(const std::out_of_range& oor)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                "Couldn't find index for block %d %d.",
                nBlockXOff, nBlockXOff);
        return CE_Failure;
    }

    // seek to where this block starts    
    VSIFSeekL(poEMUDS->m_fp, val.offset, SEEK_SET);

    uint8_t compression;
    VSIFReadL(&compression, sizeof(compression), 1, poEMUDS->m_fp);
    
    // we need to work out whether we are partial
    int nXValid, nYValid;
    CPLErr err = GetActualBlockSize(nBlockXOff, nBlockYOff, &nXValid, &nYValid);
    if( err != CE_None)
        return err;
    
    if( (nXValid != nBlockXSize) || (nYValid != nBlockYSize) ) 
    {
        // partial. GDAL expects a full block so let's read the 
        // partial and expand to fit full block.
        int typeSize = GDALGetDataTypeSize(eDataType) / 8;
        Bytef *pSubData = static_cast<Bytef*>(CPLMalloc(val.size));
        VSIFReadL(pSubData, val.size, 1, poEMUDS->m_fp);
        
        Bytef *pUncompressed = static_cast<Bytef*>(CPLMalloc(val.uncompressedSize)); 
        doUncompression(compression, pSubData, val.size, pUncompressed, val.uncompressedSize);
        
        char *pDstData = static_cast<char*>(pData);
        int nSrcIdx = 0, nDstIdx = 0;
        for( int nRow = 0; nRow < nYValid; nRow++ )
        {
            memcpy(&pDstData[nDstIdx], &pUncompressed[nSrcIdx], nXValid * typeSize);
            nSrcIdx += (nXValid * typeSize);
            nDstIdx += (nBlockXSize * typeSize);
        }
        
        CPLFree(pUncompressed);
        CPLFree(pSubData);
    }
    else
    {
        // full block. Just read.
        Bytef *pSubData = static_cast<Bytef*>(CPLMalloc(val.size));
        VSIFReadL(pSubData, val.size, 1, poEMUDS->m_fp);
        // uncompress directly into GDAL's buffer
        doUncompression(compression, pSubData, val.size, static_cast<Bytef*>(pData), val.uncompressedSize);
        CPLFree(pSubData);        
    }

    return CE_None;
}

CPLErr EMURasterBand::IWriteBlock(int nBlockXOff, int nBlockYOff, void *pData)
{
    const std::lock_guard<std::mutex> lock(*m_mutex);

    EMUDataset *poEMUDS = cpl::down_cast<EMUDataset *>(poDS);
    
    // GDAL deals in blocks - if we are at the end of a row
    // we need to adjust the amount read so we don't go over the edge
    int nXValid, nYValid;
    CPLErr err = GetActualBlockSize(nBlockXOff, nBlockYOff, &nXValid, &nYValid);
    if( err != CE_None)
        return err;
    
    vsi_l_offset tileOffset = VSIFTellL(poEMUDS->m_fp);
    int typeSize = GDALGetDataTypeSize(eDataType) / 8;

    // TODO: set compression
    uint8_t compression = COMPRESSION_ZLIB;
    VSIFWriteL(&compression, sizeof(compression), 1, poEMUDS->m_fp);

    size_t uncompressedSize = (nXValid * nYValid) * typeSize;
    size_t compressedSize = uncompressedSize;
    
    if( (nXValid != nBlockXSize) || (nYValid != nBlockYSize) ) 
    {
        // a partial block. They actually give the full block so we must subset
        Bytef *pSubData = static_cast<Bytef*>(CPLMalloc(uncompressedSize));
        Bytef *pSrcData = static_cast<Bytef*>(pData);
        int nSrcIdx = 0, nDstIdx = 0;
        for( int nRow = 0; nRow < nYValid; nRow++ )
        {
            memcpy(&pSubData[nDstIdx], &pSrcData[nSrcIdx], nXValid * typeSize);
            nSrcIdx += (nBlockXSize * typeSize);
            nDstIdx += (nXValid * typeSize);
        }
        
        AccumulateData(pSubData, nXValid * nYValid, nXValid);
        
        bool bFree;
        Bytef *pCompressed = doCompression(compression, pSubData, uncompressedSize, &compressedSize, &bFree);
        VSIFWriteL(pCompressed, compressedSize, 1, poEMUDS->m_fp);
        CPLFree(pSubData);
        if( bFree ) 
        {
            CPLFree(pCompressed);
        }
    }
    else
    {
        // full data
        AccumulateData(pData, nXValid * nYValid, nXValid);

        bool bFree;
        void *pCompressed = doCompression(compression, static_cast<Bytef*>(pData), uncompressedSize, &compressedSize, &bFree);
        VSIFWriteL(pCompressed, compressedSize, 1, poEMUDS->m_fp);
        if( bFree ) 
        {
            CPLFree(pCompressed);
        }
    }
    
    // update map
    poEMUDS->setTileOffset(0, nBand, nBlockXOff, nBlockYOff, tileOffset, compressedSize, uncompressedSize);

    // TODO: read data to work out stats and pyramid layers 
    
    return CE_None;
}

double EMURasterBand::GetNoDataValue(int *pbSuccess/* = nullptr*/)
{
    if( pbSuccess )
        *pbSuccess = m_bNoDataSet;
    return m_nNoData;
}

int64_t EMURasterBand::GetNoDataValueAsInt64(int *pbSuccess/* = nullptr*/)
{
    if( pbSuccess )
        *pbSuccess = m_bNoDataSet;
    return m_nNoData;
}

uint64_t EMURasterBand::GetNoDataValueAsUInt64(int *pbSuccess/* = nullptr*/)
{
    if( pbSuccess )
        *pbSuccess = m_bNoDataSet;
    return m_nNoData;
}

CPLErr EMURasterBand::SetNoDataValue(double dfNoData)
{
    m_nNoData = dfNoData;
    m_bNoDataSet = true;
    return CE_None;
}

CPLErr EMURasterBand::SetNoDataValueAsInt64(int64_t nNoData)
{
    m_nNoData = nNoData;
    m_bNoDataSet = true;
    return CE_None;
}

CPLErr EMURasterBand::SetNoDataValueAsUInt64(uint64_t nNoData)
{
    m_nNoData = nNoData;
    m_bNoDataSet = true;
    return CE_None;
}

CPLErr EMURasterBand::DeleteNoDataValue()
{
    m_bNoDataSet = false;   
    return CE_None;
}

void EMURasterBand::AccumulateData(void *pData, size_t nLength, size_t nXValid)
{
    switch(eDataType)
    {
        case GDT_Byte:
            AccumulateDataForType<uint8_t>(pData, nLength, nXValid);
            break;
        case GDT_UInt16:
            AccumulateDataForType<uint16_t>(pData, nLength, nXValid);
            break;
        case GDT_Int16:
            AccumulateDataForType<int16_t>(pData, nLength, nXValid);
            break;
        default:
            fprintf(stderr, "Unknown pixel type\n");
            break;
    }
}

// accumulate the raster data into the histogram
template<class T>
void EMURasterBand::AccumulateDataForType(void *pData, size_t nLength, size_t nXValid)
{
    T *pTypeData = static_cast<T*>(pData);
    for( size_t n = 0; n < nLength; n++ )
    {
        T val = pTypeData[n];
        if( m_bNoDataSet && (val == m_nNoData))
        {
            continue;
        }
        auto search = m_histogram.find(val);
        if( search != m_histogram.end() )
        {
            // update 
            m_histogram[val] = m_histogram[val]++;
        }
        else
        {
            // set 
            m_histogram[val] = 1;
        }
    }
}

void EMURasterBand::EstimateStatsFromHistogram()
{
    // histogram stored as a std::map so keys should be sorted
    // (important for median, below)    
    double dSum = 0;
    uint64_t pixCount = 0;
    uint32_t nCountAtMode = 0;
    for( auto i = m_histogram.begin(); i != m_histogram.end(); i++)
    {
        if( std::isnan(m_dMin ) || (i->first < m_dMin) )
        {
            m_dMin = i->first;
        }
        if( std::isnan(m_dMax ) || (i->first > m_dMax) )
        {
            m_dMax = i->first;
        }
        if( std::isnan(m_dMode) || (i->second > nCountAtMode))
        {
            m_dMode = i->first;
            nCountAtMode = i->second;
        }
        dSum += (i->first * i->second);
        pixCount += i->second;
    }
    
    m_dMean = dSum / pixCount;
    double dSumSq = 0;
    for( auto i = m_histogram.begin(); i != m_histogram.end(); i++)
    {
        dSumSq += (i->second * pow(i->first - m_dMean, 2));
    }
    double dVariance = dSumSq / pixCount;
    m_dStdDev = sqrt(dVariance);
    
    uint64_t nCountAtMedian = pixCount / 2;
    pixCount = 0;
    uint32_t lastVal = 0;
    for( auto i = m_histogram.begin(); i != m_histogram.end(); i++)
    {
        if( pixCount > nCountAtMedian)
        {
            m_dMean = lastVal;
            break;
        }
        pixCount += i->second;
        lastVal = i->first;
    }    
}

double EMURasterBand::GetMinimum(int *pbSuccess)
{
    if(poDS->GetAccess() == GA_Update)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
             "The EMU driver only supports retrieving stats when the file is open in read only mode.");
        *pbSuccess = 0;
        return 0.0;
    }
    *pbSuccess = 1;
    return m_dMin;
}

double EMURasterBand::GetMaximum(int *pbSuccess)
{
    if(poDS->GetAccess() == GA_Update)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
             "The EMU driver only supports retrieving stats when the file is open in read only mode.");
        *pbSuccess = 0;
        return 0.0;
    }
    *pbSuccess = 1;
    return m_dMax;
}

CPLErr EMURasterBand::ComputeRasterMinMax(int bApproxOK, double *adfMinMax)
{
    if(poDS->GetAccess() == GA_Update)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
             "The EMU driver only supports retrieving stats when the file is open in read only mode.");
        return CE_Failure;
    }
    adfMinMax[0] = m_dMin;
    adfMinMax[1] = m_dMax;
    return CE_None;
}

CPLErr EMURasterBand::GetStatistics(int bApproxOK, int bForce, double *pdfMin,
                                 double *pdfMax, double *pdfMean,
                                 double *padfStdDev)
{
    // ignore bApproxOK and bForce
    
    if(poDS->GetAccess() == GA_Update)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
             "The EMU driver only supports retrieving stats when the file is open in read only mode.");
        return CE_Failure;
    }
    
    *pdfMin = m_dMin;
    *pdfMax = m_dMax;
    *pdfMean = m_dMean;
    *padfStdDev = m_dStdDev;
    return CE_None;
}

CPLErr EMURasterBand::ComputeStatistics(int bApproxOK, double *pdfMin, double *pdfMax,
		double *pdfMean, double *pdfStdDev,	GDALProgressFunc pfnProgress,
		void *pProgressData) 		
{
    // ignore bApproxOK and bForce
    
    if(poDS->GetAccess() == GA_Update)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
             "The EMU driver only supports retrieving stats when the file is open in read only mode.");
        return CE_Failure;
    }
    
    *pdfMin = m_dMin;
    *pdfMax = m_dMax;
    *pdfMean = m_dMean;
    *pdfStdDev = m_dStdDev;
    return CE_None;
}

CPLErr EMURasterBand::SetStatistics(double dfMin, double dfMax, double dfMean,	
	    double dfStdDev)
{
    CPLError(CE_Failure, CPLE_NotSupported,
         "The EMU driver only supports calculating stats itself.");
    return CE_Failure;
}


CPLErr EMURasterBand::SetMetadataItem(const char *pszName, const char *pszValue, 
	   const char *pszDomain)
{
    if(poDS->GetAccess() != GA_Update)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
             "The EMU driver only supports setting metadata on creation");
        return CE_Failure;
    }

    if( EQUAL( pszName, "LAYER_TYPE" ) )
    {
        if( EQUAL( pszValue, "athematic" ) )
        {
            m_bThematic = false;
        }
        else
        {
            m_bThematic = false;
        }
        return CE_None;
    }
    else
    {
        CPLError(CE_Failure, CPLE_NotSupported,
             "The EMU driver only supports calculating stats itself.");
        return CE_Failure;
    }    
}

const char *EMURasterBand::GetMetadataItem (const char *pszName, const char *pszDomain)
{
    // TODO
    return nullptr;
}
