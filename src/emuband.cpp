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
    : m_rat(pDataset, this, other)
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

    m_dHistMin = std::numeric_limits<double>::quiet_NaN();
    m_dHistMax = std::numeric_limits<double>::quiet_NaN();
    m_dHistStep = std::numeric_limits<double>::quiet_NaN();
    m_eHistBinFunc = direct;
    m_nHistNBins = 0;
    m_pHistogram = nullptr;

    m_papszMetadataList = nullptr;
    UpdateMetadataList();
    
    m_mutex = other;
}

EMURasterBand::~EMURasterBand()
{
    CSLDestroy(m_papszMetadataList);
    if( m_pHistogram != nullptr)
    {
        CPLFree(m_pHistogram);
    }
}

CPLErr EMURasterBand::IReadBlock(int nBlockXOff, int nBlockYOff, void *pData)
{
    if(poDS->GetAccess() == GA_Update)
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
    size_t compressedSize = uncompressedSize + 100;
    
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
            m_dMedian = lastVal;
            break;
        }
        pixCount += i->second;
        lastVal = i->first;
    }
    
    // now the actual histogram
    if( eDataType == GDT_Byte)
    {
        m_dHistMin = 0;
        m_dHistMax = 256;
        m_dHistStep = 1.0;
        m_nHistNBins = 256;
        m_eHistBinFunc = direct;
    }
    else if(GetThematic())
    {
        m_dHistMin = 0;
        m_dHistMax = uint64_t(ceil(m_dMax));
        m_dHistStep = 1.0;
        m_nHistNBins = m_dHistMax + 1;
        m_eHistBinFunc = direct;
    }
    else if((eDataType == GDT_Int16) || (eDataType == GDT_UInt16) || (eDataType == GDT_Int32) ||
        (eDataType == GDT_UInt32) || (eDataType == GDT_Int64) || (eDataType == GDT_UInt64))
    {
        uint64_t nHistRange = uint64_t(ceil(m_dMax) - floor(m_dMin));
        m_dHistMin = m_dMin;
        m_dHistMax = m_dMax;
        if( nHistRange <= 256 )
        {
            m_nHistNBins = nHistRange;
            m_dHistStep = 1.0;
            m_eHistBinFunc = direct;
        }
        else
        {
            m_nHistNBins = 256;
            m_eHistBinFunc = linear;
            m_dHistStep = (m_dHistMax - m_dHistMin) / m_nHistNBins;
        }
    }
    else if((eDataType == GDT_Float32) || (eDataType == GDT_Float64))
    {
        m_dHistMin = m_dMin;
        m_dHistMax = m_dMax;
        m_nHistNBins = 256;
        m_eHistBinFunc = linear;
        if( m_dHistMin == m_dHistMax )
        {
            m_dHistMax = m_dHistMin + 0.5;
            m_nHistNBins = 1;
        }
        m_dHistStep = (m_dHistMax - m_dHistMin) / m_nHistNBins;
    }
    
    m_pHistogram = static_cast<uint64_t*>(CPLCalloc(sizeof(uint64_t), m_nHistNBins));
    for( auto i = m_histogram.begin(); i != m_histogram.end(); i++)
    {
        uint64_t nBin = (i->first - m_dHistMin) / m_dHistStep;
        m_pHistogram += i->second;
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

void EMURasterBand::UpdateMetadataList()
{
    if( m_bThematic )
    {
        m_papszMetadataList = CSLSetNameValue(m_papszMetadataList, "LAYER_TYPE", "thematic" );        
    }
    else
    {
        m_papszMetadataList = CSLSetNameValue(m_papszMetadataList, "LAYER_TYPE", "athematic" );        
    }
    
    CPLString osWorkingResult;
    osWorkingResult.Printf( "%f", m_dMin);
    m_papszMetadataList = CSLSetNameValue(m_papszMetadataList, "STATISTICS_MINIMUM", osWorkingResult);

    osWorkingResult.Printf( "%f", m_dMax);
    m_papszMetadataList = CSLSetNameValue(m_papszMetadataList, "STATISTICS_MAXIMUM", osWorkingResult);

    osWorkingResult.Printf( "%f", m_dMean);
    m_papszMetadataList = CSLSetNameValue(m_papszMetadataList, "STATISTICS_MEAN", osWorkingResult);

    osWorkingResult.Printf( "%f", m_dMedian);
    m_papszMetadataList = CSLSetNameValue(m_papszMetadataList, "STATISTICS_MEDIAN", osWorkingResult);

    osWorkingResult.Printf( "%f", m_dStdDev);
    m_papszMetadataList = CSLSetNameValue(m_papszMetadataList, "STATISTICS_STDDEV", osWorkingResult);

    osWorkingResult.Printf( "%f", m_dMode);
    m_papszMetadataList = CSLSetNameValue(m_papszMetadataList, "STATISTICS_MODE", osWorkingResult);
    
    m_papszMetadataList = CSLSetNameValue(m_papszMetadataList, "STATISTICS_SKIPFACTORX", "1");
    m_papszMetadataList = CSLSetNameValue(m_papszMetadataList, "STATISTICS_SKIPFACTORY", "1");

    // TODO: STATISTICS_HISTO*
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
        UpdateMetadataList();
        return CE_None;
    }
    else
    {
        CPLError(CE_Failure, CPLE_NotSupported,
             "The EMU driver only supports calculating stats itself.");
        return CE_Failure;
    }    
}

const char *EMURasterBand::GetMetadataItem(const char *pszName, const char *pszDomain)
{
    // only deal with 'default' domain - no geolocation etc
    if( ( pszDomain != nullptr ) && ( *pszDomain != '\0' ) )
        return nullptr;
    // get it out of the CSLStringList so we can be sure it is persistant
    return CSLFetchNameValue(m_papszMetadataList, pszName);
}

// get all the metadata as a CSLStringList - not thread safe
char **EMURasterBand::GetMetadata(const char *pszDomain)
{
    // only deal with 'default' domain - no geolocation etc
    if( ( pszDomain != nullptr ) && ( *pszDomain != '\0' ) )
        return nullptr;

    // conveniently we already have it in this format
    return m_papszMetadataList; 
}

// set the metadata as a CSLStringList
CPLErr EMURasterBand::SetMetadata(char **papszMetadata, const char *pszDomain)
{
    // only deal with 'default' domain - no geolocation etc
    if( ( pszDomain != nullptr ) && ( *pszDomain != '\0' ) )
        return CE_Failure;
    int nIndex = 0;
    char *pszName;
    const char *pszValue;
    // iterate through each one
    while( papszMetadata[nIndex] != nullptr )
    {
        pszValue = CPLParseNameValue( papszMetadata[nIndex], &pszName );

        // it is LAYER_TYPE?
        if( EQUAL( pszName, "LAYER_TYPE" ) )
        {
            if( EQUAL( pszValue, "athematic" ) )
            {
                m_bThematic = false;
            }
            else
            {
                m_bThematic = true;
            }
        }
        // ignore any others.
        nIndex++;
    }
    UpdateMetadataList();
    return CE_None;
}

GDALRasterAttributeTable *EMURasterBand::GetDefaultRAT()
{
    return &m_rat;
}

CPLErr EMURasterBand::SetDefaultRAT(const GDALRasterAttributeTable *poRAT)
{
    CPLError(CE_Failure, CPLE_FileIO,
            "Setting RAT not yet supported");
    return CE_Failure;
}

