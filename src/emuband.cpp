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

EMUBaseBand::EMUBaseBand(EMUDataset *pDataset, int nBandIn, GDALDataType eType, 
        uint64_t nLevel, int nXSize, int nYSize, int nBlockSize, const std::shared_ptr<std::mutex>& other)
{
    poDS = pDataset;
    nBlockXSize = nBlockSize;
    nBlockYSize = nBlockSize;
    nBand = nBandIn;
    eDataType = eType;
    nRasterXSize = nXSize;
    nRasterYSize = nYSize;
    eAccess = pDataset->GetAccess();
    m_nLevel = nLevel;    
    m_mutex = other;
}

EMUBaseBand::~EMUBaseBand()
{
    FlushCache();
}

CPLErr EMUBaseBand::IReadBlock(int nBlockXOff, int nBlockYOff, void *pData)
{
    if(poDS->GetAccess() == GA_Update)
    {
        fprintf(stderr, "%d %d \n", m_nLevel, nBlockXSize);
        CPLError(CE_Failure, CPLE_NotSupported,
             "The EMU driver only supports reading when open in readonly mode");
        return CE_Failure;
        /*int nXValid, nYValid;
        CPLErr err = GetActualBlockSize(nBlockXOff, nBlockYOff, &nXValid, &nYValid);
        if( err != CE_None)
            return err;
        int typeSize = GDALGetDataTypeSize(eDataType) / 8;
        memset(pData, 0, nXValid * nYValid * typeSize);*/
    }

    const std::lock_guard<std::mutex> lock(*m_mutex);

    EMUDataset *poEMUDS = cpl::down_cast<EMUDataset *>(poDS);

    EMUTileValue val;
    try
    {
        val = poEMUDS->getTileOffset(m_nLevel, nBand, nBlockXOff, nBlockYOff);
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

CPLErr EMUBaseBand::IWriteBlock(int nBlockXOff, int nBlockYOff, void *pData)
{
    const std::lock_guard<std::mutex> lock(*m_mutex);

    EMUDataset *poEMUDS = cpl::down_cast<EMUDataset *>(poDS);
    
    // GDAL deals in blocks - if we are at the end of a row
    // we need to adjust the amount read so we don't go over the edge
    int nXValid, nYValid;
    CPLErr err = GetActualBlockSize(nBlockXOff, nBlockYOff, &nXValid, &nYValid);
    if( err != CE_None)
    {
        fprintf(stderr, "Failed GetActualBlockSize\n");
        return err;
    }
    
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
        bool bFree;
        void *pCompressed = doCompression(compression, static_cast<Bytef*>(pData), uncompressedSize, &compressedSize, &bFree);
        VSIFWriteL(pCompressed, compressedSize, 1, poEMUDS->m_fp);
        if( bFree ) 
        {
            CPLFree(pCompressed);
        }
    }
    
    // update map
    poEMUDS->setTileOffset(m_nLevel, nBand, nBlockXOff, nBlockYOff, tileOffset, compressedSize, uncompressedSize);

    return CE_None;
}


CPLErr EMUBaseBand::IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize, int nYSize,
              void *pData, int nBufXSize, int nBufYSize, GDALDataType eBufType,
              GSpacing nPixelSpace, GSpacing nLineSpace,
              GDALRasterIOExtraArg *psExtraArg)
{
    if( eRWFlag == GF_Read )
    {
        // safe to use the default implementation
        return GDALRasterBand::IRasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize, pData, nBufXSize, nBufYSize,
            eBufType, nPixelSpace, nLineSpace, psExtraArg);
    }
    else
    {
        // our own implementation that doesn't read
        if( ((nXOff % nBlockXSize ) > 0) || ((nYOff % nBlockYSize ) > 0 ) )
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                 "The EMU driver only supports writing on block boundaries");
            return CE_Failure;
        }
        else
        {  
            int nBlockXOff = nXOff / nBlockXSize;
            int nBlockYOff = nYOff / nBlockYSize;
            return IWriteBlock(nBlockXOff, nBlockYOff, pData);
        }
    }
  
}


EMURasterBand::EMURasterBand(EMUDataset *pDataset, int nBandIn, GDALDataType eType, 
        int nXSize, int nYSize, int nBlockSize, const std::shared_ptr<std::mutex>& other)
    : EMUBaseBand(pDataset, nBandIn, eType, 0, nXSize, nYSize, nBlockSize, other),
        m_rat(pDataset, this, other)
{
    m_bNoDataSet = false;
    m_nNoData = 0;

    m_dMin = std::numeric_limits<double>::quiet_NaN();
    m_dMax = std::numeric_limits<double>::quiet_NaN();
    m_dMean = std::numeric_limits<double>::quiet_NaN();
    m_dStdDev = std::numeric_limits<double>::quiet_NaN();

    // initialise overview variables
    m_nOverviews = 0;
    m_panOverviewBands = nullptr;

    m_papszMetadataList = nullptr;
    UpdateMetadataList();
}

EMURasterBand::~EMURasterBand()
{
    for( int nCount = 0; nCount < m_nOverviews; nCount++ )
    {
        delete m_panOverviewBands[nCount];
    }
    CPLFree(m_panOverviewBands);

    CSLDestroy(m_papszMetadataList);
    // FlushCache happens in ~EMUBaseBand
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

CPLErr EMURasterBand::SetStatistics(double dfMin, double dfMax, double dfMean,	
	    double dfStdDev)
{
    m_dMin = dfMin;
    m_dMax = dfMax;
    m_dMean = dfMean;
    m_dStdDev = dfStdDev;
    UpdateMetadataList();
    return CE_None;
}

void EMURasterBand::UpdateMetadataList()
{
    CPLString osWorkingResult;
    osWorkingResult.Printf( "%f", m_dMin);
    m_papszMetadataList = CSLSetNameValue(m_papszMetadataList, "STATISTICS_MINIMUM", osWorkingResult);

    osWorkingResult.Printf( "%f", m_dMax);
    m_papszMetadataList = CSLSetNameValue(m_papszMetadataList, "STATISTICS_MAXIMUM", osWorkingResult);

    osWorkingResult.Printf( "%f", m_dMean);
    m_papszMetadataList = CSLSetNameValue(m_papszMetadataList, "STATISTICS_MEAN", osWorkingResult);

    osWorkingResult.Printf( "%f", m_dStdDev);
    m_papszMetadataList = CSLSetNameValue(m_papszMetadataList, "STATISTICS_STDDEV", osWorkingResult);

    // TODO: STATISTICS_HISTO* from RAT (if set)
}

CPLErr EMURasterBand::SetMetadataItem(const char *pszName, const char *pszValue, 
	   const char *pszDomain)
{

    bool bUpdateNeeded = false;
    if(EQUAL(pszName, "STATISTICS_MINIMUM"))
    {
        m_dMin = atof(pszValue);
        bUpdateNeeded = true;
    }
    else if(EQUAL(pszName, "STATISTICS_MAXIMUM"))
    {
        m_dMax = atof(pszValue);
        bUpdateNeeded = true;
    }
    else if(EQUAL(pszName, "STATISTICS_MEAN"))
    {
        m_dMean = atof(pszValue);
        bUpdateNeeded = true;
    }
    else if(EQUAL(pszName, "STATISTICS_STDDEV"))
    {
        m_dStdDev = atof(pszValue);
        bUpdateNeeded = true;
    }
    else
    {
        m_papszMetadataList = CSLSetNameValue(m_papszMetadataList, pszName, pszValue);
    }

    if(bUpdateNeeded)
    {
        UpdateMetadataList();
    }
    return CE_None;
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

        SetMetadataItem(pszName, pszValue, pszDomain);
        
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

int EMURasterBand::GetOverviewCount()
{
    return m_nOverviews;
}
    
GDALRasterBand* EMURasterBand::GetOverview(int nOverview)
{
    if( nOverview >= m_nOverviews )
    {
        return nullptr;
    }
    else
    {
        return m_panOverviewBands[nOverview];
    }
}

CPLErr EMURasterBand::CreateOverviews(int nOverviews, const int *panOverviewList)
{
    if( m_panOverviewBands != nullptr )
    {
        CPLError(CE_Failure, CPLE_FileIO,
            "Can't update overviews once set");
        return CE_Failure;
    }

    m_panOverviewBands = (EMUBaseBand**)CPLMalloc(sizeof(EMUBaseBand*) * nOverviews);
    m_nOverviews = nOverviews;

    // loop through and create the overviews
    int nFactor, nXSize, nYSize, nBlockSize;
    for( int nCount = 0; nCount < m_nOverviews; nCount++ )
    {
        nFactor = panOverviewList[nCount];
        // divide by the factor to get the new size
        nXSize = this->nRasterXSize / nFactor;
        nYSize = this->nRasterYSize / nFactor;
        // note: different from KEA we shrink the blocksize by factor for the overviews
        // so we don't get partial overview blocks when creating a file with RIOS.
        // 
        nBlockSize = this->nBlockXSize / nFactor;
        m_panOverviewBands[nCount] = new EMUBaseBand(cpl::down_cast<EMUDataset*>(poDS), 
            nBand, eDataType, nCount + 1, nXSize, nYSize, nBlockSize, m_mutex);
    }
    
    return CE_None;
}

CPLErr EMURasterBand::CreateOverviews(const std::vector<std::pair<int, int> > &sizes)
{
    if( m_panOverviewBands != nullptr )
    {
        CPLError(CE_Failure, CPLE_FileIO,
            "Can't update overviews once set");
        return CE_Failure;
    }

    m_nOverviews = sizes.size();
    m_panOverviewBands = (EMUBaseBand**)CPLMalloc(sizeof(EMUBaseBand*) * m_nOverviews);

    // loop through and create the overviews
    int nCount = 0;
    for( auto & s : sizes)
    {
        // note: different from KEA we shrink the blocksize by factor for the overviews
        // so we don't get partial overview blocks when creating a file with RIOS.
        //
        int nFactor = this->nRasterXSize / s.first;
        int nBlockSize = this->nBlockXSize / nFactor;
        m_panOverviewBands[nCount] = new EMUBaseBand(cpl::down_cast<EMUDataset*>(poDS), 
                nBand, eDataType, nCount + 1, 
                s.first, s.second, nBlockSize, m_mutex);
        nCount++;
    }    

    return CE_None;
}
