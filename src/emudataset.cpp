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

EMUDataset::EMUDataset(VSILFILE *fp, GDALDataType eType, int nXSize, int nYSize, GDALAccess eInAccess)
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
    
    // TODO: allow override
    m_tileSize = 512;
    
    m_mutex = std::make_shared<std::mutex>();
}

EMUDataset::~EMUDataset()
{
    Close();
}

CPLErr EMUDataset::Close()
{
    const std::lock_guard<std::mutex> lock(*m_mutex);

    CPLErr eErr = CE_None;
    if( (nOpenFlags != OPEN_FLAGS_CLOSED ) && (eAccess == GA_Update) )
    {
        if( FlushCache(true) != CE_None )
            eErr = CE_Failure;

        if( m_fp )  
        {
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
            
            // number of overviews
            val = 0;
            VSIFWriteL(&val, sizeof(val), 1, m_fp);
            
            // TODO: info on each overview here
            
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
                uint8_t nThematic = pBand->GetThematic();
                VSIFWriteL(&nThematic, sizeof(nThematic), 1, m_fp);
                
                pBand->EstimateStatsFromHistogram();
                
                VSIFWriteL(&pBand->m_dMin, sizeof(pBand->m_dMin), 1, m_fp);
                VSIFWriteL(&pBand->m_dMax, sizeof(pBand->m_dMax), 1, m_fp);
                VSIFWriteL(&pBand->m_dMean, sizeof(pBand->m_dMean), 1, m_fp);
                VSIFWriteL(&pBand->m_dStdDev, sizeof(pBand->m_dStdDev), 1, m_fp);
                VSIFWriteL(&pBand->m_dMedian, sizeof(pBand->m_dMedian), 1, m_fp);
                VSIFWriteL(&pBand->m_dMode, sizeof(pBand->m_dMode), 1, m_fp);
            }
            
            // tilesize
            VSIFWriteL(&m_tileSize, sizeof(m_tileSize), 1, m_fp);
            
            // geo transform
            VSIFWriteL(&m_padfTransform, sizeof(m_padfTransform), 1, m_fp);
            
            // projection
            char *pszWKT;
            m_oSRS.exportToWkt(&pszWKT);
            val = strlen(pszWKT) + 1;
            VSIFWriteL(&val, sizeof(val), 1, m_fp);
            VSIFWriteL(pszWKT, strlen(pszWKT) + 1, 1, m_fp);
            CPLFree(pszWKT);
            
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
    
    // TODO: get version    
    
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
    
    uint64_t count;
    VSIFReadL(&count, sizeof(count), 1, poOpenInfo->fpL);
    
    uint64_t xsize;
    VSIFReadL(&xsize, sizeof(xsize), 1, poOpenInfo->fpL);

    uint64_t ysize;
    VSIFReadL(&ysize, sizeof(ysize), 1, poOpenInfo->fpL);

    uint64_t nover;
    VSIFReadL(&nover, sizeof(nover), 1, poOpenInfo->fpL);
    
    // grap ownership of poOpenInfo->fpL
    VSILFILE *fp = nullptr;
    std::swap(fp, poOpenInfo->fpL);

    GDALDataType eType = (GDALDataType)ftype;
    EMUDataset *pDS = new EMUDataset(fp, eType, xsize, ysize, GA_ReadOnly);

    // nodata and stats for each band. 
    for( int n = 0; n < count; n++ )
    {
        uint8_t n8NoDataSet;
        VSIFReadL(&n8NoDataSet, sizeof(n8NoDataSet), 1, fp);
        int64_t nodata;
        VSIFReadL(&nodata, sizeof(nodata), 1, fp);
        uint8_t nThematic;
        VSIFReadL(&nThematic, sizeof(nThematic), 1, fp);

        EMURasterBand *pBand = new EMURasterBand(pDS, n + 1, eType, nThematic, pDS->m_mutex);
        if(n8NoDataSet)
            pBand->SetNoDataValueAsInt64(nodata);
            
        VSIFReadL(&pBand->m_dMin, sizeof(pBand->m_dMin), 1, fp);
        VSIFReadL(&pBand->m_dMax, sizeof(pBand->m_dMax), 1, fp);
        VSIFReadL(&pBand->m_dMean, sizeof(pBand->m_dMean), 1, fp);
        VSIFReadL(&pBand->m_dStdDev, sizeof(pBand->m_dStdDev), 1, fp);
        VSIFReadL(&pBand->m_dMedian, sizeof(pBand->m_dMedian), 1, fp);
        VSIFReadL(&pBand->m_dMode, sizeof(pBand->m_dMode), 1, fp);
        pBand->UpdateMetadataList();
        
        pDS->SetBand(n + 1, pBand);
    }
    
    uint64_t tilesize;
    VSIFReadL(&tilesize, sizeof(tilesize), 1, fp);
    
    double transform[6];
    VSIFReadL(transform, sizeof(transform), 1, fp);
    pDS->SetGeoTransform(transform);
    
    uint64_t wktSize;
    VSIFReadL(&wktSize, sizeof(wktSize), 1, fp);
    
    char *pszWKT = new char[wktSize];
    VSIFReadL(pszWKT, wktSize, 1, fp);
    OGRSpatialReference sr(pszWKT);
    pDS->SetSpatialRef(&sr);
    delete[] pszWKT;
    
    uint64_t ntiles;
    VSIFReadL(&ntiles, sizeof(ntiles), 1, fp);
    
    for( uint64_t n = 0; n < ntiles; n++ )
    {
        uint64_t offset;
        VSIFReadL(&offset, sizeof(offset), 1, fp);
        uint64_t size;
        VSIFReadL(&size, sizeof(size), 1, fp);
        uint64_t uncompressedSize;
        VSIFReadL(&uncompressedSize, sizeof(uncompressedSize), 1, fp);
        uint64_t ovrLevel;
        VSIFReadL(&ovrLevel, sizeof(ovrLevel), 1, fp);
        uint64_t band;
        VSIFReadL(&band, sizeof(band), 1, fp);
        uint64_t x;
        VSIFReadL(&x, sizeof(x), 1, fp);
        uint64_t y;
        VSIFReadL(&y, sizeof(y), 1, fp);
        
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

GDALDataset *EMUDataset::Create(const char * pszFilename,
                                int nXSize, int nYSize, int nBands,
                                GDALDataType eType,
                                char ** /* papszParamList */)
{
    if( (eType < GDT_Byte ) || (eType > GDT_Int16) )
    {
        // for now, I'm worried about building the histogram otherwise
        // things will blow out
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "Attempt to create EMU labeled dataset with an illegal "
            "data type (%s).",
            GDALGetDataTypeName(eType));
        return NULL;
    }

    // Try to create the file.
    VSILFILE *fp = VSIFOpenL(pszFilename, "w");
    if( fp == NULL )
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                "Attempt to create file `%s' failed.",
                pszFilename);
        return NULL;
    }
    
    VSIFPrintfL(fp, "EMU%04d", EMU_VERSION);
    
    EMUDataset *pDS = new EMUDataset(fp, eType, nXSize, nYSize, GA_Update);
    for( int n = 0; n < nBands; n++ )
    {
        pDS->SetBand(n + 1, new EMURasterBand(pDS, n + 1, eType, false, pDS->m_mutex));
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

    poDriver->pfnOpen = EMUDataset::Open;
    poDriver->pfnIdentify = EMUDataset::Identify;
    poDriver->pfnCreate = EMUDataset::Create;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
