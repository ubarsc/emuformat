/*
 *  emudataset.cpp
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

#include "emudataset.h"
#include "emuband.h"
#include "emucompress.h"

const int DFLT_TILESIZE = 512; // TODO: allow adjust
const int S3_MAX_PARTS = 1000;
const int S3_MIN_PART_SIZE = 50;  // MB - is actually 5, but let's not let it go down that far
const int S3_MAX_PART_SIZE = 5000;  // MB
// I have no idea here, but we need to guess the file size so we can work out 
// the size of each chunk of the multi part upload
const double AVG_COMPRESSION_RATIO = 0.5;
const int ONE_MB = 1048576; 

//#define EMU_DEBUG
#ifdef EMU_DEBUG
    #define EMU_U64(VAR) fprintf(stderr, #VAR " = %" PRIu64 "\n", VAR);
    #define EMU_64(VAR) fprintf(stderr, #VAR " = %" PRId64 "\n", VAR);
    #define EMU_U32(VAR) fprintf(stderr, #VAR " = %" PRIu32 "\n", VAR);
    #define EMU_U16(VAR) fprintf(stderr, #VAR " = %" PRIu16 "\n", VAR);
    #define EMU_U8(VAR) fprintf(stderr, #VAR " = %" PRIu8 "\n", VAR);
    #define EMU_F64(VAR) fprintf(stderr, #VAR " = %f\n", VAR);
    #define EMU_S(VAR) fprintf(stderr, #VAR " = %s\n", VAR);
#else
    #define EMU_U64(VAR)
    #define EMU_64(VAR)
    #define EMU_U32(VAR)
    #define EMU_U16(VAR)
    #define EMU_U8(VAR)
    #define EMU_F64(VAR)
    #define EMU_S(VAR)
#endif



EMUDataset::EMUDataset(VSILFILE *fp, GDALDataType eType, int nXSize, int nYSize, GDALAccess eInAccess, bool bCloudOptimised, int nTileSize)
{
    m_fp = fp;
    for( int i = 0; i < 6; i++ )
        m_padfTransform[i] = 0;
    m_padfTransform[1] = 1;
    m_padfTransform[5] = -1;
    
    m_eType = eType;
    nRasterXSize = nXSize;
    nRasterYSize = nYSize; 
    eAccess = eInAccess;
    m_bCloudOptimised = bCloudOptimised;
    
    m_tileSize = nTileSize;

    m_papszMetadataList = nullptr;
    UpdateMetadataList();
        
    m_mutex = std::make_shared<std::mutex>();
}

EMUDataset::~EMUDataset()
{
    Close();
}

CPLErr EMUDataset::Close()
{
    // don't lock here as the IWriteBlock function when called will try to lock again...

    CPLErr eErr = CE_None;
    if( 
        (nOpenFlags != OPEN_FLAGS_CLOSED ) && 
        (eAccess == GA_Update) )
    {
        if( m_fp )  
        {
            // ensure all bands written their data now
            for( int n = 0; n < GetRasterCount(); n++)
            {
                GDALRasterBand *pband = GetRasterBand(n + 1);

                // all overviews
                for( int o = 0; o < pband->GetOverviewCount(); o++)
                {
                    GDALRasterBand *pOverview = pband->GetOverview(o);
                    eErr = pOverview->FlushCache(true);
                    if( eErr != CE_None )
                    {
                        return eErr;
                    }
                }

                eErr = pband->FlushCache(true);
                if( eErr != CE_None )
                {
                    return eErr;
                }
                
            }

            eErr = FlushCache(true);
            if( eErr != CE_None )
            {
                return eErr;
            }
            // now write header
            vsi_l_offset headerOffset = VSIFTellL(m_fp);
            VSIFWriteL("HDR", 4, 1, m_fp);
            
            // TODO: endianness
            uint64_t val = m_eType;
            VSIFWriteL(&val, sizeof(val), 1, m_fp);
            
            val = GetRasterCount();
            VSIFWriteL(&val, sizeof(val), 1, m_fp);
            
            val = GetRasterXSize();
            VSIFWriteL(&val, sizeof(val), 1, m_fp);
            
            val = GetRasterYSize();
            VSIFWriteL(&val, sizeof(val), 1, m_fp);
            
            VSIFWriteL(&m_tileSize, sizeof(m_tileSize), 1, m_fp);

            // nodata and stats for each band. 
            for( int n = 0; n < GetRasterCount(); n++ )
            {
                EMURasterBand *pBand = cpl::down_cast<EMURasterBand*>(GetRasterBand(n + 1));
                int nNoDataSet;
                int64_t nodata = pBand->GetNoDataValueAsInt64(&nNoDataSet);
                // coerce so we know the size
                uint8_t n8NoDataSet = nNoDataSet;
                VSIFWriteL(&n8NoDataSet, sizeof(n8NoDataSet), 1, m_fp);
                VSIFWriteL(&nodata, sizeof(nodata), 1, m_fp);
                
                VSIFWriteL(&pBand->m_dMin, sizeof(pBand->m_dMin), 1, m_fp);
                VSIFWriteL(&pBand->m_dMax, sizeof(pBand->m_dMax), 1, m_fp);
                VSIFWriteL(&pBand->m_dMean, sizeof(pBand->m_dMean), 1, m_fp);
                VSIFWriteL(&pBand->m_dStdDev, sizeof(pBand->m_dStdDev), 1, m_fp);
                
                // overviews
                uint32_t noverviews = pBand->GetOverviewCount();
                VSIFWriteL(&noverviews, sizeof(noverviews), 1, m_fp);
                for( uint32_t n = 0; n < noverviews; n++)
                {
                    GDALRasterBand *pOv = pBand->GetOverview(n);
                    val = pOv->GetXSize();
                    VSIFWriteL(&val, sizeof(val), 1, m_fp);
                    val = pOv->GetYSize();
                    VSIFWriteL(&val, sizeof(val), 1, m_fp);
                    int nXSize, nYSize;
                    pOv->GetBlockSize(&nXSize, &nYSize);
                    uint16_t val16 = nXSize;
                    VSIFWriteL(&val16, sizeof(val16), 1, m_fp);
                }
                
                // RAT
                pBand->m_rat.WriteIndex();

                // metadata
                char **ppszMetadata = pBand->GetMetadata();
                if(ppszMetadata != nullptr)
                {
                    size_t nOutputSize, nInputSize;
                    Bytef *pCompressed = doCompressMetadata(COMPRESSION_ZLIB, ppszMetadata, &nInputSize, &nOutputSize);
                    val = nInputSize;
                    VSIFWriteL(&val, sizeof(val), 1, m_fp);
                    if( nInputSize > 0 )
                    {
                        val = nOutputSize;
                        VSIFWriteL(&val, sizeof(val), 1, m_fp);
                        VSIFWriteL(pCompressed, nOutputSize, 1, m_fp);
                        CPLFree(pCompressed);
                    }
                }
                else
                {
                    val = 0;
                    VSIFWriteL(&val, sizeof(val), 1, m_fp);
                }
                

            }
            
            
            // geo transform
            VSIFWriteL(m_padfTransform, sizeof(m_padfTransform), 1, m_fp);
            
            // projection
            char *pszWKT = const_cast<char*>("");
            bool bFree = false;
            if( m_oSRS.exportToWkt(&pszWKT) == OGRERR_NONE )
            {
                bFree = true;
            } 
            val = strlen(pszWKT) + 1;
            VSIFWriteL(&val, sizeof(val), 1, m_fp);
            VSIFWriteL(pszWKT, val, 1, m_fp);
            if( bFree )
            {
                CPLFree(pszWKT);
            }
            
            // metadata (dataset)
            char **ppszMetadata = GetMetadata();
            if(ppszMetadata != nullptr)
            {
                size_t nOutputSize, nInputSize;
                Bytef *pCompressed = doCompressMetadata(COMPRESSION_ZLIB, ppszMetadata, &nInputSize, &nOutputSize);
                val = nInputSize;
                VSIFWriteL(&val, sizeof(val), 1, m_fp);
                if( nInputSize > 0 )
                {   
                    val = nOutputSize;
                    VSIFWriteL(&val, sizeof(val), 1, m_fp);
                    VSIFWriteL(pCompressed, nOutputSize, 1, m_fp);
                    CPLFree(pCompressed);
                }
            }
            else
            {
                val = 0;
                VSIFWriteL(&val, sizeof(val), 1, m_fp);
            }
            
            // number of tile offsets
            val = m_tileOffsets.size();
            VSIFWriteL(&val, sizeof(val), 1, m_fp);
            
            // now all the tile offsets
            for( const std::pair<const EMUTileKey, EMUTileValue>& n: m_tileOffsets)
            {
                VSIFWriteL(&n.second.offset, sizeof(n.second.offset), 1, m_fp);
                VSIFWriteL(&n.second.size, sizeof(n.second.size), 1, m_fp);
                VSIFWriteL(&n.second.uncompressedSize, sizeof(n.second.uncompressedSize), 1, m_fp);
                VSIFWriteL(&n.first.ovrLevel, sizeof(n.first.ovrLevel), 1, m_fp);
                VSIFWriteL(&n.first.band, sizeof(n.first.band), 1, m_fp);
                VSIFWriteL(&n.first.x, sizeof(n.first.x), 1, m_fp);
                VSIFWriteL(&n.first.y, sizeof(n.first.y), 1, m_fp);
            }
            
            // now the offset of the start of the header
            VSIFWriteL(&headerOffset, sizeof(headerOffset), 1, m_fp);
                   
            VSIFCloseL(m_fp);
            m_fp = nullptr;
        }
    }
    return eErr;
}

void EMUDataset::setTileOffset(uint64_t o, uint64_t band, uint64_t x, 
    uint64_t y, vsi_l_offset offset, uint64_t size, uint64_t uncompressedSize)
{
    m_tileOffsets.insert({{o, band, x, y}, {offset, size, uncompressedSize}});
}

EMUTileValue EMUDataset::getTileOffset(uint64_t o, uint64_t band, uint64_t x, uint64_t y)
{
    return m_tileOffsets[{o, band, x, y}];
}


CPLErr EMUDataset::IBuildOverviews(const char *pszResampling, int nOverviews, const int *panOverviewList, 
                                    int nListBands, const int *panBandList, GDALProgressFunc pfnProgress, 
                                    void *pProgressData, CSLConstList papszOptions)
{
    // this is a bit of a fake since we always expect this to be only called when 
    // nothing has been written to the file (yet)
    // go through the list of bands that have been passed in
    int nCurrentBand;
    for( int nBandCount = 0; nBandCount < nListBands; nBandCount++ )
    {
        // get the band number
        nCurrentBand = panBandList[nBandCount];
        // get the band
        EMURasterBand *pBand = (EMURasterBand*)this->GetRasterBand(nCurrentBand);
        // create the overview objects
        CPLErr err = pBand->CreateOverviews( nOverviews, panOverviewList );
        if( err != CE_None )
        {
            return err;
        }
    }
    return CE_None;
}


GDALDataset *EMUDataset::Open(GDALOpenInfo *poOpenInfo)
{
    if (!Identify(poOpenInfo))
        return nullptr;
        
     // Confirm the requested access is supported.
    if( poOpenInfo->eAccess == GA_Update )
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "The EMU driver does not support update access to existing "
                 "datasets.");
        return nullptr;
    }

    if( (poOpenInfo->pabyHeader[0] != 'E') || 
        (poOpenInfo->pabyHeader[1] != 'M') ||
        (poOpenInfo->pabyHeader[2] != 'U') )
    {
        return nullptr;
    }
    
    // flags
    // skip version info
    uint32_t nFlags;
    memcpy(&nFlags, &poOpenInfo->pabyHeader[7], sizeof(nFlags));
    EMU_U32(nFlags)
    bool bCloudOptimised = nFlags & 1; // TODO
    
    // Check that the file pointer from GDALOpenInfo* is available.
    if( poOpenInfo->fpL == nullptr )
    {
        return nullptr;
    }
    

    // seek to the end of the file
    VSIFSeekL(poOpenInfo->fpL, 0 , SEEK_END);
    vsi_l_offset fsize = VSIFTellL(poOpenInfo->fpL);
    
    // seek to the size of the header offset
    // hopefully these accesses are cached?
    uint64_t headerOffset; 
    VSIFSeekL(poOpenInfo->fpL, fsize - sizeof(headerOffset), SEEK_SET);
    VSIFReadL(&headerOffset, sizeof(headerOffset), 1, poOpenInfo->fpL);
    EMU_U64(headerOffset)
    VSIFSeekL(poOpenInfo->fpL, headerOffset, SEEK_SET);
    
    char headerChars[4];
    VSIFReadL(headerChars, 4, 1, poOpenInfo->fpL);
    if( strcmp(headerChars, "HDR") != 0 )
    {
         CPLError(CE_Failure, CPLE_OpenFailed,
                 "Failed to read header");
        return nullptr;       
    }
    
    uint64_t ftype;
    VSIFReadL(&ftype, sizeof(ftype), 1, poOpenInfo->fpL);
    EMU_U64(ftype)
    
    uint64_t bandcount;
    VSIFReadL(&bandcount, sizeof(bandcount), 1, poOpenInfo->fpL);
    EMU_U64(bandcount)
    
    uint64_t xsize;
    VSIFReadL(&xsize, sizeof(xsize), 1, poOpenInfo->fpL);
    EMU_U64(xsize)

    uint64_t ysize;
    VSIFReadL(&ysize, sizeof(ysize), 1, poOpenInfo->fpL);
    EMU_U64(ysize)

    uint32_t ntilesize;
    VSIFReadL(&ntilesize, sizeof(ntilesize), 1, poOpenInfo->fpL);
    EMU_U32(ntilesize)
    
    // grap ownership of poOpenInfo->fpL
    VSILFILE *fp = nullptr;
    std::swap(fp, poOpenInfo->fpL);

    GDALDataType eType = (GDALDataType)ftype;
    EMUDataset *pDS = new EMUDataset(fp, eType, xsize, ysize, GA_ReadOnly, bCloudOptimised, ntilesize);

    // nodata and stats for each band. 
    for( int n = 0; n < bandcount; n++ )
    {
        uint8_t n8NoDataSet;
        VSIFReadL(&n8NoDataSet, sizeof(n8NoDataSet), 1, fp);
        EMU_U8(n8NoDataSet)
        int64_t nodata;
        VSIFReadL(&nodata, sizeof(nodata), 1, fp);
        EMU_64(nodata)

        EMURasterBand *pBand = new EMURasterBand(pDS, n + 1, eType, xsize, ysize, ntilesize, pDS->m_mutex);
        if(n8NoDataSet)
            pBand->SetNoDataValueAsInt64(nodata);
            
        VSIFReadL(&pBand->m_dMin, sizeof(pBand->m_dMin), 1, fp);
        EMU_F64(pBand->m_dMin)
        VSIFReadL(&pBand->m_dMax, sizeof(pBand->m_dMax), 1, fp);
        EMU_F64(pBand->m_dMax)
        VSIFReadL(&pBand->m_dMean, sizeof(pBand->m_dMean), 1, fp);
        EMU_F64(pBand->m_dMean)
        VSIFReadL(&pBand->m_dStdDev, sizeof(pBand->m_dStdDev), 1, fp);
        EMU_F64(pBand->m_dStdDev)
        
        pDS->SetBand(n + 1, pBand);
        
        uint32_t nOverviews;
        VSIFReadL(&nOverviews, sizeof(nOverviews), 1, fp);
        EMU_U32(nOverviews)
        std::vector<std::tuple<int, int, int> > sizes;
        for( uint32_t n = 0; n < nOverviews; n++)
        {
            uint64_t oxsize;
            VSIFReadL(&oxsize, sizeof(oxsize), 1, fp);
            EMU_U64(oxsize)
            uint64_t oysize;
            VSIFReadL(&oysize, sizeof(oysize), 1, fp);
            EMU_U64(oysize)
            uint16_t oblocksize;
            VSIFReadL(&oblocksize, sizeof(oblocksize), 1, fp);
            EMU_U16(oblocksize)
            sizes.push_back(std::tuple<int, int, int>(oxsize, oysize, oblocksize));
        }
        pBand->CreateOverviews(sizes);

        // RAT
        pBand->m_rat.ReadIndex();
        
        // metadata - note sizes opposite order from writing
        uint64_t nOutputSize;
        VSIFReadL(&nOutputSize, sizeof(nOutputSize), 1, fp);
        EMU_U64(nOutputSize)
        if( nOutputSize >  0)
        {
            uint64_t nInputSize;
            VSIFReadL(&nInputSize, sizeof(nInputSize), 1, fp);
            EMU_U64(nInputSize)
            Byte *pBuf = static_cast<Bytef*>(CPLMalloc(nInputSize));
            VSIFReadL(pBuf, nInputSize, 1, fp);
            
            char **ppszMetadata = doUncompressMetadata(COMPRESSION_ZLIB, pBuf, nInputSize, nOutputSize);
            pBand->SetMetadata(ppszMetadata);
            CSLDestroy(ppszMetadata);
            CPLFree(pBuf);
            
            // ensure all in sync
            pBand->UpdateMetadataList();
        }
    }

    double transform[6];
    VSIFReadL(transform, sizeof(transform), 1, fp);
    for( int i = 0; i < 6; i++)
    {
        EMU_F64(transform[i])
    }
    pDS->SetGeoTransform(transform);

    uint64_t wktSize;
    VSIFReadL(&wktSize, sizeof(wktSize), 1, fp);
    EMU_U64(wktSize)
    
    char *pszWKT = new char[wktSize];
    VSIFReadL(pszWKT, wktSize, 1, fp);
    EMU_S(pszWKT)
    OGRSpatialReference sr(pszWKT);
    pDS->SetSpatialRef(&sr);
    delete[] pszWKT;
    
    // metadata - note sizes opposite order from writing
    uint64_t val;
    VSIFReadL(&val, sizeof(val), 1, fp);
    if( val >  0)
    {
        size_t nOutputSize = val, nInputSize;
        VSIFReadL(&val, sizeof(val), 1, fp);
        nInputSize = val;
        Byte *pBuf = static_cast<Bytef*>(CPLMalloc(nInputSize));
        VSIFReadL(pBuf, nInputSize, 1, fp);
        
        char **ppszMetadata = doUncompressMetadata(COMPRESSION_ZLIB, pBuf, nInputSize, nOutputSize);
        pDS->SetMetadata(ppszMetadata);
        CSLDestroy(ppszMetadata);
        CPLFree(pBuf);
        
        // ensure all in sync
        pDS->UpdateMetadataList();
    }
    
    uint64_t ntiles;
    VSIFReadL(&ntiles, sizeof(ntiles), 1, fp);
    EMU_U64(ntiles)
    
    for( uint64_t n = 0; n < ntiles; n++ )
    {
        uint64_t offset;
        VSIFReadL(&offset, sizeof(offset), 1, fp);
        EMU_U64(offset)
        uint64_t size;
        VSIFReadL(&size, sizeof(size), 1, fp);
        EMU_U64(size)
        uint64_t uncompressedSize;
        VSIFReadL(&uncompressedSize, sizeof(uncompressedSize), 1, fp);
        EMU_U64(uncompressedSize)
        uint64_t ovrLevel;
        VSIFReadL(&ovrLevel, sizeof(ovrLevel), 1, fp);
        EMU_U64(ovrLevel)
        uint64_t band;
        VSIFReadL(&band, sizeof(band), 1, fp);
        EMU_U64(band)
        uint64_t x;
        VSIFReadL(&x, sizeof(x), 1, fp);
        EMU_U64(x)
        uint64_t y;
        VSIFReadL(&y, sizeof(y), 1, fp);
        EMU_U64(y)
        
        pDS->setTileOffset(ovrLevel, band, x, y, offset, size, uncompressedSize);
    }

    return pDS;
}

int EMUDataset::Identify(GDALOpenInfo *poOpenInfo)
{
    if( !EQUAL(CPLGetExtension(poOpenInfo->pszFilename), "EMU") )
        return FALSE;
        
    if (poOpenInfo->pabyHeader == nullptr ||
        memcmp(poOpenInfo->pabyHeader, "EMU", 3) != 0)
    {
        return FALSE;
    }
    
    return TRUE;
}

VSILFILE *EMUDataset::CreateEMU(const char * pszFilename,
                                int nXSize, int nYSize, int nBands,
                                GDALDataType eType)
{
    char **papszOptions = nullptr;
    if( STARTS_WITH(pszFilename, "/vsis3") )
    {
        // try and determine a sensible chunk size in MB
        double dApproxFileSize = ((nXSize * nYSize * nBands * 
            GDALGetDataTypeSizeBytes(eType)) / ONE_MB) * AVG_COMPRESSION_RATIO;
        int nChunkSize = ceil(dApproxFileSize / S3_MAX_PARTS);
        if( nChunkSize < S3_MIN_PART_SIZE )
        {
            nChunkSize = S3_MIN_PART_SIZE;
        }
        
        if( (nChunkSize * S3_MAX_PARTS) > (S3_MAX_PART_SIZE * S3_MAX_PARTS) )
        {
            CPLError(CE_Failure, CPLE_OpenFailed,
                    "Attempt to create file `%s' failed. Too big for multi part upload",
                    pszFilename);
            return NULL;
        }
        
        papszOptions = CSLAppendPrintf(papszOptions, "CHUNK_SIZE=%d", nChunkSize);
        printf("CHUNK_SIZE=%d dApproxFileSize=%f\n", nChunkSize, dApproxFileSize);
    }

    // Try to create the file.
    VSILFILE *fp = VSIFOpenEx2L(pszFilename, "w", FALSE, papszOptions);
    CSLDestroy(papszOptions);
    
    return fp;    
}

GDALDataset *EMUDataset::Create(const char * pszFilename,
                                int nXSize, int nYSize, int nBands,
                                GDALDataType eType,
                                char ** /* papszParamList */)
{
    VSILFILE *fp = CreateEMU(pszFilename, nXSize, nYSize, nBands, eType);
    if( fp == NULL )
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                "Attempt to create file `%s' failed.",
                pszFilename);
        return NULL;
    }
    
    VSIFPrintfL(fp, "EMU%04d", EMU_VERSION);
    uint32_t nFlags = 0;  // No COG
    VSIFWriteL(&nFlags, sizeof(nFlags), 1, fp);
    
    EMUDataset *pDS = new EMUDataset(fp, eType, nXSize, nYSize, GA_Update, false, DFLT_TILESIZE);
    for( int n = 0; n < nBands; n++ )
    {
        pDS->SetBand(n + 1, new EMURasterBand(pDS, n + 1, eType, nXSize, nYSize, DFLT_TILESIZE, pDS->m_mutex));
    }
    
    return pDS;
}

bool CopyBand(GDALRasterBand *pSrc, GDALRasterBand *pDst, int &nDoneBlocks, int nTotalBlocks, GDALProgressFunc pfnProgress, void *pProgressData)
{
    int nXSize = pSrc->GetXSize();
    int nYSize = pSrc->GetYSize();
    GDALDataType eGDALType = pSrc->GetRasterDataType();
    
    int nBlockSize;
    pDst->GetBlockSize(&nBlockSize, &nBlockSize);

    // allocate some space
    int nPixelSize = GDALGetDataTypeSize( eGDALType ) / 8;
    void *pData = CPLMalloc( nPixelSize * nBlockSize * nBlockSize);
    double dLastFraction = -1;
    
    // go through the image
    for( unsigned int nY = 0; nY < nYSize; nY += nBlockSize )
    {
        // adjust for edge blocks
        unsigned int nysize = nBlockSize;
        unsigned int nytotalsize = nY + nBlockSize;
        if( nytotalsize > nYSize )
            nysize -= (nytotalsize - nYSize);
        for( unsigned int nX = 0; nX < nXSize; nX += nBlockSize )
        {
            // adjust for edge blocks
            unsigned int nxsize = nBlockSize;
            unsigned int nxtotalsize = nX + nBlockSize;
            if( nxtotalsize > nXSize )
                nxsize -= (nxtotalsize - nXSize);
                
            // read in from In Band 
            if( pSrc->RasterIO( GF_Read, nX, nY, nxsize, nysize, pData, nxsize, nysize, eGDALType, nPixelSize, nPixelSize * nBlockSize) != CE_None )
            {
                CPLError( CE_Failure, CPLE_AppDefined, "Unable to read block at %d %d\n", nX, nY );
                return false;
            }
            // write out
            if( pDst->WriteBlock(nX / nBlockSize, nY / nBlockSize, pData) != CE_None )
            {
                CPLError( CE_Failure, CPLE_AppDefined, "Unable to write block at %d %d\n", nX, nY );
                return false;
            }

            // progress
            nDoneBlocks++;
            double dFraction = (double)nDoneBlocks / (double)nTotalBlocks;
            if( dFraction != dLastFraction )
            {
                if( !pfnProgress( dFraction, nullptr, pProgressData ) )
                {
                    CPLFree( pData );
                    return false;
                }
                dLastFraction = dFraction;
            }
        }
    }

    
    CPLFree(pData);
    return true;
}

int GetBandTotalTiles(GDALRasterBand *pSrc, int nBlockSize)
{
    int nXTiles = std::ceil(pSrc->GetXSize() / double(nBlockSize));
    int nYTiles = std::ceil(pSrc->GetYSize() / double(nBlockSize));
    return nXTiles * nYTiles; 
}

GDALDataset *EMUDataset::CreateCopy( const char * pszFilename, GDALDataset *pSrcDs,
                                int bStrict, char **  papszParmList, 
                                GDALProgressFunc pfnProgress, void *pProgressData )
{
    int nXSize = pSrcDs->GetRasterXSize();
    int nYSize = pSrcDs->GetRasterYSize();
    int nBands = pSrcDs->GetRasterCount();
    GDALRasterBand *pFirstBand = pSrcDs->GetRasterBand(1);
    GDALDataType eType = pFirstBand->GetRasterDataType();
    int nBlockXsize, nBlockYsize;
    pFirstBand->GetBlockSize(&nBlockXsize, &nBlockYsize);
    if( nBlockXsize != nBlockYsize)
    {
        CPLError( CE_Failure, CPLE_AppDefined, "Block sizes must be square\n");
        return nullptr;
    }

    VSILFILE *fp = CreateEMU(pszFilename, nXSize, nYSize, nBands, eType);
    if( fp == NULL )
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                "Attempt to create file `%s' failed.",
                pszFilename);
        return NULL;
    }
    
    VSIFPrintfL(fp, "EMU%04d", EMU_VERSION);
    uint32_t nFlags = 1;  // COG
    VSIFWriteL(&nFlags, sizeof(nFlags), 1, fp);
    
    EMUDataset *pDS = new EMUDataset(fp, eType, nXSize, nYSize, GA_Update, true, nBlockXsize);
    for( int n = 0; n < nBands; n++ )
    {
        pDS->SetBand(n + 1, new EMURasterBand(pDS, n + 1, eType, nXSize, nYSize, nBlockXsize, pDS->m_mutex));
    }
    
    // find the highest overview level
    int nMaxOverview = 0;
    int nTotalBlocks = 0;
    for( int n = 0; n < nBands; n++ )
    {
        GDALRasterBand *pSrcBand = pSrcDs->GetRasterBand(n + 1);
        int nOverviews = pSrcBand->GetOverviewCount();
        if( nOverviews > nMaxOverview )
        {
            nMaxOverview = nOverviews;
        }
        int nBandBlockXsize, nBandBlockYsize;
        pSrcBand->GetBlockSize(&nBandBlockXsize, &nBandBlockYsize);
        if( (nBandBlockXsize != nBlockXsize) || (nBandBlockYsize != nBlockYsize) )
        {
            CPLError( CE_Failure, CPLE_AppDefined, "Bands must have all the same block sizes\n");
            return nullptr;
        }
        
        nTotalBlocks += GetBandTotalTiles(pSrcBand, nBlockXsize);
        
        // create the overviews same as the input (could be different lengths for each band, but unlikely)
        std::vector<std::tuple<int, int, int> > sizes;
        for( int nOvCount = 0; nOvCount < nOverviews; nOvCount++)
        {
            GDALRasterBand *pOv = pSrcBand->GetOverview(nOvCount);

            int nOvBlockXsize, nOvBlockYsize;
            pOv->GetBlockSize(&nOvBlockXsize, &nOvBlockYsize);
            if( nOvBlockXsize != nOvBlockYsize)
            {
                CPLError( CE_Failure, CPLE_AppDefined, "Block sizes must be square\n");
                return nullptr;
            }

            sizes.push_back(std::tuple<int, int, int>(pOv->GetXSize(), pOv->GetYSize(), nOvBlockXsize));
            nTotalBlocks += GetBandTotalTiles(pOv, nOvBlockXsize);
        }
        
        EMURasterBand *pDestBand = cpl::down_cast<EMURasterBand*>(pDS->GetRasterBand(n + 1));
        pDestBand->CreateOverviews(sizes);
    }
    // now go through each level,  and then do each band
    
    int nDoneBlocks = 0;
    for( int nOverviewLevel = nMaxOverview - 1; nOverviewLevel >= 0; nOverviewLevel--)
    {
        // TODO: should we be doing the first tile here, for all bands, then second tile for all bands??
        for( int nBand = 0; nBand < nBands; nBand++ )
        {
            GDALRasterBand *pSrcBand = pSrcDs->GetRasterBand(nBand + 1);
            if( pSrcBand->GetOverviewCount() >= nOverviewLevel)
            {
                GDALRasterBand *pDestBand = pDS->GetRasterBand(nBand + 1);
                GDALRasterBand *DestOv = pDestBand->GetOverview(nOverviewLevel);
                GDALRasterBand *SrcOv = pSrcBand->GetOverview(nOverviewLevel);
                if(! CopyBand(SrcOv, DestOv, nDoneBlocks, nTotalBlocks, pfnProgress, pProgressData) )
                {
                    delete pDS;
                    return nullptr;
                }
            }
        }
    }
    
    // now just do the band data last
    for( int nBand = 0; nBand < nBands; nBand++ )
    {
        GDALRasterBand *pSrcBand = pSrcDs->GetRasterBand(nBand + 1);
        EMURasterBand *pDestBand = cpl::down_cast<EMURasterBand*>(pDS->GetRasterBand(nBand + 1));
        if(! CopyBand(pSrcBand, pDestBand, nDoneBlocks, nTotalBlocks, pfnProgress, pProgressData) )
        {
            delete pDS;
            return nullptr;
        }
        
        // metadata
        char **ppsz = pSrcBand->GetMetadata();
        if( ppsz != nullptr )
        {
            pDestBand->SetMetadata(ppsz);
            pDestBand->UpdateMetadataList();
        }
    }
    
    // metadata
    char **ppsz = pSrcDs->GetMetadata();
    if( ppsz != nullptr )
    {
        pDS->SetMetadata(ppsz);
        pDS->UpdateMetadataList();
    }
    
    return pDS;
}


CPLErr EMUDataset::GetGeoTransform( double * padfTransform )
{
    for( int i = 0; i < 6; i++ )
    {
        padfTransform[i] = m_padfTransform[i];
    }
    
    return CE_None;
}

CPLErr EMUDataset::SetGeoTransform( double * padfTransform )
{
    for( int i = 0; i < 6; i++ )
    {
        m_padfTransform[i] = padfTransform[i];
    }

    return CE_None;
}

const OGRSpatialReference* EMUDataset::GetSpatialRef() const
{
    return &m_oSRS;
}

CPLErr EMUDataset::SetSpatialRef(const OGRSpatialReference* poSRS)
{
    m_oSRS = *poSRS;
    return CE_None;
}

void EMUDataset::UpdateMetadataList()
{
    if( m_bCloudOptimised )
    {
        m_papszMetadataList = CSLSetNameValue(m_papszMetadataList, "CLOUD_OPTIMISED", "TRUE");
    }
    else
    {
        m_papszMetadataList = CSLSetNameValue(m_papszMetadataList, "CLOUD_OPTIMISED", "FALSE");
    }
}

CPLErr EMUDataset::SetMetadataItem(const char *pszName, const char *pszValue, 
	   const char *pszDomain)
{

    // can't update CLOUD_OPTIMISED
    if(!EQUAL(pszName, "CLOUD_OPTIMISED"))
    {
        m_papszMetadataList = CSLSetNameValue(m_papszMetadataList, pszName, pszValue);
    }
    return CE_None;
}

const char *EMUDataset::GetMetadataItem(const char *pszName, const char *pszDomain)
{
    // only deal with 'default' domain - no geolocation etc
    if( ( pszDomain != nullptr ) && ( *pszDomain != '\0' ) )
        return nullptr;
    // get it out of the CSLStringList so we can be sure it is persistant
    return CSLFetchNameValue(m_papszMetadataList, pszName);
}

// get all the metadata as a CSLStringList - not thread safe
char **EMUDataset::GetMetadata(const char *pszDomain)
{
    // only deal with 'default' domain - no geolocation etc
    if( ( pszDomain != nullptr ) && ( *pszDomain != '\0' ) )
        return nullptr;

    // conveniently we already have it in this format
    return m_papszMetadataList; 
}

// set the metadata as a CSLStringList
CPLErr EMUDataset::SetMetadata(char **papszMetadata, const char *pszDomain)
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
    return CE_None;
}

CPL_C_START
void CPL_DLL GDALRegister_EMU(void);
CPL_C_END

void GDALRegister_EMU()
{
    if( !GDAL_CHECK_VERSION("EMU") )
        return;

    if( GDALGetDriverByName("EMU") != nullptr )
        return;
    
    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("EMU");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "UBARSC Streaming Format (.emu)");
    poDriver->SetMetadataItem(GDAL_DMD_EXTENSIONS, "emu");
    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CREATE, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CREATECOPY, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_CREATIONDATATYPES, 
            "Byte Int8 Int16 UInt16 Int32 UInt32 Int64 UInt64 Float32 Float64");

    poDriver->pfnOpen = EMUDataset::Open;
    poDriver->pfnIdentify = EMUDataset::Identify;
    poDriver->pfnCreate = EMUDataset::Create;
    poDriver->pfnCreateCopy = EMUDataset::CreateCopy;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
