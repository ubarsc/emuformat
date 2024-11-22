/*
 *  emudataset.h
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

#ifndef EMUDATASET_H
#define EMUDATASET_H

#include "gdal_priv.h"

#include <mutex>
#include <thread>
#include <unordered_map>


#if (GDAL_VERSION_MAJOR > 3) || ((GDAL_VERSION_MAJOR == 3) && (GDAL_VERSION_MINOR >= 7))
    #define HAVE_RFC91
#endif

const int EMU_VERSION = 1;

struct EMUTileKey
{
    uint64_t ovrLevel;  // 0 for full res
    uint64_t band;
    uint64_t x;
    uint64_t y;
    
    bool operator==(const EMUTileKey &other) const
    {
        return ((ovrLevel == other.ovrLevel)
            && (band == other.band)
            && (x == other.x) && (y == other.y));
    }
};

template <>
struct std::hash<EMUTileKey>
{
    std::size_t operator()(const EMUTileKey& k) const
    {
        std::size_t h1 = std::hash<uint64_t>{}(k.ovrLevel);
        std::size_t h2 = std::hash<uint64_t>{}(k.band);
        std::size_t h3 = std::hash<uint64_t>{}(k.x);
        std::size_t h4 = std::hash<uint64_t>{}(k.y);
        return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
    }
};

struct EMUTileValue
{
    uint64_t offset;
    uint64_t size;
    uint64_t uncompressedSize;
};

class EMUDataset final: public GDALDataset
{
public:
    EMUDataset(VSILFILE *, GDALDataType eType, int nXSize, int nYSize, GDALAccess eInAccess, bool bCloudOptimised, int nTileSize);
    ~EMUDataset();

    static GDALDataset *Open( GDALOpenInfo * );
    static int Identify( GDALOpenInfo * );
    static GDALDataset *Create(const char * pszFilename,
                                int nXSize, int nYSize, int nBands,
                                GDALDataType eType,
                                char **);
    static GDALDataset *CreateCopy( const char * pszFilename, GDALDataset *pSrcDs,
                                int bStrict, char **  papszParmList, 
                                GDALProgressFunc pfnProgress, void *pProgressData );

    CPLErr GetGeoTransform( double * padfTransform ) override;
    CPLErr SetGeoTransform( double * padfTransform ) override;
    const OGRSpatialReference* GetSpatialRef() const override;
    CPLErr SetSpatialRef(const OGRSpatialReference*) override;
#ifdef HAVE_RFC91
    CPLErr Close() override;
#else
    CPLErr Close();
#endif
    
protected:
    virtual CPLErr IBuildOverviews(const char *pszResampling, int nOverviews, const int *panOverviewList, 
                                    int nListBands, const int *panBandList, GDALProgressFunc pfnProgress, 
                                    void *pProgressData, CSLConstList papszOptions) override;
    
private:
    void setTileOffset(uint64_t o, uint64_t band, uint64_t x, 
        uint64_t y, vsi_l_offset offset, uint64_t size, uint64_t uncompressedSize);
    EMUTileValue getTileOffset(uint64_t o, uint64_t band, uint64_t x, uint64_t y);
    static VSILFILE *CreateEMU(const char * pszFilename,
                                int nXSize, int nYSize, int nBands,
                                GDALDataType eType);


    VSILFILE  *m_fp = nullptr;
    OGRSpatialReference m_oSRS{};
    std::unordered_map<EMUTileKey, EMUTileValue> m_tileOffsets;
    double m_padfTransform[6];
    uint32_t m_tileSize;
    std::shared_ptr<std::mutex> m_mutex;
    GDALDataType m_eType;
    bool m_bCloudOptimised;
    
    friend class EMUBaseBand;
    friend class EMURat;
};
#endif //EMUDATASET_H
