

#include "gdal_pam.h"

#include <mutex>
#include <thread>
#include <unordered_map>

const int COMPRESSION_NONE = 0;
const int COMPRESSION_ZLIB = 1;

struct EMUTileKey
{
    uint64_t ovrLevel;  // 0 for full res
    uint64_t band;
    uint64_t x;
    uint64_t y;
    
    bool operator==(const EMUTileKey &other) const
    {
        return (ovrLevel == other.ovrLevel
            && band == other.band
            && x == other.x && y == other.y);
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
    vsi_l_offset offset;
    uint64_t size;
};

class EMUDataset final: public GDALPamDataset
{
public:
    EMUDataset(VSILFILE *);
    ~EMUDataset();

    static GDALDataset *Open( GDALOpenInfo * );
    static int Identify( GDALOpenInfo * );
    static GDALDataset *Create(const char * pszFilename,
                                int nXSize, int nYSize, int nBands,
                                GDALDataType eType,
                                char **);

    CPLErr GetGeoTransform( double * padfTransform ) override;
    CPLErr SetGeoTransform( double * padfTransform ) override;
    const OGRSpatialReference* GetSpatialRef() const override;
    CPLErr SetSpatialRef(const OGRSpatialReference*) override;
    CPLErr Close() override;
    
private:
    void setTileOffset(uint64_t o, uint64_t band, uint64_t x, 
        uint64_t y, vsi_l_offset offset, uint64_t size);
    EMUTileValue getTileOffset(uint64_t o, uint64_t band, uint64_t x, uint64_t y);


    VSILFILE  *m_fp = nullptr;
    OGRSpatialReference m_oSRS{};
    std::unordered_map<EMUTileKey, EMUTileValue> m_tileOffsets;
    double m_padfTransform[6];
    uint64_t m_tileSize;
    std::mutex m_mutex;
    
    friend class EMURasterBand;
};

class EMURasterBand final: public GDALPamRasterBand
{
public:
    EMURasterBand(EMUDataset *, int nBandIn, GDALDataType eType);
    ~EMURasterBand();
    
    virtual CPLErr IReadBlock( int, int, void * ) override;
    virtual CPLErr IWriteBlock( int, int, void * ) override;
    
private:
    friend class EMUDataset;
};


EMUDataset::EMUDataset(VSILFILE *fp)
{
    m_fp = fp;
    for( int i = 0; i < 6; i++ )
        m_padfTransform[i] = 0;
    m_padfTransform[1] = 1;
    m_padfTransform[5] = -1;
    
    // TODO: allow override
    m_tileSize = 512;
}

EMUDataset::~EMUDataset()
{
    Close();
}

CPLErr EMUDataset::Close()
{
    CPLErr eErr = CE_None;
    if( nOpenFlags != OPEN_FLAGS_CLOSED )
    {
        if( FlushCache(true) != CE_None )
            eErr = CE_Failure;

        if( m_fp )  
        {
            // now write header
            vsi_l_offset headerOffset = VSIFTellL(m_fp);
            VSIFWriteL("HDR", 4, 1, m_fp);
            
            // TODO: endianness
            uint64_t val = GetRasterCount();
            VSIFWriteL(&val, sizeof(val), 1, m_fp);
            
            val = GetRasterXSize();
            VSIFWriteL(&val, sizeof(val), 1, m_fp);
            
            val = GetRasterYSize();
            VSIFWriteL(&val, sizeof(val), 1, m_fp);
            
            // number of overviews
            val = 0;
            VSIFWriteL(&val, sizeof(val), 1, m_fp);
            
            // tilesize
            VSIFWriteL(&m_tileSize, sizeof(m_tileSize), 1, m_fp);
            
            // geo transform
            VSIFWriteL(&m_padfTransform, sizeof(m_padfTransform), 1, m_fp);
            
            // projection
            char *pszWKT;
            m_oSRS.exportToWkt(&pszWKT);
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
    uint64_t y, vsi_l_offset offset, uint64_t size)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    m_tileOffsets.insert({{o, band, x, y}, {offset, size}});
}

EMUTileValue EMUDataset::getTileOffset(uint64_t o, uint64_t band, uint64_t x, uint64_t y)
{
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_tileOffsets[{o, band, x, y}];
}


GDALDataset *EMUDataset::Open(GDALOpenInfo *poOpenInfo)
{
    return nullptr;
}

int EMUDataset::Identify(GDALOpenInfo *poOpenInfo)
{
    return FALSE;
}

GDALDataset *EMUDataset::Create(const char * pszFilename,
                                int nXSize, int nYSize, int nBands,
                                GDALDataType eType,
                                char ** /* papszParamList */)
{
    // Try to create the file.
    VSILFILE *fp = VSIFOpenL(pszFilename, "w");
    if( fp == NULL )
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                "Attempt to create file `%s' failed.",
                pszFilename);
        return NULL;
    }
    
    VSIFWriteL("EMU", 4, 1, fp);
    
    EMUDataset *pDS = new EMUDataset(fp);
    for( int n = 0; n < nBands; n++ )
    {
        pDS->SetBand(n + 1, new EMURasterBand(pDS, n + 1, eType));
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

EMURasterBand::EMURasterBand(EMUDataset *pDataset, int nBandIn, GDALDataType eType)
{
    poDS = pDataset;
    nBlockXSize = 512;
    nBlockYSize = 512;
    nBand = nBandIn;
    eDataType = eType;
}


CPLErr EMURasterBand::IReadBlock(int nBlockXOff, int nBlockYOff, void *pData)
{
    return CE_Failure;
}

CPLErr EMURasterBand::IWriteBlock(int nBlockXOff, int nBlockYOff, void *pData)
{
    EMUDataset *poEMUDS = cpl::down_cast<EMUDataset *>(poDS);
    
    vsi_l_offset tileOffset = VSIFTellL(poEMUDS->m_fp);
    int typeSize = GDALGetDataTypeSize(eDataType) / 8;
    
    // size of block
    uint64_t blockSize = (nBlockXSize * typeSize) * (nBlockYSize * typeSize);
    
    // TODO: compression
    uint64_t compression = COMPRESSION_NONE;
    VSIFWriteL(&compression, sizeof(compression), 1, poEMUDS->m_fp);

    // data
    VSIFWriteL(pData, blockSize, 1, poEMUDS->m_fp);
    
    // update map
    poEMUDS->setTileOffset(0, nBand, nBlockXOff, nBlockXOff, tileOffset, blockSize);
    
    return CE_None;
}

#ifdef MSVC
    #define EMU_EXPORT __declspec(dllexport)
#else
    #define EMU_EXPORT
#endif

EMU_EXPORT void GDALRegister_EMU()
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
